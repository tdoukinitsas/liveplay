// ============================================================================
// meter_test.cpp — standalone assertions for the Meter DSP.
// ----------------------------------------------------------------------------
// No test framework: each check prints PASS/FAIL and the binary exits non-zero
// if anything failed. Build via the LIVEPLAY_BUILD_TESTS CMake option:
//     cmake -DLIVEPLAY_BUILD_TESTS=ON .. && cmake --build . --target liveplay-meter-tests
//     ./liveplay-meter-tests
//
// Covers:
//   1. Sine level accuracy      — peak / RMS of a -20 dBFS sine
//   2. Lossless max-since-read  — single-sample impulse between reads;
//                                 consuming read resets the accumulator
//   3. Ballistics               — release follows the configured time constant
//   4. True peak                — fs/4 @ 45° intersample case reads ≈ 0 dBTP
//                                 while sample peak reads ≈ -3.01 dBFS
//   5. K-weighting              — 997 Hz stereo pair at -23 dBFS reads
//                                 -23 LUFS momentary; 100 Hz reads lower;
//                                 kw_ms is 0 when loudness is disabled
// ============================================================================
#include "liveplay/audio/meter.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace liveplay::audio;

namespace {

int g_failures = 0;

void check(bool ok, const char* name, double got, double expect, double tol) {
    std::printf("%-52s %s  (got %+8.3f, expect %+8.3f ±%.3f)\n",
                name, ok ? "PASS" : "FAIL", got, expect, tol);
    if (!ok) ++g_failures;
}
void check_near(const char* name, double got, double expect, double tol) {
    check(std::fabs(got - expect) <= tol, name, got, expect, tol);
}
void check_less(const char* name, double got, double bound) {
    check(got < bound, name, got, bound, 0.0);
}

constexpr SampleRate kFs = 48'000;
constexpr double     kPi = 3.14159265358979323846;

std::vector<Sample> make_sine(double freq, double amp, double seconds,
                              double phase = 0.0) {
    std::vector<Sample> v(static_cast<std::size_t>(seconds * kFs));
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<Sample>(amp * std::sin(2.0 * kPi * freq * i / kFs + phase));
    }
    return v;
}

void push_in_blocks(Meter& m, const std::vector<Sample>& v, std::size_t block = 256) {
    for (std::size_t off = 0; off < v.size(); off += block) {
        m.push_block(v.data() + off, std::min(block, v.size() - off));
    }
}

// ---------------------------------------------------------------------------
void test_sine_levels() {
    Meter m;
    m.configure(kFs);   // defaults: 1 ms attack / 300 ms release / 300 ms RMS
    const auto sine = make_sine(997.0, 0.1, 2.0);   // -20 dBFS peak
    push_in_blocks(m, sine);
    const auto s = m.snapshot();
    check_near("sine: ballistic peak ≈ -20 dBFS",   s.peak_db,     -20.0,  0.2);
    check_near("sine: RMS ≈ -23.01 dBFS",           s.rms_db,      -23.01, 0.2);
    check_near("sine: raw max ≈ -20 dBFS",          s.peak_max_db, -20.0,  0.05);
}

// ---------------------------------------------------------------------------
void test_lossless_max() {
    Meter m;
    m.configure(kFs, 0.0f, 300.0f, 300.0f);
    // 100 ms of silence with ONE full-scale sample in the middle.
    std::vector<Sample> v(4800, 0.0f);
    v[2400] = 1.0f;
    push_in_blocks(m, v);
    // ~50 ms of release decay follow the impulse: with the 300 ms release
    // the ballistic peak should sit ≈ 8.686·(50/300) ≈ -1.45 dB down, while
    // the raw max must still report the impulse at full scale.
    auto s = m.snapshot_consume_max();
    check_near("impulse: raw max caught at 0 dBFS",   s.peak_max_db, 0.0,   0.01);
    check_near("impulse: ballistic peak ≈ -1.45 dB", s.peak_db,     -1.45, 0.3);
    // The consuming read must have reset the accumulator.
    push_in_blocks(m, std::vector<Sample>(256, 0.0f));
    s = m.snapshot();
    check_near("impulse: max reset after consume",  s.peak_max_db, -120.0, 0.01);
}

// ---------------------------------------------------------------------------
void test_ballistics() {
    // One-pole release: after exactly one time constant the envelope should
    // have fallen to 1/e of its start ≈ -8.686 dB.
    Meter m;
    m.configure(kFs, 0.0f, 200.0f, 300.0f);
    const auto burst = make_sine(997.0, 1.0, 0.5);
    push_in_blocks(m, burst);
    const float start_db = m.snapshot().peak_db;
    push_in_blocks(m, std::vector<Sample>(9600, 0.0f));   // 200 ms silence
    const float after_db = m.snapshot().peak_db;
    check_near("release: -8.686 dB after 1 time constant",
               after_db - start_db, -8.686, 0.8);
}

