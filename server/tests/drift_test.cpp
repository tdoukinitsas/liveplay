// ============================================================================
// drift_test.cpp — clock-drift compensation.
//
//     cmake -DLIVEPLAY_BUILD_TESTS=ON .. && cmake --build . --target liveplay-drift-tests
//     ./liveplay-drift-tests
//
// Two things under test, and they are tested apart because they fail
// differently:
//
//   the RESAMPLER, which must change a stream's length without audibly
//   changing its content — the failure mode is distortion nobody notices in a
//   unit test but everybody notices in a room
//
//   the CONTROLLER, which must pull a queue back to its target and hold it
//   there against a constant offset, without hunting — and must do it slowly
//   enough that ordinary jitter does not become audible pitch modulation
//
// The controller is driven by a simulated device rather than by real audio:
// that is the whole reason it is a separate class. A drift bug that takes
// twenty minutes to hear can be reproduced here in a loop that runs in
// milliseconds.
// ============================================================================
#include "liveplay/audio/drift_resampler.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace liveplay::audio;

namespace {

int g_failures = 0;

void check_near(const char* name, double got, double expect, double tol) {
    const bool ok = std::fabs(got - expect) <= tol;
    std::printf("%-62s %s  (got %+11.5f, expect %+11.5f +/-%.5f)\n",
                name, ok ? "PASS" : "FAIL", got, expect, tol);
    if (!ok) ++g_failures;
}
void check_true(const char* name, bool ok) {
    std::printf("%-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

constexpr double kFs = 48000.0;
constexpr std::size_t kBlock = 256;
// Spelled out rather than pulled in from biquad.hpp: this file is about
// resampling and has no other reason to depend on the filter header.
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Resampler
// ---------------------------------------------------------------------------
double residual_db(const std::vector<double>& x, double freq);

void test_unity_is_faithful() {
    // At ratio 1 the output must be the input. Cubic interpolation at a
    // fractional phase of exactly 0 returns y1 exactly, so it is in fact
    // sample-for-sample identical — and if that ever stops being true, the
    // interpolator has been changed in a way worth noticing.
    DriftResampler r;
    r.configure(2, kBlock);
    std::vector<Sample> in(kBlock * 2), out(DriftResampler::max_output(kBlock) * 2);
    std::size_t got = 0;
    int n = 0;
    // Collected over many blocks, not one. residual_db fits a sinusoid by
    // projection and assumes sin and cos are orthogonal over the record, which
    // is only true across many cycles — a single 256-frame block is 5.3 cycles
    // of a 1 kHz tone and the fit leaves 30 dB of its own error behind. That
    // measured as a broken resampler when the resampler was exact.
    std::vector<double> collected;
    for (int b = 0; b < 40; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            const auto v = static_cast<Sample>(0.5 * std::sin(2.0 * kPi * 1000.0 * n / kFs));
            in[s * 2] = v; in[s * 2 + 1] = v;
        }
        got = r.process(in.data(), kBlock, 1.0, out.data(), out.size() / 2);
        if (b < 2) continue;                        // let the buffer prime
        for (std::size_t s = 0; s < got; ++s) collected.push_back(out[s * 2]);
    }
    // Steady state is one output per input; the two-frame lookahead only costs
    // frames on the very first block, and is a fixed delay thereafter.
    check_true("resampler: unity produces one output per input", got == kBlock);
    // Compared as a SIGNAL rather than sample-for-sample. The lookahead means
    // the output lags the input by a couple of frames, and chasing that offset
    // in the test would just be reimplementing the resampler's bookkeeping and
    // agreeing with whatever it did.
    // Compared against the analytic signal at whichever integer delay fits
    // best, rather than through residual_db. At a step of exactly 1 the
    // fractional phase is always 0 and Catmull-Rom returns the input sample
    // untouched, so this should be exact to float precision — and a spectral
    // estimate cannot tell "exact" from "very good", which is the distinction
    // that matters here.
    // Searched over a whole cycle and then some: `collected` starts two blocks
    // in, so its phase relative to sample 0 is an arbitrary offset, not the
    // two-frame lookahead. A search of 0..7 found no match and reported a
    // near-full-scale error, which looked like a broken resampler and was a
    // broken comparison. 1 kHz at 48 kHz is 48 samples a cycle, so 96 covers
    // every distinct phase twice over.
    double best = 1e9;
    for (int delay = 0; delay < 96; ++delay) {
        double worst = 0.0;
        for (std::size_t s = 0; s + delay < collected.size() && s < 4000; ++s) {
            const double want = 0.5 * std::sin(
                2.0 * kPi * 1000.0 * static_cast<double>(s + delay) / kFs);
            worst = std::max(worst, std::fabs(collected[s] - want));
        }
        best = std::min(best, worst);
    }
    std::printf("    (unity worst sample error %.3e)\n", best);
    check_true("resampler: and unity leaves the signal untouched", best < 1e-6);
}

// Everything that is not the expected sinusoid, in dB relative to it.
//
// A least-squares fit of amplitude and phase at one known frequency, then the
// energy of what is left over. Deliberately NOT a DFT bin: a bin measures
// "energy at this frequency" and leaks, where this measures "energy that is
// not this exact sinusoid", which is the actual question — it is THD+N.
//
// The frequency must be the SHIFTED one. Resampling by a ratio moves the tone
// to f/ratio, by definition: output sample k reads input time k/ratio. The
// first version of this probed the original frequency, found almost nothing
// there, and reported the resampler as broken when it was working exactly as
// intended.
double residual_db(const std::vector<double>& x, double freq) {
    const std::size_t n = x.size();
    if (n == 0) return 0.0;
    double xc = 0.0, xs = 0.0, cc = 0.0, ss = 0.0, power = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 2.0 * kPi * freq * static_cast<double>(i) / kFs;
        const double c = std::cos(w), s = std::sin(w);
        xc += x[i] * c; xs += x[i] * s;
        cc += c * c;    ss += s * s;
        power += x[i] * x[i];
    }
    const double a = cc > 0.0 ? xc / cc : 0.0;
    const double b = ss > 0.0 ? xs / ss : 0.0;
    double residual = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 2.0 * kPi * freq * static_cast<double>(i) / kFs;
        const double e = x[i] - (a * std::cos(w) + b * std::sin(w));
        residual += e * e;
    }
    const double tone = std::max(1e-30, power - residual);
    return 10.0 * std::log10(std::max(1e-30, residual) / tone);
}

