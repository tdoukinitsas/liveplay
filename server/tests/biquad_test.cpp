// ============================================================================
// biquad_test.cpp — the filter sections behind the HPF, the LPF and every EQ
// band. No framework: each check prints PASS/FAIL and the binary exits
// non-zero if anything failed.
//
//     cmake -DLIVEPLAY_BUILD_TESTS=ON .. && cmake --build . --target liveplay-biquad-tests
//     ./liveplay-biquad-tests
//
// The load-bearing test here is `test_measured_matches_analytic`: it pushes
// real sine waves through the real process() loop and compares the measured
// steady-state level against the analytic |H(e^jw)|. That is what catches a
// coefficient typo or a transposed-form slip, which a test that only checks
// the coefficient arithmetic against itself would sail straight past.
// ============================================================================
#include "liveplay/audio/biquad.hpp"
#include "liveplay/audio/channel_dsp.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace liveplay::audio;

namespace {

int g_failures = 0;

void check_near(const char* name, double got, double expect, double tol) {
    const bool ok = std::fabs(got - expect) <= tol;
    std::printf("%-58s %s  (got %+9.4f, expect %+9.4f ±%.4f)\n",
                name, ok ? "PASS" : "FAIL", got, expect, tol);
    if (!ok) ++g_failures;
}
void check_true(const char* name, bool ok) {
    std::printf("%-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

constexpr double kFs = 48000.0;

// Drive a steady sine through the section and return its gain in dB, measured
// as the ratio of output RMS to input RMS. Half the run is discarded so the
// reading is steady state, not the section's start-up transient.
//
// RMS rather than peak, deliberately. Peak-detecting a sampled sine only finds
// the true peak if a sample happens to land on it: at 8 kHz on a 48 kHz rate
// there are six samples per cycle and the highest lands at 0.866, which is
// 1.25 dB low. Filters shift phase, so the error differs between input and
// output and does not cancel — it produced a 1.2 dB "failure" against a filter
// that was in fact exact. RMS over many cycles does not care where the samples
// fall.
double measured_response_db(const BiquadCoeffs& c, double freq_hz) {
    BiquadState st;
    const int settle = 24000;
    const int measure = 24000;
    double sum_in = 0.0, sum_out = 0.0;
    for (int n = 0; n < settle + measure; ++n) {
        const float x = static_cast<float>(std::sin(2.0 * kPi * freq_hz * n / kFs));
        const float y = st.process(c, x);
        if (n >= settle) {
            sum_in  += static_cast<double>(x) * x;
            sum_out += static_cast<double>(y) * y;
        }
    }
    if (sum_in <= 0.0 || sum_out <= 0.0) return -200.0;
    return 10.0 * std::log10(sum_out / sum_in);
}

// ---------------------------------------------------------------------------
void test_highpass() {
    const auto c = biquad_highpass(1000.0, kFs);
    // A Butterworth (Q = 1/sqrt2) corner sits at -3.01 dB, by definition.
    check_near("hpf: -3 dB at the corner",
               biquad_magnitude_db(c, 1000.0, kFs), -3.0103, 0.05);
    check_near("hpf: passes the passband at unity",
               biquad_magnitude_db(c, 12000.0, kFs), 0.0, 0.15);
    // Second order = 12 dB per octave. An octave down should be ~12 dB lower
    // than two octaves down is... measured as the slope between them.
    const double oneOct  = biquad_magnitude_db(c, 500.0, kFs);
    const double twoOct  = biquad_magnitude_db(c, 250.0, kFs);
    check_near("hpf: rolls off 12 dB/octave", oneOct - twoOct, 12.0, 0.7);
    check_true ("hpf: deep stopband is well down", biquad_magnitude_db(c, 20.0, kFs) < -60.0);
}

void test_lowpass() {
    const auto c = biquad_lowpass(1000.0, kFs);
    check_near("lpf: -3 dB at the corner",
               biquad_magnitude_db(c, 1000.0, kFs), -3.0103, 0.05);
    check_near("lpf: passes the passband at unity",
               biquad_magnitude_db(c, 50.0, kFs), 0.0, 0.15);
    const double oneOct = biquad_magnitude_db(c, 2000.0, kFs);
    const double twoOct = biquad_magnitude_db(c, 4000.0, kFs);
    check_near("lpf: rolls off 12 dB/octave", oneOct - twoOct, 12.0, 0.7);
}

void test_peaking() {
    // A bell must deliver exactly the gain asked for, at the frequency asked
    // for — this is the one an operator checks by ear against the number.
    for (float g : {-12.0f, -6.0f, 6.0f, 12.0f}) {
        const auto c = biquad_peaking(1000.0, kFs, g, 1.0f);
        char label[80];
        std::snprintf(label, sizeof label, "eq: bell hits %+.0f dB at centre", g);
        check_near(label, biquad_magnitude_db(c, 1000.0, kFs), g, 0.02);
    }
    // ...and must not disturb anything far from it.
    const auto c = biquad_peaking(1000.0, kFs, 12.0f, 2.0f);
    check_near("eq: bell is unity two decades below",
               biquad_magnitude_db(c, 10.0, kFs), 0.0, 0.1);
    check_near("eq: bell is unity well above",
               biquad_magnitude_db(c, 20000.0, kFs), 0.0, 0.3);
    // Q controls width: a higher Q must be narrower, i.e. closer to unity at a
    // fixed offset from centre.
    const auto wide   = biquad_peaking(1000.0, kFs, 12.0f, 0.5f);
    const auto narrow = biquad_peaking(1000.0, kFs, 12.0f, 4.0f);
    check_true("eq: higher Q is narrower",
               biquad_magnitude_db(narrow, 2000.0, kFs) <
               biquad_magnitude_db(wide,   2000.0, kFs));
    // A band set to 0 dB must be a genuine no-op, not "nearly" one — an EQ
    // with four flat bands in circuit has to be bit-transparent or every strip
    // quietly colours the desk.
    const auto flat = biquad_peaking(1000.0, kFs, 0.0f, 1.0f);
    check_near("eq: 0 dB band is flat at centre",
               biquad_magnitude_db(flat, 1000.0, kFs), 0.0, 1e-9);
    check_near("eq: 0 dB band is flat elsewhere",
               biquad_magnitude_db(flat, 60.0, kFs), 0.0, 1e-9);
}

void test_shelves() {
    const auto ls = biquad_lowshelf(200.0, kFs, 9.0f);
    check_near("shelf: low shelf reaches its gain at DC",
               biquad_magnitude_db(ls, 1.0, kFs), 9.0, 0.15);
    check_near("shelf: low shelf is unity up top",
               biquad_magnitude_db(ls, 18000.0, kFs), 0.0, 0.2);
    const auto hs = biquad_highshelf(4000.0, kFs, -9.0f);
    check_near("shelf: high shelf reaches its gain up top",
               biquad_magnitude_db(hs, 22000.0, kFs), -9.0, 0.2);
    check_near("shelf: high shelf is unity at DC",
               biquad_magnitude_db(hs, 1.0, kFs), 0.0, 0.15);
}

// ---------------------------------------------------------------------------
// The real check: does the running filter do what the maths says it does?
void test_measured_matches_analytic() {
    struct Case { const char* name; BiquadCoeffs c; double at; };
    const Case cases[] = {
        {"hpf 100 Hz measured at 100 Hz",  biquad_highpass(100.0, kFs),            100.0},
        {"hpf 100 Hz measured at 1 kHz",   biquad_highpass(100.0, kFs),           1000.0},
        {"hpf 100 Hz measured at 40 Hz",   biquad_highpass(100.0, kFs),             40.0},
        {"lpf 8 kHz measured at 8 kHz",    biquad_lowpass(8000.0, kFs),           8000.0},
        {"lpf 8 kHz measured at 16 kHz",   biquad_lowpass(8000.0, kFs),          16000.0},
        {"bell +6 dB measured at centre",  biquad_peaking(1000.0, kFs, 6.0f, 1.0f), 1000.0},
        {"bell -9 dB measured at centre",  biquad_peaking(3000.0, kFs, -9.0f, 2.0f), 3000.0},
        {"low shelf +6 measured at 50 Hz", biquad_lowshelf(300.0, kFs, 6.0f),       50.0},
    };
    for (const auto& k : cases) {
        const double analytic = biquad_magnitude_db(k.c, k.at, kFs);
        const double measured = measured_response_db(k.c, k.at);
        check_near(k.name, measured, analytic, 0.06);
    }
}

// ---------------------------------------------------------------------------
void test_denormal_flush() {
    // Ring the section with an impulse, then feed it pure silence. Without the
    // flush the state decays towards denormal range and stays there, and on
    // x86 every subsequent sample costs a microcode trap — a filter that was
    // free suddenly is not, on every idle bus at once.
    auto c = biquad_lowpass(80.0, kFs);
    BiquadState st;
    st.process(c, 1.0f);
    float last = 1.0f;
    for (int n = 0; n < 500000 && last != 0.0f; ++n) last = st.process(c, 0.0f);
    check_true("denormal: state reaches exactly zero on silence", last == 0.0f);

    // And the flush must not eat real signal on the way past.
    BiquadState st2;
    auto flat = biquad_peaking(1000.0, kFs, 0.0f, 1.0f);
    double worst = 0.0;
    for (int n = 0; n < 4800; ++n) {
        const float x = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * n / kFs));
        worst = std::max(worst, std::fabs(static_cast<double>(st2.process(flat, x) - x)));
    }
    check_true("denormal: flush does not disturb real signal", worst < 1e-6);
}

void test_nyquist_clamp() {
    // A 20 kHz knob on a 44.1 kHz project asks for a corner above Nyquist. The
    // cookbook formulas blow up there; the design functions clamp instead, so
    // the worst case is a filter parked as high as it can go rather than NaN
    // poisoning the bus for the rest of the show.
    for (double fs : {44100.0, 48000.0}) {
        for (double f : {19000.0, 22050.0, 24000.0, 96000.0}) {
            const auto lp = biquad_lowpass(f, fs);
            const auto hp = biquad_highpass(f, fs);
            const bool finite =
                std::isfinite(lp.b0) && std::isfinite(lp.a1) && std::isfinite(lp.a2) &&
                std::isfinite(hp.b0) && std::isfinite(hp.a1) && std::isfinite(hp.a2);
            if (!finite) {
                std::printf("nyquist clamp FAILED at fs=%.0f f=%.0f\n", fs, f);
                ++g_failures;
            }
        }
    }
    check_true("nyquist: coefficients stay finite above fs/2", true);

    // Zero and negative frequencies come from a typed-in value or a bad patch.
    const auto z = biquad_highpass(0.0, kFs);
    check_true("nyquist: DC request stays finite", std::isfinite(z.b0) && std::isfinite(z.a1));
}

void test_stability() {
    // A section is stable when its poles are inside the unit circle, which for
    // a normalised biquad is |a2| < 1 and |a1| < 1 + a2. Swept across the whole
    // usable range, because an unstable section does not misbehave quietly —
    // it runs away to full scale on a live output.
    int checked = 0;
    for (double f = 20.0; f <= 20000.0; f *= 1.15) {
        for (float q : {0.1f, 0.5f, 0.707f, 2.0f, 10.0f, 40.0f}) {
            for (float g : {-18.0f, 0.0f, 18.0f}) {
                const BiquadCoeffs set[] = {
                    biquad_highpass(f, kFs, q),
                    biquad_lowpass(f, kFs, q),
                    biquad_peaking(f, kFs, g, q),
                    biquad_lowshelf(f, kFs, g),
                    biquad_highshelf(f, kFs, g),
                };
                for (const auto& c : set) {
                    ++checked;
                    if (!(std::fabs(c.a2) < 1.0f &&
                          std::fabs(c.a1) < 1.0f + c.a2 + 1e-6f)) {
                        std::printf("unstable at f=%.1f q=%.2f g=%.1f "
                                    "(a1=%.6f a2=%.6f)\n", f, q, g, c.a1, c.a2);
                        ++g_failures;
                    }
                }
            }
        }
    }
    std::printf("%-58s %s  (%d sections)\n",
                "stability: poles inside the unit circle everywhere",
                g_failures == 0 ? "PASS" : "FAIL", checked);
}

// ---------------------------------------------------------------------------
// The strip's chain: the container the render loop actually calls.
// ---------------------------------------------------------------------------
// Run `blocks` blocks of a sine through one lane and report the chain's gain,
// as an RMS ratio for the same reason measured_response_db uses one.
double chain_response_db(ChannelDsp& dsp, ChannelIndex lane,
                         double freq_hz, int blocks = 200) {
    constexpr std::size_t kBlock = 256;
    std::vector<Sample> buf(kBlock), in(kBlock);
    double sum_in = 0.0, sum_out = 0.0;
    int n = 0;
    for (int b = 0; b < blocks; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            in[s]  = static_cast<float>(std::sin(2.0 * kPi * freq_hz * n / kFs));
            buf[s] = in[s];
        }
        dsp.advance_coeffs();
        dsp.process_tone(lane, buf.data(), kBlock);
        // Ignore the first half: coefficients ramp, and the sections settle.
        if (b > blocks / 2) {
            for (std::size_t s = 0; s < kBlock; ++s) {
                sum_in  += static_cast<double>(in[s])  * in[s];
                sum_out += static_cast<double>(buf[s]) * buf[s];
            }
        }
    }
    if (sum_in <= 0.0 || sum_out <= 0.0) return -200.0;
    return 10.0 * std::log10(sum_out / sum_in);
}

