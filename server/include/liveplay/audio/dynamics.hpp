// ============================================================================
// liveplay/audio/dynamics.hpp
// ----------------------------------------------------------------------------
// The channel strip's two dynamics processors: a downward expander / gate and
// a compressor / limiter, in that order.
//
// They share a shape — linked peak detector, static curve in decibels, one-pole
// gain smoothing — and differ in the one place it matters: the gate attenuates
// BELOW its threshold and the compressor attenuates ABOVE it, so attack and
// release mean opposite things on the two. That is spelled out at each of them.
//
// Real-time contract, as everywhere else in the chain: process() allocates
// nothing, locks nothing, and every coefficient it needs was computed on the
// control thread.
//
// Four departures from the notes in _DSP_DOCS/expander_gate.md, three of them
// corrections rather than additions.
//
// 1. THE EXPANSION FORMULA IN THE NOTES IS THE WRONG ONE. They compute
//    `(threshold - level) * (1 - 1/ratio)`, which is the COMPRESSION law: at
//    ratio 4 it attenuates by 0.75 dB per dB below the threshold, so quiet
//    material is pulled UP towards the threshold relative to where expansion
//    would put it. A downward expander has to attenuate by (ratio - 1) dB per
//    dB below — 3 dB per dB at ratio 4 — which is what makes it the mirror of
//    a compressor above the threshold. With the notes' formula a "gate" at
//    ratio 20 would only ever reach 0.95 dB of attenuation per dB, and would
//    never actually gate anything.
//
// 2. HYSTERESIS, which the notes do not mention. A single threshold makes a
//    gate chatter: material sitting near it crosses back and forth every few
//    milliseconds and the gate stutters audibly. Real units open at the
//    threshold and close a few dB below it, so the level has to genuinely fall
//    away before the gate lets go.
//
// 3. HOLD, which the notes' parameter list omits and the surface already
//    offers. Without it a gate closes on the gaps inside a decaying sound —
//    speech pauses, a reverb tail — and chops it up.
//
// 4. STEREO LINKING, which the notes do not mention at all and which matters
//    more than any of the above. The detector runs on both lanes together, so
//    the two are always attenuated by the same amount. Gating each lane on its
//    own level makes the image lurch sideways every time one side happens to
//    be louder, which on a stereo bus is constantly.
//
// Attack and release also mean the opposite of what they mean on a compressor
// and the notes do not say so: here attack is how fast the gate OPENS (gain
// rising) and release is how fast it CLOSES. Getting that backwards produces
// a processor that is technically working and useless.
// ============================================================================
#pragma once

#include "liveplay/audio/biquad.hpp"
#include "liveplay/audio/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace liveplay::audio {

// How far below the open threshold the level must fall before the gate closes.
// Three decibels is the usual figure on hardware and is enough to stop
// chatter without the gate feeling sticky.
inline constexpr float kGateHysteresisDb = 3.0f;

// The side-chain's own release. Separate from the user's release, which
// controls the GAIN; this controls how fast the detector forgets a peak, and
// wants to be quick enough to track a signal without rippling on it.
inline constexpr float kGateDetectorReleaseMs = 8.0f;

// Quietest level the detector will report. Low enough to be silence, high
// enough to keep the logarithm and the expansion arithmetic finite.
inline constexpr float kDetectorFloorDb = -120.0f;

struct GateParams {
    bool  enabled      = false;
    float threshold_db = -40.0f;
    float ratio        = 2.0f;     // 1 = off, large = hard gate
    float range_db     = -20.0f;   // deepest attenuation; 0 = no gating at all
    float attack_ms    = 1.0f;     // how fast it opens
    float hold_ms      = 10.0f;
    float release_ms   = 100.0f;   // how fast it closes
};

// What the render thread actually needs: everything already reduced to
// coefficients and comparisons.
struct GateCoeffs {
    bool      enabled       = false;
    float     open_db       = -40.0f;
    float     close_db      = -43.0f;
    float     slope         = 1.0f;   // ratio - 1
    float     range_db      = -20.0f;
    float     attack_coeff  = 0.0f;
    float     release_coeff = 0.0f;
    float     det_coeff     = 0.0f;
    long long hold_samples  = 0;
};

// One-pole smoothing coefficient for a time constant in milliseconds. The
// convention is the usual one: the coefficient is how much of the OLD value
// survives each sample, so 0 is instant and just under 1 is very slow.
inline float time_constant_coeff(float ms, double sample_rate) noexcept {
    if (ms <= 0.0f) return 0.0f;
    const double samples = (static_cast<double>(ms) * 0.001) * sample_rate;
    if (samples <= 0.0) return 0.0f;
    return static_cast<float>(std::exp(-1.0 / samples));
}

