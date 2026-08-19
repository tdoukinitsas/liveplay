// ============================================================================
// liveplay/audio/channel_dsp.hpp
// ----------------------------------------------------------------------------
// The channel strip's fixed processing chain, in console order:
//
//     HPF -> LPF -> EQ (4 bands) -> gate -> compressor -> width
//
// Fixed, not a plugin rack. These are known blocks that every strip has,
// exactly as a console channel does, so they are laid out as fields rather
// than dispatched through an insert interface. User-orderable plugins arrive
// later as a separate list alongside this — building a virtual-dispatch
// framework for six blocks that are always present and always in this order
// would be scaffolding around a thing that does not move.
//
// Width sits LAST, after the dynamics, which is where a mastering widener
// goes: the compressor and gate then key off the source image, so moving the
// width control never changes how hard the compressor is working. The cost is
// that this strip's own compressor cannot catch a peak the widening creates —
// the strip meter shows it and the master limiter catches it instead.
//
// Where it runs
// -------------
// On the bus accumulators, after the items have summed into them and BEFORE
// the fader, the PFL tap and the mute. So:
//
//     items -> [this] -> PFL tap -> fader/mute -> pan send -> master
//
// It therefore runs even while a bus is muted. That is required, because PFL
// is pre-mute and post-processing, and it is also what stops the compressor's
// envelope from lurching when a bus is unmuted mid-show.
//
// Threading
// ---------
// Parameters are set on the CONTROL thread, which turns them into filter
// coefficients (std::sin and std::cos have no business in an audio callback)
// and publishes them into one of two slots, flipping an atomic index. The
// render thread reads that index once per block. No locks, no allocation, and
// a reader can never see half of a coefficient set.
//
// Coefficients are then ramped toward the published target over a few blocks
// rather than snapped to it. Jumping straight to new coefficients is the
// "audio pops when a user drags a slider" problem the notes in _DSP_DOCS warn
// about; ramping per block (not per sample) costs five lerps per section and
// removes it.
// ============================================================================
#pragma once

#include "liveplay/audio/biquad.hpp"
#include "liveplay/audio/dynamics.hpp"
#include "liveplay/audio/stereo_width.hpp"
#include "liveplay/audio/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace liveplay::audio {

// How many EQ bands a strip has. Matches the four the channel view lays out
// (LF / LMF / HMF / HF).
inline constexpr std::size_t kEqBands = 4;

// Every filter section on one strip, in chain order. One set of coefficients
// is shared by both lanes; the per-lane STATE is what must stay separate.
struct StripCoeffs {
    BiquadCoeffs hpf;
    BiquadCoeffs lpf;
    std::array<BiquadCoeffs, kEqBands> eq;
    // The dynamics ride in the same published slot as the filter coefficients,
    // so there is one handover to the render thread rather than several that
    // could land a block apart. Not ramped: a threshold or a ratio is meant to
    // take effect when you set it, and each processor's own attack and release
    // are already the smoothing that matters here.
    GateCoeffs       gate;
    CompressorCoeffs comp;
    // Width IS ramped, unlike the dynamics beside it: its side gain is a gain,
    // and stepping a gain mid-drag is the click the ramp exists to remove. Its
    // filter ramps for the same reason the tone sections' do.
    WidthCoeffs      width;
};

// What the operator set. Plain values, owned by the control thread.
struct FilterParams {
    bool  enabled = false;
    float freq_hz = 80.0f;
    float q       = 0.70710678f;
};

// One EQ band. A bell unless `shelf` is set, in which case `low_shelf` picks
// which end it turns up.
//
// Q and slope are SEPARATE fields rather than one number read two ways. They
// are different quantities — Q is the width of a bell, slope is the steepness
// of a shelf's transition, and their useful ranges barely overlap — so sharing
// a field would mean switching a band to shelf and back silently changed the
// bell's width. Same argument as the section bypasses: flipping a switch has
// to put things back exactly as they were.
struct EqBandParams {
    bool  enabled   = false;
    bool  shelf     = false;
    bool  low_shelf = false;   // ignored unless shelf
    float freq_hz   = 1000.0f;
    float gain_db   = 0.0f;
    float q         = 1.0f;
    float slope     = 1.0f;
};

struct StripDspParams {
    GateParams       gate;
    CompressorParams comp;
    WidthParams      width;
    FilterParams hpf{false, 80.0f,    0.70710678f};
    FilterParams lpf{false, 18000.0f, 0.70710678f};
    std::array<EqBandParams, kEqBands> eq{{
        {false, false, true,  100.0f,   0.0f, 0.7f, 1.0f},
        {false, false, false, 500.0f,   0.0f, 1.0f, 1.0f},
        {false, false, false, 2500.0f,  0.0f, 1.0f, 1.0f},
        {false, false, false, 10000.0f, 0.0f, 0.7f, 1.0f},
    }};
};

class ChannelDsp {
public:
    void configure(SampleRate sample_rate) noexcept {
        sample_rate_ = sample_rate;
        // Publish a flat chain so the very first block has something coherent
        // to read even if no parameters are ever set.
        set_params(StripDspParams{});
        // ...and land on it immediately rather than ramping up from nothing.
        active_ = slot(published_.load(std::memory_order_acquire));
        for (auto& lane : state_) lane.reset();
    }

