// ============================================================================
// crash_handler.cpp — see crash_handler.hpp.
// ============================================================================
#include "liveplay/crash_handler.hpp"
#include "liveplay/logger.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dbghelp.h>
#  pragma comment(lib, "dbghelp.lib")
#else
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define LIVEPLAY_HAVE_EXECINFO 1
#  endif
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#endif

namespace liveplay {
namespace {

// ---------------------------------------------------------------------------
// Crash-safe global state (fixed-size C arrays; no dynamic allocation in
// handler paths). Written from normal threads, read from the crash handler.
// ---------------------------------------------------------------------------
std::filesystem::path g_crash_log_dir;   // fallback if no project dir is set
std::once_flag        g_install_flag;
std::atomic<bool>     g_in_handler{false};

constexpr std::size_t kPathBuf = 4096;
constexpr std::size_t kArgBuf  = 8192;
constexpr std::size_t kUuidBuf = 256;

char g_exe_path[kPathBuf]           = {};
char g_restart_args[kArgBuf]        = {};
char g_project_dir[kPathBuf]        = {};
char g_resume_project_file[kPathBuf]= {};
char g_resume_item_uuid[kUuidBuf]   = {};
// Plain double is fine here; worst case: a torn read gives a slightly wrong
// seek position — acceptable in a crash-resume scenario.
double g_resume_position_sec        = 0.0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string timestamp_for_filename() {
    using clock = std::chrono::system_clock;
    const auto now = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    // Format: YYYY_MM_DD-HHMM  (as requested)
    std::snprintf(buf, sizeof(buf), "%04d_%02d_%02d-%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min);
    return std::string{buf};
}

std::filesystem::path resolve_crash_log_path() {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path dir;
    if (g_project_dir[0] != '\0') {
        // Preferred: <project_dir>/logs/
        dir = fs::path{g_project_dir} / "logs";
    } else if (!g_crash_log_dir.empty()) {
        dir = g_crash_log_dir;
    } else {
        dir = fs::current_path(ec);
        if (ec) dir = ".";
    }
    fs::create_directories(dir, ec);
    return dir / ("liveplay-crash-" + timestamp_for_filename() + ".log");
}

// Minimal JSON string escaper — safe to call from the crash handler.
std::string json_escape(const char* s) {
    std::string out;
    for (; *s; ++s) {
        switch (*s) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += *s;     break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stack-trace helpers
// ---------------------------------------------------------------------------
#if defined(_WIN32)
std::string format_stack_trace_windows(CONTEXT* context_in) {
    static std::mutex sym_mutex;
    std::lock_guard lock{sym_mutex};

    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    static bool sym_initialized = false;
    if (!sym_initialized) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        sym_initialized = true;
    }

    CONTEXT local_context{};
    CONTEXT* context = context_in;
    if (!context) {
        local_context.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&local_context);
        context = &local_context;
    }

    STACKFRAME64 frame{};
    DWORD machine_type;
#if defined(_M_AMD64) || defined(__x86_64__)
    machine_type = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = context->Rip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp; frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86) || defined(__i386__)
    machine_type = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = context->Eip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = context->Ebp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Esp; frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64) || defined(__aarch64__)
    machine_type = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = context->Pc;  frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = context->Fp;  frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Sp;  frame.AddrStack.Mode = AddrModeFlat;
#else
    return "(stack trace unavailable: unsupported architecture)\n";
#endif

    std::ostringstream out;
    constexpr int kMaxFrames = 64;
    char sym_buf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 512;

    for (int i = 0; i < kMaxFrames; ++i) {
        if (!StackWalk64(machine_type, process, thread, &frame, context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (frame.AddrPC.Offset == 0) break;

        DWORD64 displacement = 0;
        std::string name = "(unknown)";
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, sym))
            name = sym->Name;

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD line_disp = 0;
        std::string file_line;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_disp, &line)) {
            std::ostringstream fl;
            fl << "  (" << line.FileName << ":" << line.LineNumber << ")";
            file_line = fl.str();
        }

        out << "  #" << std::setw(2) << std::setfill(' ') << i
            << "  0x" << std::hex << std::setw(16) << std::setfill('0')
            << frame.AddrPC.Offset << std::dec
            << "  " << name << "+0x" << std::hex << displacement << std::dec
            << file_line << "\n";
    }
    return out.str();
}
#endif // _WIN32

#if !defined(_WIN32)
std::string format_stack_trace_posix() {
#if defined(LIVEPLAY_HAVE_EXECINFO)
    void* buf[64];
    int n = backtrace(buf, 64);
    char** syms = backtrace_symbols(buf, n);
    std::ostringstream out;
    for (int i = 0; i < n; ++i)
        out << "  #" << i << "  " << (syms ? syms[i] : "(?)") << "\n";
    if (syms) std::free(syms);
    return out.str();
#else
    return "(stack trace unavailable: execinfo.h not present)\n";
#endif
}
#endif