// Decibels to a linear multiplier, for the gain a dynamics processor applies.
//
// Written as exp rather than pow(10, db/20) because this runs once per sample
// per strip whenever either processor is working — the gate escapes it while it
// is open, but a compressor that is compressing cannot — and exp is the
// cheaper of the two by a useful margin for an identical result.
inline float db_to_gain(float db) noexcept {
    constexpr float kLn10Over20 = 0.1151292546497f;   // ln(10) / 20
    return std::exp(db * kLn10Over20);
}

inline GateCoeffs gate_coeffs(const GateParams& p, double sample_rate) noexcept {
    GateCoeffs c;
    c.enabled = p.enabled;
    c.open_db = p.threshold_db;
    c.close_db = p.threshold_db - kGateHysteresisDb;
    // Clamped at 1: a ratio below 1 would expand upward, which is not what
    // this control means and would make quiet material louder without warning.
    c.slope    = std::max(1.0f, p.ratio) - 1.0f;
    // Range is an attenuation, so it is negative. A positive value would be a
    // boost when the gate closes, which is never what is wanted.
    c.range_db = std::min(0.0f, p.range_db);
    c.attack_coeff  = time_constant_coeff(p.attack_ms,  sample_rate);
    c.release_coeff = time_constant_coeff(p.release_ms, sample_rate);
    c.det_coeff     = time_constant_coeff(kGateDetectorReleaseMs, sample_rate);
    c.hold_samples  = static_cast<long long>(
        std::max(0.0f, p.hold_ms) * 0.001f * static_cast<float>(sample_rate));
    return c;
}

// Per-strip state. One instance drives every lane, which is what links them.
class GateState {
public:
    void reset() noexcept {
        detector_ = 0.0f;
        gain_db_  = 0.0f;
        hold_left_ = 0;
        open_ = false;
        worst_db_ = 0.0f;
    }

    // Process every lane in step. `lanes` holds `count` pointers to blocks of
    // `frames` samples; they are filtered in place.
    void process(const GateCoeffs& c, Sample* const* lanes,
                 ChannelCount count, std::size_t frames) noexcept {
        if (!c.enabled || count == 0) {
            // Clear the meter rather than leaving it holding the last reading
            // from before the gate was switched out.
            worst_db_ = 0.0f;
            return;
        }
        float worst = 0.0f;

        for (std::size_t s = 0; s < frames; ++s) {
            // ---- Linked detection ----
            // The loudest lane drives the gate, so both are attenuated
            // identically and the stereo image cannot move.
            float peak = 0.0f;
            for (ChannelCount l = 0; l < count; ++l) {
                peak = std::max(peak, std::fabs(lanes[l][s]));
            }
            // Peak follower: instant attack, exponential release. Instant
            // attack matters — a gate that has to charge before it notices a
            // transient clips the front off every one of them.
            detector_ = (peak > detector_) ? peak
                                           : flush_denormal(detector_ * c.det_coeff);

            const float level_db =
                detector_ > 0.0f
                    ? std::max(kDetectorFloorDb, 20.0f * std::log10(detector_))
                    : kDetectorFloorDb;

            // ---- Open / closed, with hysteresis and hold ----
            // Once open the gate judges itself against the LOWER threshold, so
            // the level has to fall away properly before it lets go.
            if (level_db >= c.open_db) {
                open_      = true;
                hold_left_ = c.hold_samples;
            } else if (open_ && level_db < c.close_db) {
                if (hold_left_ > 0) --hold_left_;
                else                open_ = false;
            }

            // ---- Static curve ----
            // Above the working threshold the gate is transparent. Below it,
            // every decibel down costs (ratio - 1) more, until the range floor.
            // While open the gate is transparent, including through the
            // hysteresis window and the hold — that is what those are for. The
            // state machine above has already dropped `open_` by the time the
            // hold runs out, so there is exactly one closed case to handle.
            float target_db = 0.0f;
            if (!open_ && level_db < c.open_db) {
                target_db = std::max(c.range_db, -c.slope * (c.open_db - level_db));
            }

            // ---- Gain smoothing ----
            // Attack when the gain is RISING (the gate opening), release when
            // it is falling. This is the opposite way round from a compressor,
            // and getting it backwards yields a processor that runs but is
            // useless: it would snap shut and crawl open.
            const float coeff = (target_db > gain_db_) ? c.attack_coeff : c.release_coeff;
            gain_db_ = flush_denormal(target_db + (gain_db_ - target_db) * coeff);

            const float g = (gain_db_ >= -0.0001f) ? 1.0f : db_to_gain(gain_db_);
            for (ChannelCount l = 0; l < count; ++l) lanes[l][s] *= g;

            worst = std::min(worst, gain_db_);
        }
        worst_db_ = worst;
    }

