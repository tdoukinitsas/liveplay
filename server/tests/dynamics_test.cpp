// ============================================================================
// dynamics_test.cpp — the channel strip's expander / gate and compressor.
//
//     cmake -DLIVEPLAY_BUILD_TESTS=ON .. && cmake --build . --target liveplay-dynamics-tests
//     ./liveplay-dynamics-tests
//
// These are behavioural rather than arithmetic: loud material passes, quiet
// material is attenuated by the amount the ratio implies, the two lanes always
// move together, and the gate does not chatter on a signal sitting on its
// threshold. A test that only checked the static curve would pass on a gate
// that stuttered its way through every show.
//
// The compressor's are the same in spirit, with one addition: several of them
// exist to catch a processor that is running and doing the WRONG thing — the
// attack and release the wrong way round, the knee applied on the wrong side of
// the threshold, makeup folded into the gain reduction meter. Each of those
// produces a compressor that meters plausibly and sounds wrong.
// ============================================================================
#include "liveplay/audio/dynamics.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace liveplay::audio;

namespace {

int g_failures = 0;

void check_near(const char* name, double got, double expect, double tol) {
    const bool ok = std::fabs(got - expect) <= tol;
    std::printf("%-60s %s  (got %+9.3f, expect %+9.3f ±%.3f)\n",
                name, ok ? "PASS" : "FAIL", got, expect, tol);
    if (!ok) ++g_failures;
}
void check_true(const char* name, bool ok) {
    std::printf("%-60s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

constexpr double kFs = 48000.0;
constexpr std::size_t kBlock = 256;

double db_to_lin(double db) { return std::pow(10.0, db / 20.0); }
double lin_to_db(double v)  { return v <= 0.0 ? -200.0 : 20.0 * std::log10(v); }

// Run a steady sine at `level_db` through the gate for `ms`, and report the
// output level of the final block once the envelope has settled.
struct Run {
    double out_db;      // level of the last block
    double gr_db;       // gain reduction the gate reported
};

Run run_tone(GateState& st, const GateCoeffs& c, double level_db, double ms,
             ChannelCount lanes = 2, double freq = 1000.0) {
    const int blocks = static_cast<int>((ms / 1000.0) * kFs / kBlock);
    std::vector<Sample> l(kBlock), r(kBlock);
    Sample* bufs[2] = {l.data(), r.data()};
    const double amp = db_to_lin(level_db);
    double peak = 0.0;
    int n = 0;
    for (int b = 0; b < blocks; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            const auto v = static_cast<Sample>(amp * std::sin(2.0 * kPi * freq * n / kFs));
            l[s] = v; r[s] = v;
        }
        st.process(c, bufs, lanes, kBlock);
        if (b == blocks - 1) {
            peak = 0.0;
            for (std::size_t s = 0; s < kBlock; ++s) peak = std::max(peak, std::fabs((double)l[s]));
        }
    }
    return {lin_to_db(peak), st.gain_reduction_db()};
}

// ---------------------------------------------------------------------------
void test_passes_loud_material() {
    GateParams p;
    p.enabled = true;
    p.threshold_db = -40.0f;
    p.ratio = 4.0f;
    p.range_db = -40.0f;
    p.attack_ms = 1.0f;
    p.release_ms = 100.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    const auto r = run_tone(st, c, -12.0, 500.0);
    check_near("gate: material above the threshold passes untouched", r.out_db, -12.0, 0.1);
    check_near("gate: and reports no reduction", r.gr_db, 0.0, 0.05);
    check_true("gate: reports itself open", st.is_open());
}

void test_expands_quiet_material() {
    // At ratio 4, every dB below the threshold costs 3 more. A tone 10 dB
    // below a -40 dB threshold should come out 30 dB down, not 7.5 dB down as
    // the notes' compression formula would give.
    GateParams p;
    p.enabled = true;
    p.threshold_db = -40.0f;
    p.ratio = 4.0f;
    p.range_db = -60.0f;       // deep enough not to clamp the answer
    p.attack_ms = 1.0f;
    p.release_ms = 20.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    const auto r = run_tone(st, c, -50.0, 1500.0);
    check_near("gate: 10 dB under at 4:1 expands by 30 dB", r.out_db, -80.0, 1.5);

    // The notes' formula would have produced this instead. Named so the
    // difference is on the record rather than implied.
    check_true("gate: NOT the compression law from the notes (-57.5 dB)",
               std::fabs(r.out_db - (-57.5)) > 10.0);
}

void test_range_limits_the_attenuation() {
    // Range is what stops a gate slamming to silence. With it set to -12 the
    // deepest cut must be 12 dB however far below the threshold the signal is.
    GateParams p;
    p.enabled = true;
    p.threshold_db = -30.0f;
    p.ratio = 20.0f;
    p.range_db = -12.0f;
    p.attack_ms = 1.0f;
    p.release_ms = 20.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    const auto r = run_tone(st, c, -70.0, 1500.0);
    check_near("gate: range floors the attenuation at 12 dB", r.out_db, -82.0, 1.0);
    check_near("gate: and the meter agrees", r.gr_db, -12.0, 0.5);
}

void test_ratio_one_is_transparent() {
    GateParams p;
    p.enabled = true;
    p.threshold_db = -20.0f;
    p.ratio = 1.0f;             // 1:1 — no expansion at all
    p.range_db = -40.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    const auto r = run_tone(st, c, -60.0, 500.0);
    check_near("gate: ratio 1:1 passes even far below the threshold", r.out_db, -60.0, 0.1);
}

void test_stereo_is_linked() {
    // One lane loud, the other silent. A gate keyed per lane would open on the
    // loud side and stay shut on the quiet one; linked, both see the same gain
    // and the quiet lane stays exactly silent rather than being gated
    // separately. The real symptom of getting this wrong is the image lurching
    // sideways, which is what this is really testing for.
    GateParams p;
    p.enabled = true;
    p.threshold_db = -40.0f;
    p.ratio = 10.0f;
    p.range_db = -60.0f;
    p.attack_ms = 1.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    std::vector<Sample> l(kBlock), r(kBlock);
    Sample* bufs[2] = {l.data(), r.data()};
    const double amp = db_to_lin(-6.0);
    int n = 0;
    double left_peak = 0.0, right_peak = 0.0;
    for (int b = 0; b < 200; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            l[s] = static_cast<Sample>(amp * std::sin(2.0 * kPi * 1000.0 * n / kFs));
            r[s] = 0.0f;
        }
        st.process(c, bufs, 2, kBlock);
        if (b == 199) {
            for (std::size_t s = 0; s < kBlock; ++s) {
                left_peak  = std::max(left_peak,  std::fabs((double)l[s]));
                right_peak = std::max(right_peak, std::fabs((double)r[s]));
            }
        }
    }
    check_near("link: the loud lane is passed", lin_to_db(left_peak), -6.0, 0.2);
    check_true("link: the silent lane stays silent", right_peak == 0.0);
    check_true("link: one loud lane holds the gate open for both", st.is_open());
}

void test_hysteresis_stops_chatter() {
    // A level sitting exactly on the threshold is the classic chatter case.
    // Without hysteresis the gate crosses back and forth constantly; with it,
    // once open the level has to fall a few dB before it lets go.
    GateParams p;
    p.enabled = true;
    p.threshold_db = -30.0f;
    p.ratio = 10.0f;
    p.range_db = -40.0f;
    p.attack_ms = 1.0f;
    p.hold_ms = 0.0f;           // hold off, so hysteresis alone is on trial
    p.release_ms = 20.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    // Open it first, then sit 1 dB below the threshold — inside the hysteresis
    // window, where a single-threshold gate would immediately start closing.
    run_tone(st, c, -10.0, 200.0);
    const auto r = run_tone(st, c, -31.0, 400.0);
    check_true("hysteresis: stays open just below the threshold", st.is_open());
    check_near("hysteresis: and passes that material untouched", r.out_db, -31.0, 0.3);

    // Drop well past the close threshold and it must let go.
    const auto shut = run_tone(st, c, -50.0, 800.0);
    check_true("hysteresis: closes once the level really falls", !st.is_open());
    check_true("hysteresis: and attenuates once closed", shut.out_db < -60.0);
}

void test_hold_keeps_it_open() {
    // Hold is what stops a gate chopping up the gaps inside a decaying sound.
    GateParams p;
    p.enabled = true;
    p.threshold_db = -30.0f;
    p.ratio = 10.0f;
    p.range_db = -40.0f;
    p.attack_ms = 1.0f;
    p.release_ms = 5.0f;
    p.hold_ms = 250.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    run_tone(st, c, -10.0, 200.0);            // open it
    // 100 ms of silence: well inside the hold, so it must still be open.
    run_tone(st, c, -90.0, 100.0);
    check_true("hold: still open 100 ms into a 250 ms hold", st.is_open());
    // Past the hold, it closes.
    run_tone(st, c, -90.0, 400.0);
    check_true("hold: closed once the hold has run out", !st.is_open());
}

void test_attack_and_release_are_the_right_way_round() {
    // On a gate, attack opens and release closes — the opposite of a
    // compressor. A fast attack with a slow release must therefore open
    // promptly and close slowly.
    GateParams p;
    p.enabled = true;
    p.threshold_db = -30.0f;
    p.ratio = 10.0f;
    p.range_db = -40.0f;
    p.attack_ms  = 0.5f;
    p.release_ms = 400.0f;
    p.hold_ms = 0.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    // From closed, 20 ms of loud material must already be through: 40 attack
    // time constants.
    const auto opened = run_tone(st, c, -6.0, 20.0);
    check_near("timing: a 0.5 ms attack is open within 20 ms", opened.out_db, -6.0, 0.5);

    // Then 20 ms of silence, which is a twentieth of the release: it must have
    // barely moved, not slammed shut.
    const auto closing = run_tone(st, c, -90.0, 20.0);
    check_true("timing: a 400 ms release has barely closed after 20 ms",
               closing.gr_db > -6.0);
}

void test_disabled_is_transparent() {
    GateParams p;                 // enabled defaults to false
    p.threshold_db = -10.0f;      // would gate almost everything if it ran
    p.ratio = 20.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    std::vector<Sample> l(kBlock), r(kBlock), orig(kBlock);
    Sample* bufs[2] = {l.data(), r.data()};
    double worst = 0.0;
    int n = 0;
    for (int b = 0; b < 20; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            orig[s] = static_cast<Sample>(0.01 * std::sin(2.0 * kPi * 1000.0 * n / kFs));
            l[s] = orig[s]; r[s] = orig[s];
        }
        st.process(c, bufs, 2, kBlock);
        for (std::size_t s = 0; s < kBlock; ++s)
            worst = std::max(worst, std::fabs((double)(l[s] - orig[s])));
    }
    check_true("disabled: bit-transparent", worst == 0.0);
    check_near("disabled: meter reads zero", st.gain_reduction_db(), 0.0, 0.0);
}

