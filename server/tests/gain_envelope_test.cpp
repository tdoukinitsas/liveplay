// ============================================================================
// gain_envelope_test.cpp — a cue's volume automation (2.6).
//
//     cmake -DLIVEPLAY_BUILD_TESTS=ON .. && cmake --build . --target liveplay-envelope-tests
//     ./liveplay-envelope-tests
//
// Pins the rules the client's waveform drawing mirrors
// (client/app/utils/volumeAutomation.ts): linear-in-dB segments, holds before
// the first and after the last keyframe, -60 dB and below is silence, and
// apply() lands on the same numbers as gain_at() at every position.
// ============================================================================
#include "liveplay/audio/gain_envelope.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace liveplay::audio;

namespace {

int g_failures = 0;

void check_near(const char* name, double got, double expect, double tol) {
    const bool ok = std::fabs(got - expect) <= tol;
    std::printf("%-62s %s  (got %+10.5f, expect %+10.5f ±%.5f)\n",
                name, ok ? "PASS" : "FAIL", got, expect, tol);
    if (!ok) ++g_failures;
}
void check_true(const char* name, bool ok) {
    std::printf("%-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

GainEnvelope make(std::initializer_list<GainEnvelope::Point> pts) {
    GainEnvelope e;
    e.points = pts;
    e.normalise();
    return e;
}

void test_empty_is_unity() {
    GainEnvelope e;
    check_near("empty: 0 dB everywhere", e.db_at(12345), 0.0, 1e-9);
    check_near("empty: unity gain", e.gain_at(0), 1.0, 1e-9);
}

void test_holds_outside_the_keyframes() {
    const auto e = make({{1000, -6.0f}, {2000, -12.0f}});
    check_near("hold: before the first keyframe = its level", e.db_at(0), -6.0, 1e-6);
    check_near("hold: after the last keyframe = its level", e.db_at(999999), -12.0, 1e-6);
}

void test_linear_in_db() {
    const auto e = make({{0, 0.0f}, {1000, -20.0f}});
    check_near("segment: halfway is -10 dB (linear in dB)", e.db_at(500), -10.0, 1e-4);
    check_near("segment: halfway gain is 10^(-10/20)", e.gain_at(500),
               std::pow(10.0, -0.5), 1e-5);
    check_near("segment: quarter is -5 dB", e.db_at(250), -5.0, 1e-4);
}

void test_floor_is_silence() {
    const auto e = make({{0, 0.0f}, {1000, kEnvelopeFloorDb}});
    check_near("floor: the keyframe itself is exact silence", e.gain_at(1000), 0.0, 0.0);
    check_near("floor: halfway interpolates against -60 (-30 dB)", e.db_at(500), -30.0, 1e-4);
    const auto below = make({{0, -200.0f}});
    check_near("floor: levels below the floor clamp to it", below.db_at(0), kEnvelopeFloorDb, 1e-6);
    check_near("floor: and are silent", below.gain_at(0), 0.0, 0.0);
}

void test_normalise() {
    const auto e = make({{2000, -3.0f}, {0, 0.0f}, {1000, 50.0f}, {1000, -6.0f}});
    check_true("normalise: sorted, duplicate frame collapsed (3 points)", e.points.size() == 3);
    check_near("normalise: later duplicate wins", e.db_at(1000), -6.0, 1e-6);
    const auto hot = make({{0, 50.0f}});
    check_near("normalise: clamped to +10 dB", hot.db_at(0), kEnvelopeMaxDb, 1e-6);
    check_true("unity: all-0-dB envelope reports unity", make({{0, 0.0f}, {9, 0.0f}}).is_unity());
    check_true("unity: a dip is not unity", !e.is_unity());
}

void test_apply_matches_gain_at() {
    // Stereo DC of 1.0, so the output IS the gain applied.
    const auto e = make({{100, 0.0f}, {600, -24.0f}, {700, kEnvelopeFloorDb}, {900, 6.0f}});
    const std::size_t frames = 1000, ch = 2;
    std::vector<Sample> buf(frames * ch, 1.0f);
    e.apply(buf.data(), frames, ch, 0);
    double worst = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double want = e.gain_at(i);
        worst = std::max(worst, std::fabs(buf[i * ch] - want));
        worst = std::max(worst, std::fabs(buf[i * ch + 1] - want));
    }
    // Geometric stepping is the dB curve itself, re-seeded at every keyframe:
    // the only difference from gain_at() is float rounding, even on a ramp as
    // steep as -60 -> +6 dB in 200 frames.
    check_near("apply: matches gain_at() at every frame", worst, 0.0, 1e-4);

    // The offset is a source position: applying at 500 reads the envelope at 500+.
    std::vector<Sample> one(ch, 1.0f);
    e.apply(one.data(), 1, ch, 650);
    check_near("apply: start_frame is a source-file position", one[0], e.gain_at(650), 1e-4);
}

} // namespace

int main() {
    test_empty_is_unity();
    test_holds_outside_the_keyframes();
    test_linear_in_db();
    test_floor_is_silence();
    test_normalise();
    test_apply_matches_gain_at();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
