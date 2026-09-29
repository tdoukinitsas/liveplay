// ============================================================================
// stereo_width_test.cpp — the strip's stereo image controls.
//
//     cmake -DLIVEPLAY_BUILD_TESTS=ON .. && cmake --build . --target liveplay-stereo-tests
//     ./liveplay-stereo-tests
//
// Four things under test, and they are related:
//
//   - the balance law, which is deliberately NOT the pan law
//   - the M/S width matrix and its bass-mono side filter
//   - the correlation meter that warns about what width can do
//   - the chain-level behaviour: width ramps in, and drops out bit-transparent
//
// Several of these exist to pin down decisions that could be quietly reversed:
// that balance is unity at centre where pan is -3 dB, that widening leaves mono
// material completely alone (which is why there is no level compensation), and
// that the mid is never touched, so L+R survives any setting.
// ============================================================================
#include "liveplay/audio/channel_dsp.hpp"
#include "liveplay/audio/stereo_width.hpp"
#include "liveplay/audio/types.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace liveplay::audio;

namespace {

int g_failures = 0;

void check_near(const char* name, double got, double expect, double tol) {
    const bool ok = std::fabs(got - expect) <= tol;
    std::printf("%-62s %s  (got %+9.4f, expect %+9.4f ±%.4f)\n",
                name, ok ? "PASS" : "FAIL", got, expect, tol);
    if (!ok) ++g_failures;
}
void check_true(const char* name, bool ok) {
    std::printf("%-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

constexpr double kFs = 48000.0;
constexpr std::size_t kBlock = 256;

// ---------------------------------------------------------------------------
// Balance
// ---------------------------------------------------------------------------
void test_balance_is_unity_at_centre() {
    const auto b = balance_gains_db(0.0f);
    check_near("balance: centre is unity on both lanes (L)", b.left,  0.0, 0.001);
    check_near("balance: centre is unity on both lanes (R)", b.right, 0.0, 0.001);

    // The distinction this whole law exists for. Pan has to hold constant power
    // as a mono source sweeps, so its centre is -3 dB; applying that to a
    // stereo bus would cost 3 dB for doing nothing.
    const auto p = pan_gains_db(0.0f);
    check_near("balance: and pan's centre is -3.01 dB, which is why they differ",
               p.left, -3.0103, 0.001);
}

void test_balance_only_attenuates() {
    // Hard over: one lane gone, the other still exactly unity. Never louder.
    const auto hard_r = balance_gains_db(1.0f);
    check_true("balance: hard right silences the left lane", hard_r.left <= kSilentGainDb);
    check_near("balance: and leaves the right at unity", hard_r.right, 0.0, 0.001);

    const auto half_r = balance_gains_db(0.5f);
    check_near("balance: half right takes 6 dB off the left", half_r.left, -6.0206, 0.01);
    check_near("balance: and still nothing off the right", half_r.right, 0.0, 0.001);

    // The property that matters on a lopsided mix: correcting the quiet side
    // cannot push the loud side up into the limiter.
    bool ever_boosts = false;
    for (int i = -20; i <= 20; ++i) {
        const auto g = balance_gains_db(i * 0.05f);
        if (g.left > 0.0001f || g.right > 0.0001f) ever_boosts = true;
    }
    check_true("balance: never adds gain to either lane, at any setting", !ever_boosts);
}

// ---------------------------------------------------------------------------
// The width matrix
// ---------------------------------------------------------------------------
struct Block { std::vector<Sample> l, r; };

Block make_block(double l_amp, double r_amp, double freq = 1000.0, int phase0 = 0) {
    Block b{std::vector<Sample>(kBlock), std::vector<Sample>(kBlock)};
    for (std::size_t s = 0; s < kBlock; ++s) {
        const double t = std::sin(2.0 * kPi * freq * (double)(phase0 + (int)s) / kFs);
        b.l[s] = static_cast<Sample>(l_amp * t);
        b.r[s] = static_cast<Sample>(r_amp * t);
    }
    return b;
}

WidthCoeffs coeffs(float width, float bass_hz = kBassMonoParkedHz) {
    WidthParams p;
    p.width = width;
    p.bass_mono_hz = bass_hz;
    return width_coeffs(p, kFs);
}

void test_unity_is_identity() {
    check_true("width: unity with the filter parked reports identity",
               width_near_identity(coeffs(1.0f)));
    check_true("width: any other width does not", !width_near_identity(coeffs(1.4f)));
    check_true("width: nor does bass-mono in circuit at unity width",
               !width_near_identity(coeffs(1.0f, 120.0f)));
}

void test_zero_width_is_mono() {
    auto b = make_block(1.0, 0.25);
    WidthState st; st.reset();
    st.process(coeffs(0.0f), b.l.data(), b.r.data(), kBlock);
    double worst = 0.0;
    for (std::size_t s = 0; s < kBlock; ++s)
        worst = std::max(worst, std::fabs((double)(b.l[s] - b.r[s])));
    check_near("width: 0 collapses the lanes onto each other", worst, 0.0, 1e-7);
}

void test_double_width_matrix() {
    // L = 1, R = 0 gives M = 0.5, S = 0.5. At w = 2 the side doubles, so
    // L = 0.5 + 1.0 = 1.5 and R = 0.5 - 1.0 = -0.5.
    auto b = make_block(1.0, 0.0);
    WidthState st; st.reset();
    st.process(coeffs(2.0f), b.l.data(), b.r.data(), kBlock);
    double lpk = 0.0, rpk = 0.0;
    for (std::size_t s = 0; s < kBlock; ++s) {
        lpk = std::max(lpk, (double)b.l[s]);
        rpk = std::min(rpk, (double)b.r[s]);
    }
    check_near("width: 2 puts a hard-left signal at +1.5 on the left", lpk, 1.5, 0.01);
    check_near("width: and -0.5 on the right, out of phase", rpk, -0.5, 0.01);
}

void test_mono_material_is_untouched_at_any_width() {
    // The argument against compensating the level by 1/max(1,w). A mono source
    // has no side content, so widening does not change it by a single sample —
    // and a compensation term would still have pulled it down 6 dB at w = 2.
    for (float w : {0.0f, 0.5f, 1.5f, 2.0f}) {
        auto b = make_block(0.7, 0.7);
        const auto orig = b.l;
        WidthState st; st.reset();
        st.process(coeffs(w), b.l.data(), b.r.data(), kBlock);
        double worst = 0.0;
        for (std::size_t s = 0; s < kBlock; ++s) {
            worst = std::max(worst, std::fabs((double)(b.l[s] - orig[s])));
            worst = std::max(worst, std::fabs((double)(b.r[s] - orig[s])));
        }
        char name[96];
        std::snprintf(name, sizeof name,
                      "width: mono material is untouched at w = %.1f", w);
        check_near(name, worst, 0.0, 1e-7);
    }
}

void test_mid_survives_every_setting() {
    // L + R = 2M whatever the side is doing. This is what guarantees a mono
    // fold-down never changes level as width is moved, and it is the reason
    // bass-mono can be a filter on the side alone rather than a crossover.
    double worst = 0.0;
    for (float w : {0.0f, 0.5f, 2.0f}) {
        for (float bass : {kBassMonoParkedHz, 200.0f}) {
            auto b = make_block(1.0, 0.2, 80.0);
            std::vector<Sample> sum0(kBlock);
            for (std::size_t s = 0; s < kBlock; ++s) sum0[s] = b.l[s] + b.r[s];
            WidthState st; st.reset();
            st.process(coeffs(w, bass), b.l.data(), b.r.data(), kBlock);
            for (std::size_t s = 0; s < kBlock; ++s)
                worst = std::max(worst, std::fabs((double)(b.l[s] + b.r[s] - sum0[s])));
        }
    }
    check_near("width: L+R is preserved exactly at every width and bass setting",
               worst, 0.0, 1e-6);
}

// How far apart the lanes are, as a fraction of how big they are. 0 is mono.
double sidedness(const Block& b) {
    double s = 0.0, m = 0.0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        s += std::fabs((double)(b.l[i] - b.r[i]));
        m += std::fabs((double)(b.l[i] + b.r[i]));
    }
    return m > 0.0 ? s / m : 0.0;
}

void test_bass_mono() {
    // A low tone spread across the lanes collapses to the centre; the same
    // arrangement up in the treble is left alone. Run long enough for the
    // filter to settle rather than measuring its startup transient.
    const auto c = coeffs(1.0f, 200.0f);

    WidthState low; low.reset();
    Block lb;
    for (int i = 0; i < 40; ++i) {
        lb = make_block(1.0, 0.0, 40.0, i * (int)kBlock);
        low.process(c, lb.l.data(), lb.r.data(), kBlock);
    }
    check_true("bass mono: 40 Hz spread collapses toward the centre",
               sidedness(lb) < 0.1);

    WidthState high; high.reset();
    Block hb;
    for (int i = 0; i < 40; ++i) {
        hb = make_block(1.0, 0.0, 5000.0, i * (int)kBlock);
        high.process(c, hb.l.data(), hb.r.data(), kBlock);
    }
    check_true("bass mono: 5 kHz spread is left alone", sidedness(hb) > 0.9);
}

// ---------------------------------------------------------------------------
// Correlation
// ---------------------------------------------------------------------------
float settle_correlation(double l_amp, double r_amp, double side_amp = 0.0) {
    CorrelationMeter cm; cm.reset();
    std::vector<Sample> l(kBlock), r(kBlock);
    int n = 0;
    for (int b = 0; b < 200; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            // A mid tone both lanes share, plus a side tone at a different
            // frequency they disagree about — uncorrelated, so the two powers
            // simply add.
            const double mid  = std::sin(2.0 * kPi * 500.0  * n / kFs);
            const double side = std::sin(2.0 * kPi * 1300.0 * n / kFs) * side_amp;
            l[s] = static_cast<Sample>(l_amp * mid + side);
            r[s] = static_cast<Sample>(r_amp * mid - side);
        }
        cm.process(l.data(), r.data(), kBlock);
    }
    return cm.value();
}

void test_correlation() {
    check_near("correlation: identical lanes read +1", settle_correlation(1.0, 1.0), 1.0, 0.01);
    check_near("correlation: inverted lanes read -1", settle_correlation(1.0, -1.0), -1.0, 0.01);

    // (m^2 - s^2) / (m^2 + s^2) with equal powers is zero — genuinely wide.
    check_near("correlation: equal mid and side reads 0",
               settle_correlation(1.0, 1.0, 1.0), 0.0, 0.05);
    // Side dominating goes negative: this is the warning that matters, because
    // it is material a mono sum will partly cancel.
    check_near("correlation: side twice the mid goes negative",
               settle_correlation(1.0, 1.0, 2.0), -0.6, 0.05);

    CorrelationMeter silent; silent.reset();
    std::vector<Sample> z(kBlock, 0.0f);
    for (int b = 0; b < 50; ++b) silent.process(z.data(), z.data(), kBlock);
    check_near("correlation: silence reads +1, not 0", silent.value(), 1.0, 0.001);

    // One lane silent is not a phase problem — a hard-left signal sums to mono
    // perfectly well — so it must not read as one.
    CorrelationMeter one; one.reset();
    std::vector<Sample> tone(kBlock);
    for (std::size_t s = 0; s < kBlock; ++s)
        tone[s] = static_cast<Sample>(std::sin(2.0 * kPi * 1000.0 * s / kFs));
    for (int b = 0; b < 50; ++b) one.process(tone.data(), z.data(), kBlock);
    check_near("correlation: one silent lane reads +1, not a warning",
               one.value(), 1.0, 0.001);
}

// ---------------------------------------------------------------------------
// The block inside the chain
// ---------------------------------------------------------------------------
void run_chain(ChannelDsp& dsp, Block& b) {
    Sample* lanes[2] = {b.l.data(), b.r.data()};
    dsp.process(lanes, 2, kBlock);
}

void test_chain_identity_is_bit_transparent() {
    // A strip with nothing set must not be altered by the matrix. The /2 M/S
    // form does not reconstruct exactly in floating point, so this only holds
    // because identity is detected and the block skipped.
    ChannelDsp dsp;
    dsp.configure(static_cast<SampleRate>(kFs));
    double worst = 0.0;
    for (int i = 0; i < 20; ++i) {
        auto b = make_block(0.6, 0.31, 700.0, i * (int)kBlock);
        const auto ol = b.l, orr = b.r;
        run_chain(dsp, b);
        for (std::size_t s = 0; s < kBlock; ++s) {
            worst = std::max(worst, std::fabs((double)(b.l[s] - ol[s])));
            worst = std::max(worst, std::fabs((double)(b.r[s] - orr[s])));
        }
    }
    check_true("chain: a flat strip is bit-transparent through the width block",
               worst == 0.0);

    // needs_processing() keeps a strip hot for a settle tail after any
    // parameter change, so coefficients can ramp out instead of dropping the
    // chain in one sample. configure() counts as a change, so the tail has to
    // be drained before asking whether a flat strip costs anything. The engine
    // drains it by calling this once per block; nothing else does.
    int blocks = 0;
    while (dsp.needs_processing() && blocks < 500) ++blocks;
    check_true("chain: a flat strip settles to reporting nothing to do",
               !dsp.needs_processing());
    check_true("chain: within the settle tail, not indefinitely", blocks < 200);
}

void test_chain_width_ramps_in_and_out() {
    ChannelDsp dsp;
    dsp.configure(static_cast<SampleRate>(kFs));

    StripDspParams p;
    p.width.width = 2.0f;
    dsp.set_params(p);
    check_true("chain: a widened strip reports work to do", dsp.needs_processing());

    // Ramp on: the first block must not already be at full width, or the ramp
    // is not doing its job and a knob drag would step.
    auto first = make_block(1.0, 0.0);
    run_chain(dsp, first);
    double first_pk = 0.0;
    for (std::size_t s = 0; s < kBlock; ++s) first_pk = std::max(first_pk, (double)first.l[s]);
    check_true("chain: width ramps rather than jumping to the new value",
               first_pk < 1.45);

    for (int i = 0; i < 60; ++i) { auto b = make_block(1.0, 0.0); run_chain(dsp, b); }
    auto settled = make_block(1.0, 0.0);
    run_chain(dsp, settled);
    double pk = 0.0;
    for (std::size_t s = 0; s < kBlock; ++s) pk = std::max(pk, (double)settled.l[s]);
    check_near("chain: and arrives at the full matrix", pk, 1.5, 0.01);

    // Back to unity, and it must become bit-transparent again rather than
    // running the matrix forever at 0.9999.
    dsp.set_params(StripDspParams{});
    for (int i = 0; i < 80; ++i) { auto b = make_block(1.0, 0.0); run_chain(dsp, b); }
    auto after = make_block(0.6, 0.31, 700.0);
    const auto ol = after.l, orr = after.r;
    run_chain(dsp, after);
    double worst = 0.0;
    for (std::size_t s = 0; s < kBlock; ++s) {
        worst = std::max(worst, std::fabs((double)(after.l[s] - ol[s])));
        worst = std::max(worst, std::fabs((double)(after.r[s] - orr[s])));
    }
    check_true("chain: returning to unity is bit-transparent again", worst == 0.0);
}

void test_chain_leaves_a_mono_strip_alone() {
    // A mono strip carries nothing on lane 1. Matrixing the pair would read
    // that silence as the right channel and hard-pan the strip left.
    ChannelDsp dsp;
    dsp.configure(static_cast<SampleRate>(kFs));
    StripDspParams p;
    p.width.width = 2.0f;
    dsp.set_params(p);

    double worst = 0.0;
    for (int i = 0; i < 80; ++i) {
        auto b = make_block(0.5, 0.0, 700.0, i * (int)kBlock);
        const auto ol = b.l;
        Sample* lanes[2] = {b.l.data(), b.r.data()};
        dsp.process(lanes, 1, kBlock);          // one lane: a mono strip
        for (std::size_t s = 0; s < kBlock; ++s)
            worst = std::max(worst, std::fabs((double)(b.l[s] - ol[s])));
    }
    check_true("chain: width never touches a mono strip", worst == 0.0);
}

} // namespace

int main() {
    std::printf("== balance ==\n");
    test_balance_is_unity_at_centre();
    test_balance_only_attenuates();

    std::printf("\n== width matrix ==\n");
    test_unity_is_identity();
    test_zero_width_is_mono();
    test_double_width_matrix();
    test_mono_material_is_untouched_at_any_width();
    test_mid_survives_every_setting();
    test_bass_mono();

    std::printf("\n== correlation ==\n");
    test_correlation();

    std::printf("\n== inside the chain ==\n");
    test_chain_identity_is_bit_transparent();
    test_chain_width_ramps_in_and_out();
    test_chain_leaves_a_mono_strip_alone();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
