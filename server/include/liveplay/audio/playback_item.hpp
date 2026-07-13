// ============================================================================
// liveplay/audio/playback_item.hpp
// ----------------------------------------------------------------------------
// A single live cue instance — Tier 1 of the mixer tree.
//
// Each PlaybackItem owns:
//   * Its own ma_decoder (independent state per cue, even when two cues load
//     the same file — solves the LivePlay 1.x state-sharing bug)
//   * Per-item linear gain + fade envelope (in/out)
//   * Transport state (Stopped / Playing / FadingOut / Paused)
//   * An optional LTCGenerator that occupies a synthetic source channel
//     appended after the file's real channels
//   * A per-source-channel Meter
//
// Public mutators are control-thread safe (atomics + mutex for the decoder).
// render_block() is audio-thread-only.
//
// Manual-stop fade contract:
//   stop()        → if the configured fade-out duration is non-zero, transition
//                   into FadingOut for that duration, then Stopped.
//   stop_now()    → immediate stop, ignoring fade duration (panic button).
//   master_stop() → goes through stop() (so fades are honoured).
//   natural end-of-file → also funnels through stop() with the fade.
//   ⇒ all three paths converge on the same envelope code → guaranteed
//     consistent behaviour, fixing the 1.x "fade only on natural end" gap.
// ============================================================================
#pragma once

#include "liveplay/audio/ltc_generator.hpp"
#include "liveplay/audio/meter.hpp"
#include "liveplay/audio/types.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward-declare to keep miniaudio.h out of this public header.
struct ma_decoder;

namespace liveplay::audio {

enum class TransportState : std::uint8_t {
    Stopped     = 0,
    Playing     = 1,
    FadingIn    = 2,
    FadingOut   = 3,
    Paused      = 4,
};

struct PlaybackItemDesc {
    CueId                    id;
    std::filesystem::path    file_path;
    SampleRate               mix_sample_rate = kDefaultMixSampleRate;
    FrameCount               render_block    = kDefaultRenderBlock;

    // Default channel count emitted by the decoder when the file doesn't
    // declare one we recognise. 2 (stereo) covers ~all real-world cues.
    ChannelCount             fallback_channels = 2;

    // Per-cue fade durations (0 ms = instant).
    std::chrono::milliseconds fade_in_duration  {0};
    std::chrono::milliseconds fade_out_duration {0};

    // LTC: nullopt = no LTC on this cue. When set, the LTCGenerator's mono
    // output is appended as an extra source channel after the file's
    // channels — index = file_channels (0-based).
    bool                      ltc_enabled = false;
    LTCFrameRate              ltc_frame_rate = LTCFrameRate::Fps30;
    std::chrono::nanoseconds  ltc_offset    {0};
};

// Statistics that the control thread can read for UI / debugging.
struct PlaybackItemStats {
    TransportState  transport      = TransportState::Stopped;
    FrameCount      playhead_frame = 0;
    double          playhead_seconds = 0.0;
    ChannelCount    source_channels = 0;     // including LTC if enabled
    bool            file_loaded     = false;
    bool            at_end          = false;
};

class PlaybackItem {
public:
    explicit PlaybackItem(PlaybackItemDesc desc);
    ~PlaybackItem();

    PlaybackItem(const PlaybackItem&) = delete;
    PlaybackItem& operator=(const PlaybackItem&) = delete;

    // Open the decoder. Returns false on failure (file missing, format
    // unsupported, etc.). Idempotent — calling twice is allowed but rebuilds
    // the decoder.
    bool load();

    // Close the decoder and free its memory. Stops playback first.
    void unload();

    // ---- Transport (control thread) --------------------------------------
    void play();
    void stop();                                  // honours fade_out_duration
    void stop_now();                              // hard stop, ignores fade
    void pause();
    void resume();
    void seek_seconds(double seconds);

    // ---- Mutators --------------------------------------------------------
    void set_gain_db(float db) noexcept;
    void set_fade_in (std::chrono::milliseconds d) noexcept { desc_.fade_in_duration  = d; }
    void set_fade_out(std::chrono::milliseconds d) noexcept { desc_.fade_out_duration = d; }
    void set_ltc_enabled(bool enabled);
    void set_ltc_frame_rate(LTCFrameRate fr);
    void set_ltc_offset(std::chrono::nanoseconds offset) noexcept;

    // Configure a soft end-of-playback point in seconds (the item's "out
    // point"). When the playhead reaches this frame, the same code path as
    // natural EOF runs — stop() honours fade_out_duration. Pass <= 0 to
    // disable (play to natural EOF). Safe to set while playing.
    void set_out_point_seconds(double seconds) noexcept;

    // Enable/disable seamless looping. When enabled, reaching EOF / out-point
    // seeks the decoder back to `in_seconds` and continues playing without
    // transitioning to FadingOut/Stopped — so the broadcast loop never emits a
    // transient "Stopped" cue_state edge mid-loop and the client UI keeps the
    // cue visible the whole time. Safe to call while playing.
    void set_loop(bool enabled, double in_seconds = 0.0) noexcept;

    // Returns true (and clears the flag) if this item finished playing
    // naturally (reached EOF or out-point, including any configured
    // fade-out). Returns false if the item was explicitly stopped or
    // hasn't stopped yet. Called by the ProjectState sequencer thread.
    bool take_natural_end() noexcept;

