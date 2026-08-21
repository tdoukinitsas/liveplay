// ============================================================================
// liveplay/audio/drift_resampler.hpp
// ----------------------------------------------------------------------------
// Clock-drift compensation for devices that are not the clock source.
//
// The problem
// -----------
// Two audio devices run on independent crystals. Nominally both are 48 kHz;
// really they are 48 kHz give or take tens of parts per million, and nothing
// keeps them together. The engine renders at ONE rate — the clock device's —
// and every other device consumes at its own. The difference accumulates: a
// device running 50 ppm fast eats 50 extra microseconds of audio per second,
// so it drains its queue by 3 ms a minute and eventually starves. A slow one
// fills instead, and its queue backs up until frames are dropped.
//
// A deep buffer only postpones this. At 50 ppm, 37 ms of slack lasts about
// twelve minutes and 427 ms lasts a couple of hours; neither survives a show,
// and the deep one costs latency on every control in exchange.
//
// The fix
// -------
// Resample each non-clock device's stream by a ratio very close to 1, adjusted
// continuously so its queue stays at a target depth. If a device is consuming
// faster than we produce, we produce very slightly more samples for it. The
// correction is a few parts per million — far below anything audible — and it
// never stops, so the error never accumulates.
//
// Two pieces, deliberately separate:
//
//   DriftController   watches the queue and decides the ratio. Pure control
//                     logic, no audio, so its behaviour can be tested by
//                     driving it with numbers.
//   DriftResampler    applies a ratio to a stream. Pure DSP, no policy.
//
// Why cubic interpolation
// -----------------------
// The ratio sits at ~1.000005, so it is tempting to interpolate linearly and
// have done. But the fractional phase still sweeps slowly through the whole
// 0..1 range, and linear interpolation's error varies with that phase — the
// result is a comb filter whose notch wanders, heard as the top end shimmering
// every few seconds. Catmull-Rom costs three more multiplies per sample and
// measures about -77 dB against linear's -45 (see drift_test.cpp).
//
// -77 dB is inaudible against programme material but is not transparent in the
// mastering sense. Genuine transparency wants a windowed-sinc polyphase
// resampler, which is a great deal more code than this correction of a few
// parts per million justifies. Worth revisiting only if this is ever asked to
// do real sample-rate conversion.
//
// The lookahead that costs
// ------------------------
// Cubic interpolation needs one sample BEFORE the read position and two AFTER
// it. The two after are the awkward ones: at the end of a block they have not
// arrived yet. Clamping to the last available sample instead — the obvious
// shortcut — puts an error at every block boundary, which is an impulse every
// 256 samples and measures as broadband noise around -47 dB. Audible, and
// entirely self-inflicted.
//
// So the resampler keeps its own input buffer and simply does not read closer
// than two frames to the end of it, carrying the remainder into the next
// block. The cost is two frames of latency, which is 40 microseconds, and the
// interpolation error drops by more than seventy decibels.
// ============================================================================
#pragma once

#include "liveplay/audio/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace liveplay::audio {

// How far the controller is allowed to bend the clock. Real crystals are
// within about 100 ppm of nominal, so 0.2% is a very large multiple of what
// drift can ever require — it is a guard against a runaway loop, not a working
// range. Corrections in normal use are three orders of magnitude smaller.
inline constexpr double kMaxDriftRatio = 0.002;

// Where a non-clock device's queue is held. Half of its ring: equidistant from
// starving and from overflowing, so a disturbance in either direction has the
// same room to recover.
inline constexpr double kDriftTargetFill = 0.5;

// Turns queue depth into a resampling ratio.
//
// A proportional-integral loop. The integral term is what actually does the
// work: a constant clock offset needs a constant ratio correction, and a purely
// proportional loop can only produce that by sitting at a permanent depth
// error. The proportional term is there to damp the approach.
//
// Both gains are deliberately tiny. This loop has to be far slower than
// anything it reacts to — queue depth jitters by milliseconds block to block
// from ordinary scheduling, and a loop fast enough to chase that would
// modulate pitch audibly. It should take tens of seconds to absorb a real
// offset and simply ignore the jitter.
class DriftController {
public:
    void reset() noexcept {
        integral_  = 0.0;
        ratio_     = 1.0;
        smoothed_  = -1.0;
    }

