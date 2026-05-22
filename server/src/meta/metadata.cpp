// ============================================================================
// metadata.cpp — TagLib-backed extractor with a miniaudio fallback for
// duration. See metadata.hpp for the contract.
// ============================================================================
#include "liveplay/meta/metadata.hpp"
#include "liveplay/logger.hpp"

#include <miniaudio.h>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tstring.h>

namespace liveplay::meta {

namespace {

std::string tstring_to_utf8(const TagLib::String& s) {
    return s.toCString(true);   // true = UTF-8
}

// Best-effort duration via miniaudio when TagLib's AudioProperties is missing.
std::chrono::milliseconds duration_via_miniaudio(const std::filesystem::path& path) noexcept {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder{};
    const std::string p = path.string();
    if (ma_decoder_init_file(p.c_str(), &cfg, &decoder) != MA_SUCCESS) return {};
    ma_uint64 length_frames = 0;
    ma_uint32 sample_rate   = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &length_frames);
    ma_decoder_get_data_format(&decoder, nullptr, nullptr, &sample_rate, nullptr, 0);
    ma_decoder_uninit(&decoder);
    if (sample_rate == 0 || length_frames == 0) return {};
    return std::chrono::milliseconds{
        static_cast<long long>(length_frames) * 1000LL / static_cast<long long>(sample_rate)};
}

} // namespace

AudioFileMetadata read_metadata(const std::filesystem::path& path) noexcept {
    AudioFileMetadata out;
    try {
        const std::string p = path.string();
        TagLib::FileRef ref{p.c_str()};
        if (ref.isNull()) {
            Logger::warn("TagLib: could not open '{}'", p);
            out.title    = path.stem().string();
            out.duration = duration_via_miniaudio(path);
            out.valid    = out.duration.count() > 0;
            return out;
        }

        if (auto* tag = ref.tag()) {
            out.artist = tstring_to_utf8(tag->artist());
            out.title  = tstring_to_utf8(tag->title());
            out.album  = tstring_to_utf8(tag->album());
            out.genre  = tstring_to_utf8(tag->genre());
            out.year   = static_cast<int>(tag->year());
            out.track_number = static_cast<int>(tag->track());
        }
        if (out.title.empty()) out.title = path.stem().string();

        if (auto* props = ref.audioProperties()) {
            out.duration     = std::chrono::milliseconds{props->lengthInMilliseconds()};
            out.sample_rate  = static_cast<std::uint32_t>(props->sampleRate());
            out.channels     = static_cast<std::uint32_t>(props->channels());
            out.bitrate_kbps = static_cast<std::uint32_t>(props->bitrate());
        }
        if (out.duration.count() == 0) {
            out.duration = duration_via_miniaudio(path);
        }
        out.valid = true;
        return out;
    } catch (const std::exception& ex) {
        Logger::error("read_metadata exception for '{}': {}", path.string(), ex.what());
        out.title    = path.stem().string();
        out.duration = duration_via_miniaudio(path);
        out.valid    = out.duration.count() > 0;
        return out;
    }
}

} // namespace liveplay::meta
