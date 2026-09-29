// ============================================================================
// liveplay/audio/channel_dsp.hpp
// ----------------------------------------------------------------------------
// The channel strip's fixed processing chain, in console order:
//
//     HPF -> LPF -> EQ (up to kEqBands bands) -> gate -> compressor -> width
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

#include "liveplay/audio/analyser_tap.hpp"
#include "liveplay/audio/biquad.hpp"
#include "liveplay/audio/dynamics.hpp"
#include "liveplay/audio/stereo_width.hpp"
#include "liveplay/audio/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace liveplay::audio {

// How many EQ bands a strip CAN have — capacity, preallocated in every array
// below so nothing is ever allocated on the render thread. A project starts
// with the classic four (LF / LMF / HMF / HF) and may add up to this many.
//
// Capacity costs nothing at run time: the render loop keeps a list of the
// bands that are actually doing something (see advance_coeffs) and runs only
// those, so a strip with four bands in use pays for four, not thirty-two.
inline constexpr std::size_t kEqBands = 32;

// What shape an EQ band is. `Auto` is the pre-2.5 reading of the two flags
// below (a bell, or a shelf whose end `low_shelf` picks), so existing callers
// and tests that never heard of `kind` behave exactly as before.
enum class EqKind : std::uint8_t {
    Auto = 0, Bell, LowShelf, HighShelf, LowCut, HighCut, Notch,
};

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
    // Last, so designated initialisers written before it existed still work.
    EqKind kind     = EqKind::Auto;
};

