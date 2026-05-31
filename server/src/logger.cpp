// ============================================================================
// logger.cpp — see include/liveplay/logger.hpp for the public contract.
// ============================================================================
#include "liveplay/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#if defined(_WIN32)
    #include <windows.h>
    #include <io.h>
    #define LP_ISATTY(fd) _isatty(fd)
    #define LP_FILENO     _fileno
#else
    #include <unistd.h>
    #define LP_ISATTY(fd) isatty(fd)
    #define LP_FILENO     fileno
#endif

namespace liveplay {

namespace {
// Single global state block. Initialised lazily; init() makes it explicit.
struct LoggerState {
    std::atomic<bool>      initialised{false};
    std::atomic<bool>      color_enabled{false};
    std::atomic<LogLevel>  min_level{LogLevel::Info};
};

// Fixed-size circular buffer storing formatted (ANSI-stripped) log lines.
// Protected by Logger::mutex(). Capacity tuned for ~8 h of busy logging.
struct RingBuffer {
    static constexpr std::size_t kCapacity = 16384;
    std::string lines[kCapacity];
    std::size_t head  = 0;   // index of the oldest entry
    std::size_t count = 0;   // number of valid entries

    void push(std::string line) noexcept {
        if (count < kCapacity) {
            lines[(head + count) % kCapacity] = std::move(line);
            ++count;
        } else {
            lines[head] = std::move(line);
            head = (head + 1) % kCapacity;
        }
    }
};

RingBuffer& ring_buffer() {
    static RingBuffer rb;
    return rb;
}

LoggerState& state() {
    static LoggerState s;
    return s;
}

// Strip ANSI CSI sequences from a message when color is disabled.
std::string strip_ansi(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\033' && i + 1 < in.size() && in[i + 1] == '[') {
            i += 2;
            while (i < in.size() && !(in[i] >= '@' && in[i] <= '~')) ++i;
            // i now points at the terminator; loop will ++ past it.
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

#if defined(_WIN32)
// Flip on virtual-terminal processing for stdout/stderr so ANSI escapes work
// on Win10+ consoles. Also switch the output codepage to UTF-8 so non-ASCII
// characters in the banner render correctly.
bool enable_windows_vt() {
    SetConsoleOutputCP(CP_UTF8);
    const auto enable = [](DWORD handle_id) -> bool {
        HANDLE h = GetStdHandle(handle_id);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
        DWORD mode = 0;
        if (!GetConsoleMode(h, &mode)) return false; // not a console (piped)
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
        return SetConsoleMode(h, mode) != 0;
    };
    const bool ok_out = enable(STD_OUTPUT_HANDLE);
    const bool ok_err = enable(STD_ERROR_HANDLE);
    return ok_out && ok_err;
}
#endif

// Detect whether the destination stream is "color-capable".
bool tty_supports_color() {
#if defined(_WIN32)
    return LP_ISATTY(LP_FILENO(stdout)) != 0;
#else
    if (!LP_ISATTY(LP_FILENO(stdout))) return false;
    const char* term = std::getenv("TERM");
    if (!term) return false;
    std::string_view t{term};
    return t != "dumb";
#endif
}

std::string timestamp_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t   = system_clock::to_time_t(now);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string{buf};
}

struct LevelStyle {
    std::string_view tag;     // four-char fixed-width label
    std::string_view color;   // ANSI color sequence
};

LevelStyle style_for(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:       return {"DBUG",         ansi::cyan};
        case LogLevel::Info:        return {"INFO",         ansi::green};
        case LogLevel::Success:     return {"OK",           ansi::bright_green};
        case LogLevel::Warn:        return {"WARNING",      ansi::yellow};
        case LogLevel::Error:       return {"ERROR",        ansi::bright_red};
        case LogLevel::ApiRequest:  return {"API REQUEST",  ansi::magenta};
        case LogLevel::ApiResponse: return {"API RESPONSE", ansi::cyan};
        case LogLevel::Playback:    return {"PLAYBACK",     ansi::orange};
    }
    return {"????", ansi::white};
}

} // namespace

std::mutex& Logger::mutex() {
    static std::mutex m;
    return m;
}

void Logger::init() {
    auto& s = state();
    if (s.initialised.exchange(true)) return;

    bool color = tty_supports_color();
#if defined(_WIN32)
    // Best-effort VT enable. If it fails (e.g. legacy console), color stays
    // off and we fall back to plain text — never to garbled escape sequences.
    if (!enable_windows_vt()) color = false;
#endif
    s.color_enabled.store(color);

    // Honour NO_COLOR (https://no-color.org/) and FORCE_COLOR conventions.
    if (std::getenv("NO_COLOR") != nullptr) s.color_enabled.store(false);
    if (std::getenv("FORCE_COLOR") != nullptr) s.color_enabled.store(true);
}

void Logger::set_color_enabled(bool enabled) {
    state().color_enabled.store(enabled);
}

void Logger::set_min_level(LogLevel level) {
    state().min_level.store(level);
}

bool Logger::color_enabled() {
    return state().color_enabled.load();
}

void Logger::log(LogLevel level, std::string_view msg) {
    if (!state().initialised.load()) init();
    if (static_cast<int>(level) < static_cast<int>(state().min_level.load())) return;

    const auto style    = style_for(level);
    const auto ts       = timestamp_now();
    const bool colored  = state().color_enabled.load();
    auto&      stream   = (level == LogLevel::Error || level == LogLevel::Warn)
                              ? std::cerr
                              : std::cout;

    const std::string plain_line =
        '[' + ts + "] [" + std::string{style.tag} + "] " + strip_ansi(msg);

    std::lock_guard lock{mutex()};
    ring_buffer().push(plain_line);
    if (colored) {
        stream << style.color << ansi::dim << '[' << ts << ']' << ansi::reset << ' '
               << style.color << ansi::bold << '[' << style.tag << ']' << ansi::reset << ' '
               << style.color << msg << ansi::reset
               << '\n';
    } else {
        stream << plain_line << '\n';
    }
    stream.flush();
}

std::string Logger::dump_history() {
    // try_lock so a crash that happened while holding the Logger mutex doesn't
    // deadlock the crash handler — we just return what we can.
    auto& m = mutex();
    const bool locked = m.try_lock();
    std::string out;
    if (locked) {
        auto& rb = ring_buffer();
        out.reserve(rb.count * 90);
        for (std::size_t i = 0; i < rb.count; ++i) {
            out += rb.lines[(rb.head + i) % RingBuffer::kCapacity];
            out += '\n';
        }
        m.unlock();
    } else {
        out = "(log history unavailable — mutex held at crash time)\n";
    }
    return out;
}

void Logger::raw(std::string_view line) {
    if (!state().initialised.load()) init();
    const bool colored = state().color_enabled.load();
    std::lock_guard lock{mutex()};
    if (colored) {
        std::cout << line << '\n';
    } else {
        std::cout << strip_ansi(line) << '\n';
    }
    std::cout.flush();
}

void Logger::rule() {
    raw("\033[2m" "--------------------------------------------------------------------------------" "\033[0m");
}

} // namespace liveplay
