// ============================================================================
// mixer_channel_test.cpp — standalone assertions for MixerChannel gain/fades.
// ----------------------------------------------------------------------------
// No test framework: each check prints PASS/FAIL and the binary exits non-zero
// if anything failed. Build via the LIVEPLAY_BUILD_TESTS CMake option:
//     cmake -DLIVEPLAY_BUILD_TESTS=ON .. && cmake --build . --target liveplay-mixer-tests
//     ./liveplay-mixer-tests
//
// Covers the peek/advance split (#45): reading the gain must never move the
// fade envelope, so a caller that reads twice in a block (metering + gain
// application) can't run every fade at double speed.
//   1. Static gain            — set_gain_db reads back, repeatedly
//   2. Peek is pure           — N peeks leave the envelope where it was
//   3. Advance drives it      — the ramp completes in exactly the configured
//                               number of blocks, and only then
//   4. begin_fade is pure     — arming a fade from the control thread doesn't
//                               consume a block of the previous one
//   5. Zero-length fade       — applies immediately
//
// Also covers the pan law and, since Stage 3, PFL state on the strip: the flag
// the engine turns into monitor taps, and the width that decides where a
// tapped strip lands in the monitor.
// ============================================================================
#include "liveplay/audio/mixer_channel.hpp"
#include "liveplay/audio/monitor_tap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace liveplay::audio;

