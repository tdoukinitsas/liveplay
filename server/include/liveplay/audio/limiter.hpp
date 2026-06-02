// ============================================================================
// liveplay/audio/limiter.hpp
// ----------------------------------------------------------------------------
// Brick-wall lookahead limiter living on every Master output channel. Stops
// the engine from producing samples whose magnitude exceeds a configurable
// ceiling (typically -0.3 dBFS) — replacing the legacy "reduce every cue's
// level just-in-case" hack that LivePlay 1.x used.
//
// Design:
//   * Lookahead buffer of L samples (default ~5 ms). The detector sees future
//     peaks before the delayed signal is output, so the gain envelope can
//     ramp down in time to catch them.
//   * Attack ≈ the lookahead time (the detector hits the new target gain by
//     the time the corresponding sample is output).
//   * Release: configurable one-pole release on gain reduction.
//   * Output level is mathematically guaranteed ≤ ceiling for in-range
//     inputs; for pathological NaN/Inf inputs we sanitise to silence.
//
// Per-channel: one instance per channel, no inter-channel linking by default.
// If you want stereo-linked behaviour, call link_with() to share the envelope.
// ============================================================================
#pragma once

#include "liveplay/audio/types.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace liveplay::audio {

class Limiter {
public:
    Limiter();
    ~Limiter();

    // Configure / reconfigure. Call from control thread while paused, or
    // before start. Reallocates the lookahead ring.
    //   ceiling_db    : output ceiling (must be ≤ 0). Default -0.3 dB.
    //   lookahead_ms  : detector lookahead (samples buffered). Default 5 ms.
    //   release_ms    : time constant for gain-reduction release.
    void configure(SampleRate sample_rate,
                   float ceiling_db   = -0.3f,
                   float lookahead_ms = 5.0f,
                   float release_ms   = 50.0f);

    // Process one mono buffer in-place. Real-time safe (no allocations).
    void process(Sample* samples, std::size_t frame_count) noexcept;

    // Current gain reduction in dB, suitable for a UI meter (control thread).
    float gain_reduction_db() const noexcept {
        return gain_reduction_db_.load(std::memory_order_relaxed);
    }

    // Drain the lookahead buffer to silence (e.g. after a stop-all). Audio
    // thread; constant time.
    void reset() noexcept;

private:
    SampleRate sample_rate_   = kDefaultMixSampleRate;
    float      ceiling_lin_   = 1.0f;     // 10^(-0.3/20)
    float      release_coef_  = 0.0f;
    std::size_t lookahead_    = 0;

    // Sliding peak detector
    std::vector<float> peak_window_;       // size = lookahead_
    std::size_t        peak_window_pos_ = 0;
    std::size_t        peak_window_max_idx_ = 0;
    float              peak_window_max_val_ = 0.0f;

    // Delay line (matches lookahead so detected peaks line up with samples).
    std::vector<Sample> delay_;
    std::size_t         delay_pos_ = 0;

    // Current gain (linear). Audio-thread state.
    float current_gain_ = 1.0f;

    // Published UI value.
    std::atomic<float> gain_reduction_db_{0.0f};

    // Recompute window max after the leaving sample turned out to be the max.
    void recompute_window_max() noexcept;
};

} // namespace liveplay::audio
