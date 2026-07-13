// ============================================================================
// liveplay/audio/mixer_channel.hpp
// ----------------------------------------------------------------------------
// A virtual mixer strip — Tier 2 of the engine's routing tree. Items send into
// it; it sums per lane (kMixerLanes parallel lanes, stereo L/R), applies its
// own gain/fade/mute/solo across all lanes, then each lane sends to one or
// more Master output channels. The lane buffers themselves are owned by the
// engine; the strip owns control state and per-lane meters.
//
// State lives in atomics so the control thread can adjust gain etc. without
// stalling the render thread. The per-block contribution buffer is owned by
// the engine, not the channel (avoids cache-thrashing if many channels share
// a render thread).
// ============================================================================
#pragma once

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
    void set_solo(bool soloed) noexcept;

    // Begin a smooth gain ramp toward `target_db` over `duration`. Used both
    // for explicit "fade" requests and as the universal stop transition.
    void begin_fade(float target_db, std::chrono::milliseconds duration) noexcept;

    // ---- Audio-thread reads ----------------------------------------------
    float current_gain_linear() noexcept;     // applies any active ramp
    bool  is_muted() const noexcept           { return muted_.load(std::memory_order_relaxed); }
    bool  is_soloed() const noexcept          { return soloed_.load(std::memory_order_relaxed); }

    // Push one lane's block of samples (already mixed contribution from items
    // routed to this strip) into that lane's meter.
    void update_meter(ChannelIndex lane,
                      const Sample* samples, std::size_t frame_count) noexcept;

    // Initialise audio-thread state (call from engine setup).
    void configure(SampleRate sample_rate, FrameCount render_block) noexcept;

    // Combined strip reading: element-wise max across lanes (what a single
    // strip meter widget should show).
    MeterSnapshot meter_snapshot() const noexcept;
    // Per-lane reading (L = 0, R = 1) for stereo strip meters.
    MeterSnapshot meter_snapshot(ChannelIndex lane) const noexcept;
    // Combined consuming read (resets every lane's max-since-read).
    // Broadcaster only.
    MeterSnapshot meter_snapshot_consume() noexcept;

private:
    MixerChannelId id_;
    std::string    display_name_;

    // Atomically-updated target gain (linear).
    std::atomic<float> target_gain_linear_{1.0f};
    std::atomic<bool>  muted_{false};
    std::atomic<bool>  soloed_{false};

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