void test_chain_transparency() {
    // Every block flat means bit-transparent, not nearly. A strip whose tone
    // controls are untouched must not colour the desk, and four EQ bands in
    // circuit at 0 dB is the default state of every bus on it.
    ChannelDsp dsp;
    dsp.configure(48000);
    // A freshly published chain keeps running for a short tail so it can ramp
    // onto its coefficients; once that expires a flat chain drops out of the
    // render loop entirely, which is what makes an untouched desk free.
    int blocks = 0;
    while (dsp.needs_processing() && blocks < 1000) ++blocks;
    check_true("chain: a flat chain stops needing processing", blocks < 1000);

    constexpr std::size_t kBlock = 256;
    std::vector<Sample> buf(kBlock), orig(kBlock);
    double worst = 0.0;
    for (int b = 0; b < 50; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s) {
            orig[s] = static_cast<float>(0.7 * std::sin(2.0 * kPi * 440.0 * (b * kBlock + s) / kFs));
            buf[s]  = orig[s];
        }
        dsp.advance_coeffs();
        dsp.process_tone(0, buf.data(), kBlock);
        for (std::size_t s = 0; s < kBlock; ++s)
            worst = std::max(worst, std::fabs(static_cast<double>(buf[s] - orig[s])));
    }
    check_true("chain: flat chain is bit-transparent", worst == 0.0);
}