    // Call once per rendered block. `fill_frames` is what the device has
    // queued; `capacity_frames` its ring size.
    double update(std::uint32_t fill_frames, std::uint32_t capacity_frames) noexcept {
        if (capacity_frames == 0) return ratio_;
        const double target = capacity_frames * kDriftTargetFill;

        // Smooth the MEASUREMENT, not the output.
        //
        // Queue depth jitters by milliseconds block to block from ordinary
        // scheduling, and the loop needs two incompatible things from its
        // gains: enough proportional term to damp the approach, and little
        // enough to ignore that jitter. Detuning kP to buy jitter rejection
        // just made the loop ring and overshoot its target by 20%.
        //
        // Filtering here separates the two. Roughly a three-second time
        // constant: far slower than the jitter, far faster than drift, and it
        // leaves the steady offset the loop actually has to cancel completely
        // intact.
        constexpr double kSmooth = 0.002;
        const auto fill = static_cast<double>(fill_frames);
        smoothed_ = (smoothed_ < 0.0) ? fill : smoothed_ + (fill - smoothed_) * kSmooth;

        // Normalised by the target rather than by the capacity, so the loop
        // behaves the same whatever ring depth it is given.
        const double error = (target - smoothed_) / target;

        // A queue that is too full means the device is consuming slower than
        // we produce, so we must produce fewer samples: ratio below 1. error
        // is positive when the queue is too EMPTY, which needs ratio above 1,
        // so the signs line up as written.
        // The integral does the work — a constant clock offset needs a
        // constant ratio, which only an integrator can hold without a standing
        // error. kP damps the approach.
        //
        // Both are derived, not tuned by hand. The plant is itself an
        // integrator (the queue accumulates the rate difference) at a gain of
        // block/target per block, so with a second integrator in the loop this
        // is a second-order system and the wrong gains ring rather than
        // settle. Picking a natural period of ~4000 blocks — comfortably
        // slower than the 500-block measurement filter above, or the filter's
        // lag would destabilise what it was added to protect — and damping of
        // about 0.7 gives these. An earlier pair chosen by feel was three
        // orders of magnitude too fast and simply oscillated between the
        // limits.
        constexpr double kP = 1.2e-3;
        constexpr double kI = 2.2e-7;

        integral_ += error * kI;
        // Clamped independently of the output, so a long disturbance cannot
        // wind the integrator somewhere it takes just as long to unwind from.
        integral_ = std::clamp(integral_, -kMaxDriftRatio, kMaxDriftRatio);

        ratio_ = std::clamp(1.0 + error * kP + integral_,
                            1.0 - kMaxDriftRatio, 1.0 + kMaxDriftRatio);
        return ratio_;
    }

    double ratio() const noexcept { return ratio_; }
    // Parts per million of correction being applied. What a diagnostic should
    // show: a healthy pair of devices settles at a small steady figure, and a
    // number pinned at the limit means something other than drift is wrong.
    double ppm() const noexcept { return (ratio_ - 1.0) * 1.0e6; }

    // The queue depth the loop is ACTUALLY regulating, in frames. Negative
    // until the first update() seeds it.
    //
    // This, not an instantaneous read of the ring, is what a diagnostic should
    // report against kDriftTargetFill. The two differ by more than they look
    // as though they should: the loop samples the ring once per rendered
    // block, which is immediately after a device callback has drained a
    // period, whereas anything asking from outside lands at a uniformly random
    // point in that period. On a device whose period is a third of its ring
    // that is a twenty-point offset — enough that a healthy locked loop reads
    // 70% to an outside observer while holding its own measurement at exactly
    // the 50% it is aiming for. Reporting the raw number invites someone to
    // chase a fault that is not there.
    double smoothed_fill_frames() const noexcept { return smoothed_; }

private:
    double integral_ = 0.0;
    double ratio_    = 1.0;
    double smoothed_ = -1.0;   // negative until the first measurement seeds it
};

