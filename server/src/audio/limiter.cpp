// ============================================================================
// limiter.cpp — see limiter.hpp.
// ============================================================================
#include "liveplay/audio/limiter.hpp"

#include <algorithm>
#include <cmath>

namespace liveplay::audio {

namespace {
constexpr float kMinSignal = 1e-10f;

inline float lin_to_db(float lin) noexcept {
    if (lin <= kMinSignal) return -120.0f;
    return 20.0f * std::log10(lin);
}

inline float db_to_lin(float db) noexcept {
    return std::pow(10.0f, db * 0.05f);
}

inline bool finite_and_finite_enough(float x) noexcept {
    return std::isfinite(x);
}
} // namespace

Limiter::Limiter()  = default;
Limiter::~Limiter() = default;

void Limiter::configure(SampleRate sample_rate,
                        float ceiling_db,
                        float lookahead_ms,
                        float release_ms) {
    sample_rate_  = sample_rate;
    ceiling_lin_  = db_to_lin(std::min(ceiling_db, 0.0f));

    const float seconds = std::max(lookahead_ms, 0.1f) * 0.001f;
    lookahead_ = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(seconds * static_cast<float>(sample_rate))));

    // One-pole release coef: y[n] = α y[n-1] + (1-α) target  with α = exp(-1/(τ·Fs)).
    const float rel_seconds = std::max(release_ms, 1.0f) * 0.001f;
    release_coef_ = std::exp(-1.0f / (rel_seconds * static_cast<float>(sample_rate)));

    peak_window_.assign(lookahead_, 0.0f);
    delay_.assign(lookahead_, 0.0f);

    peak_window_pos_     = 0;
    peak_window_max_idx_ = 0;
    peak_window_max_val_ = 0.0f;
    delay_pos_           = 0;
    current_gain_        = 1.0f;
    gain_reduction_db_.store(0.0f, std::memory_order_relaxed);
}

void Limiter::reset() noexcept {
    std::fill(peak_window_.begin(), peak_window_.end(), 0.0f);
    std::fill(delay_.begin(),       delay_.end(),       0.0f);
    peak_window_pos_     = 0;
    peak_window_max_idx_ = 0;
    peak_window_max_val_ = 0.0f;
    delay_pos_           = 0;
    current_gain_        = 1.0f;
    gain_reduction_db_.store(0.0f, std::memory_order_relaxed);
}

void Limiter::recompute_window_max() noexcept {
    float max_val = 0.0f;
    std::size_t max_idx = 0;
    for (std::size_t i = 0; i < peak_window_.size(); ++i) {
        if (peak_window_[i] > max_val) {
            max_val = peak_window_[i];
            max_idx = i;
        }
    }
    peak_window_max_val_ = max_val;
    peak_window_max_idx_ = max_idx;
}

void Limiter::process(Sample* samples, std::size_t frame_count) noexcept {
    if (!samples || frame_count == 0) return;
    if (peak_window_.empty() || delay_.empty()) return;   // not configured

    const std::size_t window_size = peak_window_.size();
    const float       ceiling     = ceiling_lin_;
    const float       rel         = release_coef_;

    for (std::size_t i = 0; i < frame_count; ++i) {
        // Sanitise pathological input.
        float in = samples[i];
        if (!finite_and_finite_enough(in)) in = 0.0f;
        const float abs_in = std::fabs(in);

        // Push |x[n]| into the peak window. Maintain running max in O(1)
        // amortised — only recompute on the rare event that the leaving sample
        // was the previous maximum.
        const std::size_t leaving_idx = peak_window_pos_;
        const float leaving_val = peak_window_[leaving_idx];

        peak_window_[leaving_idx] = abs_in;
        peak_window_pos_ = (peak_window_pos_ + 1) % window_size;

        if (abs_in >= peak_window_max_val_) {
            // New entry sets a new max.
            peak_window_max_val_ = abs_in;
            peak_window_max_idx_ = leaving_idx;
        } else if (leaving_idx == peak_window_max_idx_ &&
                   leaving_val == peak_window_max_val_) {
            // We just evicted the running max → rescan.
            recompute_window_max();
        }

        // Target gain ensures detected peak * gain ≤ ceiling.
        float target_gain = 1.0f;
        if (peak_window_max_val_ > ceiling) {
            target_gain = ceiling / peak_window_max_val_;
        }

        // Attack: snap immediately downward (lookahead gives time for the
        // delayed signal to catch up). Release: one-pole back toward unity.
        if (target_gain < current_gain_) {
            current_gain_ = target_gain;
        } else {
            current_gain_ = rel * current_gain_ + (1.0f - rel) * target_gain;
        }

        // Output is the delayed signal multiplied by the current gain.
        const Sample delayed = delay_[delay_pos_];
        delay_[delay_pos_]   = in;
        delay_pos_           = (delay_pos_ + 1) % window_size;

        float out = delayed * current_gain_;
        // Final hard clip as a backstop against floating-point edge cases.
        if (out >  ceiling) out =  ceiling;
        if (out < -ceiling) out = -ceiling;
        samples[i] = out;
    }

    gain_reduction_db_.store(lin_to_db(current_gain_),
                             std::memory_order_relaxed);
}

float db_to_linear_precise(float db) noexcept {
    return std::pow(10.0f, db * 0.05f);
}

float linear_to_db_precise(float lin) noexcept {
    if (lin <= kMinSignal) return -120.0f;
    return 20.0f * std::log10(lin);
}

} // namespace liveplay::audio