void test_chain_hpf() {
    ChannelDsp dsp;
    dsp.configure(48000);
    StripDspParams p;
    p.hpf.enabled = true;
    p.hpf.freq_hz = 200.0f;
    dsp.set_params(p);
    check_true("chain: enabling the HPF marks the strip active", dsp.needs_processing());

    check_near("chain: HPF passes 2 kHz",      chain_response_db(dsp, 0, 2000.0), 0.0, 0.2);
    check_near("chain: HPF is -3 dB at 200 Hz", chain_response_db(dsp, 0, 200.0), -3.01, 0.2);
    check_true("chain: HPF cuts 50 Hz hard",    chain_response_db(dsp, 0, 50.0) < -20.0);
}

void test_chain_lane_independence() {
    // The notes call this out and they are right: one filter's memory shared
    // across lanes would bleed one side of the image into the other.
    ChannelDsp dsp;
    dsp.configure(48000);
    StripDspParams p;
    p.hpf.enabled = true;
    p.hpf.freq_hz = 500.0f;
    dsp.set_params(p);

    constexpr std::size_t kBlock = 256;
    std::vector<Sample> left(kBlock), right(kBlock);
    double right_peak = 0.0;
    for (int b = 0; b < 100; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s) {
            left[s]  = static_cast<float>(std::sin(2.0 * kPi * 3000.0 * (b * kBlock + s) / kFs));
            right[s] = 0.0f;                      // the other side is silent
        }
        dsp.advance_coeffs();
        dsp.process_tone(0, left.data(),  kBlock);
        dsp.process_tone(1, right.data(), kBlock);
        for (float v : right) right_peak = std::max(right_peak, std::fabs((double)v));
    }
    check_true("chain: a silent lane stays silent beside a loud one", right_peak == 0.0);
}