void test_mono_strip() {
    // A mono strip has one lane. The detector must not read the idle lane and
    // conclude there is nothing there.
    GateParams p;
    p.enabled = true;
    p.threshold_db = -40.0f;
    p.ratio = 4.0f;
    p.range_db = -40.0f;
    const auto c = gate_coeffs(p, kFs);

    GateState st; st.reset();
    const auto r = run_tone(st, c, -12.0, 500.0, /*lanes=*/1);
    check_near("mono: a single-lane strip passes its material", r.out_db, -12.0, 0.1);
    check_true("mono: and the gate is open", st.is_open());
}

// ---------------------------------------------------------------------------
// Compressor / limiter
// ---------------------------------------------------------------------------

// Same shape as run_tone, for the other processor.
Run run_comp(CompressorState& st, const CompressorCoeffs& c, double level_db,
             double ms, ChannelCount lanes = 2, double freq = 1000.0) {
    const int blocks = static_cast<int>((ms / 1000.0) * kFs / kBlock);
    std::vector<Sample> l(kBlock), r(kBlock);
    Sample* bufs[2] = {l.data(), r.data()};
    const double amp = db_to_lin(level_db);
    double peak = 0.0;
    // Phase restarts at a zero crossing on every call, so a level change
    // between calls is a change of amplitude and not also a step the peak
    // detector would read as a transient.
    int n = 0;
    for (int b = 0; b < blocks; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            const auto v = static_cast<Sample>(amp * std::sin(2.0 * kPi * freq * n / kFs));
            l[s] = v; r[s] = v;
        }
        st.process(c, bufs, lanes, kBlock);
        if (b == blocks - 1) {
            peak = 0.0;
            for (std::size_t s = 0; s < kBlock; ++s) peak = std::max(peak, std::fabs((double)l[s]));
        }
    }
    return {lin_to_db(peak), st.gain_reduction_db()};
}

