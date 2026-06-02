// ============================================================================
// liveplay/audio/mixer_channel.hpp
// ----------------------------------------------------------------------------
// A virtual mixer strip — Tier 2 of the engine's routing tree. Items send into
// it; it sums, applies its own gain/fade/mute/solo, then sends to one or more
// Master output channels.
//
// State lives in atomics so the control thread can adjust gain etc. without
// stalling the render thread. The per-block contribution buffer is owned by
// the engine, not the channel (avoids cache-thrashing if many channels share
// a render thread).
// ============================================================================
#pragma once

#include "liveplay/audio/meter.hpp"
#include "liveplay/audio/types.hpp"

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

    // Push a block of mono samples (already mixed contribution from items
    // routed to this channel) into the meter.
    void update_meter(const Sample* samples, std::size_t frame_count) noexcept;

    // Initialise audio-thread state (call from engine setup).
    void configure(SampleRate sample_rate, FrameCount render_block) noexcept;

    MeterSnapshot meter_snapshot() const noexcept { return meter_.snapshot(); }

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

    Meter meter_;
};

} // namespace liveplay::audio