// ---------------------------------------------------------------------------
// Core: write crash log + console output + restart after 5 s.
// ---------------------------------------------------------------------------
void emit_crash_report(const std::string& reason, const std::string& trace) {
    // 1. Console output — visible to the operator while the window is up.
    try {
        Logger::error("==================== SERVER CRASH ====================");
        Logger::error("Reason : {}", reason);
        Logger::error("Restart: server will relaunch in 5 seconds.");
        Logger::error("Log    : see liveplay-crash-*.log in the project /logs/ folder.");
        Logger::error("Stack trace:");
        std::istringstream is{trace};
        std::string line;
        while (std::getline(is, line)) Logger::error("  {}", line);
        Logger::error("======================================================");
    } catch (...) {
        std::fprintf(stderr, "CRASH: %s\n%s\n", reason.c_str(), trace.c_str());
    }

    // 2. Crash log file: header + stack trace + full session history.
    try {
        const auto path = resolve_crash_log_path();
        std::ofstream f{path, std::ios::binary | std::ios::trunc};
        if (f) {
            f << "LivePlay Server — Crash Report\n"
              << "================================\n"
              << "Time   : " << timestamp_for_filename() << "\n"
              << "Reason : " << reason << "\n\n"
              << "Stack trace:\n" << trace << "\n"
              << "================================\n"
              << "Session log (oldest first):\n\n";
            try { f << Logger::dump_history(); } catch (...) {}
            f << std::flush;
            try { Logger::error("Crash log written: {}", path.string()); } catch (...) {}
        }
    } catch (...) {}

    // 3. Persist resume state so the new instance can reopen the project and
    //    resume playback from approximately where it stopped.
    if (g_exe_path[0] != '\0' && g_resume_project_file[0] != '\0') {
        try {
            namespace fs = std::filesystem;

            // Store next to the exe so the new instance finds it at startup.
            const fs::path resume_path =
                fs::path{g_exe_path}.parent_path() / ".crash-resume.json";
            std::ofstream rf{resume_path, std::ios::binary | std::ios::trunc};
            if (rf) {
                char pos_buf[64];
                std::snprintf(pos_buf, sizeof(pos_buf), "%.3f", g_resume_position_sec);
                rf << "{\n"
                   << "  \"projectFile\": \""  << json_escape(g_resume_project_file) << "\",\n"
                   << "  \"itemUuid\": \""      << json_escape(g_resume_item_uuid)    << "\",\n"
                   << "  \"positionSec\": "     << pos_buf                            << "\n"
                   << "}\n";
            }
        } catch (...) {}
    }

    // 4. Relaunch the server, then exit so the OS releases our listening port.
    //    Rather than sleeping here (which would hold the port for 5 s and race
    //    the new instance for it), we spawn immediately and pass --start-delay-ms
    //    so the *new* instance waits before binding — by which point we're gone.
#if defined(_WIN32)
    if (g_exe_path[0] != '\0') {
        std::string cmd = std::string{"\""} + g_exe_path + "\"";
        if (g_restart_args[0] != '\0') { cmd += ' '; cmd += g_restart_args; }
        cmd += " --start-delay-ms 5000";

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<char> cmdline(cmd.begin(), cmd.end());
        cmdline.push_back('\0');
        CreateProcessA(nullptr, cmdline.data(),
                       nullptr, nullptr, FALSE,
                       CREATE_NEW_CONSOLE,
                       nullptr, nullptr, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);
    }
#else
    // Fork a child that sleeps then execs the server; the crashing parent exits.
    if (g_exe_path[0] != '\0') {
        const pid_t pid = ::fork();
        if (pid == 0) {
            // Child: sleep 5 s then exec the original binary.
            ::sleep(5);
            if (g_restart_args[0] != '\0') {
                // Use sh to re-parse the arg string.
                std::string cmd = std::string{g_exe_path} + " " + g_restart_args;
                ::execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            } else {
                ::execl(g_exe_path, g_exe_path, nullptr);
            }
            ::_exit(1);
        }
        // Parent falls through and exits normally below.
    }
#endif
}

