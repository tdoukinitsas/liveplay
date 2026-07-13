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

// 4× oversampling interpolator prototype for true-peak detection
// (ITU-R BS.1770-4 style): 48-tap Blackman-Harris-windowed sinc, split into
// 4 polyphase branches of 12 taps. Branch 0 is (near) pass-through; branches
// 1-3 estimate the waveform at +0.25 / +0.5 / +0.75 sample offsets. Each
// branch is normalised to unity DC gain so a steady full-scale signal reads
// exactly 0 dBTP. Built once, thread-safe (magic static), on first use from
// the control thread (Meter construction).
const std::array<float, 48>& tp_filter_taps() {
    static const std::array<float, 48> taps = [] {
        std::array<float, 48> t{};
        constexpr int    N  = 48;
        constexpr double L  = 4.0;
        constexpr double PI = 3.14159265358979323846;
        for (int n = 0; n < N; ++n) {
            // Sinc centred on tap 24 → branch 0 lands on integer offsets
            // (pure delay), branches 1-3 on the intersample positions.
            const double x = (static_cast<double>(n) - 24.0) / L;
            const double s = (x == 0.0) ? 1.0 : std::sin(PI * x) / (PI * x);
            const double w = 0.35875
                           - 0.48829 * std::cos(2.0 * PI * n / (N - 1))
                           + 0.14128 * std::cos(4.0 * PI * n / (N - 1))
                           - 0.01168 * std::cos(6.0 * PI * n / (N - 1));
            t[n] = static_cast<float>(s * w);
        }
        for (int p = 0; p < 4; ++p) {
            double sum = 0.0;
            for (int m = 0; m < 12; ++m) sum += t[m * 4 + p];
            for (int m = 0; m < 12; ++m)
                t[m * 4 + p] = static_cast<float>(t[m * 4 + p] / sum);
        }
        return t;
    }();
    return taps;
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
    const float atk   = attack_coef_.load(std::memory_order_relaxed);
    const float rel   = release_coef_.load(std::memory_order_relaxed);
    const float roma  = rms_one_minus_a_.load(std::memory_order_relaxed);
    const bool  tp_on = true_peak_enabled_.load(std::memory_order_relaxed);

    float peak = peak_env_;
    float rms  = rms_sq_;
    float blk_max = 0.0f;
    float tp_env = tp_env_;
    float tp_blk_max = 0.0f;
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

        // True peak: 4× oversample, same ballistics on the TP stream.
        if (tp_on) {
            const float tp = tp_process_sample(s);
            if (tp > tp_blk_max) tp_blk_max = tp;
            if (tp > tp_env) tp_env = atk * tp_env + (1.0f - atk) * tp;
            else             tp_env = rel * tp_env + (1.0f - rel) * tp;
        }
    }

    peak_env_ = peak;
    rms_sq_   = rms;

    peak_db_published_.store(clamp_db(lin_to_db(peak)), std::memory_order_relaxed);
    rms_db_published_ .store(clamp_db(lin_to_db(std::sqrt(rms))), std::memory_order_relaxed);
    fold_max(peak_max_since_read_db_, clamp_db(lin_to_db(blk_max)));
    if (tp_on) {
        tp_env_ = tp_env;
        tp_db_published_.store(clamp_db(lin_to_db(tp_env)), std::memory_order_relaxed);
        fold_max(tp_max_since_read_db_, clamp_db(lin_to_db(tp_blk_max)));
    }
}

