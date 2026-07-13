// ============================================================================
// meter.cpp
// ----------------------------------------------------------------------------
// VU-ballistics peak/RMS meter. See meter.hpp for the public contract.
// ============================================================================
#include "liveplay/audio/meter.hpp"

#include <algorithm>
#include <cmath>

namespace liveplay::audio {

namespace {

// One-pole IIR coefficient for a given time constant.
// "Time constant" here = time to traverse 1 - 1/e (~63%) of the gap to target.
inline float onepole_coef(float time_ms, SampleRate sr) noexcept {
    if (time_ms <= 0.0f) return 0.0f;   // instantaneous
    const float seconds = time_ms * 0.001f;
    return std::exp(-1.0f / (seconds * static_cast<float>(sr)));
}

inline float clamp_db(float db) noexcept {
    if (db < -120.0f) return -120.0f;
    if (db >  60.0f)  return  60.0f;
    return db;
}

inline float lin_to_db(float lin) noexcept {
    if (lin <= 1e-10f) return -120.0f;
    return 20.0f * std::log10(lin);
}

} // namespace

std::optional<MeterBallistics> meter_ballistics_from_preset(std::string_view id) noexcept {
    // Approximations of the broadcast standards mapped onto the one-pole
    // envelope. Release values tuned by eye against reference meters; the
    // exact dB/s fall of a true IEC PPM needs a linear-dB release mode
    // (possible future enhancement).
    if (id == "digital-ppm") return MeterBallistics{0.0f,   350.0f, 300.0f};
    if (id == "ppm-i")       return MeterBallistics{5.0f,   550.0f, 300.0f};
    if (id == "ppm-ii")      return MeterBallistics{10.0f,  950.0f, 300.0f};
    if (id == "vu")          return MeterBallistics{150.0f, 150.0f, 300.0f};
    if (id == "instant")     return MeterBallistics{0.0f,   50.0f,  100.0f};
    return std::nullopt;
}

Meter::Meter() noexcept {
    configure(kDefaultMixSampleRate);
}

void Meter::configure(SampleRate sample_rate,
                      float attack_ms,
                      float release_ms,
                      float rms_window_ms) noexcept {
    attack_coef_ .store(onepole_coef(attack_ms,  sample_rate), std::memory_order_relaxed);
    release_coef_.store(onepole_coef(release_ms, sample_rate), std::memory_order_relaxed);
    // RMS uses a leaky integrator on squared samples. Larger 'alpha' → smoother.
    rms_one_minus_a_.store(1.0f - onepole_coef(rms_window_ms, sample_rate),
                           std::memory_order_relaxed);
    // Deliberately NOT reset() here: live ballistics changes (project setting
    // flipped mid-playback) should re-shape the envelope, not blank the meter.
}

void Meter::push_block(const Sample* samples, std::size_t frame_count) noexcept {
    if (!samples || frame_count == 0) return;

    // Load coefficients once per block (control thread may retune them live).
    const float atk  = attack_coef_.load(std::memory_order_relaxed);
    const float rel  = release_coef_.load(std::memory_order_relaxed);
    const float roma = rms_one_minus_a_.load(std::memory_order_relaxed);

    float peak = peak_env_;
    float rms  = rms_sq_;
    float blk_max = 0.0f;
    const float rms_alpha = 1.0f - roma;

    for (std::size_t i = 0; i < frame_count; ++i) {
        const float s   = samples[i];
        const float abs = std::fabs(s);
        if (abs > blk_max) blk_max = abs;

        // Peak envelope with attack/release ballistics.
        if (abs > peak) {
            peak = atk * peak + (1.0f - atk) * abs;
        } else {
            peak = rel * peak + (1.0f - rel) * abs;
        }

        // Leaky-integrator RMS (alpha applied to squared samples).
        rms = rms_alpha * rms + roma * (s * s);
    }

    peak_env_ = peak;
    rms_sq_   = rms;

    peak_db_published_.store(clamp_db(lin_to_db(peak)), std::memory_order_relaxed);
    rms_db_published_ .store(clamp_db(lin_to_db(std::sqrt(rms))), std::memory_order_relaxed);
    fold_peak_max(clamp_db(lin_to_db(blk_max)));
}

void Meter::push_interleaved(const Sample* interleaved,
                             std::size_t frame_count,
                             ChannelCount channel_stride,
                             ChannelIndex channel_index) noexcept {
    if (!interleaved || frame_count == 0 || channel_stride == 0) return;

    const float atk  = attack_coef_.load(std::memory_order_relaxed);
    const float rel  = release_coef_.load(std::memory_order_relaxed);
    const float roma = rms_one_minus_a_.load(std::memory_order_relaxed);

    float peak = peak_env_;
    float rms  = rms_sq_;
    float blk_max = 0.0f;
    const float rms_alpha = 1.0f - roma;

    const Sample* p = interleaved + channel_index;
    for (std::size_t i = 0; i < frame_count; ++i) {
        const float s   = *p;
        p += channel_stride;
        const float abs = std::fabs(s);
        if (abs > blk_max) blk_max = abs;

        if (abs > peak) {
            peak = atk * peak + (1.0f - atk) * abs;
        } else {
            peak = rel * peak + (1.0f - rel) * abs;
        }
        rms = rms_alpha * rms + roma * (s * s);
    }

    peak_env_ = peak;
    rms_sq_   = rms;

    peak_db_published_.store(clamp_db(lin_to_db(peak)), std::memory_order_relaxed);
    rms_db_published_ .store(clamp_db(lin_to_db(std::sqrt(rms))), std::memory_order_relaxed);
    fold_peak_max(clamp_db(lin_to_db(blk_max)));
}

void Meter::fold_peak_max(float db) noexcept {
    float cur = peak_max_since_read_db_.load(std::memory_order_relaxed);
    while (db > cur &&
           !peak_max_since_read_db_.compare_exchange_weak(
               cur, db, std::memory_order_relaxed)) {
        // cur reloaded by compare_exchange_weak on failure.
    }
}

MeterSnapshot Meter::snapshot() const noexcept {
    return {
        peak_db_published_.load(std::memory_order_relaxed),
        rms_db_published_ .load(std::memory_order_relaxed),
        peak_max_since_read_db_.load(std::memory_order_relaxed),
    };
}

MeterSnapshot Meter::snapshot_consume_max() noexcept {
    return {
        peak_db_published_.load(std::memory_order_relaxed),
        rms_db_published_ .load(std::memory_order_relaxed),
        peak_max_since_read_db_.exchange(-120.0f, std::memory_order_relaxed),
    };
}

void Meter::reset() noexcept {
    peak_env_ = 0.0f;
    rms_sq_   = 0.0f;
    peak_db_published_.store(-120.0f, std::memory_order_relaxed);
    rms_db_published_ .store(-120.0f, std::memory_order_relaxed);
    peak_max_since_read_db_.store(-120.0f, std::memory_order_relaxed);
}

} // namespace liveplay::audio