    // ---- Control thread ---------------------------------------------------
    // Turn parameters into coefficients and publish them. A disabled block
    // becomes a passthrough section rather than being branched around, so the
    // render loop runs the same straight line either way.
    void set_params(const StripDspParams& p) {
        const double fs = static_cast<double>(sample_rate_);
        StripCoeffs c;
        c.hpf = p.hpf.enabled ? biquad_highpass(p.hpf.freq_hz, fs, p.hpf.q)
                              : biquad_passthrough();
        c.lpf = p.lpf.enabled ? biquad_lowpass(p.lpf.freq_hz, fs, p.lpf.q)
                              : biquad_passthrough();
        for (std::size_t i = 0; i < kEqBands; ++i) {
            const auto& b = p.eq[i];
            // A band sitting at 0 dB is a no-op; make it literally one so four
            // flat bands in circuit cannot colour the desk. That holds for a
            // shelf as much as a bell — both are identities at unity gain.
            if (!b.enabled || b.gain_db == 0.0f) {
                c.eq[i] = biquad_passthrough();
            } else if (b.shelf) {
                c.eq[i] = b.low_shelf
                              ? biquad_lowshelf(b.freq_hz, fs, b.gain_db, b.slope)
                              : biquad_highshelf(b.freq_hz, fs, b.gain_db, b.slope);
            } else {
                c.eq[i] = biquad_peaking(b.freq_hz, fs, b.gain_db, b.q);
            }
        }
        c.gate  = gate_coeffs(p.gate, fs);
        c.comp  = compressor_coeffs(p.comp, fs);
        c.width = width_coeffs(p.width, fs);
        publish(c);
        any_active_.store(needs_processing(p), std::memory_order_release);
    }

    // Whether the render loop needs to run this chain for the coming block.
    //
    // True while anything is doing something — and for a short tail after it
    // stops, so the coefficients can ramp out. Without that tail, flattening
    // the last band would drop the chain out of circuit in a single sample and
    // a +12 dB boost would end in a click, which is the very thing the ramp
    // exists to avoid. Render thread only: it owns the settle counter.
    bool needs_processing() noexcept {
        const std::uint32_t gen = generation_.load(std::memory_order_acquire);
        if (gen != seen_generation_) {
            seen_generation_ = gen;
            settle_blocks_   = kSettleBlocks;
        }
        if (any_active_.load(std::memory_order_acquire)) {
            settle_blocks_ = kSettleBlocks;   // stays hot while it is in use
            return true;
        }
        if (settle_blocks_ > 0) { --settle_blocks_; return true; }
        return false;
    }

    // ---- Render thread ----------------------------------------------------
    // Advance the coefficient ramp by one block. Call once per block, before
    // process_block, so both lanes are filtered by the same coefficients.
    void advance_coeffs() noexcept {
        const StripCoeffs& target = slot(published_.load(std::memory_order_acquire));
        // ~20 ms to travel, at any block size, so a knob drag glides instead
        // of stepping. Small enough that nobody hears lag on it.
        constexpr float kRamp = 0.25f;
        lerp(active_.hpf, target.hpf, kRamp);
        lerp(active_.lpf, target.lpf, kRamp);
        for (std::size_t i = 0; i < kEqBands; ++i) lerp(active_.eq[i], target.eq[i], kRamp);
        // Taken whole, not ramped. These are thresholds and time constants
        // rather than filter coefficients: interpolating them would mean a
        // threshold crawling to where it was set, and the gate's own attack
        // and release already provide the smoothing that matters.
        //
        // They also have to be copied at all, which is the point. Leaving the
        // gate out meant the render thread kept reading a default-constructed
        // one — permanently disabled — while every parameter change published
        // correctly into a slot nothing ever looked at.
        active_.gate = target.gate;
        active_.comp = target.comp;
        // Ramped, not taken whole. A width knob drag is a gain drag, and
        // stepping a gain is the click the ramp exists to remove.
        active_.width.side_gain +=
            (target.width.side_gain - active_.width.side_gain) * kRamp;
        lerp(active_.width.side_hpf, target.width.side_hpf, kRamp);
        // Once the ramp is inaudibly close to identity, snap onto it exactly so
        // the block drops out of circuit. Without this a strip that had ever
        // been widened would run the matrix forever at 0.9999 and never be
        // bit-transparent again. Dropping the side filter's memory with it,
        // because the tail of a filter that is no longer in circuit is not
        // something to reintroduce the next time it is.
        width_active_ = !width_near_identity(active_.width);
        if (!width_active_) {
            active_.width = WidthCoeffs{};
            width_.reset();
        }
    }

