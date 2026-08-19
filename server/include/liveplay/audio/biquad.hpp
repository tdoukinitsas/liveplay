// ============================================================================
// liveplay/audio/biquad.hpp
// ----------------------------------------------------------------------------
// One second-order IIR section, plus the RBJ Audio EQ Cookbook coefficient
// formulas for the shapes this desk needs: high-pass, low-pass, peaking bell,
// low shelf and high shelf.
//
// Everything a channel strip does to tone is built from these: the HPF and LPF
// are one section each, and every EQ band is one section, cascaded.
//
// Real-time contract
// ------------------
//   * process() allocates nothing, locks nothing and branches only on data it
//     already holds. Safe to call from the render thread.
//   * Coefficients are computed on the CONTROL thread and handed over as a
//     whole `Coeffs` struct. A section never computes its own coefficients
//     while rendering — std::sin/std::cos in the audio callback is exactly the
//     kind of thing that turns a dropout into a support ticket.
//
// Two deliberate departures from the notes in _DSP_DOCS
// -----------------------------------------------------
//   1. **Transposed Direct Form II, not Direct Form I.** The notes' snippets
//      use DF-I and then say, correctly, that production code should use
//      TDF-II: half the state, and markedly better behaviour with float
//      coefficients at low frequencies, which is exactly where a 30 Hz HPF at
//      48 kHz lives. There was no reason to ship the form the notes themselves
//      advise against.
//   2. **Denormal protection**, which the notes do not mention and which
//      matters more than anything else in them. An IIR section fed silence
//      decays towards zero and lands in denormal range, where x86 traps into
//      microcode and a filter that cost nothing suddenly costs hundreds of
//      cycles per sample. On a desk with a filter on every bus that is a
//      dropout in the middle of a show. Each section flushes its own state.
// ============================================================================
#pragma once

#include "liveplay/audio/types.hpp"

#include <cmath>
#include <cstddef>

namespace liveplay::audio {

// pi, spelled out rather than taken from <cmath>. M_PI is not standard C++ and
// MSVC does not define it without _USE_MATH_DEFINES — the notes' snippets use
// it and would not compile here. types.hpp already writes it out for the pan
// law; same choice, same reason.
inline constexpr double kPi = 3.14159265358979323846;

// Anything below this is inaudible at any sane monitoring level, and keeping
// state above it is what stops the section from decaying into denormals.
inline constexpr float kDenormalFloor = 1.0e-20f;

inline float flush_denormal(float v) noexcept {
    return (v > -kDenormalFloor && v < kDenormalFloor) ? 0.0f : v;
}

// Normalised coefficients: a0 has already been divided out.
struct BiquadCoeffs {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
};

// A section's per-channel memory. Stereo needs two of these; sharing one
// across channels smears the image, because each channel's history would be
// polluted by the other's.
class BiquadState {
public:
    void reset() noexcept { z1_ = 0.0f; z2_ = 0.0f; }