    // How far the gate pulled the signal down over the last block, for the
    // meter. Zero means it is passing everything.
    float gain_reduction_db() const noexcept { return worst_db_; }
    bool  is_open() const noexcept { return open_; }

private:
    float     detector_  = 0.0f;
    float     gain_db_   = 0.0f;
    long long hold_left_ = 0;
    bool      open_      = false;
    float     worst_db_  = 0.0f;
};

// ============================================================================
// Compressor / limiter
// ----------------------------------------------------------------------------
// The gate's mirror image: it attenuates ABOVE the threshold instead of below
// it, so `attack` is the gain coming DOWN and `release` is it recovering —
// exactly the opposite of the gate a few lines up, and the notes in
// _DSP_DOCS/compressor_limiter.md never say which way round either processor
// goes. The two sit in the same panel with identically-named knobs, so that is
// worth being explicit about.
//
// Departures from the notes:
//
// 1. SOFT KNEE, which the notes omit entirely and which is the single biggest
//    difference between a compressor that sounds clinical and one that sounds
//    musical. A hard knee applies nothing at all a hair under the threshold and
//    the full ratio a hair over, so material sitting around the threshold —
//    which is most of it, since that is where you set it — is alternately
//    untouched and squashed. The knee spreads that transition over a few
//    decibels so the ratio arrives gradually.
//
// 2. THE NOTES HARD-CODE COEFFICIENTS (`attack_coefficent = 0.01`) rather than
//    deriving them from a time. That number means nothing without a sample
//    rate: the same constant is 0.2 ms at 48 kHz and 0.1 ms at 96. The
//    parameters are in milliseconds and the coefficients come from
//    time_constant_coeff(), so a 10 ms attack is 10 ms at any rate.
//
// 3. PEAK VERSUS RMS, which the notes raise ("peak or RMS") and never decide.
//    One control set has to cover both a bus compressor and a limiter here, so
//    this detects peak and lets the ratio decide the character: at 4:1 the
//    attack and release do the averaging and it behaves like a bus compressor,
//    at 60:1 with a fast attack it catches peaks. An RMS detector could not be
//    made to do the second job at all.
//
// 4. MAKEUP GAIN, in the notes' parameter list and absent from their snippet.
//
// 5. STEREO LINKING, again unmentioned, and it matters more here than on the
//    gate: a compressor works continuously rather than at the edges, so keying
//    the lanes separately pumps the stereo image on every loud note.
//
// What this deliberately is NOT: a true brickwall limiter. That needs lookahead
// — delaying the audio so the gain is already down when the peak arrives — and
// lookahead means latency, which the mixer does not yet compensate for. At high
// ratios this is a fast compressor, so a transient shorter than the attack gets
// through. The master bus limiter is a separate thing and unaffected.
// ============================================================================

// Ceiling on the side-chain's own release. As on the gate this controls how
// fast the detector forgets a peak, not how fast the GAIN recovers.
//
// It is a ceiling rather than a constant because the two ends of the release
// range want different things. Long releases want the detector slow enough not
// to ripple on bass — a 50 Hz cycle is 20 ms, and a detector that decays
// substantially between peaks modulates the gain at the signal's own frequency,
// which is distortion. Short releases are set short precisely to hear the
// compressor move, and a fixed 15 ms floor would quietly cancel that. So the
// detector follows the release setting until it reaches a speed that would
// start rippling, and stops there.
inline constexpr float kCompDetectorReleaseMs = 15.0f;

struct CompressorParams {
    bool  enabled      = false;
    float threshold_db = -18.0f;
    float ratio        = 4.0f;
    float knee_db      = 6.0f;     // total width, centred on the threshold
    float attack_ms    = 10.0f;    // how fast it clamps down
    float release_ms   = 200.0f;   // how fast it recovers
    float makeup_db    = 0.0f;
};

