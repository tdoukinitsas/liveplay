// ============================================================================
// waveform.cpp — see waveform.hpp.
// ============================================================================
#include "liveplay/meta/waveform.hpp"
#include "liveplay/logger.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace liveplay::meta {

Waveform compute_waveform(const std::filesystem::path& path,
                          std::uint32_t bucket_count) noexcept {
    Waveform out;
    bucket_count = std::clamp<std::uint32_t>(bucket_count, 16, 16384);

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder{};
    const std::string p = path.string();
    if (ma_decoder_init_file(p.c_str(), &cfg, &decoder) != MA_SUCCESS) {
        Logger::warn("compute_waveform: cannot decode '{}'", p);
        return out;
    }

    ma_uint32 channels    = 0;
    ma_uint32 sample_rate = 0;
    ma_decoder_get_data_format(&decoder, nullptr, &channels, &sample_rate, nullptr, 0);
    if (channels == 0) channels = 2;

    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

    if (total_frames == 0) {
        ma_decoder_uninit(&decoder);
        Logger::warn("compute_waveform: zero-length file '{}'", p);
        return out;
    }

    // Frames per bucket — at least 1. Last bucket may be short; we account
    // for that with the running counter pattern below.
    const double frames_per_bucket = static_cast<double>(total_frames) /
                                     static_cast<double>(bucket_count);

    out.channels.assign(channels, WaveformChannel{});
    for (auto& c : out.channels) {
        c.peak.assign(bucket_count, 0.0f);
        c.rms .assign(bucket_count, 0.0f);
    }

    // Streamed read in 4096-frame chunks so we don't allocate a giant buffer
    // for long files.
    constexpr ma_uint64 kChunk = 4096;
    std::vector<float> buf(kChunk * channels);

    std::vector<double> bucket_sum_sq(channels, 0.0);
    std::vector<float>  bucket_peak  (channels, 0.0f);
    std::uint64_t       bucket_count_samples = 0;
    std::uint32_t       current_bucket = 0;
    double              next_bucket_boundary = frames_per_bucket;
    std::uint64_t       frames_consumed = 0;

    while (current_bucket < bucket_count) {
        ma_uint64 frames_read = 0;
        if (ma_decoder_read_pcm_frames(&decoder, buf.data(), kChunk, &frames_read) != MA_SUCCESS
            || frames_read == 0) break;

        for (ma_uint64 i = 0; i < frames_read; ++i) {
            for (ma_uint32 c = 0; c < channels; ++c) {
                const float s   = buf[i * channels + c];
                const float abs = std::fabs(s);
                if (abs > bucket_peak[c]) bucket_peak[c] = abs;
                bucket_sum_sq[c] += static_cast<double>(s) * static_cast<double>(s);
            }
            ++bucket_count_samples;
            ++frames_consumed;

            if (static_cast<double>(frames_consumed) >= next_bucket_boundary &&
                current_bucket < bucket_count) {
                for (ma_uint32 c = 0; c < channels; ++c) {
                    out.channels[c].peak[current_bucket] =
                        std::clamp(bucket_peak[c], 0.0f, 1.0f);
                    const double mean_sq = bucket_count_samples > 0
                        ? bucket_sum_sq[c] / static_cast<double>(bucket_count_samples) : 0.0;
                    out.channels[c].rms[current_bucket] =
                        std::clamp(static_cast<float>(std::sqrt(mean_sq)), 0.0f, 1.0f);
                    bucket_peak[c]  = 0.0f;
                    bucket_sum_sq[c] = 0.0;
                }
                bucket_count_samples = 0;
                ++current_bucket;
                next_bucket_boundary += frames_per_bucket;
            }
        }
    }

    // Flush any tail samples into the last bucket.
    if (current_bucket < bucket_count && bucket_count_samples > 0) {
        for (ma_uint32 c = 0; c < channels; ++c) {
            out.channels[c].peak[current_bucket] =
                std::clamp(bucket_peak[c], 0.0f, 1.0f);
            const double mean_sq = bucket_sum_sq[c] / static_cast<double>(bucket_count_samples);
            out.channels[c].rms[current_bucket] =
                std::clamp(static_cast<float>(std::sqrt(mean_sq)), 0.0f, 1.0f);
        }
    }

    out.bucket_count    = bucket_count;
    out.sample_rate     = sample_rate;
    out.source_channels = channels;
    out.duration = std::chrono::milliseconds{
        static_cast<long long>(total_frames) * 1000LL /
        static_cast<long long>(std::max<ma_uint32>(sample_rate, 1))};
    out.ok = true;

    ma_decoder_uninit(&decoder);
    return out;
}

} // namespace liveplay::meta
