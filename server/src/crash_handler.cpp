// ============================================================================
// crash_handler.cpp — see crash_handler.hpp.
// ----------------------------------------------------------------------------
// Two crash-emit paths:
//   * emit_crash_report()      — rich report (session history, symbolised
//                                stack). Used from contexts where it is safe to
//                                allocate / lock / touch the filesystem:
//                                Windows SEH + CRT handlers, and std::terminate.
//   * async_signal_crash()     — POSIX-only, async-signal-SAFE path used from
//                                the fatal-signal handler (SIGSEGV/ABRT/…). Uses
//                                only write()/open()/backtrace_symbols_fd()/fork/
//                                execve, never malloc / mutex / iostream, so it
//                                can't deadlock on a crash that happened while
//                                the faulting thread held the malloc arena lock.
//
// Both paths share a persisted consecutive-crash counter so a deterministic
// crash can't relaunch the server forever (crash-loop protection), and both
// read crash-resume state from a lock-free double buffer that is never observed
// half-written.
// ============================================================================
#include "liveplay/crash_handler.hpp"
#include "liveplay/logger.hpp"

#include <algorithm>
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
#  include <cerrno>
#  include <fcntl.h>
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

char g_exe_path[kPathBuf]     = {};
char g_restart_args[kArgBuf]  = {};
char g_project_dir[kPathBuf]  = {};

// Pre-resolved + pre-created crash-log directory. Refreshed in safe context
// (install / set_crash_project_dir) so the signal handler never has to call
// create_directories(). Empty until first refresh.
char g_crash_log_dir_buf[kPathBuf] = {};

// ---- Crash-loop protection -------------------------------------------------
char             g_counter_file[kPathBuf] = {};
std::atomic<int> g_crash_count{0};       // consecutive crashes so far
std::atomic<int> g_max_consecutive{0};   // 0 = guard disabled (always restart)

// ---- Crash-resume state (lock-free double buffer) --------------------------
// The writer fills the inactive buffer completely, then atomically flips the
// active index. A reader (the crash handler) only ever reads the active index,
// which always points at a fully-written record — even if the crashing thread
// was interrupted mid-write, it was writing the *inactive* buffer.
struct ResumeState {
    char   project_file[kPathBuf] = {};
    char   item_uuid[kUuidBuf]    = {};
    double position_sec           = 0.0;
};
ResumeState      g_resume[2];
std::atomic<int> g_resume_active{-1};   // -1 = no record yet

long long current_pid() {
#if defined(_WIN32)
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

// ---------------------------------------------------------------------------
// Helpers (safe context)
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
    // Seconds resolution + PID keep filenames unique across a fast crash loop
    // (minute resolution + O_TRUNC used to overwrite prior logs).
    std::snprintf(buf, sizeof(buf), "%04d_%02d_%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string{buf};
}

// Recompute + create the crash-log directory into g_crash_log_dir_buf. Safe
// context only (allocates / touches the filesystem).
void refresh_log_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir;
    if (g_project_dir[0] != '\0') {
        dir = fs::path{g_project_dir} / "logs";
    } else if (!g_crash_log_dir.empty()) {
        dir = g_crash_log_dir;
    } else {
        dir = fs::current_path(ec);
        if (ec) dir = ".";
    }
    fs::create_directories(dir, ec);
    const std::string s = dir.string();
    std::strncpy(g_crash_log_dir_buf, s.c_str(), kPathBuf - 1);
    g_crash_log_dir_buf[kPathBuf - 1] = '\0';
}