    // Transposed Direct Form II:
    //     y[n] = b0*x[n] + z1
    //     z1   = b1*x[n] - a1*y[n] + z2
    //     z2   = b2*x[n] - a2*y[n]
    float process(const BiquadCoeffs& c, float x) noexcept {
        const float y = c.b0 * x + z1_;
        z1_ = flush_denormal(c.b1 * x - c.a1 * y + z2_);
        z2_ = flush_denormal(c.b2 * x - c.a2 * y);
        return y;
    }

private:
    float z1_ = 0.0f;
    float z2_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Coefficient design — control thread only.
//
// Every one of these clamps the corner frequency below Nyquist. The cookbook
// formulas divide by tan/sin of omega and blow up as the corner approaches
// fs/2: a 20 kHz low-pass at 44.1 kHz is close enough to matter, and a UI that
// lets the knob reach 20 kHz will get there. The notes do not mention it.
// ---------------------------------------------------------------------------
namespace detail {

// Keep the corner inside the band the maths is valid over. 0.49 * fs leaves a
// little room below Nyquist rather than sitting exactly on it.
inline double clamp_freq(double freq_hz, double sample_rate) noexcept {
    const double max_hz = 0.49 * sample_rate;
    if (freq_hz < 1.0)    return 1.0;
    if (freq_hz > max_hz) return max_hz;
    return freq_hz;
}

inline float clamp_q(float q) noexcept {
    // Below about 0.1 the section is so broad it is doing nothing; above 40 it
    // rings hard enough to be a fault rather than a setting.
    if (q < 0.1f)  return 0.1f;
    if (q > 40.0f) return 40.0f;
    return q;
}

inline BiquadCoeffs normalise(double b0, double b1, double b2,
                              double a0, double a1, double a2) noexcept {
    BiquadCoeffs c;
    c.b0 = static_cast<float>(b0 / a0);
    c.b1 = static_cast<float>(b1 / a0);
    c.b2 = static_cast<float>(b2 / a0);
    c.a1 = static_cast<float>(a1 / a0);
    c.a2 = static_cast<float>(a2 / a0);
    return c;
}

} // namespace detail

// A section that does nothing. Used when a block is bypassed, so the render
// loop runs the same code either way instead of branching per sample.
inline BiquadCoeffs biquad_passthrough() noexcept { return BiquadCoeffs{}; }

inline BiquadCoeffs biquad_highpass(double freq_hz, double sample_rate,
                                    float q = 0.70710678f) noexcept {
    const double w0    = 2.0 * kPi * detail::clamp_freq(freq_hz, sample_rate) / sample_rate;
    const double cosw  = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * detail::clamp_q(q));
    return detail::normalise(
        (1.0 + cosw) / 2.0, -(1.0 + cosw), (1.0 + cosw) / 2.0,
        1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
}

inline BiquadCoeffs biquad_lowpass(double freq_hz, double sample_rate,
                                   float q = 0.70710678f) noexcept {
    const double w0    = 2.0 * kPi * detail::clamp_freq(freq_hz, sample_rate) / sample_rate;
    const double cosw  = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * detail::clamp_q(q));
    return detail::normalise(
        (1.0 - cosw) / 2.0, 1.0 - cosw, (1.0 - cosw) / 2.0,
        1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
}

inline BiquadCoeffs biquad_peaking(double freq_hz, double sample_rate,
                                   float gain_db, float q) noexcept {
    const double A     = std::pow(10.0, gain_db / 40.0);
    const double w0    = 2.0 * kPi * detail::clamp_freq(freq_hz, sample_rate) / sample_rate;
    const double cosw  = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * detail::clamp_q(q));
    return detail::normalise(
        1.0 + alpha * A, -2.0 * cosw, 1.0 - alpha * A,
        1.0 + alpha / A, -2.0 * cosw, 1.0 - alpha / A);
}

inline BiquadCoeffs biquad_lowshelf(double freq_hz, double sample_rate,
                                    float gain_db, float slope = 1.0f) noexcept {
    const double A    = std::pow(10.0, gain_db / 40.0);
    const double w0   = 2.0 * kPi * detail::clamp_freq(freq_hz, sample_rate) / sample_rate;
    const double cosw = std::cos(w0);
    const double s    = (slope < 0.1f) ? 0.1 : (slope > 2.0f ? 2.0 : slope);
    const double alpha = std::sin(w0) / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / s - 1.0) + 2.0);
    const double tsa   = 2.0 * std::sqrt(A) * alpha;
    return detail::normalise(
        A * ((A + 1.0) - (A - 1.0) * cosw + tsa),
        2.0 * A * ((A - 1.0) - (A + 1.0) * cosw),
        A * ((A + 1.0) - (A - 1.0) * cosw - tsa),
        (A + 1.0) + (A - 1.0) * cosw + tsa,
        -2.0 * ((A - 1.0) + (A + 1.0) * cosw),
        (A + 1.0) + (A - 1.0) * cosw - tsa);
}

inline BiquadCoeffs biquad_highshelf(double freq_hz, double sample_rate,
                                     float gain_db, float slope = 1.0f) noexcept {
    const double A    = std::pow(10.0, gain_db / 40.0);
    const double w0   = 2.0 * kPi * detail::clamp_freq(freq_hz, sample_rate) / sample_rate;
    const double cosw = std::cos(w0);
    const double s    = (slope < 0.1f) ? 0.1 : (slope > 2.0f ? 2.0 : slope);
    const double alpha = std::sin(w0) / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / s - 1.0) + 2.0);
    const double tsa   = 2.0 * std::sqrt(A) * alpha;
    return detail::normalise(
        A * ((A + 1.0) + (A - 1.0) * cosw + tsa),
        -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw),
        A * ((A + 1.0) + (A - 1.0) * cosw - tsa),
        (A + 1.0) - (A - 1.0) * cosw + tsa,
        2.0 * ((A - 1.0) - (A + 1.0) * cosw),
        (A + 1.0) - (A - 1.0) * cosw - tsa);
}

// ---------------------------------------------------------------------------
// Magnitude response at one frequency, for tests and for drawing the EQ curve.
// Evaluates |H(e^jw)| directly from the coefficients, so it describes the
// filter that is actually running rather than a second model of it that could
// drift away from it.
// ---------------------------------------------------------------------------
inline double biquad_magnitude_db(const BiquadCoeffs& c,
                                  double freq_hz, double sample_rate) noexcept {
    const double w  = 2.0 * kPi * freq_hz / sample_rate;
    const double cw = std::cos(w), sw = std::sin(w);
    const double c2w = std::cos(2.0 * w), s2w = std::sin(2.0 * w);
    const double num_re = c.b0 + c.b1 * cw + c.b2 * c2w;
    const double num_im = -(c.b1 * sw + c.b2 * s2w);
    const double den_re = 1.0 + c.a1 * cw + c.a2 * c2w;
    const double den_im = -(c.a1 * sw + c.a2 * s2w);
    const double num = std::sqrt(num_re * num_re + num_im * num_im);
    const double den = std::sqrt(den_re * den_re + den_im * den_im);
    if (den <= 0.0) return -200.0;
    const double mag = num / den;
    return mag <= 0.0 ? -200.0 : 20.0 * std::log10(mag);
}

} // namespace liveplay::audio
