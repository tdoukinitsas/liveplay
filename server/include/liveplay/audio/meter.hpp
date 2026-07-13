// ============================================================================
// liveplay/audio/meter.hpp
// ----------------------------------------------------------------------------
// Real-time amplitude meter computing both Peak and RMS with VU-style
// ballistics. One instance per metering point — Items, MixerChannels, and
// MasterChannels each own their own.
//
// The audio thread calls push_block() once per render block (no allocations,
// constant-time). The control thread reads the latest values via snapshot()
// which loads two atomics — safe to call at GUI rates from any thread.
// ============================================================================
#pragma once

#include "liveplay/audio/types.hpp"

#include <atomic>
#include <cstddef>

namespace liveplay::audio {

// Plain-old-data snapshot. dBFS values; -120 dB == silence.
struct MeterSnapshot {
    float peak_db     = -120.0f;   // instantaneous peak, ballistically-released
    float rms_db      = -120.0f;   // ~300 ms RMS window
    // Raw sample maximum since the last *consuming* read. Unlike peak_db it
    // has no ballistics — a single-sample transient between two reads is
    // reported at full amplitude, so no peak can ever be missed regardless
    // of how slowly the reader polls.
    float peak_max_db = -120.0f;
};

class Meter {
public:
    Meter() noexcept;

    // Configure ballistics + window. Must be called from the control thread
    // before the audio thread starts pushing blocks (or while it's paused).
    //   attack_ms  : peak attack time constant (default 0 → instantaneous)
    //   release_ms : peak release ("fall back to noise") time constant
    //   rms_window_ms : RMS averaging window
    void configure(SampleRate sample_rate,
                   float attack_ms     = 1.0f,
                   float release_ms    = 300.0f,
                   float rms_window_ms = 300.0f) noexcept;

    // Audio-thread entrypoint. Pushes one mono channel's worth of samples.
    // For multi-channel streams call push_block() per channel into separate
    // Meter instances, or call push_interleaved() with stride.
    void push_block(const Sample* samples, std::size_t frame_count) noexcept;

    void push_interleaved(const Sample* interleaved,
                          std::size_t frame_count,
                          ChannelCount channel_stride,
                          ChannelIndex channel_index) noexcept;

    // Control-thread read. Lock-free; values are eventually-consistent.
    // peak_max_db is reported but NOT reset.
    MeterSnapshot snapshot() const noexcept;

    // Control-thread read that also RESETS the max-since-read accumulator.
    // Exactly one reader may use this (the meter broadcaster) — two consuming
    // readers would steal each other's peaks.
    MeterSnapshot snapshot_consume_max() noexcept;

    // Reset to silence. Audio thread or control thread (atomic).
    void reset() noexcept;

private:
    // Coefficients (set by configure(); read by audio thread).
    float attack_coef_      = 0.0f;
    float release_coef_     = 0.0f;
    float rms_one_minus_a_  = 0.0f;   // 1 - alpha for the RMS leaky integrator

    // Audio-thread-only state.
    float peak_env_         = 0.0f;
    float rms_sq_           = 0.0f;

    // Published values (atomic for the control-thread reader).
    std::atomic<float> peak_db_published_{-120.0f};
    std::atomic<float> rms_db_published_{-120.0f};
    // Raw max since the last consuming read. Audio thread folds each block's
    // maximum in via a CAS fetch-max loop; snapshot_consume_max() exchanges
    // it back to silence.
    std::atomic<float> peak_max_since_read_db_{-120.0f};

    // Fold `db` into peak_max_since_read_db_ (CAS fetch-max — safe against a
    // concurrent consuming reader).
    void fold_peak_max(float db) noexcept;
};

} // namespace liveplay::audio