struct CompressorCoeffs {
    bool  enabled       = false;
    float threshold_db  = -18.0f;
    float knee_db       = 6.0f;
    float half_knee_db  = 3.0f;
    float knee_denom    = 12.0f;    // 2W, precomputed; unused when W is 0
    float slope         = 0.75f;    // 1 - 1/ratio
    float attack_coeff  = 0.0f;
    float release_coeff = 0.0f;
    float det_coeff     = 0.0f;
    float makeup_lin    = 1.0f;
};

inline CompressorCoeffs compressor_coeffs(const CompressorParams& p,
                                          double sample_rate) noexcept {
    CompressorCoeffs c;
    c.enabled      = p.enabled;
    c.threshold_db = p.threshold_db;
    // Clamped at 1: below it the "compressor" would expand upward, making loud
    // material louder, which is not what this control means.
    const float ratio = std::max(1.0f, p.ratio);
    c.slope        = 1.0f - 1.0f / ratio;
    c.knee_db      = std::max(0.0f, p.knee_db);
    c.half_knee_db = c.knee_db * 0.5f;
    c.knee_denom   = 2.0f * c.knee_db;
    c.attack_coeff  = time_constant_coeff(p.attack_ms,  sample_rate);
    c.release_coeff = time_constant_coeff(p.release_ms, sample_rate);
    c.det_coeff     = time_constant_coeff(
        std::min(kCompDetectorReleaseMs, std::max(0.0f, p.release_ms)), sample_rate);
    c.makeup_lin    = db_to_gain(p.makeup_db);
    return c;
}

// Per-strip state. As with the gate, one instance drives every lane, which is
// what links them.
class CompressorState {
public:
    void reset() noexcept {
        detector_ = 0.0f;
        gain_db_  = 0.0f;
        worst_db_ = 0.0f;
    }

    void process(const CompressorCoeffs& c, Sample* const* lanes,
                 ChannelCount count, std::size_t frames) noexcept {
        if (!c.enabled || count == 0) {
            worst_db_ = 0.0f;
            return;
        }
        float worst = 0.0f;

        for (std::size_t s = 0; s < frames; ++s) {
            // ---- Linked detection ----
            float peak = 0.0f;
            for (ChannelCount l = 0; l < count; ++l) {
                peak = std::max(peak, std::fabs(lanes[l][s]));
            }
            detector_ = (peak > detector_) ? peak
                                           : flush_denormal(detector_ * c.det_coeff);

            const float level_db =
                detector_ > 0.0f
                    ? std::max(kDetectorFloorDb, 20.0f * std::log10(detector_))
                    : kDetectorFloorDb;

            // ---- Static curve, with a soft knee ----
            // Three regions, and the knee is the middle one. Its quadratic is
            // chosen so the curve is continuous in both value and slope at the
            // joins: it leaves the lower edge flat and arrives at the upper
            // edge already at the full ratio, rather than kinking onto it.
            const float over = level_db - c.threshold_db;
            float target_db = 0.0f;
            if (c.knee_db > 0.0f && over > -c.half_knee_db && over < c.half_knee_db) {
                const float k = over + c.half_knee_db;
                target_db = -c.slope * k * k / c.knee_denom;
            } else if (over > 0.0f) {
                target_db = -c.slope * over;
            }

            // ---- Gain smoothing ----
            // Attack when the gain is FALLING (clamping down on something
            // loud), release when it is recovering. The gate above is the other
            // way round; swapping these gives a processor that runs, meters,
            // and does the wrong thing to every transient.
            const float coeff = (target_db < gain_db_) ? c.attack_coeff : c.release_coeff;
            gain_db_ = flush_denormal(target_db + (gain_db_ - target_db) * coeff);

            // Makeup rides on the same multiply. It is deliberately outside the
            // gain reduction reported below: the meter answers "how hard is it
            // working", and folding a +12 dB makeup into that would show a
            // compressor doing nothing as though it were pushing 12 dB up.
            const float g = ((gain_db_ >= -0.0001f) ? 1.0f : db_to_gain(gain_db_))
                            * c.makeup_lin;
            for (ChannelCount l = 0; l < count; ++l) lanes[l][s] *= g;

            worst = std::min(worst, gain_db_);
        }
        worst_db_ = worst;
    }

    // How far the compressor pulled the signal down over the last block, for
    // the meter. Zero means it is passing everything.
    float gain_reduction_db() const noexcept { return worst_db_; }

private:
    float detector_ = 0.0f;
    float gain_db_  = 0.0f;
    float worst_db_ = 0.0f;
};

} // namespace liveplay::audio
