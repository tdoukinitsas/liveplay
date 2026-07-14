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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace liveplay::audio {

// ---------------------------------------------------------------------------
// Ballistics — how fast the peak envelope rises/falls and how long the RMS
// window is. User-selectable per project via settings.meterBallistics.
// ---------------------------------------------------------------------------
struct MeterBallistics {
    float attack_ms     = 1.0f;     // peak attack time constant (0 = instant)
    float release_ms    = 300.0f;   // peak release ("fall back") time constant
    float rms_window_ms = 300.0f;   // RMS averaging window
};

// Resolve a ballistics preset id. Known ids:
//   "digital-ppm" (default) — instant attack, ~350 ms release
//   "ppm-i"                 — DIN PPM Type I approximation
//   "ppm-ii"                — BBC PPM Type II approximation
//   "vu"                    — classic VU (slow symmetric ~300 ms)
//   "instant"               — near-raw; relies on peak_max_db for transients
// Returns nullopt for unknown ids (callers fall back to the default).
std::optional<MeterBallistics> meter_ballistics_from_preset(std::string_view id) noexcept;

// Plain-old-data snapshot. dBFS values; -120 dB == silence.
struct MeterSnapshot {
    float peak_db     = -120.0f;   // instantaneous peak, ballistically-released
    float rms_db      = -120.0f;   // ~300 ms RMS window
    // Raw sample maximum since the last *consuming* read. Unlike peak_db it
    // has no ballistics — a single-sample transient between two reads is
    // reported at full amplitude, so no peak can ever be missed regardless
    // of how slowly the reader polls.
    float peak_max_db = -120.0f;
    // True peak (dBTP): 4× oversampled per ITU-R BS.1770-4, catching
    // intersample peaks the raw sample values miss. When true-peak metering
    // is disabled these mirror peak_db / peak_max_db so a client left in
    // dBTP display mode still shows sensible values.
    float true_peak_db     = -120.0f;   // ballistic envelope of the TP stream
    float true_peak_max_db = -120.0f;   // raw TP max since last consuming read
    // K-weighted mean square (BS.1770), LINEAR power — not dB. Loudness of a
    // channel group is
    //   LUFS = -0.691 + 10·log10(Σ kw_ms_i)
    // summed over the group's channels (the reader does the pairing, so the
    // engine never needs to know which mono buses form a stereo pair).
    // Both 0.0 when loudness metering is disabled.
    float kw_ms   = 0.0f;   // momentary  — 400 ms window (EBU "M")
    float kw_ms_s = 0.0f;   // short-term — 3 s window   (EBU "S")
};

class Meter {
public:
    Meter() noexcept;

    // Configure ballistics + window. Coefficients are atomics, so this is
    // safe to call from the control thread at ANY time — including while the
    // audio thread is pushing blocks (the new ballistics take effect on the
    // next block).
    void configure(SampleRate sample_rate,
                   float attack_ms     = 1.0f,
                   float release_ms    = 300.0f,
                   float rms_window_ms = 300.0f) noexcept;
    void configure(SampleRate sample_rate, const MeterBallistics& b) noexcept {
        configure(sample_rate, b.attack_ms, b.release_ms, b.rms_window_ms);
    }

    // Audio-thread entrypoint. Pushes one mono channel's worth of samples.
    // For multi-channel streams call push_block() per channel into separate
    // Meter instances, or call push_interleaved() with stride.
    void push_block(const Sample* samples, std::size_t frame_count) noexcept;

    void push_interleaved(const Sample* interleaved,
                          std::size_t frame_count,
                          ChannelCount channel_stride,
                          ChannelIndex channel_index) noexcept;

    // Enable/disable 4× oversampled true-peak detection (≈48 extra mults per
    // sample when on — gated to projects whose meter mode is dBTP). Control
    // thread; takes effect on the next block.
    void set_true_peak_enabled(bool enabled) noexcept {
        true_peak_enabled_.store(enabled, std::memory_order_relaxed);
    }
    bool true_peak_enabled() const noexcept {
        return true_peak_enabled_.load(std::memory_order_relaxed);
    }

    // Enable/disable K-weighted momentary loudness (2 biquads + 400 ms
    // window per sample when on — gated to projects whose meter mode is
    // LUFS). Control thread; takes effect on the next block.
    void set_loudness_enabled(bool enabled) noexcept {
        loudness_enabled_.store(enabled, std::memory_order_relaxed);
    }
    bool loudness_enabled() const noexcept {
        return loudness_enabled_.load(std::memory_order_relaxed);
    }

    // Control-thread read. Lock-free; values are eventually-consistent.
    // peak_max_db is reported but NOT reset.
    MeterSnapshot snapshot() const noexcept;

    // Control-thread read that also RESETS the max-since-read accumulator.
    // Exactly one reader may use this (the meter broadcaster) — two consuming
    // readers would steal each other's peaks.
    MeterSnapshot snapshot_consume_max() noexcept;

    // Reset to silence. Touches audio-thread envelope/history state — call
    // from the audio thread, or while it is not pushing blocks.
    void reset() noexcept;

private:
    // Coefficients (set by configure() on the control thread; loaded once per
    // block by the audio thread — atomics make live reconfiguration safe).
    std::atomic<float> attack_coef_      {0.0f};
    std::atomic<float> release_coef_     {0.0f};
    std::atomic<float> rms_one_minus_a_  {0.0f};   // 1 - alpha for the RMS leaky integrator

    // Audio-thread-only state.
    float peak_env_         = 0.0f;
    float rms_sq_           = 0.0f;

    // True-peak state (audio-thread-only except the enable flag + published
    // atomics). 12-sample history ring feeding the 4-phase interpolator.
    static constexpr std::size_t kTpTapsPerPhase = 12;
    static constexpr std::size_t kTpPhases       = 4;
    std::atomic<bool>                  true_peak_enabled_{false};
    std::array<float, kTpTapsPerPhase> tp_hist_{};
    std::size_t                        tp_hist_pos_ = 0;
    float                              tp_env_      = 0.0f;

    // Run the oversampler on one input sample; returns the max |value| of
    // the 4 interpolated output samples.
    float tp_process_sample(float s) noexcept;

    // ---- Loudness (K-weighted mean square, BS.1770) -----------------------
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    std::atomic<bool> loudness_enabled_{false};
    SampleRate kw_designed_rate_ = 0;    // rate the biquads were designed for
    Biquad kw_shelf_{};                  // stage 1: high-frequency shelf
    Biquad kw_hp_{};                     // stage 2: RLB high-pass
    // Biquad states (DF2T, audio-thread-only).
    float kw1_z1_ = 0, kw1_z2_ = 0, kw2_z1_ = 0, kw2_z2_ = 0;

    // Rectangular mean-square window as a ring of per-block sums — fixed
    // size, allocation-free on the audio thread. At the default 256-frame
    // block the 3 s short-term window occupies ~563 entries; if callers push
    // unusually tiny blocks the ring capacity bounds the effective window.
    struct LoudnessWindow {
        static constexpr std::size_t kCap = 1024;
        std::array<float, kCap>         sum{};
        std::array<std::uint32_t, kCap> n{};
        std::size_t   head = 0, count = 0;
        double        total_sum = 0.0;
        std::uint64_t total_n = 0;
        std::uint64_t window_samples = 0;

        void  push(float sum_sq, std::uint32_t n_samples) noexcept;
        float mean() const noexcept {
            return total_n == 0 ? 0.0f
                : static_cast<float>(std::max(0.0, total_sum) /
                                     static_cast<double>(total_n));
        }
    };
    LoudnessWindow loud_m_{};            // momentary  (400 ms)
    LoudnessWindow loud_s_{};            // short-term (3 s)
    std::atomic<float> kw_ms_published_{0.0f};
    std::atomic<float> kw_ms_s_published_{0.0f};

    // Filter one sample through the K-weighting chain (audio thread).
    float kw_process_sample(float s) noexcept;
    // Fold one block's squared-sum into both windows + publish (audio thread).
    void  kw_push_block_sum(float sum_sq, std::uint32_t n) noexcept;

    // Published values (atomic for the control-thread reader).
    std::atomic<float> peak_db_published_{-120.0f};
    std::atomic<float> rms_db_published_{-120.0f};
    // Raw max since the last consuming read. Audio thread folds each block's
    // maximum in via a CAS fetch-max loop; snapshot_consume_max() exchanges
    // it back to silence.
    std::atomic<float> peak_max_since_read_db_{-120.0f};
    // True-peak published values (same pattern as the sample-peak pair).
    std::atomic<float> tp_db_published_{-120.0f};
    std::atomic<float> tp_max_since_read_db_{-120.0f};

    // Fold `db` into a max-since-read accumulator (CAS fetch-max — safe
    // against a concurrent consuming reader).
    static void fold_max(std::atomic<float>& acc, float db) noexcept;
};

} // namespace liveplay::audio
