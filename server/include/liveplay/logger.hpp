// ============================================================================
// liveplay/logger.hpp
// ----------------------------------------------------------------------------
// Thread-safe, ANSI-color-coded logger for the LivePlay server CLI.
//
// The server is headless — its stdout is the user interface. Color helps the
// operator triage what is information, what needs attention, and what is on
// fire. On Windows we explicitly enable virtual-terminal processing so the
// escape sequences render in cmd.exe and PowerShell.
//
// Usage:
//     liveplay::Logger::init();                          // call once at startup
//     liveplay::Logger::info("listening on {}", port);   // fmt-style
//     liveplay::Logger::warn("device {} dropped", id);
//     liveplay::Logger::error("decode failed: {}", err);
//
// The format API is std::format (C++20). Plain strings are also accepted.
// ============================================================================
#pragma once

#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace liveplay {

enum class LogLevel {
    Debug,
    Info,
    Success,
    Warn,
    Error,
};

class Logger {
public:
    // Enable color where supported (Windows VT on, isatty check elsewhere) and
    // capture program start time for timestamps. Idempotent.
    static void init();

    // Globally disable colors (for piping into files, log aggregators, etc.).
    static void set_color_enabled(bool enabled);

    // Set the minimum level that will be emitted (default: Info; Debug below).
    static void set_min_level(LogLevel level);

    // -------------------- primary API ---------------------------------------
    template <typename... Args>
    static void debug(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    static void info(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    static void success(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Success, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    static void warn(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    static void error(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
    }

    // Plain-string overloads for already-formatted messages.
    static void debug(std::string_view msg)   { log(LogLevel::Debug, msg); }
    static void info(std::string_view msg)    { log(LogLevel::Info, msg); }
    static void success(std::string_view msg) { log(LogLevel::Success, msg); }
    static void warn(std::string_view msg)    { log(LogLevel::Warn, msg); }
    static void error(std::string_view msg)   { log(LogLevel::Error, msg); }

    // -------------------- low-level / utilities -----------------------------
    // Write a raw line *with* ANSI codes preserved if color is on, stripped
    // otherwise. Used by the startup banner which builds its own colorisation.
    static void raw(std::string_view line);

    // Convenience: print a hairline separator the width of an 80-col terminal.
    static void rule();

    // Direct access if you want to query state in tests / startup banner.
    static bool color_enabled();

private:
    static void log(LogLevel level, std::string_view msg);
    static std::mutex& mutex();
};

// ----------------------------------------------------------------------------
// ANSI escape constants. Exposed because the startup banner composes them
// inline; everyday logging should use Logger::info()/warn()/error() instead.
// ----------------------------------------------------------------------------
namespace ansi {
inline constexpr std::string_view reset      = "\033[0m";
inline constexpr std::string_view bold       = "\033[1m";
inline constexpr std::string_view dim        = "\033[2m";
inline constexpr std::string_view underline  = "\033[4m";

inline constexpr std::string_view black      = "\033[30m";
inline constexpr std::string_view red        = "\033[31m";
inline constexpr std::string_view green      = "\033[32m";
inline constexpr std::string_view yellow     = "\033[33m";
inline constexpr std::string_view blue       = "\033[34m";
inline constexpr std::string_view magenta    = "\033[35m";
inline constexpr std::string_view cyan       = "\033[36m";
inline constexpr std::string_view white      = "\033[37m";

inline constexpr std::string_view bright_red    = "\033[91m";
inline constexpr std::string_view bright_green  = "\033[92m";
inline constexpr std::string_view bright_yellow = "\033[93m";
inline constexpr std::string_view bright_blue   = "\033[94m";
inline constexpr std::string_view bright_cyan   = "\033[96m";
inline constexpr std::string_view bright_white  = "\033[97m";
} // namespace ansi

} // namespace liveplay