namespace {

int g_failures = 0;

void check(bool ok, const char* name, double got, double expect, double tol) {
    std::printf("%-56s %s  (got %+9.5f, expect %+9.5f ±%.5f)\n",
                name, ok ? "PASS" : "FAIL", got, expect, tol);
    if (!ok) ++g_failures;
}
void check_near(const char* name, double got, double expect, double tol) {
    check(std::fabs(got - expect) <= tol, name, got, expect, tol);
}
void check_true(const char* name, bool ok) {
    std::printf("%-56s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

constexpr SampleRate kFs    = 48'000;
constexpr FrameCount kBlock = 256;

// MixerChannel owns per-lane Meters (atomics), so it is neither copyable nor
// movable — construct it in place.
#define MAKE_CHANNEL(name)                                   \
    MixerChannel name{MixerChannelId{"test"}, "Test"};       \
    name.configure(kFs, kBlock)

// ---------------------------------------------------------------------------
void test_static_gain() {
    MAKE_CHANNEL(m);
    check_near("static: unity by default",   m.peek_gain_linear(), 1.0, 1e-6);
    m.set_gain_db(-6.0f);
    const float first = m.peek_gain_linear();
    check_near("static: -6 dB ≈ 0.5012",     first, 0.50119, 1e-4);
    check_near("static: repeat read stable", m.peek_gain_linear(), first, 0.0);
    m.advance_block();
    check_near("static: advance is a no-op without a fade",
                                             m.peek_gain_linear(), first, 0.0);
}

// ---------------------------------------------------------------------------
void test_peek_does_not_advance() {
    MAKE_CHANNEL(m);
    // 100 blocks' worth of fade to silence.
    const auto duration = std::chrono::milliseconds{
        static_cast<long long>(100.0 * kBlock * 1000.0 / kFs)};
    m.begin_fade(-120.0f, duration);

    check_near("peek: starts at unity", m.peek_gain_linear(), 1.0, 1e-6);
    for (int i = 0; i < 50; ++i) m.peek_gain_linear();
    check_near("peek: 50 reads left the envelope alone",
               m.peek_gain_linear(), 1.0, 1e-6);

    // Half the blocks → cosine ramp is at its midpoint (0.5 of the way down).
    for (int i = 0; i < 50; ++i) m.advance_block();
    check_near("peek: half-way through the fade", m.peek_gain_linear(), 0.5, 0.02);
    // Reading it a second time must give the same answer.
    check_near("peek: still half-way on re-read", m.peek_gain_linear(), 0.5, 0.02);
}

// ---------------------------------------------------------------------------
void test_advance_completes_on_schedule() {
    MAKE_CHANNEL(m);
    const int blocks = 40;
    m.begin_fade(-120.0f, std::chrono::milliseconds{
        static_cast<long long>(blocks * kBlock * 1000.0 / kFs)});

    for (int i = 0; i < blocks - 1; ++i) m.advance_block();
    check_true("advance: not finished one block early", m.peek_gain_linear() > 0.0f);
    m.advance_block();
    check_near("advance: silent exactly on schedule", m.peek_gain_linear(), 0.0, 1e-6);
    // Overshooting must not resurrect the fade.
    for (int i = 0; i < 10; ++i) m.advance_block();
    check_near("advance: stays at the target afterwards", m.peek_gain_linear(), 0.0, 1e-6);
}

// ---------------------------------------------------------------------------
void test_begin_fade_does_not_consume_a_block() {
    MAKE_CHANNEL(m);
    const int blocks = 40;
    const auto duration = std::chrono::milliseconds{
        static_cast<long long>(blocks * kBlock * 1000.0 / kFs)};

    // Arm a fade, then immediately re-arm it (as a control-thread caller might).
    // The re-arm snapshots the current gain; it must not step the envelope.
    m.begin_fade(-120.0f, duration);
    m.begin_fade(-120.0f, duration);
    m.begin_fade(-120.0f, duration);
    check_near("begin_fade: re-arming kept the start gain",
               m.peek_gain_linear(), 1.0, 1e-6);

    for (int i = 0; i < blocks; ++i) m.advance_block();
    check_near("begin_fade: full duration still required",
               m.peek_gain_linear(), 0.0, 1e-6);
}

// ---------------------------------------------------------------------------
void test_zero_length_fade() {
    MAKE_CHANNEL(m);
    m.begin_fade(-120.0f, std::chrono::milliseconds{0});
    check_near("zero fade: applied immediately", m.peek_gain_linear(), 0.0, 1e-6);
    m.advance_block();
    check_near("zero fade: stable after advance", m.peek_gain_linear(), 0.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Constant-power pan law (pan_gains_db). Two properties carry real weight and
// are easy to break silently:
//
//   * Constant power. l^2 + r^2 == 1 everywhere, so a source sweeping across
//     the image holds its perceived level instead of dipping in the middle.
//   * Centre agrees with the downmix constant. A centred mono source and a
//     stereo fold-down must land at the same level, which is the whole reason
//     kDefaultDownmixDb and this law are described as one law in the design.
//     If someone "rounds" one of them, this catches it.
void test_pan_law() {
    const auto lin = [](float db) { return std::pow(10.0, db / 20.0); };

    const auto centre = pan_gains_db(0.0f);
    check_near("pan: centre is symmetric",
               centre.left - centre.right, 0.0, 1e-6);
    check_near("pan: centre matches the downmix constant",
               centre.left, kDefaultDownmixDb, 0.02);

    const auto left = pan_gains_db(-1.0f);
    check_near("pan: hard left is unity on L", left.left, 0.0, 1e-4);
    check_true ("pan: hard left is silent on R", left.right <= kSilentGainDb + 1e-3f);

    const auto right = pan_gains_db(1.0f);
    check_near("pan: hard right is unity on R", right.right, 0.0, 1e-4);
    check_true ("pan: hard right is silent on L", right.left <= kSilentGainDb + 1e-3f);

    // Power stays constant across the sweep, which is what stops the image
    // from dipping as it crosses centre.
    double worst = 0.0;
    for (int i = -10; i <= 10; ++i) {
        const auto g = pan_gains_db(static_cast<float>(i) / 10.0f);
        const double power = lin(g.left) * lin(g.left) + lin(g.right) * lin(g.right);
        worst = std::max(worst, std::fabs(power - 1.0));
    }
    check_near("pan: constant power across the sweep", worst, 0.0, 1e-6);

    // Out-of-range input must clamp, not wrap into the far side of the arc.
    check_near("pan: clamps below -1", pan_gains_db(-4.0f).left, 0.0, 1e-4);
    check_near("pan: clamps above +1", pan_gains_db(4.0f).right, 0.0, 1e-4);
}

// ---------------------------------------------------------------------------
// PFL state on the strip (§2.4). The tap itself lives in the render loop and
// is verified against a running server; what is checkable here is the state
// the engine derives those taps from, and the gain a centred mono tap uses.
void test_pfl_state() {
    MAKE_CHANNEL(m);
    check_true("pfl: off by default",            !m.is_pfl());
    m.set_pfl(true);
    check_true("pfl: reads back",                m.is_pfl());
    // PFL must not touch the audio path here — the strip's own gain is the
    // house level, and PFL is a separate tap taken before it.
    check_near("pfl: does not move the strip gain", m.peek_gain_linear(), 1.0, 1e-6);
    m.set_mute(true);
    check_true("pfl: survives mute",             m.is_pfl());
    m.set_pfl(false);
    check_true("pfl: clears",                    !m.is_pfl());

    check_true("width: stereo by default",       m.width() == kMixerLanes);
    m.set_width(1);
    check_true("width: mono reads back",         m.width() == 1);
    // Out of range must clamp: the strip only owns kMixerLanes lanes, and a
    // wider value would have the engine tapping a lane that isn't there.
    m.set_width(7);
    check_true("width: clamps to the lane count", m.width() == kMixerLanes);
    m.set_width(0);
    check_true("width: clamps up to mono",       m.width() == 1);
}

// A mono strip is PFL'd into both monitor lanes at the downmix constant, and
// a mono bus is panned into a stereo destination by the pan law. Those are two
// different code paths that have to land a mono source at the same level, or
// PFL'ing a channel would change how loud it seems relative to the house.
void test_pfl_centre_matches_pan_centre() {
    const double tap = kDefaultDownmixDb;
    const double pan = pan_gains_db(0.0f).left;
    check_near("pfl: mono tap gain matches pan centre", tap - pan, 0.0, 0.02);
}

// ---------------------------------------------------------------------------
// The monitor mix itself: which lane a tapped strip lands in, at what level,
// and that several taps sum rather than replace one another.
//
// This is the arithmetic the render loop runs. It is exercised directly, on
// the same function the engine calls, because everything around it in that
// loop needs an open audio device and this is the part with a rule in it.
namespace {
// Two strips plus the monitor, laid out exactly as the render thread lays out
// its accumulators: [strip_index * kMixerLanes + lane].
struct Bench {
    std::shared_ptr<MixerChannel> a, b, monitor;
    std::unordered_map<std::string, std::size_t> index;
    std::vector<std::vector<Sample>> lanes;
    static constexpr std::size_t kBlockN = 8;

    Bench() {
        a       = std::make_shared<MixerChannel>(MixerChannelId{"a"}, "A");
        b       = std::make_shared<MixerChannel>(MixerChannelId{"b"}, "B");
        monitor = std::make_shared<MixerChannel>(MixerChannelId{"mon"}, "Monitor");
        index = {{"a", 0}, {"b", 1}, {"mon", 2}};
        lanes.assign(3 * kMixerLanes, std::vector<Sample>(kBlockN, 0.0f));
    }
    void fill(std::size_t strip, ChannelIndex lane, Sample v) {
        auto& buf = lanes[strip * kMixerLanes + lane];
        std::fill(buf.begin(), buf.end(), v);
    }
    Sample mon(ChannelIndex lane) const { return lanes[2 * kMixerLanes + lane][0]; }
};
} // namespace

void test_monitor_tap_mix() {
    // ---- A stereo strip lands lane-for-lane at unity ----
    {
        Bench bench;
        bench.a->set_width(2);
        bench.fill(0, 0, 0.25f);
        bench.fill(0, 1, 0.5f);

        std::vector<MonitorTap> taps;
        append_monitor_taps(taps, bench.a);
        check_true("tap: a stereo strip contributes two taps", taps.size() == 2);
        mix_monitor_taps(2, taps, bench.index, bench.lanes, Bench::kBlockN);
        check_near("tap: stereo L arrives on monitor L at unity", bench.mon(0), 0.25, 1e-6);
        check_near("tap: stereo R arrives on monitor R at unity", bench.mon(1), 0.5,  1e-6);
    }

    // ---- A mono strip is centred, not hard left ----
    {
        Bench bench;
        bench.a->set_width(1);
        bench.fill(0, 0, 1.0f);   // lane 1 stays silent, as a mono strip's does

        std::vector<MonitorTap> taps;
        append_monitor_taps(taps, bench.a);
        mix_monitor_taps(2, taps, bench.index, bench.lanes, Bench::kBlockN);
        // The pan law's own centre, not the downmix constant it approximates.
        // The tap carries pan now (PFL is post-pan), so it calls the same
        // function the master send does and lands on 1/sqrt(2) exactly —
        // constant power, and 0.01 dB off the -3.0 dB constant it used to use.
        // Sharing the function is the point: the two cannot drift apart.
        const double expect = std::pow(10.0, pan_gains_db(0.0f).left / 20.0);
        check_near("tap: mono at centre uses the pan law's centre",
                   bench.mon(0), expect, 1e-5);
        check_near("tap: mono at centre is the same on both lanes",
                   bench.mon(1), expect, 1e-5);
        check_near("tap: mono is centred (L equals R)",
                   bench.mon(0) - bench.mon(1), 0.0, 1e-9);
    }

    // ---- PFL is POST-PAN ----
    // Pan lives in the strip's send to the master, which is downstream of this
    // tap, so the tap has to place the signal itself or every mono bus would
    // sit dead centre in the phones however it was panned.
    {
        Bench bench;
        bench.a->set_width(1);
        bench.a->set_pan(-1.0f);           // hard left
        bench.fill(0, 0, 1.0f);

        std::vector<MonitorTap> taps;
        append_monitor_taps(taps, bench.a);
        mix_monitor_taps(2, taps, bench.index, bench.lanes, Bench::kBlockN);
        check_near("tap: hard left arrives at unity on monitor L", bench.mon(0), 1.0, 1e-4);
        check_true("tap: hard left is silent on monitor R",        bench.mon(1) < 1e-5);
    }
    {
        Bench bench;
        bench.a->set_width(1);
        bench.a->set_pan(1.0f);            // hard right
        bench.fill(0, 0, 1.0f);

        std::vector<MonitorTap> taps;
        append_monitor_taps(taps, bench.a);
        mix_monitor_taps(2, taps, bench.index, bench.lanes, Bench::kBlockN);
        check_true("tap: hard right is silent on monitor L",        bench.mon(0) < 1e-5);
        check_near("tap: hard right arrives at unity on monitor R", bench.mon(1), 1.0, 1e-4);
    }
    {
        // Part-panned keeps constant power, same as it does into the house.
        Bench bench;
        bench.a->set_width(1);
        bench.a->set_pan(-0.5f);
        bench.fill(0, 0, 1.0f);

        std::vector<MonitorTap> taps;
        append_monitor_taps(taps, bench.a);
        mix_monitor_taps(2, taps, bench.index, bench.lanes, Bench::kBlockN);
        const double power = bench.mon(0) * bench.mon(0) + bench.mon(1) * bench.mon(1);
        check_near("tap: a part-panned bus holds constant power", power, 1.0, 1e-4);
        check_true("tap: panned left really is louder on the left",
                   bench.mon(0) > bench.mon(1) * 1.5);
    }

    // ---- Taps sum. PFL is additive; that is why it needs a clear-all ----
    {
        Bench bench;
        bench.a->set_width(2);
        bench.b->set_width(2);
        bench.fill(0, 0, 0.2f);
        bench.fill(1, 0, 0.3f);

        std::vector<MonitorTap> taps;
        append_monitor_taps(taps, bench.a);
        append_monitor_taps(taps, bench.b);
        mix_monitor_taps(2, taps, bench.index, bench.lanes, Bench::kBlockN);
        check_near("tap: two PFL'd strips sum in the monitor", bench.mon(0), 0.5, 1e-6);
    }

    // ---- The monitor never taps itself ----
    {
        Bench bench;
        bench.monitor->set_width(2);
        bench.fill(2, 0, 0.4f);

        std::vector<MonitorTap> taps;
        append_monitor_taps(taps, bench.monitor);
        mix_monitor_taps(2, taps, bench.index, bench.lanes, Bench::kBlockN);
        check_near("tap: the monitor does not feed itself", bench.mon(0), 0.4, 1e-6);
    }

    // ---- A strip that is no longer live is skipped, not dereferenced ----
    {
        Bench bench;
        auto gone = std::make_shared<MixerChannel>(MixerChannelId{"gone"}, "Gone");
        gone->set_width(2);
        std::vector<MonitorTap> taps;
        append_monitor_taps(taps, gone);       // never in the index
        taps.push_back({nullptr, 0, 0, 1.0f}); // and one outright null
        mix_monitor_taps(2, taps, bench.index, bench.lanes, Bench::kBlockN);
        check_near("tap: a removed strip contributes nothing", bench.mon(0), 0.0, 1e-9);
    }
}

} // namespace

int main() {
    std::printf("== mixer channel ==\n");
    test_static_gain();
    test_peek_does_not_advance();
    test_advance_completes_on_schedule();
    test_begin_fade_does_not_consume_a_block();
    test_zero_length_fade();
    test_pan_law();
    test_pfl_state();
    test_pfl_centre_matches_pan_centre();
    test_monitor_tap_mix();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