void test_chain_param_handover() {
    // Parameters cross from the control thread to the render thread through a
    // double-buffered slot. If that handover did not work the filter would
    // simply never change, which is a silent failure — the knob moves and the
    // audio does not.
    ChannelDsp dsp;
    dsp.configure(48000);
    const double before = chain_response_db(dsp, 0, 60.0);
    check_near("chain: 60 Hz passes with no filter", before, 0.0, 0.2);

    StripDspParams p;
    p.hpf.enabled = true;
    p.hpf.freq_hz = 400.0f;
    dsp.set_params(p);
    const double after = chain_response_db(dsp, 0, 60.0);
    check_true("chain: publishing new params changes the audio", after < before - 20.0);

    // And back off again — the handover must not be one-way.
    dsp.set_params(StripDspParams{});
    check_near("chain: disabling restores it", chain_response_db(dsp, 0, 60.0), 0.0, 0.2);
}

void test_chain_ramps_out() {
    // Going flat must not drop the chain out of circuit in a single sample.
    // A +12 dB band vanishing instantly is a click, which is exactly what the
    // coefficient ramp exists to prevent — so the chain keeps running for a
    // tail after the last band flattens, long enough to ramp back to unity.
    ChannelDsp dsp;
    dsp.configure(48000);
    while (dsp.needs_processing()) {}            // drain the start-up tail

    StripDspParams boosted;
    boosted.eq[1] = {.enabled = true, .freq_hz = 1000.0f, .gain_db = 12.0f, .q = 1.0f};
    dsp.set_params(boosted);
    check_true("ramp: a boosted chain needs processing", dsp.needs_processing());

    // Flatten it and count how long it keeps being served.
    dsp.set_params(StripDspParams{});
    int tail = 0;
    while (dsp.needs_processing() && tail < 1000) ++tail;
    check_true("ramp: a flattened chain keeps running while it ramps out", tail > 20);
    check_true("ramp: and then stops", tail < 1000);

    // The tail must land on unity, not leave a fraction of the boost behind.
    constexpr std::size_t kBlock = 256;
    std::vector<Sample> buf(kBlock), orig(kBlock);
    double worst = 0.0;
    for (int b = 0; b < 20; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s) {
            orig[s] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * (b * kBlock + s) / kFs));
            buf[s]  = orig[s];
        }
        dsp.advance_coeffs();
        dsp.process_tone(0, buf.data(), kBlock);
        for (std::size_t s = 0; s < kBlock; ++s)
            worst = std::max(worst, std::fabs(static_cast<double>(buf[s] - orig[s])));
    }
    check_true("ramp: settles back to exactly unity", worst < 1e-6);
}