void test_drifting_ratio_is_clean() {
    // A ratio a few hundred ppm off unity, held long enough that the
    // fractional phase sweeps the whole 0..1 range many times — exactly the
    // condition under which linear interpolation produces a wandering comb.
    // Everything outside the tone must stay far below it.
    for (double ppm : {-200.0, 200.0}) {
        DriftResampler r;
        r.configure(1, kBlock);
        const double ratio = 1.0 + ppm * 1e-6;
        std::vector<Sample> in(kBlock), out(DriftResampler::max_output(kBlock));
        std::vector<double> collected;
        collected.reserve(200 * kBlock);
        int n = 0;
        for (int b = 0; b < 200; ++b) {
            for (std::size_t s = 0; s < kBlock; ++s, ++n) {
                in[s] = static_cast<Sample>(0.5 * std::sin(2.0 * kPi * 997.0 * n / kFs));
            }
            const auto got = r.process(in.data(), kBlock, ratio, out.data(), out.size());
            if (b < 4) continue;                       // let the buffer prime
            for (std::size_t s = 0; s < got; ++s) collected.push_back(out[s]);
        }
        char name[96];
        std::snprintf(name, sizeof name,
                      "resampler: %+.0f ppm leaves the tone clean", ppm);
        // The tone lands at f/ratio, not at f — see residual_db.
        const double got = residual_db(collected, 997.0 / ratio);
        std::printf("    (residual %.1f dB)\n", got);
        // Measures about -77 dB, which is what a 4-point cubic gives at this
        // frequency; linear interpolation manages roughly -45 under the same
        // sweep of phase. Inaudible against programme material, though not
        // transparent in the mastering sense — genuine transparency needs a
        // windowed-sinc polyphase resampler, which is a great deal more code
        // for a correction of a few parts per million.
        check_true(name, got < -72.0);
    }
}