    // Current target gain in dB. Used by the sequencer to snapshot a
    // cue's gain before ducking so it can be restored afterward.
    float gain_db() const noexcept;

    // Fade out over `dur` and stop, without modifying the stored
    // fade_out_duration. Used by the crossfade and stop-fade sequencer
    // so these fades don't disturb the "explicit-stop" fade setting.
    void stop_with_fade(std::chrono::milliseconds dur);

    // Pre-warm the decoder by reading and discarding `seconds` of audio
    // starting from `start_seconds`, then leaving the decoder cursor at
    // `start_seconds` (so a subsequent play() returns to the same point).
    // This populates the OS file cache and primes the decoder's internal
    // state so the *first* read during actual playback doesn't block on
    // disk I/O — the dominant cause of crackling at the start of a cue.
    // Returns true on success, false if the decoder isn't ready. Safe to
    // call while NOT playing; should not be called concurrently with
    // playback (it holds the decoder mutex).
    bool prime(double seconds = 2.0, double start_seconds = 0.0) noexcept;

    // ---- Introspection ---------------------------------------------------
    const CueId&     id() const noexcept                  { return desc_.id; }
    const PlaybackItemDesc& desc() const noexcept         { return desc_; }
    ChannelCount     source_channel_count() const noexcept;  // includes LTC if enabled
    PlaybackItemStats stats() const noexcept;

    MeterSnapshot source_meter(ChannelIndex ch) const noexcept;
    // Consuming read (resets the channel's max-since-read). Broadcaster only.
    MeterSnapshot source_meter_consume(ChannelIndex ch) noexcept;

    // ---- Audio thread ----------------------------------------------------
    // Render `frame_count` frames into `out` (deinterleaved per source channel).
    // `out` must point at an array of `source_channel_count()` channel pointers,
    // each at least `frame_count` Samples long. The engine pre-allocates these
    // buffers and reuses them.
    //
    // Returns the number of frames actually written. May be less than
    // frame_count when end-of-file is reached this block; the rest is silenced
    // and the transport transitions to Stopped (via the fade pathway).
    std::size_t render_block(Sample* const* out_channel_buffers,
                             ChannelCount   out_channel_count,
                             std::size_t    frame_count) noexcept;

private:
    PlaybackItemDesc desc_;

    // Owned decoder. Pointer because miniaudio types stay out of this header.
    // Protected by decoder_mutex_ for load/unload/seek; the audio thread
    // grabs a try_lock and falls back to silence on contention (rare).
    std::unique_ptr<ma_decoder> decoder_;
    std::mutex                  decoder_mutex_;
    bool                        decoder_ready_ = false;
    ChannelCount                file_channels_ = 0;

    // Transport + gain state (hot atomics).
    std::atomic<TransportState> transport_{TransportState::Stopped};
    std::atomic<float>          gain_target_linear_{1.0f};
    std::atomic<float>          gain_current_linear_{1.0f};     // smoothed

    // Per-block fade state (active when transport_ == FadingIn or FadingOut).
    std::atomic<float>          fade_start_linear_{0.0f};
    std::atomic<float>          fade_end_linear_{1.0f};
    std::atomic<long long>      fade_duration_samples_{0};
    std::atomic<long long>      fade_elapsed_samples_{0};

    // Playhead in mix-rate frames. Audio thread is the only writer.
    std::atomic<std::uint64_t>  playhead_frames_{0};

    // Out-point: when playhead_frames_ reaches this value, render_block
    // triggers the natural-EOF code path (fade-out then Stopped). 0 disables
    // (play to file end).
    std::atomic<std::uint64_t>  out_point_frames_{0};

    // Seamless-loop state: when loop_enabled_ is true, hitting EOF / out-point
    // inside render_block() seeks the decoder back to loop_in_frames_ and
    // continues playing instead of fading out. Owned by the control thread for
    // writes, read by the audio thread per block.
    std::atomic<bool>           loop_enabled_{false};
    std::atomic<std::uint64_t>  loop_in_frames_{0};

    // Set to true inside render_block() when the natural-end fade-out
    // starts (EOF or out-point triggered). Cleared on explicit stop().
    // When the FadingOut→Stopped transition completes, stopped_naturally_
    // is set and this flag is cleared.
    std::atomic<bool> fading_out_naturally_{false};
    // Set to true when a naturally-initiated fade-out finishes (transport
    // becomes Stopped). Cleared by take_natural_end() or play().
    std::atomic<bool> stopped_naturally_{false};

    // LTC generator (optional). Built fresh whenever LTC config changes.
    std::unique_ptr<LTCGenerator> ltc_;
    std::atomic<bool>             ltc_enabled_atomic_{false};
    // Atomic offset so set_ltc_offset() can be called while playing without
    // touching the LTCGenerator from the control thread.
    std::atomic<long long>        ltc_offset_ns_{0};

    // Per-source-channel meters (including LTC if enabled). Sized at load().
    std::vector<std::unique_ptr<Meter>> source_meters_;

    // Helpers ----------------------------------------------------------
    void start_fade(float from_lin, float to_lin, std::chrono::milliseconds dur,
                    TransportState during, TransportState after_complete) noexcept;
    void resize_meters(ChannelCount n);
};

} // namespace liveplay::audio