// A workable starting point for most of these: hard knee, so the arithmetic
// under test is the ratio alone.
CompressorParams comp_base() {
    CompressorParams p;
    p.enabled      = true;
    p.threshold_db = -20.0f;
    p.ratio        = 4.0f;
    p.knee_db      = 0.0f;
    p.attack_ms    = 5.0f;
    p.release_ms   = 100.0f;
    p.makeup_db    = 0.0f;
    return p;
}

void test_comp_passes_below_threshold() {
    const auto c = compressor_coeffs(comp_base(), kFs);
    CompressorState st; st.reset();
    const auto r = run_comp(st, c, -40.0, 500.0);
    check_near("comp: material below the threshold passes untouched", r.out_db, -40.0, 0.1);
    check_near("comp: and reports no reduction", r.gr_db, 0.0, 0.05);
}

void test_comp_ratio_above_threshold() {
    // 10 dB over a -20 dB threshold at 4:1 comes out 2.5 dB over, which is
    // 7.5 dB of reduction. The gate's law would give 30.
    const auto c = compressor_coeffs(comp_base(), kFs);
    CompressorState st; st.reset();
    const auto r = run_comp(st, c, -10.0, 1000.0);
    check_near("comp: 10 dB over at 4:1 is reduced by 7.5 dB", r.out_db, -17.5, 0.4);
    check_near("comp: and the meter agrees", r.gr_db, -7.5, 0.5);
}

