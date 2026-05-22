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

Meter::Meter() noexcept {
    configure(kDefaultMixSampleRate);
}

void Meter::configure(SampleRate sample_rate,
                      float attack_ms,
                      float release_ms,
                      float rms_window_ms) noexcept {
    attack_coef_  = onepole_coef(attack_ms,  sample_rate);
    release_coef_ = onepole_coef(release_ms, sample_rate);
    // RMS uses a leaky integrator on squared samples. Larger 'alpha' → smoother.
    const float rms_coef = onepole_coef(rms_window_ms, sample_rate);
    rms_one_minus_a_ = 1.0f - rms_coef;
    // Keep the alpha around implicitly via 1 - this.
    (void)rms_coef;
    reset();
}

void Meter::push_block(const Sample* samples, std::size_t frame_count) noexcept {
    if (!samples || frame_count == 0) return;

    float peak = peak_env_;
    float rms  = rms_sq_;
    const float rms_alpha = 1.0f - rms_one_minus_a_;

    for (std::size_t i = 0; i < frame_count; ++i) {
        const float s   = samples[i];
        const float abs = std::fabs(s);

        // Peak envelope with attack/release ballistics.
        if (abs > peak) {
            peak = attack_coef_ * peak + (1.0f - attack_coef_) * abs;
        } else {
            peak = release_coef_ * peak + (1.0f - release_coef_) * abs;
        }

        // Leaky-integrator RMS (alpha applied to squared samples).
        rms = rms_alpha * rms + rms_one_minus_a_ * (s * s);
    }

    peak_env_ = peak;
    rms_sq_   = rms;

    peak_db_published_.store(clamp_db(lin_to_db(peak)), std::memory_order_relaxed);
    rms_db_published_ .store(clamp_db(lin_to_db(std::sqrt(rms))), std::memory_order_relaxed);
}

void Meter::push_interleaved(const Sample* interleaved,
                             std::size_t frame_count,
                             ChannelCount channel_stride,
                             ChannelIndex channel_index) noexcept {
    if (!interleaved || frame_count == 0 || channel_stride == 0) return;

    float peak = peak_env_;
    float rms  = rms_sq_;
    const float rms_alpha = 1.0f - rms_one_minus_a_;

    const Sample* p = interleaved + channel_index;
    for (std::size_t i = 0; i < frame_count; ++i) {
        const float s   = *p;
        p += channel_stride;
        const float abs = std::fabs(s);

        if (abs > peak) {
            peak = attack_coef_ * peak + (1.0f - attack_coef_) * abs;
        } else {
            peak = release_coef_ * peak + (1.0f - release_coef_) * abs;
        }
        rms = rms_alpha * rms + rms_one_minus_a_ * (s * s);
    }

    peak_env_ = peak;
    rms_sq_   = rms;

    peak_db_published_.store(clamp_db(lin_to_db(peak)), std::memory_order_relaxed);
    rms_db_published_ .store(clamp_db(lin_to_db(std::sqrt(rms))), std::memory_order_relaxed);
}

MeterSnapshot Meter::snapshot() const noexcept {
    return {
        peak_db_published_.load(std::memory_order_relaxed),
        rms_db_published_ .load(std::memory_order_relaxed),
    };
}

void Meter::reset() noexcept {
    peak_env_ = 0.0f;
    rms_sq_   = 0.0f;
    peak_db_published_.store(-120.0f, std::memory_order_relaxed);
    rms_db_published_ .store(-120.0f, std::memory_order_relaxed);
}

} // namespace liveplay::audio