void Meter::push_interleaved(const Sample* interleaved,
                             std::size_t frame_count,
                             ChannelCount channel_stride,
                             ChannelIndex channel_index) noexcept {
    if (!interleaved || frame_count == 0 || channel_stride == 0) return;

    const float atk   = attack_coef_.load(std::memory_order_relaxed);
    const float rel   = release_coef_.load(std::memory_order_relaxed);
    const float roma  = rms_one_minus_a_.load(std::memory_order_relaxed);
    const bool  tp_on = true_peak_enabled_.load(std::memory_order_relaxed);

    float peak = peak_env_;
    float rms  = rms_sq_;
    float blk_max = 0.0f;
    float tp_env = tp_env_;
    float tp_blk_max = 0.0f;
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

        if (tp_on) {
            const float tp = tp_process_sample(s);
            if (tp > tp_blk_max) tp_blk_max = tp;
            if (tp > tp_env) tp_env = atk * tp_env + (1.0f - atk) * tp;
            else             tp_env = rel * tp_env + (1.0f - rel) * tp;
        }
    }

    peak_env_ = peak;
    rms_sq_   = rms;

    peak_db_published_.store(clamp_db(lin_to_db(peak)), std::memory_order_relaxed);
    rms_db_published_ .store(clamp_db(lin_to_db(std::sqrt(rms))), std::memory_order_relaxed);
    fold_max(peak_max_since_read_db_, clamp_db(lin_to_db(blk_max)));
    if (tp_on) {
        tp_env_ = tp_env;
        tp_db_published_.store(clamp_db(lin_to_db(tp_env)), std::memory_order_relaxed);
        fold_max(tp_max_since_read_db_, clamp_db(lin_to_db(tp_blk_max)));
    }
}

void Meter::fold_max(std::atomic<float>& acc, float db) noexcept {
    float cur = acc.load(std::memory_order_relaxed);
    while (db > cur &&
           !acc.compare_exchange_weak(cur, db, std::memory_order_relaxed)) {
        // cur reloaded by compare_exchange_weak on failure.
    }
}

float Meter::tp_process_sample(float s) noexcept {
    const auto& h = tp_filter_taps();
    tp_hist_[tp_hist_pos_] = s;
    float max_abs = 0.0f;
    for (std::size_t p = 0; p < kTpPhases; ++p) {
        float acc = 0.0f;
        std::size_t idx = tp_hist_pos_;
        for (std::size_t m = 0; m < kTpTapsPerPhase; ++m) {
            acc += h[m * kTpPhases + p] * tp_hist_[idx];
            idx = (idx == 0) ? kTpTapsPerPhase - 1 : idx - 1;
        }
        const float a = std::fabs(acc);
        if (a > max_abs) max_abs = a;
    }
    tp_hist_pos_ = (tp_hist_pos_ + 1) % kTpTapsPerPhase;
    return max_abs;
}

MeterSnapshot Meter::snapshot() const noexcept {
    MeterSnapshot s;
    s.peak_db     = peak_db_published_.load(std::memory_order_relaxed);
    s.rms_db      = rms_db_published_ .load(std::memory_order_relaxed);
    s.peak_max_db = peak_max_since_read_db_.load(std::memory_order_relaxed);
    if (true_peak_enabled_.load(std::memory_order_relaxed)) {
        s.true_peak_db     = tp_db_published_.load(std::memory_order_relaxed);
        s.true_peak_max_db = tp_max_since_read_db_.load(std::memory_order_relaxed);
    } else {
        // Mirror sample peak so a dBTP display degrades gracefully.
        s.true_peak_db     = s.peak_db;
        s.true_peak_max_db = s.peak_max_db;
    }
    return s;
}

MeterSnapshot Meter::snapshot_consume_max() noexcept {
    MeterSnapshot s;
    s.peak_db     = peak_db_published_.load(std::memory_order_relaxed);
    s.rms_db      = rms_db_published_ .load(std::memory_order_relaxed);
    s.peak_max_db = peak_max_since_read_db_.exchange(-120.0f, std::memory_order_relaxed);
    if (true_peak_enabled_.load(std::memory_order_relaxed)) {
        s.true_peak_db     = tp_db_published_.load(std::memory_order_relaxed);
        s.true_peak_max_db = tp_max_since_read_db_.exchange(-120.0f, std::memory_order_relaxed);
    } else {
        s.true_peak_db     = s.peak_db;
        s.true_peak_max_db = s.peak_max_db;
    }
    return s;
}

void Meter::reset() noexcept {
    peak_env_ = 0.0f;
    rms_sq_   = 0.0f;
    tp_env_   = 0.0f;
    tp_hist_.fill(0.0f);
    tp_hist_pos_ = 0;
    peak_db_published_.store(-120.0f, std::memory_order_relaxed);
    rms_db_published_ .store(-120.0f, std::memory_order_relaxed);
    peak_max_since_read_db_.store(-120.0f, std::memory_order_relaxed);
    tp_db_published_.store(-120.0f, std::memory_order_relaxed);
    tp_max_since_read_db_.store(-120.0f, std::memory_order_relaxed);
}

} // namespace liveplay::audio
