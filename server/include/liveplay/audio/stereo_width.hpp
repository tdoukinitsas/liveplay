// ============================================================================
// liveplay/audio/stereo_width.hpp
// ----------------------------------------------------------------------------
// Mid/side width, with optional bass-mono, for a stereo strip.
//
//     M = (L + R) / 2        L = M + w·S
//     S = (L - R) / 2        R = M - w·S
//
// w = 0 is mono, 1 is untouched, 2 is double width. Mono strips have no image
// to widen and skip this entirely.
//
// Why this cannot be a send gain
// ------------------------------
// Every other placement control on a bus — pan, balance — is expressed as gains
// on the strip's two sends to the master, so it costs the render thread nothing
// and a knob drag never rewires anything. Width looks like it should be the
// same, because the matrix expands to a pair of cross-fed sends:
//
//     L' = a·L + b·R,  R' = b·L + a·R,  a = (1+w)/2,  b = (1-w)/2
//
// but b goes NEGATIVE above w = 1, and send gains are decibels with no
// polarity. Narrowing would work; widening — the entire point — cannot be
// expressed. So it is a per-sample block in the chain.
//
// The /2 convention, not /sqrt(2)
// -------------------------------
// The symmetric sqrt(2) form is the one usually written down, because it makes
// the M/S transform orthonormal and so preserves power in the M/S domain. That
// matters if you are METERING mid and side, or applying processing that assumes
// both domains have the same scale. Nothing here does either. The /2 form used
// above needs no compensation constant anywhere and reconstructs exactly in
// real arithmetic, which is what is wanted.
//
// It is still not bit-exact in floating point — fl(L+R) and fl(L-R) each round
// — so unity width does not fall out as a no-op on its own. The chain's rule is
// that a block doing nothing must be bit-transparent, so identity is detected
// and the matrix is skipped rather than run at w = 1. See ChannelDsp.
//
// No level compensation
// ---------------------
// Widening raises the level of material that has side content, and this does
// nothing about it. That is deliberate. The obvious fix — scaling the output by
// 1/max(1, w) — is wrong for anything that is not already wide: a mono source
// has S = 0, so widening does not change it at all, yet that compensation would
// still pull it down 6 dB at w = 2. It also scales the mid, which deepens the
// hollowing-out of the phantom centre that wide settings already cause.
//
// Any honest static compensation would have to assume a mid-to-side ratio the
// processor cannot know. So the level moves, the strip meter shows it moving,
// and the fader and the compressor's makeup are there to correct it.
//
// Bass mono
// ---------
// A high-pass on the SIDE signal alone. The mid is untouched, so L + R = 2M is
// preserved exactly at every frequency — the low end collapses to the centre
// and stays mono-compatible however hard the rest is widened. This is the
// standard "elliptical" trick and it is one biquad, rather than the crossover a
// genuine multiband widener would need.
//
// Parked at the bottom of its range means out of circuit, the same convention
// the strip's HPF and LPF already use.
// ============================================================================
#pragma once

#include "liveplay/audio/biquad.hpp"
#include "liveplay/audio/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace liveplay::audio {

// Where the bass-mono filter sits when it is doing nothing. Matches the
// strip HPF's parked position so the two knobs read the same way.
inline constexpr float kBassMonoParkedHz = 20.0f;

// How close to unity the side gain has to be before the block counts as
// identity and drops out. A hundredth of a decibel on the side signal alone is
// far below anything audible, and without a threshold the per-block ramp
// approaches 1.0 asymptotically and never quite arrives.
inline constexpr float kWidthIdentityEps = 1.0e-4f;

struct WidthParams {
    float width       = 1.0f;                 // 0 mono .. 1 unity .. 2 wide
    float bass_mono_hz = kBassMonoParkedHz;   // parked = out of circuit
    float bass_mono_q  = 0.70710678f;
};

// What the render thread reads. No enable flag: identity is width 1 with a
// passthrough side filter, exactly as a parked HPF is identity, and it is
// detected rather than declared.
struct WidthCoeffs {
    float        side_gain = 1.0f;
    BiquadCoeffs side_hpf{};      // passthrough unless bass-mono is in circuit
};