// ---------------------------------------------------------------------------
void test_true_peak() {
    Meter m;
    m.configure(kFs, 0.0f, 300.0f, 300.0f);
    m.set_true_peak_enabled(true);
    // fs/4 sine with 45° phase: every sample lands at ±1/√2 (-3.01 dBFS) but
    // the continuous waveform peaks at 1.0 (0 dBTP).
    const auto isp = make_sine(kFs / 4.0, 1.0, 1.0, kPi / 4.0);
    push_in_blocks(m, isp);
    const auto s = m.snapshot();
    check_near("ISP: sample peak ≈ -3.01 dBFS", s.peak_max_db,      -3.01, 0.1);
    check_near("ISP: true peak ≈ 0 dBTP",       s.true_peak_max_db,  0.0,  0.4);

    // Disabled → true peak mirrors sample peak.
    Meter m2;
    m2.configure(kFs);
    push_in_blocks(m2, isp);
    const auto s2 = m2.snapshot();
    check_near("ISP: TP mirrors sample peak when disabled",
               s2.true_peak_max_db - s2.peak_max_db, 0.0, 0.001);
}

// ---------------------------------------------------------------------------
void test_k_weighting() {
    // Stereo 997 Hz at -23 dBFS per channel → -23.0 LUFS momentary
    // (the -0.691 offset cancels the K-filter gain at 997 Hz by design).
    auto lufs = [](double kw_sum) {
        return kw_sum <= 1e-12 ? -120.0 : -0.691 + 10.0 * std::log10(kw_sum);
    };
    const double amp = std::pow(10.0, -23.0 / 20.0);

    Meter l, r;
    l.configure(kFs); r.configure(kFs);
    l.set_loudness_enabled(true); r.set_loudness_enabled(true);
    const auto sine = make_sine(997.0, amp, 1.0);
    push_in_blocks(l, sine); push_in_blocks(r, sine);
    const double got = lufs(static_cast<double>(l.snapshot().kw_ms) +
                            static_cast<double>(r.snapshot().kw_ms));
    check_near("997 Hz stereo @ -23 dBFS ≈ -23.0 LUFS", got, -23.0, 0.15);

    // 100 Hz at equal amplitude must read lower (RLB high-pass ≈ -1.8 dB).
    Meter l2, r2;
    l2.configure(kFs); r2.configure(kFs);
    l2.set_loudness_enabled(true); r2.set_loudness_enabled(true);
    const auto low = make_sine(100.0, amp, 1.0);
    push_in_blocks(l2, low); push_in_blocks(r2, low);
    const double got_low = lufs(static_cast<double>(l2.snapshot().kw_ms) +
                                static_cast<double>(r2.snapshot().kw_ms));
    check_near("100 Hz reads ≈ -1.8 LU below 997 Hz", got_low - got, -1.8, 0.6);

    // Disabled → kw_ms stays 0.
    Meter off;
    off.configure(kFs);
    push_in_blocks(off, sine);
    check_near("kw_ms is 0 when loudness disabled",
               static_cast<double>(off.snapshot().kw_ms), 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
void test_short_term_loudness() {
    auto lufs = [](double kw_sum) {
        return kw_sum <= 1e-12 ? -120.0 : -0.691 + 10.0 * std::log10(kw_sum);
    };
    const double amp = std::pow(10.0, -23.0 / 20.0);

    // Steady state: momentary (400 ms) and short-term (3 s) must agree.
    Meter m;
    m.configure(kFs);
    m.set_loudness_enabled(true);
    push_in_blocks(m, make_sine(997.0, amp, 4.0));
    auto s = m.snapshot();
    const double lm = lufs(2.0 * s.kw_ms);     // stereo pair of identical chans
    const double ls = lufs(2.0 * s.kw_ms_s);
    check_near("steady state: S ≈ M", ls - lm, 0.0, 0.1);

    // Dynamics: 2 s of tone then 600 ms of silence. The 400 ms momentary
    // window is now all-silence (→ -∞) while the 3 s short-term window still
    // integrates the tone (2 s tone / 2.6 s total ≈ -1.1 LU below steady).
    Meter d;
    d.configure(kFs);
    d.set_loudness_enabled(true);
    push_in_blocks(d, make_sine(997.0, amp, 2.0));
    push_in_blocks(d, std::vector<Sample>(static_cast<std::size_t>(0.6 * kFs), 0.0f));
    s = d.snapshot();
    const double dm = lufs(2.0 * s.kw_ms);
    const double ds = lufs(2.0 * s.kw_ms_s);
    check_less("after silence: momentary fell silent", dm, -60.0);
    check_near("after silence: short-term still integrating tone",
               ds, -23.0 + 10.0 * std::log10(2.0 / 2.6), 0.5);
}

} // namespace

int main() {
    std::printf("liveplay meter tests (fs = %u)\n", kFs);
    std::printf("--------------------------------------------------------------------\n");
    test_sine_levels();
    test_lossless_max();
    test_ballistics();
    test_true_peak();
    test_k_weighting();
    test_short_term_loudness();
    std::printf("--------------------------------------------------------------------\n");
    std::printf("%s (%d failure%s)\n",
                g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
