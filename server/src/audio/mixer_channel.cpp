// ============================================================================
// mixer_channel.cpp — see mixer_channel.hpp for the contract.
// ============================================================================
#include "liveplay/audio/mixer_channel.hpp"

#include <algorithm>
#include <cmath>

namespace liveplay::audio {

MixerChannel::MixerChannel(MixerChannelId id, std::string display_name)
    : id_(std::move(id)),
      display_name_(std::move(display_name)) {}

void MixerChannel::configure(SampleRate sample_rate, FrameCount render_block) noexcept {
    sample_rate_  = sample_rate;
    render_block_ = render_block;
    for (auto& m : meters_) m.configure(sample_rate);
    dsp_.configure(sample_rate);
}

void MixerChannel::set_pan(float pan) noexcept {
    pan_.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void MixerChannel::update_correlation(const Sample* left, const Sample* right,
                                      std::size_t frame_count) noexcept {
    correlation_meter_.process(left, right, frame_count);
    correlation_.store(correlation_meter_.value(), std::memory_order_relaxed);
}

void MixerChannel::set_gain_db(float db) noexcept {
    const float lin = std::pow(10.0f, db * 0.05f);
    target_gain_linear_.store(lin, std::memory_order_release);
    // Cancel any in-flight fade; the new explicit set wins.
    fade_active_.store(false, std::memory_order_release);
}

void MixerChannel::set_mute(bool muted) noexcept {
    muted_.store(muted, std::memory_order_relaxed);
}

void MixerChannel::set_pfl(bool on) noexcept {
    pfl_.store(on, std::memory_order_relaxed);
}

void MixerChannel::set_width(ChannelCount w) noexcept {
    width_.store(std::clamp<ChannelCount>(w, 1, kMixerLanes), std::memory_order_relaxed);
}

void MixerChannel::begin_fade(float target_db, std::chrono::milliseconds duration) noexcept {
    const float target_lin = (target_db <= -120.0f) ? 0.0f
                                                    : std::pow(10.0f, target_db * 0.05f);
    const long long duration_samples = std::max<long long>(
        0, static_cast<long long>(duration.count()) * static_cast<long long>(sample_rate_) / 1000);

    // Snapshot current "where the audio thread is" gain so the ramp starts
    // from the present, not from the previous target. peek_ (not advance_) —
    // this runs on the control thread and must not move the render thread's
    // fade envelope.
    const float now_lin = peek_gain_linear();
    fade_start_linear_.store(now_lin, std::memory_order_relaxed);
    fade_target_linear_.store(target_lin, std::memory_order_relaxed);
    fade_duration_samples_.store(duration_samples, std::memory_order_relaxed);
    fade_elapsed_samples_.store(0, std::memory_order_relaxed);

    if (duration_samples <= 0) {
        target_gain_linear_.store(target_lin, std::memory_order_release);
        fade_active_.store(false, std::memory_order_release);
    } else {
        fade_active_.store(true, std::memory_order_release);
    }
}

// Shape of the fade ramp: equal-power-ish cosine, which feels nicer than
// linear on faders. `t` is the normalised fade position in [0, 1].
static inline float fade_curve(float t) noexcept {
    return 0.5f - 0.5f * std::cos(3.14159265358979323846f * t);
}

float MixerChannel::peek_gain_linear() const noexcept {
    if (!fade_active_.load(std::memory_order_acquire)) {
        return target_gain_linear_.load(std::memory_order_acquire);
    }

    const long long total = fade_duration_samples_.load(std::memory_order_relaxed);
    const float target    = fade_target_linear_.load(std::memory_order_relaxed);
    // A zero-length fade never stays active (begin_fade clears the flag), but a
    // begin_fade() racing with this read can transiently expose total == 0.
    if (total <= 0) return target;

    const long long elapsed = fade_elapsed_samples_.load(std::memory_order_relaxed);
    if (elapsed >= total) return target;

    const float start = fade_start_linear_.load(std::memory_order_relaxed);
    const float t     = static_cast<float>(elapsed) / static_cast<float>(total);
    return start + (target - start) * fade_curve(t);
}

void MixerChannel::advance(FrameCount frames) noexcept {
    if (!fade_active_.load(std::memory_order_acquire)) return;

    const long long total = fade_duration_samples_.load(std::memory_order_relaxed);
    if (total <= 0) {
        // Degenerate fade (see peek_gain_linear) — settle on the target.
        target_gain_linear_.store(fade_target_linear_.load(std::memory_order_relaxed),
                                  std::memory_order_release);
        fade_active_.store(false, std::memory_order_release);
        return;
    }

    long long elapsed = fade_elapsed_samples_.load(std::memory_order_relaxed);
    elapsed += static_cast<long long>(frames);

    if (elapsed >= total) {
        // Fade complete: latch the target as the new resting gain so later
        // peeks return it without touching the (now inactive) envelope.
        fade_elapsed_samples_.store(total, std::memory_order_relaxed);
        target_gain_linear_.store(fade_target_linear_.load(std::memory_order_relaxed),
                                  std::memory_order_release);
        fade_active_.store(false, std::memory_order_release);
        return;
    }

    fade_elapsed_samples_.store(elapsed, std::memory_order_relaxed);
}

void MixerChannel::update_meter(ChannelIndex lane,
                                const Sample* samples, std::size_t frame_count) noexcept {
    if (lane >= meters_.size()) return;
    meters_[lane].push_block(samples, frame_count);
}

MeterSnapshot MixerChannel::meter_snapshot() const noexcept {
    MeterSnapshot out;
    for (const auto& m : meters_) {
        const auto s = m.snapshot();
        out.peak_db          = std::max(out.peak_db,          s.peak_db);
        out.rms_db           = std::max(out.rms_db,           s.rms_db);
        out.peak_max_db      = std::max(out.peak_max_db,      s.peak_max_db);
        out.true_peak_db     = std::max(out.true_peak_db,     s.true_peak_db);
        out.true_peak_max_db = std::max(out.true_peak_max_db, s.true_peak_max_db);
        // Loudness sums across the channel group (BS.1770), it doesn't max.
        out.kw_ms   += s.kw_ms;
        out.kw_ms_s += s.kw_ms_s;
    }
    return out;
}

MeterSnapshot MixerChannel::meter_snapshot(ChannelIndex lane) const noexcept {
    if (lane >= meters_.size()) return {};
    return meters_[lane].snapshot();
}

std::array<MeterSnapshot, kMixerLanes> MixerChannel::meter_snapshot_consume_lanes() noexcept {
    std::array<MeterSnapshot, kMixerLanes> out{};
    for (std::size_t i = 0; i < meters_.size() && i < out.size(); ++i) {
        out[i] = meters_[i].snapshot_consume_max();
    }
    return out;
}

MeterSnapshot MixerChannel::meter_snapshot_consume() noexcept {
    MeterSnapshot out;
    for (auto& m : meters_) {
        const auto s = m.snapshot_consume_max();
        out.peak_db          = std::max(out.peak_db,          s.peak_db);
        out.rms_db           = std::max(out.rms_db,           s.rms_db);
        out.peak_max_db      = std::max(out.peak_max_db,      s.peak_max_db);
        out.true_peak_db     = std::max(out.true_peak_db,     s.true_peak_db);
        out.true_peak_max_db = std::max(out.true_peak_max_db, s.true_peak_max_db);
        // Loudness sums across the channel group (BS.1770), it doesn't max.
        out.kw_ms   += s.kw_ms;
        out.kw_ms_s += s.kw_ms_s;
    }
    return out;
}

} // namespace liveplay::audio