void test_comp_high_ratio_holds_the_line() {
    // At 60:1 the output should barely move above the threshold however hard it
    // is driven — the limiter end of the same control.
    auto p = comp_base();
    p.ratio     = 60.0f;
    p.attack_ms = 1.0f;
    const auto c = compressor_coeffs(p, kFs);
    CompressorState st; st.reset();
    const auto quiet = run_comp(st, c, -14.0, 800.0);
    const auto loud  = run_comp(st, c,  -2.0, 800.0);
    check_true("limiter: 12 dB more input moves the output under 0.5 dB",
               std::fabs(loud.out_db - quiet.out_db) < 0.5);
    check_near("limiter: and it sits just above the threshold", loud.out_db, -19.7, 0.5);
}

void test_comp_soft_knee() {
    // The knee is the whole difference between clinical and musical, and it is
    // the one thing the notes leave out. With a 12 dB knee the compressor is
    // already working 6 dB BELOW the threshold, gently, where a hard knee does
    // nothing at all until the threshold and then applies the full ratio.
    auto p = comp_base();
    p.knee_db = 12.0f;          // so the knee spans -26 dB to -14 dB
    const auto c = compressor_coeffs(p, kFs);

    // Just inside the bottom of the knee. Deliberately 1 dB in rather than on
    // the edge at -26: the edge is excluded by the comparison, so a test there
    // never enters the knee arithmetic at all and passes on a knee centred in
    // the wrong place. 5 dB under the threshold the curve has only just left
    // unity — 0.03 dB — which is the continuity the knee exists to give.
    {
        CompressorState st; st.reset();
        const auto r = run_comp(st, c, -25.0, 800.0);
        check_near("knee: all but transparent just inside the bottom of the knee",
                   r.out_db, -25.03, 0.15);
    }
    // And 3 dB under the threshold, where a hard knee is still doing nothing
    // and this is already easing in: 0.75 * 3^2 / 24.
    {
        CompressorState st; st.reset();
        const auto r = run_comp(st, c, -23.0, 800.0);
        check_near("knee: already working 3 dB BELOW the threshold",
                   r.out_db, -23.28, 0.25);
    }
    // Exactly on the threshold, where a hard knee is still doing nothing:
    // slope * (W/2)^2 / 2W = 0.75 * 36 / 24 = 1.125 dB.
    {
        CompressorState st; st.reset();
        const auto r = run_comp(st, c, -20.0, 800.0);
        check_near("knee: 1.125 dB of reduction at the threshold itself",
                   r.out_db, -21.125, 0.4);
    }
    // Top of the knee, where it must have arrived at the full ratio: 6 dB over
    // at 4:1 is 4.5 dB down.
    {
        CompressorState st; st.reset();
        const auto r = run_comp(st, c, -14.0, 800.0);
        check_near("knee: full ratio by the top of the knee", r.out_db, -18.5, 0.4);
    }
    // And a hard knee at the same threshold does nothing there, which is what
    // makes the reading above a knee test rather than a threshold test.
    {
        const auto hard = compressor_coeffs(comp_base(), kFs);
        CompressorState st; st.reset();
        const auto r = run_comp(st, hard, -20.0, 800.0);
        check_near("knee: a HARD knee is transparent at the threshold",
                   r.out_db, -20.0, 0.15);
    }
}

