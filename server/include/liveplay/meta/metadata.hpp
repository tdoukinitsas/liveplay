// ============================================================================
// liveplay/meta/metadata.hpp
// ----------------------------------------------------------------------------
// TagLib-backed metadata extraction. Pulls Artist / Title / Album / Duration
// out of a file at any path TagLib understands (MP3, FLAC, M4A, OGG, WAV…).
// Falls back to miniaudio for the duration when TagLib doesn't expose it
// (e.g. some WAV files), and to the bare filename for the title.
// ============================================================================
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace liveplay::meta {

struct AudioFileMetadata {
    std::string                 artist;
    std::string                 title;
    std::string                 album;
    std::string                 genre;
    int                         year         = 0;
    int                         track_number = 0;
    std::chrono::milliseconds   duration{0};
    std::uint32_t               sample_rate  = 0;
    std::uint32_t               channels     = 0;
    std::uint32_t               bitrate_kbps = 0;
    bool                        valid        = false;
};

// Read metadata from a file. Always returns a struct — `valid` indicates
// whether *any* useful information was retrieved. Best-effort: never throws.
AudioFileMetadata read_metadata(const std::filesystem::path& path) noexcept;

} // namespace liveplay::meta
