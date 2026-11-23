#pragma once
// ============================================================================
// media_names.hpp - where an imported file lands in a project's media folder.
// ----------------------------------------------------------------------------
// Two different files can share a name, and every cue that points at a file
// trusts it not to change underneath it (#92). So an import never writes over
// anything already in the folder:
//   * nothing has the name             -> it takes the name;
//   * the same bytes are already there -> that copy is reused;
//   * different bytes have the name    -> it becomes "Intro (2).wav".
//
// A file is written to a temporary name first and only moved into place once
// complete, so a cue can never open a half-written import.
// ============================================================================
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace liveplay::util {

// A folder holding this many files of one name has gone wrong somewhere; stop
// rather than walk it forever.
inline constexpr unsigned kMaxMediaNameAttempts = 10000;

// "Intro.wav", 2 -> "Intro (2).wav". 1 is the name itself.
inline std::filesystem::path numbered_file_name(const std::filesystem::path& name,
                                                unsigned n) {
    if (n <= 1) return name;
    std::filesystem::path out = name.stem();
    out += " (";
    out += std::to_string(n);
    out += ")";
    out += name.extension();
    return out;
}

// True only when both files can be read and hold the same bytes. A file that
// cannot be read counts as different: the safe outcome is one more copy.
inline bool same_file_content(const std::filesystem::path& a,
                              const std::filesystem::path& b) {
    std::error_code ec;
    const auto size_a = std::filesystem::file_size(a, ec);
    if (ec) return false;
    const auto size_b = std::filesystem::file_size(b, ec);
    if (ec || size_a != size_b) return false;

    std::ifstream fa{a, std::ios::binary};
    std::ifstream fb{b, std::ios::binary};
    if (!fa || !fb) return false;

    constexpr std::size_t kChunk = 64 * 1024;
    std::vector<char> ba(kChunk), bb(kChunk);
    for (;;) {
        fa.read(ba.data(), static_cast<std::streamsize>(kChunk));
        fb.read(bb.data(), static_cast<std::streamsize>(kChunk));
        const auto na = fa.gcount();
        const auto nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) return !fa.bad() && !fb.bad();
        if (std::memcmp(ba.data(), bb.data(), static_cast<std::size_t>(na)) != 0) return false;
    }
}

// The existing copy of `source` under `wanted` or one of its numbered names.
inline std::optional<std::filesystem::path> find_identical_media(
    const std::filesystem::path& dir,
    const std::filesystem::path& wanted,
    const std::filesystem::path& source) {
    for (unsigned n = 1; n < kMaxMediaNameAttempts; ++n) {
        const auto candidate = dir / numbered_file_name(wanted, n);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) return std::nullopt;
        if (same_file_content(source, candidate)) return candidate;
    }
    return std::nullopt;
}

// A name in `dir` for a file still being written. Same folder as its final
// home, so moving it into place is a rename and not another copy.
inline std::filesystem::path media_temp_path(const std::filesystem::path& dir) {
    static std::atomic<unsigned long long> counter{0};
    std::random_device rd;
    return dir / (".liveplay-import-" + std::to_string(rd()) + "-" +
                  std::to_string(++counter) + ".tmp");
}

// Removes a temporary file on every way out of a scope. Harmless once the file
// has been moved into place.
struct MediaTempFile {
    std::filesystem::path path;
    explicit MediaTempFile(std::filesystem::path p) : path(std::move(p)) {}
    MediaTempFile(const MediaTempFile&) = delete;
    MediaTempFile& operator=(const MediaTempFile&) = delete;
    ~MediaTempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

struct PublishedMedia {
    std::filesystem::path path;
    bool reused = false;  // an identical file was already there
};

// Moves a finished temp file to its final name under the rules at the top of
// this file. Callers serialise this per folder: between the existence check and
// the rename, nothing else may claim the name.
inline PublishedMedia publish_media_file(const std::filesystem::path& temp,
                                         const std::filesystem::path& dir,
                                         const std::filesystem::path& wanted) {
    for (unsigned n = 1; n < kMaxMediaNameAttempts; ++n) {
        const auto candidate = dir / numbered_file_name(wanted, n);
        std::error_code ec;
        const bool taken = std::filesystem::exists(candidate, ec);
        // Not knowing is not the same as free: never risk an overwrite.
        if (ec) throw std::filesystem::filesystem_error("cannot check media name", candidate, ec);
        if (!taken) {
            std::filesystem::rename(temp, candidate);
            return {candidate, false};
        }
        if (same_file_content(temp, candidate)) {
            std::filesystem::remove(temp, ec);
            return {candidate, true};
        }
    }
    throw std::runtime_error("too many media files share this name");
}

}  // namespace liveplay::util