void test_comp_makeup_gain() {
    auto p = comp_base();
    p.makeup_db = 6.0f;
    const auto c = compressor_coeffs(p, kFs);
    CompressorState st; st.reset();
    // Below the threshold, so makeup is the only thing acting.
    const auto r = run_comp(st, c, -40.0, 500.0);
    check_near("makeup: +6 dB lifts material the compressor is not touching",
               r.out_db, -34.0, 0.1);
    // The GR meter answers "how hard is it working". Folding makeup into it
    // would show an idle compressor as though it were pushing 6 dB.
    check_near("makeup: and stays out of the gain reduction meter", r.gr_db, 0.0, 0.05);
}

void test_comp_ratio_one_with_makeup_is_a_gain_stage() {
    auto p = comp_base();
    p.ratio     = 1.0f;
    p.makeup_db = 6.0f;
    const auto c = compressor_coeffs(p, kFs);
    CompressorState st; st.reset();
    const auto r = run_comp(st, c, -10.0, 500.0);
    check_near("comp: 1:1 with makeup is a clean gain stage", r.out_db, -4.0, 0.1);
}

void test_comp_attack_and_release_are_the_right_way_round() {
    // The opposite way round from the gate: attack is the gain coming DOWN.
    // A fast attack with a slow release must therefore clamp promptly and then
    // recover slowly. Swapping them gives a processor that runs, meters, and
    // lets every transient through before easing the level down afterwards.
    auto p = comp_base();
    p.attack_ms  = 1.0f;
    p.release_ms = 1000.0f;
    const auto c = compressor_coeffs(p, kFs);

    CompressorState st; st.reset();
    // 40 ms of material 14 dB over the threshold: 40 attack time constants, so
    // the full 10.5 dB should already be applied.
    const auto clamped = run_comp(st, c, -6.0, 40.0);
    check_near("timing: a 1 ms attack is fully clamped within 40 ms",
               clamped.gr_db, -10.5, 0.6);

    // Then 60 ms below the threshold, a sixteenth of the release: it must have
    // barely recovered, not sprung back.
    const auto releasing = run_comp(st, c, -40.0, 60.0);
    check_true("timing: a 1000 ms release has barely recovered after 60 ms",
               releasing.gr_db < -8.0);
}