std::filesystem::path resolve_crash_log_path() {
    namespace fs = std::filesystem;
    fs::path dir = g_crash_log_dir_buf[0] ? fs::path{g_crash_log_dir_buf} : fs::path{"."};
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / ("liveplay-crash-" + timestamp_for_filename() + "-" +
                  std::to_string(current_pid()) + ".log");
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

// Increment the persisted consecutive-crash counter and report whether the
// server should still auto-restart. Safe context (uses ofstream).
bool bump_counter_and_should_restart() {
    const int new_count = g_crash_count.load(std::memory_order_acquire) + 1;
    g_crash_count.store(new_count, std::memory_order_release);
    if (g_counter_file[0] != '\0') {
        std::ofstream cf{g_counter_file, std::ios::binary | std::ios::trunc};
        if (cf) cf << new_count;
    }
    const int maxc = g_max_consecutive.load(std::memory_order_acquire);
    return !(maxc > 0 && new_count > maxc);
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
// Rich crash report — SAFE contexts only (Windows SEH/CRT, std::terminate).
// ---------------------------------------------------------------------------
void emit_crash_report(const std::string& reason, const std::string& trace) {
    const bool will_restart = bump_counter_and_should_restart();

    // 1. Console output — visible to the operator while the window is up.
    try {
        Logger::error("==================== SERVER CRASH ====================");
        Logger::error("Reason : {}", reason);
        if (will_restart)
            Logger::error("Restart: server will relaunch in 5 seconds.");
        else
            Logger::error("Restart: DISABLED — too many consecutive crashes; not relaunching.");
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
              << "PID    : " << current_pid() << "\n"
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
    const int ri = g_resume_active.load(std::memory_order_acquire);
    if (g_exe_path[0] != '\0' && ri >= 0 && g_resume[ri].project_file[0] != '\0') {
        try {
            namespace fs = std::filesystem;
            const fs::path resume_path =
                fs::path{g_exe_path}.parent_path() / ".crash-resume.json";
            std::ofstream rf{resume_path, std::ios::binary | std::ios::trunc};
            if (rf) {
                char pos_buf[64];
                std::snprintf(pos_buf, sizeof(pos_buf), "%.3f", g_resume[ri].position_sec);
                rf << "{\n"
                   << "  \"projectFile\": \""  << json_escape(g_resume[ri].project_file) << "\",\n"
                   << "  \"itemUuid\": \""      << json_escape(g_resume[ri].item_uuid)    << "\",\n"
                   << "  \"positionSec\": "     << pos_buf                                << "\n"
                   << "}\n";
            }
        } catch (...) {}
    }

    if (!will_restart) return;

    // 4. Relaunch the server, then exit so the OS releases our listening port.
    //    We spawn immediately and the *new* instance waits before binding — by
    //    which point we're gone.
    //
    //    How long it waits is one decision, and it used to be written twice in
    //    two different units: 5000 on the Windows command line and 5 in the
    //    POSIX child's sleep. Two spellings of the same number is how they end
    //    up different, and this particular number is load-bearing — too short
    //    and the relaunched server finds the port still held by the corpse that
    //    spawned it, then exits, and the show stays down.
    constexpr int kRestartDelayMs = 5000;
#if defined(_WIN32)
    if (g_exe_path[0] != '\0') {
        std::string cmd = std::string{"\""} + g_exe_path + "\"";
        if (g_restart_args[0] != '\0') { cmd += ' '; cmd += g_restart_args; }
        // The new instance takes the wait itself, before binding.
        cmd += " --start-delay-ms " + std::to_string(kRestartDelayMs);

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
    if (g_exe_path[0] != '\0') {
        const pid_t pid = ::fork();
        if (pid == 0) {
            // No --start-delay-ms here: the fork IS the new instance, so it
            // takes the same wait directly rather than asking for it. nanosleep
            // rather than sleep() so the delay keeps its millisecond resolution
            // if it is ever changed to something that is not a whole second.
            struct timespec ts{};
            ts.tv_sec  = kRestartDelayMs / 1000;
            ts.tv_nsec = static_cast<long>(kRestartDelayMs % 1000) * 1'000'000L;
            ::nanosleep(&ts, nullptr);
            if (g_restart_args[0] != '\0') {
                std::string cmd = std::string{g_exe_path} + " " + g_restart_args;
                ::execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            } else {
                ::execl(g_exe_path, g_exe_path, nullptr);
            }
            ::_exit(1);
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Async-signal-safe crash emit — POSIX fatal-signal path ONLY.
// ---------------------------------------------------------------------------
#if !defined(_WIN32)
// Small async-signal-safe output primitives (no malloc / stdio / locale).
struct AsBuf { char* p; std::size_t cap; std::size_t len; };

void as_append(AsBuf& b, const char* s) {
    while (*s && b.len + 1 < b.cap) b.p[b.len++] = *s++;
    b.p[b.len] = '\0';
}
void as_append_ll(AsBuf& b, long long v) {
    char t[32]; int i = static_cast<int>(sizeof(t));
    const bool neg = v < 0;
    unsigned long long u = neg ? (0ULL - static_cast<unsigned long long>(v))
                               : static_cast<unsigned long long>(v);
    if (u == 0) t[--i] = '0';
    while (u) { t[--i] = static_cast<char>('0' + u % 10); u /= 10; }
    if (neg) t[--i] = '-';
    for (; i < static_cast<int>(sizeof(t)) && b.len + 1 < b.cap; ++i) b.p[b.len++] = t[i];
    b.p[b.len] = '\0';
}
void as_write(int fd, const char* s, std::size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, s, n);
        if (w <= 0) { if (errno == EINTR) continue; break; }
        s += w; n -= static_cast<std::size_t>(w);
    }
}
void as_write_str(int fd, const char* s) {
    std::size_t n = 0; while (s[n]) ++n; as_write(fd, s, n);
}
void as_write_ll(int fd, long long v) {
    char t[32]; AsBuf b{t, sizeof(t), 0}; t[0] = '\0';
    as_append_ll(b, v); as_write(fd, t, b.len);
}
void as_write_escaped(int fd, const char* s) {
    for (; *s; ++s) {
        switch (*s) {
            case '"':  as_write(fd, "\\\"", 2); break;
            case '\\': as_write(fd, "\\\\", 2); break;
            case '\n': as_write(fd, "\\n", 2);  break;
            case '\r': as_write(fd, "\\r", 2);  break;
            default:   as_write(fd, s, 1);      break;
        }
    }
}
void as_write_fixed3(int fd, double v) {
    bool neg = v < 0; if (neg) v = -v;
    long long whole = static_cast<long long>(v);
    long long frac  = static_cast<long long>((v - static_cast<double>(whole)) * 1000.0 + 0.5);
    if (frac >= 1000) { whole += 1; frac -= 1000; }
    if (neg) as_write(fd, "-", 1);
    as_write_ll(fd, whole);
    as_write(fd, ".", 1);
    char f[3] = { static_cast<char>('0' + (frac / 100) % 10),
                  static_cast<char>('0' + (frac / 10) % 10),
                  static_cast<char>('0' + frac % 10) };
    as_write(fd, f, 3);
}

void async_signal_crash(const char* reason) {
    const long long epoch = static_cast<long long>(::time(nullptr));
    const long long pid   = static_cast<long long>(::getpid());

    // Build "<dir>/liveplay-crash-<epoch>-<pid>.log" without allocating.
    char path[kPathBuf]; AsBuf pb{path, sizeof(path), 0}; path[0] = '\0';
    if (g_crash_log_dir_buf[0]) { as_append(pb, g_crash_log_dir_buf); as_append(pb, "/"); }
    as_append(pb, "liveplay-crash-");
    as_append_ll(pb, epoch); as_append(pb, "-"); as_append_ll(pb, pid);
    as_append(pb, ".log");

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        as_write_str(fd, "LivePlay Server — Crash Report\n"
                         "================================\n"
                         "Time (epoch): ");
        as_write_ll(fd, epoch);
        as_write_str(fd, "\nPID    : "); as_write_ll(fd, pid);
        as_write_str(fd, "\nReason : "); as_write_str(fd, reason);
        as_write_str(fd, "\n\nStack trace:\n");
#if defined(LIVEPLAY_HAVE_EXECINFO)
        void* bt[64];
        const int n = backtrace(bt, 64);
        backtrace_symbols_fd(bt, n, fd);   // async-signal-safe (no malloc)
#else
        as_write_str(fd, "(stack trace unavailable: execinfo.h not present)\n");
#endif
        as_write_str(fd, "\n(session history is in the rolling server log, if enabled)\n");
        ::close(fd);
    }

    // Visible operator signal on stderr.
    as_write_str(STDERR_FILENO, "\n==================== SERVER CRASH ====================\n");
    as_write_str(STDERR_FILENO, reason);
    as_write_str(STDERR_FILENO, "\nCrash log: "); as_write_str(STDERR_FILENO, path);
    as_write_str(STDERR_FILENO, "\n");

    // Persist crash-resume state from the double buffer (always complete).
    const int ri = g_resume_active.load(std::memory_order_acquire);
    if (g_exe_path[0] && ri >= 0 && g_resume[ri].project_file[0]) {
        char rpath[kPathBuf]; AsBuf rb{rpath, sizeof(rpath), 0}; rpath[0] = '\0';
        as_append(rb, g_exe_path);
        // Trim to the executable's parent directory.
        std::size_t cut = rb.len;
        while (cut > 0 && rpath[cut - 1] != '/') --cut;
        rb.len = cut; rpath[rb.len] = '\0';
        as_append(rb, ".crash-resume.json");
        const int rfd = ::open(rpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (rfd >= 0) {
            as_write_str(rfd, "{\n  \"projectFile\": \"");
            as_write_escaped(rfd, g_resume[ri].project_file);
            as_write_str(rfd, "\",\n  \"itemUuid\": \"");
            as_write_escaped(rfd, g_resume[ri].item_uuid);
            as_write_str(rfd, "\",\n  \"positionSec\": ");
            as_write_fixed3(rfd, g_resume[ri].position_sec);
            as_write_str(rfd, "\n}\n");
            ::close(rfd);
        }
    }

    // Crash-loop guard: persist the incremented counter and decide.
    const int new_count = g_crash_count.load(std::memory_order_acquire) + 1;
    if (g_counter_file[0]) {
        const int cfd = ::open(g_counter_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (cfd >= 0) { as_write_ll(cfd, new_count); ::close(cfd); }
    }
    const int maxc = g_max_consecutive.load(std::memory_order_acquire);
    if (maxc > 0 && new_count > maxc) {
        as_write_str(STDERR_FILENO,
                     "Auto-restart DISABLED — too many consecutive crashes.\n");
        return;
    }

    // Relaunch: fork a child that waits then re-execs; the parent returns and
    // the caller re-raises the signal so the OS frees our listening port.
    if (g_exe_path[0]) {
        const pid_t child = ::fork();
        if (child == 0) {
            ::sleep(5);
            if (g_restart_args[0]) {
                char cmd[kArgBuf + kPathBuf + 8];
                AsBuf cb{cmd, sizeof(cmd), 0}; cmd[0] = '\0';
                as_append(cb, g_exe_path); as_append(cb, " "); as_append(cb, g_restart_args);
                ::execl("/bin/sh", "sh", "-c", cmd, static_cast<char*>(nullptr));
            } else {
                ::execl(g_exe_path, g_exe_path, static_cast<char*>(nullptr));
            }
            ::_exit(127);
        }
    }
}
#endif // !_WIN32

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
    // Async-signal-safe path: no malloc / mutex / iostream / filesystem.
    async_signal_crash(name);
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
        refresh_log_dir();

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
    g_project_dir[kPathBuf - 1] = '\0';
    refresh_log_dir();
}

void update_crash_resume_state(const std::string& project_file,
                                const std::string& playing_item_uuid,
                                double             position_sec) {
    // Fill the inactive buffer, then publish it by flipping the active index.
    const int active = g_resume_active.load(std::memory_order_acquire);
    const int next   = (active == 0) ? 1 : 0;   // -1 or 1 → 0; 0 → 1
    ResumeState& b = g_resume[next];
    std::strncpy(b.project_file, project_file.c_str(),      kPathBuf - 1);
    b.project_file[kPathBuf - 1] = '\0';
    std::strncpy(b.item_uuid,    playing_item_uuid.c_str(), kUuidBuf - 1);
    b.item_uuid[kUuidBuf - 1] = '\0';
    b.position_sec = position_sec;
    g_resume_active.store(next, std::memory_order_release);
}

void set_crash_restart_guard(const std::string& counter_file,
                             int consecutive_so_far,
                             int max_consecutive) {
    std::strncpy(g_counter_file, counter_file.c_str(), kPathBuf - 1);
    g_counter_file[kPathBuf - 1] = '\0';
    g_crash_count.store(consecutive_so_far < 0 ? 0 : consecutive_so_far,
                        std::memory_order_release);
    g_max_consecutive.store(max_consecutive, std::memory_order_release);
}

void reset_crash_restart_guard() {
    g_crash_count.store(0, std::memory_order_release);
    if (g_counter_file[0] != '\0') {
        std::ofstream cf{g_counter_file, std::ios::binary | std::ios::trunc};
        if (cf) cf << 0;
    }
}

void prune_crash_logs(const std::string& dir, std::size_t keep) {
    namespace fs = std::filesystem;
    try {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;
        std::vector<fs::path> logs;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            const auto& p = entry.path();
            const std::string fn = p.filename().string();
            if (fn.rfind("liveplay-crash-", 0) == 0 && p.extension() == ".log")
                logs.push_back(p);
        }
        if (logs.size() <= keep) return;
        std::sort(logs.begin(), logs.end(), [](const fs::path& a, const fs::path& b) {
            std::error_code e1, e2;
            return fs::last_write_time(a, e1) < fs::last_write_time(b, e2);
        });
        for (std::size_t i = 0; i + keep < logs.size(); ++i) {
            std::error_code rec;
            fs::remove(logs[i], rec);
        }
    } catch (...) {
        // Best-effort pruning — never let it affect startup.
    }
}

} // namespace liveplay