// Cuts and notches do their work at any gain setting; bells and shelves are
// identities at 0 dB. One rule, used by both set_params and needs_processing.
inline EqKind resolved_kind(const EqBandParams& b) noexcept {
    if (b.kind != EqKind::Auto) return b.kind;
    if (!b.shelf) return EqKind::Bell;
    return b.low_shelf ? EqKind::LowShelf : EqKind::HighShelf;
}
inline bool eq_band_active(const EqBandParams& b) noexcept {
    if (!b.enabled) return false;
    const EqKind k = resolved_kind(b);
    if (k == EqKind::LowCut || k == EqKind::HighCut || k == EqKind::Notch) return true;
    return b.gain_db != 0.0f;
}

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
            // A band sitting at 0 dB is a no-op; make it literally one so flat
            // bands in circuit cannot colour the desk. That holds for a shelf
            // as much as a bell — both are identities at unity gain. Cuts and
            // notches have no gain and are never idle while enabled.
            if (!eq_band_active(b)) {
                c.eq[i] = biquad_passthrough();
                continue;
            }
            switch (resolved_kind(b)) {
                case EqKind::LowShelf:
                    c.eq[i] = biquad_lowshelf(b.freq_hz, fs, b.gain_db, b.slope); break;
                case EqKind::HighShelf:
                    c.eq[i] = biquad_highshelf(b.freq_hz, fs, b.gain_db, b.slope); break;
                case EqKind::LowCut:
                    c.eq[i] = biquad_highpass(b.freq_hz, fs, b.q); break;
                case EqKind::HighCut:
                    c.eq[i] = biquad_lowpass(b.freq_hz, fs, b.q); break;
                case EqKind::Notch:
                    c.eq[i] = biquad_notch(b.freq_hz, fs, b.q); break;
                default:
                    c.eq[i] = biquad_peaking(b.freq_hz, fs, b.gain_db, b.q); break;
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
        // EQ: ramp only the bands that are, or are becoming, something. A band
        // idle on both sides is skipped outright; one that has ramped out to
        // within a hair of identity is snapped onto it exactly and its memory
        // dropped, like the width snap below. What remains is the list the
        // per-sample loop runs — so thirty-two slots of capacity cost the same
        // as the four in use.
        eq_live_count_ = 0;
        for (std::size_t i = 0; i < kEqBands; ++i) {
            const bool target_idle = is_passthrough(target.eq[i]);
            if (target_idle && is_passthrough(active_.eq[i])) continue;
            lerp(active_.eq[i], target.eq[i], kRamp);
            if (target_idle && near_passthrough(active_.eq[i])) {
                active_.eq[i] = biquad_passthrough();
                for (auto& lane : state_) lane.eq[i].reset();
                continue;
            }
            eq_live_[eq_live_count_++] = static_cast<std::uint8_t>(i);
        }
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
            for (std::size_t k = 0; k < eq_live_count_; ++k) {
                const std::uint8_t b = eq_live_[k];
                x = st.eq[b].process(active_.eq[b], x);
            }
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
    //
    // `tap`, when this strip is being analysed, receives the strip's input and
    // its post-EQ signal (before the dynamics) for the channel view's
    // spectrum analyser. Null for every other strip.
    void process(Sample* const* lanes, ChannelCount count, std::size_t frames,
                 AnalyserTap* tap = nullptr) noexcept {
        advance_coeffs();
        const auto lc = std::min<ChannelCount>(count, kMixerLanes);
        if (tap) tap->write_pre(lanes, lc, frames);
        for (ChannelCount l = 0; l < lc; ++l) {
            process_tone(l, lanes[l], frames);
        }
        if (tap) { tap->write_post(lanes, lc, frames); tap->commit(frames); }
        // What reaches the dynamics, and what leaves them: the channel view
        // draws the input on the transfer curve, where the operator can see it
        // meet the threshold.
        fold_peak(dyn_in_peak_, block_peak(lanes, lc, frames));
        gate_.process(active_.gate, lanes, lc, frames);
        comp_.process(active_.comp, lanes, lc, frames);
        fold_peak(dyn_out_peak_, block_peak(lanes, lc, frames));
        fold_min(gate_gr_min_, gate_.gain_reduction_db());
        fold_min(comp_gr_min_, comp_.gain_reduction_db());
        // Last, and only on a strip that actually has an image. A mono strip
        // carries nothing on lane 1, so a matrix across the pair would read
        // silence as the right channel and hard-pan the strip left.
        if (width_active_ && lc >= kMixerLanes) {
            width_.process(active_.width, lanes[0], lanes[1], frames);
        }
    }

    // How far each processor is pulling the strip down, for their meters.
    // Last block only — what the unit tests read.
    float gate_reduction_db() const noexcept { return gate_.gain_reduction_db(); }
    float comp_reduction_db() const noexcept { return comp_.gain_reduction_db(); }

    // Render thread, for a block the chain skipped (nothing in circuit): the
    // dynamics' input and output are then both the strip's input, and the
    // transfer-curve meter should still show it — an operator sets the
    // threshold against the signal BEFORE switching the compressor in.
    void note_idle_block(Sample* const* lanes, ChannelCount count, std::size_t frames) noexcept {
        const auto lc = std::min<ChannelCount>(count, kMixerLanes);
        const float pk = block_peak(lanes, lc, frames);
        fold_peak(dyn_in_peak_, pk);
        fold_peak(dyn_out_peak_, pk);
    }

    // ---- Broadcast thread: consuming reads --------------------------------
    // Peak level into / out of the dynamics since the last read, in dBFS, and
    // the deepest gain reduction of each processor since the last read. Each
    // read resets, so every meter tick sees the loudest thing since the one
    // before — a 30 Hz reader of a last-block value misses most peaks.
    float take_dyn_in_db()  noexcept { return lin_to_db(dyn_in_peak_.exchange(0.0f, std::memory_order_acq_rel)); }
    float take_dyn_out_db() noexcept { return lin_to_db(dyn_out_peak_.exchange(0.0f, std::memory_order_acq_rel)); }
    float take_gate_gr_db() noexcept { return gate_gr_min_.exchange(0.0f, std::memory_order_acq_rel); }
    float take_comp_gr_db() noexcept { return comp_gr_min_.exchange(0.0f, std::memory_order_acq_rel); }

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
        for (const auto& b : p.eq) if (eq_band_active(b)) return true;
        return false;
    }

    static bool is_passthrough(const BiquadCoeffs& c) noexcept {
        return c.b0 == 1.0f && c.b1 == 0.0f && c.b2 == 0.0f && c.a1 == 0.0f && c.a2 == 0.0f;
    }
    // Within 1e-6 of identity on every coefficient: 0.75 per block gets there
    // from a full ±24 dB band in about fifty blocks — a clean ramp-out, then
    // an exact stop.
    static bool near_passthrough(const BiquadCoeffs& c) noexcept {
        constexpr float e = 1e-6f;
        return std::fabs(c.b0 - 1.0f) < e && std::fabs(c.b1) < e && std::fabs(c.b2) < e &&
               std::fabs(c.a1) < e && std::fabs(c.a2) < e;
    }
    static float block_peak(Sample* const* lanes, ChannelCount lc, std::size_t frames) noexcept {
        float pk = 0.0f;
        for (ChannelCount l = 0; l < lc; ++l)
            for (std::size_t s = 0; s < frames; ++s) pk = std::max(pk, std::fabs(lanes[l][s]));
        return pk;
    }
    static void fold_peak(std::atomic<float>& a, float v) noexcept {
        float cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_acq_rel)) {}
    }
    static void fold_min(std::atomic<float>& a, float v) noexcept {
        float cur = a.load(std::memory_order_relaxed);
        while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_acq_rel)) {}
    }
    static float lin_to_db(float lin) noexcept {
        return lin > 1e-6f ? 20.0f * std::log10(lin) : -120.0f;
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
    // The EQ bands to run this block, ascending — rebuilt by advance_coeffs.
    std::array<std::uint8_t, kEqBands> eq_live_{};        // render thread only
    std::size_t                        eq_live_count_{0}; // render thread only

    // Dynamics metering, max/min since the last consuming read (above).
    std::atomic<float> dyn_in_peak_{0.0f};
    std::atomic<float> dyn_out_peak_{0.0f};
    std::atomic<float> gate_gr_min_{0.0f};
    std::atomic<float> comp_gr_min_{0.0f};
};

} // namespace liveplay::audio