// Resamples an interleaved block by a ratio near 1.
//
// Output length is not fixed: asking for a ratio of 1.000005 means slightly
// more output than input over time, and the extra sample appears whenever the
// fractional position happens to wrap. Callers must therefore treat the return
// value as the truth rather than assuming block size.
class DriftResampler {
public:
    // `max_block` is the largest input this will be handed, so the buffer can
    // be sized once here rather than grown on the render thread.
    void configure(ChannelCount channels, std::size_t max_block) {
        channels_ = std::max<ChannelCount>(1, channels);
        buf_.reserve((max_block + kGuard * 2) * channels_);
        reset();
    }

    void reset() noexcept {
        // One frame of silence in front, so the very first read has a y0 to
        // look back at and the position can start at 1 rather than at a
        // special case.
        buf_.assign(static_cast<std::size_t>(channels_), 0.0f);
        pos_ = 1.0;
    }

    // Consume `in_frames` of interleaved input, write to `out`, return frames
    // written. Size `out` with max_output(in_frames).
    std::size_t process(const Sample* in, std::size_t in_frames,
                        double ratio, Sample* out, std::size_t out_capacity) noexcept {
        if (channels_ == 0 || in_frames == 0) return 0;
        buf_.insert(buf_.end(), in, in + in_frames * channels_);

        const double step   = 1.0 / std::clamp(ratio, 0.5, 2.0);
        const auto   frames = buf_.size() / channels_;
        std::size_t  written = 0;

        // Stop two frames short of the end: floor(pos)+2 has to be a real
        // sample, not a clamp. Whatever is left over stays in the buffer and
        // is read next time.
        const double limit = static_cast<double>(frames) - 2.0;
        while (pos_ < limit && written < out_capacity) {
            const double fpos = std::floor(pos_);
            const auto   i    = static_cast<std::size_t>(fpos);
            const float  t    = static_cast<float>(pos_ - fpos);
            for (ChannelCount c = 0; c < channels_; ++c) {
                out[written * channels_ + c] = catmull_rom(
                    buf_[(i - 1) * channels_ + c], buf_[i * channels_ + c],
                    buf_[(i + 1) * channels_ + c], buf_[(i + 2) * channels_ + c], t);
            }
            ++written;
            pos_ += step;
        }

        // Drop what has been read, keeping the one frame behind the read
        // position that the next interpolation needs.
        const auto keep_from = static_cast<std::size_t>(std::floor(pos_)) - 1;
        if (keep_from > 0) {
            buf_.erase(buf_.begin(),
                       buf_.begin() + static_cast<std::ptrdiff_t>(keep_from * channels_));
            pos_ -= static_cast<double>(keep_from);
        }
        return written;
    }

    // Upper bound on output frames for a given input, for sizing a buffer.
    static std::size_t max_output(std::size_t in_frames) noexcept {
        return static_cast<std::size_t>(in_frames / (1.0 - kMaxDriftRatio)) + 4;
    }

private:
    // Frames held either side of the read position: one behind, two ahead,
    // plus slack so the buffer never reallocates mid-show.
    static constexpr std::size_t kGuard = 8;

    static float catmull_rom(float y0, float y1, float y2, float y3, float t) noexcept {
        const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float a1 =         y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float a2 = -0.5f * y0              + 0.5f * y2;
        return ((a0 * t + a1) * t + a2) * t + y1;
    }

    ChannelCount        channels_ = 0;
    std::vector<Sample> buf_;       // unconsumed input, plus one frame behind
    double              pos_ = 1.0; // fractional read index into buf_
};

} // namespace liveplay::audio