inline WidthCoeffs width_coeffs(const WidthParams& p, double sample_rate) noexcept {
    WidthCoeffs c;
    // Clamped at 0: a negative side gain would swap the stereo image left for
    // right, which is a polarity flip wearing a width control's clothes.
    c.side_gain = std::max(0.0f, p.width);
    c.side_hpf  = p.bass_mono_hz > kBassMonoParkedHz
                      ? biquad_highpass(p.bass_mono_hz, sample_rate, p.bass_mono_q)
                      : biquad_passthrough();
    return c;
}

// True when these coefficients are close enough to identity to be snapped to
// it and the block taken out of circuit.
//
// Compared with a tolerance rather than exactly, because these are the RAMPED
// coefficients: a per-block lerp toward a target approaches it and, for the
// filter's feedback terms, can take far longer to land on it exactly than it
// takes to become inaudible. An exact test would leave a strip that had once
// used bass-mono running the matrix indefinitely.
//
// A high-pass is never near passthrough at any corner frequency — its b1 sits
// around -2 — so no real filter setting can trip this by accident.
inline bool width_near_identity(const WidthCoeffs& c) noexcept {
    const auto near0 = [](float v) { return std::fabs(v) <= kWidthIdentityEps; };
    return std::fabs(c.side_gain - 1.0f) <= kWidthIdentityEps
        && std::fabs(c.side_hpf.b0 - 1.0f) <= kWidthIdentityEps
        && near0(c.side_hpf.b1) && near0(c.side_hpf.b2)
        && near0(c.side_hpf.a1) && near0(c.side_hpf.a2);
}

// Per-strip state: the side signal's filter memory. One signal, so one biquad —
// unlike everything else in the chain, which needs a copy per lane.
class WidthState {
public:
    void reset() noexcept { side_.reset(); }

    // Both lanes, in place. Callers must have checked width_is_identity() and
    // skipped; this always runs the matrix.
    void process(const WidthCoeffs& c, Sample* left, Sample* right,
                 std::size_t frames) noexcept {
        for (std::size_t s = 0; s < frames; ++s) {
            const float l = left[s];
            const float r = right[s];
            const float m = 0.5f * (l + r);
            float       side = 0.5f * (l - r);
            side = side_.process(c.side_hpf, side) * c.side_gain;
            left[s]  = m + side;
            right[s] = m - side;
        }
    }

private:
    BiquadState side_;
};

// ---------------------------------------------------------------------------
// Inter-channel correlation, for the meter.
//
// +1 is a mono-compatible signal, 0 is uncorrelated (wide), and negative means
// the lanes are fighting: material that will partly or wholly vanish the moment
// anything sums the strip to mono. That is the failure mode wide settings
// cause, and this is the cheap 90% of the goniometer that would show it —
// three multiply-accumulates per sample against a display that needs its own
// canvas and a history buffer.
//
// Measured on the strip's OUTPUT, after the width block, because what matters
// is the correlation of what actually leaves. It is a ratio, so the fader
// cannot affect it.
// ---------------------------------------------------------------------------
class CorrelationMeter {
public:
    void reset() noexcept { value_ = 1.0f; }

    void process(const Sample* left, const Sample* right, std::size_t frames) noexcept {
        double lr = 0.0, ll = 0.0, rr = 0.0;
        for (std::size_t s = 0; s < frames; ++s) {
            const double l = left[s], r = right[s];
            lr += l * r; ll += l * l; rr += r * r;
        }
        const double denom = std::sqrt(ll * rr);
        // Silence has no image to describe. Reading 0 there would show every
        // idle strip as though it were wide; +1 is the honest answer, since
        // that is what silence sums to.
        const float target = (denom > 1.0e-12) ? static_cast<float>(lr / denom) : 1.0f;
        // Smoothed over blocks, because the per-block figure on real material
        // jitters far too much to read. ~100 ms at a 256-frame block.
        constexpr float kSmooth = 0.12f;
        value_ += (std::clamp(target, -1.0f, 1.0f) - value_) * kSmooth;
    }

    float value() const noexcept { return value_; }

private:
    float value_ = 1.0f;
};

} // namespace liveplay::audio