    // Filter one lane's block in place. `lane` selects which lane's filter
    // memory to use — sharing history across lanes would smear the image.
    //
    // Tone only. The dynamics run across every lane at once (see process), so
    // they cannot be folded into a per-lane call.
    void process_tone(ChannelIndex lane, Sample* buf, std::size_t frames) noexcept {
        if (lane >= kMixerLanes) return;
        auto& st = state_[lane];
        for (std::size_t s = 0; s < frames; ++s) {
            float x = buf[s];
            x = st.hpf.process(active_.hpf, x);
            x = st.lpf.process(active_.lpf, x);
            for (std::size_t b = 0; b < kEqBands; ++b) x = st.eq[b].process(active_.eq[b], x);
            buf[s] = x;
        }
    }

    // The whole chain, for one block: tone per lane, then dynamics across all
    // of them together.
    //
    // The dynamics have to see every lane at once, because both detectors are
    // linked — one gain for the strip, so neither processor can pull the stereo
    // image sideways. That is why this takes the lanes as a set rather than
    // being called once per lane like the filters.
    //
    // Gate before compressor, which is the console order and the useful one:
    // the gate cleans up what is below the floor first, so the compressor is
    // not asked to work on noise the gate is about to remove anyway. Reversed,
    // a compressor's makeup gain lifts that noise up over the gate's threshold
    // and holds it open.
    void process(Sample* const* lanes, ChannelCount count, std::size_t frames) noexcept {
        advance_coeffs();
        const auto lc = std::min<ChannelCount>(count, kMixerLanes);
        for (ChannelCount l = 0; l < lc; ++l) {
            process_tone(l, lanes[l], frames);
        }
        gate_.process(active_.gate, lanes, lc, frames);
        comp_.process(active_.comp, lanes, lc, frames);
        // Last, and only on a strip that actually has an image. A mono strip
        // carries nothing on lane 1, so a matrix across the pair would read
        // silence as the right channel and hard-pan the strip left.
        if (width_active_ && lc >= kMixerLanes) {
            width_.process(active_.width, lanes[0], lanes[1], frames);
        }
    }

    // How far each processor is pulling the strip down, for their meters.
    float gate_reduction_db() const noexcept { return gate_.gain_reduction_db(); }
    float comp_reduction_db() const noexcept { return comp_.gain_reduction_db(); }

    // Drop every section's memory. For when a strip's signal source changes
    // underneath it and the old tail is no longer meaningful.
    void reset() noexcept {
        for (auto& lane : state_) lane.reset();
        gate_.reset();
        comp_.reset();
        width_.reset();
    }

private:
    struct LaneState {
        BiquadState hpf;
        BiquadState lpf;
        std::array<BiquadState, kEqBands> eq;
        void reset() noexcept {
            hpf.reset();
            lpf.reset();
            for (auto& b : eq) b.reset();
        }
    };

    static bool needs_processing(const StripDspParams& p) noexcept {
        if (p.hpf.enabled || p.lpf.enabled || p.gate.enabled || p.comp.enabled) return true;
        // Asked of the parameters rather than the coefficients, like every
        // other block here — the coefficient form needs a sample rate this has
        // no reason to know.
        if (p.width.width != 1.0f || p.width.bass_mono_hz > kBassMonoParkedHz) return true;
        for (const auto& b : p.eq) if (b.enabled && b.gain_db != 0.0f) return true;
        return false;
    }

    static void lerp(BiquadCoeffs& cur, const BiquadCoeffs& to, float t) noexcept {
        cur.b0 += (to.b0 - cur.b0) * t;
        cur.b1 += (to.b1 - cur.b1) * t;
        cur.b2 += (to.b2 - cur.b2) * t;
        cur.a1 += (to.a1 - cur.a1) * t;
        cur.a2 += (to.a2 - cur.a2) * t;
    }

    void publish(const StripCoeffs& c) {
        // Write the slot the render thread is not reading, then flip. The
        // store is release-ordered so the coefficients are visible before the
        // index that points at them.
        const unsigned next = 1u - published_.load(std::memory_order_relaxed);
        slots_[next] = c;
        published_.store(next, std::memory_order_release);
        // Tells the render thread something moved, so it keeps running the
        // chain long enough to ramp onto the new coefficients even if the new
        // ones are flat.
        generation_.fetch_add(1, std::memory_order_release);
    }

    // Long enough for the 0.25-per-block ramp to converge (0.75^64 is far
    // below anything audible) with room to spare. At 256 frames and 48 kHz
    // this is about a third of a second of tail on a strip that has just gone
    // flat — irrelevant next to the cost of the strips that are still working.
    static constexpr int kSettleBlocks = 64;
    const StripCoeffs& slot(unsigned i) const noexcept { return slots_[i & 1u]; }

    SampleRate  sample_rate_ = kDefaultMixSampleRate;
    StripCoeffs slots_[2]{};
    std::atomic<unsigned>      published_{0};
    std::atomic<bool>          any_active_{false};
    std::atomic<std::uint32_t> generation_{0};

    StripCoeffs                       active_{};          // render thread only
    std::array<LaneState, kMixerLanes> state_{};          // render thread only
    GateState                         gate_{};            // render thread only
    CompressorState                   comp_{};            // render thread only
    WidthState                        width_{};           // render thread only
    bool                              width_active_{false}; // render thread only
    std::uint32_t                     seen_generation_{0}; // render thread only
    int                               settle_blocks_{0};   // render thread only
};

} // namespace liveplay::audio