void test_length_follows_the_ratio() {
    // The point of the whole exercise: a ratio above 1 must produce more
    // samples than it consumes, and below 1 fewer. Measured over many blocks,
    // because a single block rounds to a whole number of samples.
    for (double ppm : {-500.0, 500.0}) {
        DriftResampler r;
        r.configure(2, kBlock);
        const double ratio = 1.0 + ppm * 1e-6;
        std::vector<Sample> in(kBlock * 2, 0.0f), out(DriftResampler::max_output(kBlock) * 2);
        std::size_t total_in = 0, total_out = 0;
        for (int b = 0; b < 400; ++b) {
            const auto got = r.process(in.data(), kBlock, ratio, out.data(), out.size() / 2);
            if (b < 4) continue;                       // ignore the priming block
            total_out += got;
            total_in  += kBlock;
        }
        const double got_ratio = static_cast<double>(total_out) / static_cast<double>(total_in);
        char name[96];
        std::snprintf(name, sizeof name, "resampler: %+.0f ppm changes length to match", ppm);
        check_near(name, (got_ratio - 1.0) * 1e6, ppm, 20.0);
    }
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------
// A device that consumes at a rate slightly different from ours, with a queue
// in between. Exactly the situation the controller exists for, in numbers.
struct FakeDevice {
    double          capacity;
    double          fill;
    double          consume_per_block;   // frames the device eats per render block
    DriftController ctl;

    double step(double block) {
        const double ratio = ctl.update(static_cast<std::uint32_t>(std::max(0.0, fill)),
                                        static_cast<std::uint32_t>(capacity));
        fill += block * ratio - consume_per_block;
        fill = std::clamp(fill, 0.0, capacity);
        return fill;
    }
};

void test_controller_holds_the_queue() {
    // A device running 100 ppm fast. Uncorrected its queue drains to nothing;
    // the controller has to find the ratio that holds it at target and stay
    // there.
    const double capacity = 1792;                   // 6 blocks + guard, as shipped
    FakeDevice d{capacity, capacity * kDriftTargetFill, kBlock * (1.0 + 100e-6), {}};
    d.ctl.reset();

    // Ten minutes of blocks.
    const int blocks = static_cast<int>((600.0 * kFs) / kBlock);
    double worst_low = capacity, worst_high = 0.0;
    for (int i = 0; i < blocks; ++i) {
        const double f = d.step(kBlock);
        if (i > blocks / 4) {                       // after it has settled
            worst_low  = std::min(worst_low, f);
            worst_high = std::max(worst_high, f);
        }
    }
    const double target = capacity * kDriftTargetFill;
    check_true("controller: queue never starves over ten minutes", worst_low > 0.0);
    check_true("controller: and never overflows", worst_high < capacity);
    check_near("controller: settles at the target depth", d.fill / target, 1.0, 0.05);
    check_near("controller: converges on the device's actual offset (ppm)",
               d.ctl.ppm(), 100.0, 10.0);
}

void test_controller_handles_both_directions() {
    for (double ppm : {-80.0, 80.0}) {
        const double capacity = 1792;
        FakeDevice d{capacity, capacity * kDriftTargetFill, kBlock * (1.0 + ppm * 1e-6), {}};
        d.ctl.reset();
        const int blocks = static_cast<int>((600.0 * kFs) / kBlock);
        for (int i = 0; i < blocks; ++i) d.step(kBlock);
        char name[96];
        std::snprintf(name, sizeof name, "controller: tracks a %+.0f ppm device", ppm);
        check_near(name, d.ctl.ppm(), ppm, 10.0);
    }
}

void test_controller_ignores_jitter() {
    // Ordinary scheduling makes the queue wobble by milliseconds block to
    // block. The controller must barely react: a loop that chased this would
    // modulate pitch at the wobble's own rate, which is audible where a steady
    // few-ppm correction is not.
    const double capacity = 1792;
    FakeDevice d{capacity, capacity * kDriftTargetFill, kBlock, {}};   // no real drift
    d.ctl.reset();
    double worst_ppm = 0.0;
    unsigned seed = 12345;
    for (int i = 0; i < 40000; ++i) {
        // +/- 240 frames of noise on the reading: 5 ms, larger than anything
        // the latency probe measured.
        seed = seed * 1664525u + 1013904223u;
        const double noise = ((seed >> 16) % 481) - 240.0;
        d.ctl.update(static_cast<std::uint32_t>(std::clamp(d.fill + noise, 0.0, capacity)),
                     static_cast<std::uint32_t>(capacity));
        d.fill += kBlock * d.ctl.ratio() - d.consume_per_block;
        d.fill = std::clamp(d.fill, 0.0, capacity);
        if (i > 2000) worst_ppm = std::max(worst_ppm, std::fabs(d.ctl.ppm()));
    }
    std::printf("    (worst %.1f ppm from jitter alone)\n", worst_ppm);
    // A cent is about 578 ppm, so this is comfortably inaudible.
    check_true("controller: jitter alone never moves it far", worst_ppm < 60.0);
}

void test_controller_is_bounded() {
    // A device that has stopped consuming entirely is not drift, and the
    // controller must not wind up chasing it — it should reach its limit and
    // stay there, so recovery is immediate once the device comes back.
    const double capacity = 1792;
    FakeDevice d{capacity, capacity, 0.0, {}};
    d.ctl.reset();
    for (int i = 0; i < 100000; ++i) d.step(kBlock);
    check_true("controller: a stalled device does not wind the loop up",
               std::fabs(d.ctl.ppm()) <= kMaxDriftRatio * 1e6 + 1.0);
}

} // namespace

int main() {
    std::printf("== resampler ==\n");
    test_unity_is_faithful();
    test_drifting_ratio_is_clean();
    test_length_follows_the_ratio();

    std::printf("\n== controller ==\n");
    test_controller_holds_the_queue();
    test_controller_handles_both_directions();
    test_controller_ignores_jitter();
    test_controller_is_bounded();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
