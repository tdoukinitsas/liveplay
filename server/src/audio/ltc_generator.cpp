// ============================================================================
// ltc_generator.cpp — see ltc_generator.hpp for the encoding contract.
// ============================================================================
#include "liveplay/audio/ltc_generator.hpp"

#include <algorithm>
#include <cmath>

namespace liveplay::audio {

namespace {

// SMPTE 12M sync word, transmitted bits 64..79 in tx order.
// 0011 1111 1111 1101 — but the storage convention here keeps bit 64 = LSB of
// byte 8, bit 65 = next, ..., bit 79 = MSB of byte 9.
// In tx order: ...0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1
// Translating: byte[8] bits 0-7 = 0,0,1,1,1,1,1,1 → 0xFC
//              byte[9] bits 0-7 = 1,1,1,1,1,1,0,1 → 0xBF
constexpr std::uint8_t kSyncLo = 0xFC;
constexpr std::uint8_t kSyncHi = 0xBF;

inline void pack_bcd_nibble(std::uint8_t* bytes,
                            std::size_t  bit_offset,
                            int          value,
                            int          mask) noexcept {
    // Pack the low `bits-of-mask` bits of `value` into the bit stream LSB-first
    // starting at `bit_offset`. mask = e.g. 0x0F for 4 bits, 0x03 for 2 bits.
    int bits = 0;
    for (int m = mask; m; m >>= 1) ++bits;
    for (int i = 0; i < bits; ++i) {
        const std::size_t bit = bit_offset + static_cast<std::size_t>(i);
        const bool v          = (value & (1 << i)) != 0;
        if (v) {
            bytes[bit >> 3] |=  static_cast<std::uint8_t>(1u << (bit & 7));
        } else {
            bytes[bit >> 3] &= ~static_cast<std::uint8_t>(1u << (bit & 7));
        }
    }
}

inline void set_bit(std::uint8_t* bytes, std::size_t idx, bool v) noexcept {
    if (v) bytes[idx >> 3] |=  static_cast<std::uint8_t>(1u << (idx & 7));
    else   bytes[idx >> 3] &= ~static_cast<std::uint8_t>(1u << (idx & 7));
}

inline bool read_bit(const std::uint8_t* bytes, std::size_t idx) noexcept {
    return (bytes[idx >> 3] & (1u << (idx & 7))) != 0;
}

} // namespace

double frame_rate_value(LTCFrameRate fr) noexcept {
    switch (fr) {
        case LTCFrameRate::Fps24:        return 24.0;
        case LTCFrameRate::Fps25:        return 25.0;
        case LTCFrameRate::Fps2997_NDF:
        case LTCFrameRate::Fps2997_DF:   return 30000.0 / 1001.0;
        case LTCFrameRate::Fps30:        return 30.0;
    }
    return 30.0;
}

LTCGenerator::LTCGenerator() {
    configure(kDefaultMixSampleRate, LTCFrameRate::Fps30, std::chrono::nanoseconds{0}, 0.5f);
}

void LTCGenerator::configure(SampleRate sample_rate,
                             LTCFrameRate frame_rate,
                             std::chrono::nanoseconds offset,
                             float amplitude_lin) {
    sample_rate_       = sample_rate;
    frame_rate_        = frame_rate;
    seconds_per_frame_ = 1.0 / frame_rate_value(frame_rate);
    samples_per_bit_   = (static_cast<double>(sample_rate) * seconds_per_frame_) / 80.0;
    amplitude_         = std::clamp(amplitude_lin, 0.0f, 1.0f);
    reset(offset);
}

void LTCGenerator::reset(std::chrono::nanoseconds offset) noexcept {
    offset_ns_            = offset.count();
    current_frame_number_ = static_cast<std::uint64_t>(-1);
    sample_within_frame_  = 0.0;
    sample_within_bit_    = 0.0;
    bit_index_            = 0;
    polarity_             = 1.0f;
    mid_bit_emitted_      = true;
    for (auto& b : current_bits_) b = 0;
}

void LTCGenerator::timecode_for_frame(std::uint64_t frame_number,
                                      int& hh, int& mm, int& ss, int& ff) const noexcept {
    const std::uint64_t fps_int = (frame_rate_ == LTCFrameRate::Fps2997_DF ||
                                   frame_rate_ == LTCFrameRate::Fps2997_NDF)
                                      ? 30
                                      : static_cast<std::uint64_t>(frame_rate_value(frame_rate_) + 0.5);

    // Drop-frame for 29.97 DF: skip frame 0 and 1 of every minute except every
    // 10th minute, so the wall-clock matches NTSC video duration.
    std::uint64_t fn = frame_number;
    if (frame_rate_ == LTCFrameRate::Fps2997_DF) {
        const std::uint64_t frames_per_10min = 17982; // 10 * 60 * 30 - 2 * 9
        const std::uint64_t frames_per_min   = 1798;  // 60 * 30 - 2
        const std::uint64_t d = fn / frames_per_10min;
        const std::uint64_t m = fn % frames_per_10min;
        if (m > 1) {
            fn = fn + 18 * d + 2 * ((m - 2) / frames_per_min);
        } else {
            fn = fn + 18 * d;
        }
    }

    ff = static_cast<int>(fn % fps_int);
    const std::uint64_t total_seconds = fn / fps_int;
    ss = static_cast<int>(total_seconds % 60);
    mm = static_cast<int>((total_seconds / 60) % 60);
    hh = static_cast<int>((total_seconds / 3600) % 24);
}

void LTCGenerator::build_frame_bits(std::uint64_t frame_number) noexcept {
    int hh, mm, ss, ff;
    timecode_for_frame(frame_number, hh, mm, ss, ff);

    // Zero everything first; we'll set fields then sync + parity.
    for (auto& b : current_bits_) b = 0;

    // Frames
    pack_bcd_nibble(current_bits_, 0,  ff % 10, 0x0F);
    pack_bcd_nibble(current_bits_, 8,  ff / 10, 0x03);
    set_bit(current_bits_, 10, frame_rate_ == LTCFrameRate::Fps2997_DF);
    set_bit(current_bits_, 11, false);  // colour-frame flag

    // Seconds
    pack_bcd_nibble(current_bits_, 16, ss % 10, 0x0F);
    pack_bcd_nibble(current_bits_, 24, ss / 10, 0x07);
    // bit 27 = parity (set after the rest is known)

    // Minutes
    pack_bcd_nibble(current_bits_, 32, mm % 10, 0x0F);
    pack_bcd_nibble(current_bits_, 40, mm / 10, 0x07);
    set_bit(current_bits_, 43, false);  // binary group flag 1

    // Hours
    pack_bcd_nibble(current_bits_, 48, hh % 10, 0x0F);
    pack_bcd_nibble(current_bits_, 56, hh / 10, 0x03);
    set_bit(current_bits_, 58, false);  // binary group flag 2
    set_bit(current_bits_, 59, false);  // binary group flag 3

    // Sync word (bits 64..79). Per SMPTE 12M: 0x3FFD in tx order.
    current_bits_[8] = kSyncLo;
    current_bits_[9] = kSyncHi;

    // Compute and set parity bit (bit 27 for non-DF; SMPTE specifies even
    // parity over the data + sync portions). Count 1s in bits 0..63.
    int ones = 0;
    for (std::size_t i = 0; i < 64; ++i) {
        if (read_bit(current_bits_, i)) ++ones;
    }
    set_bit(current_bits_, 27, (ones & 1) != 0);  // make total 1s even
}

bool LTCGenerator::get_bit(std::size_t bit_index) const noexcept {
    return read_bit(current_bits_, bit_index);
}

void LTCGenerator::render_block(Sample* dst,
                                std::size_t frame_count,
                                std::chrono::nanoseconds playhead) noexcept {
    if (!dst || frame_count == 0) return;
    if (samples_per_bit_ <= 0.0)  { std::fill_n(dst, frame_count, 0.0f); return; }

    // Compute LTC absolute time (= playhead + offset) and the absolute frame
    // number containing the *first* sample of this block.
    const long long abs_ns = static_cast<long long>(playhead.count()) + offset_ns_;
    // Negative timecode (offset earlier than playhead start) → output silence.
    if (abs_ns < 0) {
        std::fill_n(dst, frame_count, 0.0f);
        return;
    }

    const double abs_seconds = static_cast<double>(abs_ns) * 1e-9;
    const double frames_f    = abs_seconds / seconds_per_frame_;
    const std::uint64_t target_frame_number = static_cast<std::uint64_t>(frames_f);

    // If the host has scrubbed/jumped, resync the encoder state.
    if (target_frame_number != current_frame_number_) {
        const double frac_into_frame_seconds =
            (frames_f - static_cast<double>(target_frame_number)) * seconds_per_frame_;
        const double sample_into_frame = frac_into_frame_seconds * static_cast<double>(sample_rate_);

        current_frame_number_ = target_frame_number;
        build_frame_bits(current_frame_number_);

        sample_within_frame_ = sample_into_frame;
        const double bit_pos = sample_into_frame / samples_per_bit_;
        bit_index_           = std::min<std::size_t>(79, static_cast<std::size_t>(bit_pos));
        sample_within_bit_   = (bit_pos - static_cast<double>(bit_index_)) * samples_per_bit_;
        mid_bit_emitted_     = sample_within_bit_ >= (samples_per_bit_ * 0.5);
        // Polarity unknown after a seek — start positive; will resync on next bit boundary.
        polarity_            = 1.0f;
    }

    const double half_bit = samples_per_bit_ * 0.5;

    for (std::size_t i = 0; i < frame_count; ++i) {
        dst[i] = polarity_ * amplitude_;

        // Advance one sample.
        sample_within_bit_   += 1.0;
        sample_within_frame_ += 1.0;

        // Mid-bit transition for '1' bits.
        if (!mid_bit_emitted_ && sample_within_bit_ >= half_bit) {
            if (get_bit(bit_index_)) {
                polarity_ = -polarity_;
            }
            mid_bit_emitted_ = true;
        }

        // Bit boundary?
        if (sample_within_bit_ >= samples_per_bit_) {
            sample_within_bit_ -= samples_per_bit_;
            polarity_           = -polarity_;   // every bit boundary toggles
            ++bit_index_;
            mid_bit_emitted_    = false;

            if (bit_index_ >= 80) {
                bit_index_ = 0;
                ++current_frame_number_;
                build_frame_bits(current_frame_number_);
                sample_within_frame_ = 0.0;
            }
        }
    }
}

} // namespace liveplay::audio
