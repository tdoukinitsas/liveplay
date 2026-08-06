// ============================================================================
// liveplay/audio/mixer_channel.hpp
// ----------------------------------------------------------------------------
// A virtual mixer strip — Tier 2 of the engine's routing tree. Items send into
// it; it sums per lane (kMixerLanes parallel lanes, stereo L/R), applies its
// own gain/fade/mute across all lanes, then each lane sends to one or more
// Master output channels. The lane buffers themselves are owned by the engine;
// the strip owns control state and per-lane meters.
//
// State lives in atomics so the control thread can adjust gain etc. without
// stalling the render thread. The per-block contribution buffer is owned by
// the engine, not the channel (avoids cache-thrashing if many channels share
// a render thread).
// ============================================================================
#pragma once

#include "liveplay/audio/channel_dsp.hpp"
#include "liveplay/audio/meter.hpp"
#include "liveplay/audio/types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace liveplay::audio {

class MixerChannel {
public:
    MixerChannel(MixerChannelId id, std::string display_name);

    const MixerChannelId& id() const noexcept { return id_; }
    const std::string& display_name() const noexcept { return display_name_; }
    void set_display_name(std::string name) { display_name_ = std::move(name); }

    // ---- Control-thread mutators -----------------------------------------
    void set_gain_db(float db) noexcept;
    void set_mute(bool muted) noexcept;

    // PFL — pre-fade listen. Replaces solo, which this engine never wired to
    // the UI and which the mixer design rejected outright (§2.4): solo-in-place
    // silences the room on a mis-click, PFL only adds a tap into the Monitor
    // strip. Setting it changes no audio here; the engine reads it when it
    // rebuilds the topology and turns it into monitor taps.
    void set_pfl(bool on) noexcept;

    // How many of the strip's lanes carry signal: 1 = mono (lane 0 only),
    // 2 = stereo. Semantic, not structural — every strip always owns
    // kMixerLanes lanes (§2.5). The engine needs it to place a PFL'd strip in
    // the monitor: a mono strip tapped lane-for-lane would arrive hard left.
    void set_width(ChannelCount w) noexcept;

    // Begin a smooth gain ramp toward `target_db` over `duration`. Used both
    // for explicit "fade" requests and as the universal stop transition.
    void begin_fade(float target_db, std::chrono::milliseconds duration) noexcept;

    // ---- Audio-thread reads ----------------------------------------------
    // Side-effect-free read of the strip gain, including the value an active
    // fade has reached. Safe to call any number of times per render block (for
    // metering, for gain application, from the control thread) — it never
    // advances the fade envelope.
    float peek_gain_linear() const noexcept;

    // Advance an active fade envelope by `frames`. Call exactly ONCE per render
    // block per strip, from the render thread only. Returns nothing: read the
    // resulting gain with peek_gain_linear().
    void  advance(FrameCount frames) noexcept;
    // Convenience: advance by the configured render block size.
    void  advance_block() noexcept { advance(render_block_); }

    bool  is_muted() const noexcept           { return muted_.load(std::memory_order_relaxed); }
    bool  is_pfl() const noexcept             { return pfl_.load(std::memory_order_relaxed); }
    ChannelCount width() const noexcept       { return width_.load(std::memory_order_relaxed); }

    // Push one lane's block of samples (already mixed contribution from items
    // routed to this strip) into that lane's meter.
    void update_meter(ChannelIndex lane,
                      const Sample* samples, std::size_t frame_count) noexcept;

    // The strip's position control, -1..+1. It means one of two things
    // depending on width, and the strip stores it for the same reason either
    // way: both are applied in the send to the master, which is DOWNSTREAM of
    // the PFL tap, so the tap has to place the signal itself or the phones
    // would disagree with the house.
    //
    //   mono   — pan, the position of lane 0 between the destination's lanes
    //   stereo — balance, a trim between the strip's own two lanes
    //
    // One field because it is one knob in one place on the surface; the law
    // that reads it differs (pan_gains_db vs balance_gains_db) and that is
    // where the distinction lives.
    void  set_pan(float pan) noexcept;
    float pan() const noexcept { return pan_.load(std::memory_order_relaxed); }