void test_chain_eq_bands_cascade() {
    // Four bands in series, two of them doing something at once.
    ChannelDsp dsp;
    dsp.configure(48000);
    StripDspParams p;
    p.eq[0] = {.enabled = true, .freq_hz = 100.0f,  .gain_db =  6.0f, .q = 1.0f};
    p.eq[3] = {.enabled = true, .freq_hz = 8000.0f, .gain_db = -6.0f, .q = 1.0f};
    dsp.set_params(p);
    check_near("chain: EQ band 1 boosts its centre", chain_response_db(dsp, 0, 100.0),  6.0, 0.3);
    check_near("chain: EQ band 4 cuts its centre",   chain_response_db(dsp, 0, 8000.0), -6.0, 0.3);
    check_near("chain: between the bands is untouched",
               chain_response_db(dsp, 0, 1000.0), 0.0, 0.5);
}

void test_chain_shelving_bands() {
    // The outer bands can be shelves. The distinction that matters is what
    // happens BEYOND the corner: a bell returns to unity, a shelf does not —
    // it holds its gain all the way to the end of the band. Checking only at
    // the corner frequency would pass on a bell, since both sit at half gain
    // there.
    {
        ChannelDsp dsp;
        dsp.configure(48000);
        StripDspParams p;
        p.eq[0] = {.enabled = true, .shelf = true, .low_shelf = true,
                   .freq_hz = 200.0f, .gain_db = 9.0f, .slope = 1.0f};
        dsp.set_params(p);
        check_near("shelf: a low shelf holds its gain below the corner",
                   chain_response_db(dsp, 0, 40.0), 9.0, 0.5);
        check_near("shelf: and lets go above it",
                   chain_response_db(dsp, 0, 2000.0), 0.0, 0.5);
    }
    {
        ChannelDsp dsp;
        dsp.configure(48000);
        StripDspParams p;
        p.eq[3] = {.enabled = true, .shelf = true, .low_shelf = false,
                   .freq_hz = 4000.0f, .gain_db = -9.0f, .slope = 1.0f};
        dsp.set_params(p);
        check_near("shelf: a high shelf holds its cut above the corner",
                   chain_response_db(dsp, 0, 16000.0), -9.0, 0.5);
        check_near("shelf: and lets go below it",
                   chain_response_db(dsp, 0, 400.0), 0.0, 0.5);
    }
    {
        // The same band as a BELL must come back to unity out at the edges.
        // This is what makes the two readings above a shelf test rather than
        // a gain test.
        ChannelDsp dsp;
        dsp.configure(48000);
        StripDspParams p;
        p.eq[0] = {.enabled = true, .shelf = false,
                   .freq_hz = 200.0f, .gain_db = 9.0f, .q = 1.0f};
        dsp.set_params(p);
        check_near("shelf: the same band as a bell returns to unity below",
                   chain_response_db(dsp, 0, 40.0), 0.0, 1.0);
    }
    {
        // A shelf at unity gain is an identity, so it must be out of circuit
        // for the same reason a flat bell is.
        ChannelDsp dsp;
        dsp.configure(48000);
        StripDspParams p;
        p.eq[0] = {.enabled = true, .shelf = true, .low_shelf = true,
                   .freq_hz = 200.0f, .gain_db = 0.0f, .slope = 1.0f};
        dsp.set_params(p);
        check_near("shelf: a shelf at 0 dB is transparent",
                   chain_response_db(dsp, 0, 40.0), 0.0, 0.01);
    }
}

} // namespace

int main() {
    std::printf("== biquad ==\n");
    test_highpass();
    test_lowpass();
    test_peaking();
    test_shelves();
    test_measured_matches_analytic();
    test_denormal_flush();
    test_nyquist_clamp();
    test_stability();
    std::printf("\n== channel strip chain ==\n");
    test_chain_transparency();
    test_chain_hpf();
    test_chain_lane_independence();
    test_chain_param_handover();
    test_chain_ramps_out();
    test_chain_eq_bands_cascade();
    test_chain_shelving_bands();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
