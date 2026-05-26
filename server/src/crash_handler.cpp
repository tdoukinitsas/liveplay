// ============================================================================
// crash_handler.cpp — see crash_handler.hpp.
// ============================================================================
#include "liveplay/crash_handler.hpp"
#include "liveplay/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dbghelp.h>
#  pragma comment(lib, "dbghelp.lib")
#else
#  include <csignal>
#  include <cstring>
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define LIVEPLAY_HAVE_EXECINFO 1
#  endif
#  include <unistd.h>
#endif

namespace liveplay {
namespace {

std::filesystem::path g_crash_log_dir;
std::once_flag        g_install_flag;
std::atomic<bool>     g_in_handler{false};

std::string timestamp_for_filename() {
    using clock = std::chrono::system_clock;
    const auto now = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return os.str();
}

std::filesystem::path resolve_crash_log_path() {
    namespace fs = std::filesystem;
    fs::path dir = g_crash_log_dir;
    std::error_code ec;
    if (dir.empty()) {
        dir = fs::current_path(ec);
        if (ec) dir = ".";
    } else {
        fs::create_directories(dir, ec);
    }
    return dir / ("crash-" + timestamp_for_filename() + ".log");
}

#if defined(_WIN32)
// Capture and format a stack trace using DbgHelp. The CONTEXT* is the one
// supplied by SetUnhandledExceptionFilter; for non-SEH paths we pass nullptr
// and capture the current context inline.
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
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;

        DWORD64 displacement = 0;
        std::string name = "(unknown)";
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, sym)) {
            name = sym->Name;
        }

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
    for (int i = 0; i < n; ++i) {
        out << "  #" << i << "  " << (syms ? syms[i] : "(?)") << "\n";
    }
    if (syms) std::free(syms);
    return out.str();
#else
    return "(stack trace unavailable: execinfo.h not present)\n";
#endif
}
#endif

// Write `body` to stderr (via Logger) AND to a crash-<timestamp>.log file.
// Best-effort; never throws.
void emit_crash_report(const std::string& reason, const std::string& trace) {
    try {
        Logger::error("==================== CRASH ====================");
        Logger::error("Reason: {}", reason);
        Logger::error("Stack trace:");
        std::istringstream is{trace};
        std::string line;
        while (std::getline(is, line)) Logger::error("{}", line);
        Logger::error("===============================================");
    } catch (...) {
        // Fall back to raw stderr — Logger may itself be broken mid-crash.
        std::fprintf(stderr, "CRASH: %s\n%s\n", reason.c_str(), trace.c_str());
    }

    try {
        const auto path = resolve_crash_log_path();
        std::ofstream f{path, std::ios::binary | std::ios::trunc};
        if (f) {
            f << "LivePlay server crash report\n"
              << "Time:   " << timestamp_for_filename() << "\n"
              << "Reason: " << reason << "\n\n"
              << "Stack trace:\n" << trace << std::flush;
            try { Logger::error("Crash log written to: {}", path.string()); } catch (...) {}
        }
    } catch (...) { /* nothing more we can do */ }

#if defined(_WIN32)
    // Hold the console open so the operator can read it before the window
    // closes. 30 seconds is long enough to screenshot, short enough that an
    // unattended server will eventually exit.
    try { Logger::error("Server will exit in 30 seconds — copy this log first."); } catch (...) {}
    Sleep(30000);
#endif
}

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
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        const auto kind = info->ExceptionRecord->ExceptionInformation[0];
        const auto addr2 = info->ExceptionRecord->ExceptionInformation[1];
        r << "  [" << (kind == 0 ? "read" : kind == 1 ? "write" : "execute")
          << " of 0x" << std::hex << addr2 << "]";
    }
    emit_crash_report(r.str(), format_stack_trace_windows(info->ContextRecord));
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

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

void install_crash_handlers(const std::string& log_dir) {
    std::call_once(g_install_flag, [&]{
        g_crash_log_dir = log_dir;

        std::set_terminate(&terminate_handler);

#if defined(_WIN32)
        SetUnhandledExceptionFilter(&seh_filter);
        // Make CRT errors (e.g. heap corruption) call our terminate handler
        // instead of popping a modal dialog that nobody is around to dismiss.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
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

} // namespace liveplay
