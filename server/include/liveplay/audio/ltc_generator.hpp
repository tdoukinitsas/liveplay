// ============================================================================
// liveplay/audio/ltc_generator.hpp
// ----------------------------------------------------------------------------
// SMPTE Linear Timecode (LTC, SMPTE 12M) sample generator.
//
// LTC is a self-clocking biphase-mark-coded audio signal that conveys
// hours/minutes/seconds/frames of a video timecode. We synthesise it as a
// hard ±1.0 square wave (let downstream limiter handle level) at the engine's
// mix sample rate. The encoded clock is driven by an external "frame counter"
// — typically the host PlaybackItem advances it by render() frame counts so
// the LTC stays sample-accurate with the cue's playback.
//
// Frame format (80 bits, 10 bytes, transmitted LSB-first per byte):
//   bits  0- 3 : frame number units (BCD)
//   bits  4- 7 : user bits 1
//   bits  8- 9 : frame number tens (BCD)
//   bit  10    : drop-frame flag (1 for 29.97 DF, else 0)
//   bit  11    : colour-frame flag (we leave 0)
//   bits 12-15 : user bits 2
//   bits 16-19 : seconds units (BCD)
//   bits 20-23 : user bits 3
//   bits 24-26 : seconds tens (BCD)
//   bit  27    : even-parity bit (set so the whole word has even parity)
//   bits 28-31 : user bits 4
//   bits 32-35 : minutes units (BCD)
//   bits 36-39 : user bits 5
//   bits 40-42 : minutes tens (BCD)
//   bit  43    : binary group flag 1 (0 here)
//   bits 44-47 : user bits 6
//   bits 48-51 : hours units (BCD)
//   bits 52-55 : user bits 7
//   bits 56-57 : hours tens (BCD)
//   bit  58    : binary group flag 2 (0)
//   bit  59    : binary group flag 3 (0)
//   bits 60-63 : user bits 8
//   bits 64-79 : sync word = 0x3FFD (0011 1111 1111 1101 in tx order)
//
// Biphase mark coding:
//   * A transition occurs at every bit boundary.
//   * A '1' bit has an extra transition in the middle of the bit period.
// ============================================================================
#pragma once

#include "liveplay/audio/types.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace liveplay::audio {

enum class LTCFrameRate {
    Fps24       = 0,
    Fps25       = 1,
    Fps2997_NDF = 2,   // 29.97 non-drop-frame
    Fps2997_DF  = 3,   // 29.97 drop-frame
    Fps30       = 4,
};

// Human-readable frame rate (in seconds-per-frame) for clock advance.
double frame_rate_value(LTCFrameRate fr) noexcept;

class LTCGenerator {
public:
    LTCGenerator();

    // Reconfigure. Call from control thread between blocks.
    //   sample_rate     : engine mix rate
    //   frame_rate      : SMPTE rate
    //   offset_ns       : timecode value at cue playhead = 0
    //   amplitude_lin   : output amplitude (default 0.5 ≈ -6 dBFS so the
    //                     limiter doesn't have to clamp every transition)
    void configure(SampleRate sample_rate,
                   LTCFrameRate frame_rate,
                   std::chrono::nanoseconds offset = std::chrono::nanoseconds{0},
                   float amplitude_lin = 0.5f);

    // Reset the encoder to the beginning of the cue (playhead == 0).
    void reset(std::chrono::nanoseconds offset = std::chrono::nanoseconds{0}) noexcept;

    // Render `frame_count` mono samples of LTC into `dst`. The caller passes
    // the cue's current playhead position in nanoseconds for the FIRST sample
    // of this block — this is the timestamp the encoder syncs to.
    void render_block(Sample* dst,
                      std::size_t frame_count,
                      std::chrono::nanoseconds playhead) noexcept;

    LTCFrameRate frame_rate() const noexcept { return frame_rate_; }

private:
    SampleRate   sample_rate_ = kDefaultMixSampleRate;
    LTCFrameRate frame_rate_  = LTCFrameRate::Fps30;
    double       seconds_per_frame_ = 1.0 / 30.0;
    double       samples_per_bit_   = 0.0;
    long long    offset_ns_         = 0;
    float        amplitude_         = 0.5f;

    // Currently-emitting frame state.
    std::uint64_t current_frame_number_ = static_cast<std::uint64_t>(-1);
    std::uint8_t  current_bits_[10]     = {0};

    // Position within the current frame, in samples (fractional).
    double sample_within_frame_ = 0.0;
    // Position within the current bit, in samples (fractional).
    double sample_within_bit_   = 0.0;
    // Index of the current bit being emitted (0..79).
    std::size_t bit_index_      = 0;
    // Current output polarity (the encoder is a state machine — output toggles
    // on every bit boundary plus mid-bit for '1' bits).
    float polarity_             = 1.0f;
    // True if we have already emitted the mid-bit transition for the current
    // bit (only meaningful for '1' bits).
    bool mid_bit_emitted_       = true;

    // Build 80-bit frame for the given frame number.
    void build_frame_bits(std::uint64_t frame_number) noexcept;

    // Decompose absolute frame number into HH:MM:SS:FF using current rate.
    void timecode_for_frame(std::uint64_t frame_number,
                            int& hh, int& mm, int& ss, int& ff) const noexcept;

    bool get_bit(std::size_t bit_index) const noexcept;
};

} // namespace liveplay::audio