void test_comp_stereo_is_linked() {
    // One lane loud, the other silent. Keyed per lane the loud side would be
    // compressed and the quiet side left alone, so the image would pull toward
    // the quiet side on every loud note.
    auto p = comp_base();
    p.attack_ms = 1.0f;
    const auto c = compressor_coeffs(p, kFs);

    CompressorState st; st.reset();
    std::vector<Sample> l(kBlock), r(kBlock);
    Sample* bufs[2] = {l.data(), r.data()};
    const double amp = db_to_lin(-10.0);
    int n = 0;
    double left_peak = 0.0, right_peak = 0.0;
    for (int b = 0; b < 200; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            l[s] = static_cast<Sample>(amp * std::sin(2.0 * kPi * 1000.0 * n / kFs));
            r[s] = 0.0f;
        }
        st.process(c, bufs, 2, kBlock);
        if (b == 199) {
            for (std::size_t s = 0; s < kBlock; ++s) {
                left_peak  = std::max(left_peak,  std::fabs((double)l[s]));
                right_peak = std::max(right_peak, std::fabs((double)r[s]));
            }
        }
    }
    check_near("link: the loud lane is compressed by the ratio",
               lin_to_db(left_peak), -17.5, 0.4);
    check_true("link: the silent lane stays silent", right_peak == 0.0);
    check_near("link: one figure of reduction for the strip",
               st.gain_reduction_db(), -7.5, 0.5);
}

void test_comp_disabled_is_transparent() {
    CompressorParams p;             // enabled defaults to false
    p.threshold_db = -60.0f;        // would squash everything if it ran
    p.ratio        = 20.0f;
    p.makeup_db    = 12.0f;         // and would be audible on its own
    const auto c = compressor_coeffs(p, kFs);

    CompressorState st; st.reset();
    std::vector<Sample> l(kBlock), r(kBlock), orig(kBlock);
    Sample* bufs[2] = {l.data(), r.data()};
    double worst = 0.0;
    int n = 0;
    for (int b = 0; b < 20; ++b) {
        for (std::size_t s = 0; s < kBlock; ++s, ++n) {
            orig[s] = static_cast<Sample>(0.25 * std::sin(2.0 * kPi * 1000.0 * n / kFs));
            l[s] = orig[s]; r[s] = orig[s];
        }
        st.process(c, bufs, 2, kBlock);
        for (std::size_t s = 0; s < kBlock; ++s)
            worst = std::max(worst, std::fabs((double)(l[s] - orig[s])));
    }
    check_true("comp disabled: bit-transparent", worst == 0.0);
    check_near("comp disabled: meter reads zero", st.gain_reduction_db(), 0.0, 0.0);
}

void test_comp_mono_strip() {
    const auto c = compressor_coeffs(comp_base(), kFs);
    CompressorState st; st.reset();
    const auto r = run_comp(st, c, -10.0, 1000.0, /*lanes=*/1);
    check_near("comp mono: a single-lane strip is compressed the same",
               r.out_db, -17.5, 0.4);
}

} // namespace

int main() {
    std::printf("== expander / gate ==\n");
    test_passes_loud_material();
    test_expands_quiet_material();
    test_range_limits_the_attenuation();
    test_ratio_one_is_transparent();
    test_stereo_is_linked();
    test_hysteresis_stops_chatter();
    test_hold_keeps_it_open();
    test_attack_and_release_are_the_right_way_round();
    test_disabled_is_transparent();
    test_mono_strip();

    std::printf("\n== compressor / limiter ==\n");
    test_comp_passes_below_threshold();
    test_comp_ratio_above_threshold();
    test_comp_high_ratio_holds_the_line();
    test_comp_soft_knee();
    test_comp_makeup_gain();
    test_comp_ratio_one_with_makeup_is_a_gain_stage();
    test_comp_attack_and_release_are_the_right_way_round();
    test_comp_stereo_is_linked();
    test_comp_disabled_is_transparent();
    test_comp_mono_strip();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