// ---------------------------------------------------------------------------
// Platform handlers
// ---------------------------------------------------------------------------
#if defined(_WIN32)
LONG WINAPI seh_filter(EXCEPTION_POINTERS* info) {
    if (g_in_handler.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    const DWORD code = info && info->ExceptionRecord
                         ? info->ExceptionRecord->ExceptionCode : 0;
    const void* addr = info && info->ExceptionRecord
                         ? info->ExceptionRecord->ExceptionAddress : nullptr;

    const char* name = "Unknown SEH";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         name = "Access violation"; break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    name = "Array bounds exceeded"; break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:    name = "Datatype misalignment"; break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       name = "Float divide by zero"; break;
        case EXCEPTION_FLT_INVALID_OPERATION:    name = "Float invalid operation"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:      name = "Illegal instruction"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       name = "Integer divide by zero"; break;
        case EXCEPTION_PRIV_INSTRUCTION:         name = "Privileged instruction"; break;
        case EXCEPTION_STACK_OVERFLOW:           name = "Stack overflow"; break;
        case EXCEPTION_IN_PAGE_ERROR:            name = "In-page error"; break;
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: name = "Non-continuable exception"; break;
        default: break;
    }

    std::ostringstream r;
    r << name << " (0x" << std::hex << code << std::dec
      << ") at 0x" << std::hex << reinterpret_cast<std::uintptr_t>(addr);
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        const auto kind  = info->ExceptionRecord->ExceptionInformation[0];
        const auto addr2 = info->ExceptionRecord->ExceptionInformation[1];
        r << "  [" << (kind == 0 ? "read" : kind == 1 ? "write" : "execute")
          << " of 0x" << std::hex << addr2 << "]";
    }
    emit_crash_report(r.str(), format_stack_trace_windows(info->ContextRecord));
    return EXCEPTION_EXECUTE_HANDLER;
}

// CRT abort / invalid-parameter / pure-call all funnel here.
void crt_abort_handler() {
    if (g_in_handler.exchange(true)) { std::_Exit(1); return; }
    emit_crash_report("CRT abort / invalid operation",
                      format_stack_trace_windows(nullptr));
    std::_Exit(1);
}
#endif // _WIN32

void terminate_handler() {
    if (g_in_handler.exchange(true)) std::abort();
    std::string reason = "std::terminate called";
    if (auto ex = std::current_exception()) {
        try { std::rethrow_exception(ex); }
        catch (const std::exception& e) {
            reason = std::string{"Uncaught std::exception: "} + e.what();
        } catch (...) {
            reason = "Uncaught non-std exception (foreign type)";
        }
    }
#if defined(_WIN32)
    emit_crash_report(reason, format_stack_trace_windows(nullptr));
#else
    emit_crash_report(reason, format_stack_trace_posix());
#endif
    std::abort();
}

#if !defined(_WIN32)
extern "C" void posix_fatal_signal(int sig) {
    if (g_in_handler.exchange(true)) {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }
    const char* name = "Unknown signal";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV (segmentation fault)"; break;
        case SIGABRT: name = "SIGABRT (abort)"; break;
        case SIGFPE:  name = "SIGFPE (floating-point exception)"; break;
        case SIGILL:  name = "SIGILL (illegal instruction)"; break;
#  ifdef SIGBUS
        case SIGBUS:  name = "SIGBUS (bus error)"; break;
#  endif
        default: break;
    }
    emit_crash_report(name, format_stack_trace_posix());
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void install_crash_handlers(const std::string& log_dir) {
    std::call_once(g_install_flag, [&]{
        g_crash_log_dir = log_dir;

        std::set_terminate(&terminate_handler);

#if defined(_WIN32)
        SetUnhandledExceptionFilter(&seh_filter);

        // Suppress CRT error dialogs so a headless/crashed server doesn't
        // pop a modal nobody will click. Route CRT abort paths to our handler.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        std::signal(SIGABRT, [](int) { crt_abort_handler(); });
        _set_purecall_handler([]() { crt_abort_handler(); });
        _set_invalid_parameter_handler(
            [](const wchar_t*, const wchar_t*, const wchar_t*,
               unsigned int, uintptr_t) { crt_abort_handler(); });
#else
        struct sigaction sa{};
        sa.sa_handler = &posix_fatal_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        ::sigaction(SIGSEGV, &sa, nullptr);
        ::sigaction(SIGABRT, &sa, nullptr);
        ::sigaction(SIGFPE,  &sa, nullptr);
        ::sigaction(SIGILL,  &sa, nullptr);
#  ifdef SIGBUS
        ::sigaction(SIGBUS,  &sa, nullptr);
#  endif
#endif
    });
}

void set_crash_exe_info(const std::string& exe_path,
                        const std::string& restart_args) {
    std::strncpy(g_exe_path,      exe_path.c_str(),     kPathBuf - 1);
    std::strncpy(g_restart_args,  restart_args.c_str(), kArgBuf  - 1);
}

void set_crash_project_dir(const std::string& project_dir) {
    std::strncpy(g_project_dir, project_dir.c_str(), kPathBuf - 1);
}

void update_crash_resume_state(const std::string& project_file,
                                const std::string& playing_item_uuid,
                                double             position_sec) {
    std::strncpy(g_resume_project_file, project_file.c_str(),        kPathBuf - 1);
    std::strncpy(g_resume_item_uuid,    playing_item_uuid.c_str(),   kUuidBuf - 1);
    g_resume_position_sec = position_sec;
}

} // namespace liveplay
