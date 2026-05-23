// ============================================================================
// liveplay/util/unicode_path.hpp
// ----------------------------------------------------------------------------
// Cross-platform helpers for handling UTF-8 file paths.
//
// Background: on Windows, `std::filesystem::path` constructed from a
// `std::string` interprets the bytes using the *active code page* (usually
// Windows-1252 / CP_ACP), NOT UTF-8. Any non-ASCII bytes that came in via
// JSON (which is UTF-8) get silently corrupted. miniaudio's
// `ma_decoder_init_file` further uses `fopen`, which is also code-page-aware.
//
// These helpers route UTF-8 → UTF-16 on Windows, leave POSIX alone.
// ============================================================================
#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#  include <stringapiset.h>
#endif

namespace liveplay::util {

// Build a std::filesystem::path from a UTF-8 string. Use this *instead* of
// the implicit `fs::path{std::string}` constructor whenever the bytes
// originated from JSON, the WebSocket layer, or any UTF-8 source.
inline std::filesystem::path utf8_to_path(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    if (len <= 0) {
        // Fall back to lenient decode if the input had invalid sequences.
        const int len2 = MultiByteToWideChar(CP_UTF8, 0,
                                             s.data(), static_cast<int>(s.size()),
                                             nullptr, 0);
        if (len2 <= 0) return std::filesystem::path{s};
        std::wstring w(static_cast<std::size_t>(len2), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            w.data(), len2);
        return std::filesystem::path{w};
    }
    std::wstring w(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        s.data(), static_cast<int>(s.size()),
                        w.data(), len);
    return std::filesystem::path{w};
#else
    return std::filesystem::path{s};
#endif
}

// Encode a path back to UTF-8 (for log messages, JSON responses, etc.).
inline std::string path_to_utf8(const std::filesystem::path& p) {
#if defined(_WIN32)
    const std::wstring w = p.wstring();
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                        static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return p.string();
    std::string s(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), len, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

} // namespace liveplay::util