    // Correlation between the strip's two lanes over the last block, +1..-1.
    // Fed by the render loop after the strip pass, so it describes what
    // actually leaves the strip — including anything the width block did.
    // Mono strips never call it and read a steady +1.
    void  update_correlation(const Sample* left, const Sample* right,
                             std::size_t frame_count) noexcept;
    float correlation() const noexcept {
        return correlation_.load(std::memory_order_relaxed);
    }

    // The strip's fixed processing chain: HPF, LPF, EQ, dynamics. Owned here
    // because its filter memory has to survive topology rebuilds — a routing
    // change must not clear the tail out of every EQ on the desk.
    ChannelDsp&       dsp() noexcept       { return dsp_; }
    const ChannelDsp& dsp() const noexcept { return dsp_; }

    // Initialise audio-thread state (call from engine setup).
    void configure(SampleRate sample_rate, FrameCount render_block) noexcept;

    // Retune every lane meter's ballistics. Safe mid-playback (Meter
    // coefficients are atomic).
    void configure_meters(const MeterBallistics& b) noexcept {
        for (auto& m : meters_) m.configure(sample_rate_, b);
    }

    // Toggle 4× oversampled true-peak detection on every lane meter.
    void set_true_peak_enabled(bool enabled) noexcept {
        for (auto& m : meters_) m.set_true_peak_enabled(enabled);
    }

    // Toggle K-weighted loudness on every lane meter.
    void set_loudness_enabled(bool enabled) noexcept {
        for (auto& m : meters_) m.set_loudness_enabled(enabled);
    }

    // Combined strip reading: element-wise max across lanes (what a single
    // strip meter widget should show).
    MeterSnapshot meter_snapshot() const noexcept;
    // Per-lane reading (L = 0, R = 1) for stereo strip meters.
    MeterSnapshot meter_snapshot(ChannelIndex lane) const noexcept;
    // Combined consuming read (resets every lane's max-since-read).
    // Broadcaster only.
    MeterSnapshot meter_snapshot_consume() noexcept;

    // Per-lane consuming read. Use *instead of* meter_snapshot_consume(), not
    // alongside it: both reset the max-since-read, so calling each in turn
    // would leave the second one reading silence. Returns one snapshot per
    // lane so a stereo strip can show separate L/R meters.
    std::array<MeterSnapshot, kMixerLanes> meter_snapshot_consume_lanes() noexcept;

private:
    MixerChannelId id_;
    std::string    display_name_;

    // Atomically-updated target gain (linear).
    std::atomic<float> target_gain_linear_{1.0f};
    std::atomic<bool>  muted_{false};
    std::atomic<bool>  pfl_{false};
    std::atomic<ChannelCount> width_{kMixerLanes};
    std::atomic<float> pan_{0.0f};

    ChannelDsp         dsp_;
    // Written by the render thread, read by the meter broadcaster. The state
    // lives on the audio side; only the smoothed result is published.
    CorrelationMeter    correlation_meter_{};
    std::atomic<float>  correlation_{1.0f};

    // Fade ramp parameters set by begin_fade(). Hot-read by audio thread.
    std::atomic<float>           fade_target_linear_{1.0f};
    std::atomic<float>           fade_start_linear_{1.0f};
    std::atomic<long long>       fade_duration_samples_{0};
    std::atomic<long long>       fade_elapsed_samples_{0};
    std::atomic<bool>            fade_active_{false};

    SampleRate  sample_rate_  = kDefaultMixSampleRate;
    FrameCount  render_block_ = kDefaultRenderBlock;

    std::array<Meter, kMixerLanes> meters_;   // one per strip lane (L/R)
};

} // namespace liveplay::audio
