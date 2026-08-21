// ============================================================================
// topo_order_test.cpp — standalone assertions for compute_strip_order.
// ----------------------------------------------------------------------------
// No test framework: each check prints PASS/FAIL and the binary exits non-zero
// if anything failed. Build via the LIVEPLAY_BUILD_TESTS CMake option:
//     cmake -DLIVEPLAY_BUILD_TESTS=ON .. && cmake --build . --target liveplay-topo-tests
//     ./liveplay-topo-tests
//
// compute_strip_order is what stands between a bus→bus wire and an infinite
// loop in the audio callback: the render thread walks the order it returns
// and never traverses the graph itself. So the properties pinned here are the
// safety case, not conveniences:
//   1. Chains order source-before-destination, wherever the edges start
//   2. Diamonds (a strip fed by two branches) order both branches first
//   3. A cycle NEVER reaches the order: the offending edge is dropped,
//      deterministically, and the rest of the graph still sorts
//   4. Self-loops are cycles too
//   5. The excluded node (Monitor) is absent from the order and its edges
//      are ignored rather than sorted or dropped
// ============================================================================
#include "liveplay/audio/engine.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

using namespace liveplay::audio;

namespace {

int g_failures = 0;

void check_true(const char* name, bool ok) {
    std::printf("%-64s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}

using Edge  = std::pair<std::size_t, std::size_t>;
using Edges = std::vector<Edge>;

constexpr std::size_t kNoExclude = static_cast<std::size_t>(-1);

// The core contract: every node except the excluded one appears exactly once,
// the excluded one not at all, and every surviving edge (input edges minus the
// dropped ones, minus those touching the excluded node) runs forward in the
// order.
bool is_topological(const StripOrderResult& res,
                    std::size_t count,
                    const Edges& edges,
                    std::size_t exclude = kNoExclude) {
    const std::size_t expected = count - (exclude < count ? 1 : 0);
    if (res.order.size() != expected) return false;

    std::vector<std::size_t> pos(count, static_cast<std::size_t>(-1));
    std::vector<int> seen(count, 0);
    for (std::size_t p = 0; p < res.order.size(); ++p) {
        const std::size_t n = res.order[p];
        if (n >= count || n == exclude) return false;
        if (seen[n]++) return false;                    // duplicate
        pos[n] = p;
    }
    for (const auto& e : edges) {
        if (e.first >= count || e.second >= count) continue;
        if (e.first == exclude || e.second == exclude) continue;
        if (std::find(res.dropped.begin(), res.dropped.end(), e) != res.dropped.end())
            continue;
        if (pos[e.first] >= pos[e.second]) return false;
    }
    return true;
}

// --- 1. Chains --------------------------------------------------------------
void test_chains() {
    // Forward chain, edges given out of order so insertion order can't be
    // what makes it pass.
    {
        const Edges edges{{2, 3}, {0, 1}, {1, 2}};
        const auto res = compute_strip_order(4, edges);
        check_true("chain 0->1->2->3: topological", is_topological(res, 4, edges));
        check_true("chain 0->1->2->3: nothing dropped", res.dropped.empty());
        const std::vector<std::size_t> want{0, 1, 2, 3};
        check_true("chain 0->1->2->3: exact order", res.order == want);
    }
    // Reversed chain: the smallest-index-first tie-break must not beat the
    // edges — 3 feeds 2 feeds 1 feeds 0, so 3 must come out first.
    {
        const Edges edges{{3, 2}, {2, 1}, {1, 0}};
        const auto res = compute_strip_order(4, edges);
        check_true("chain 3->2->1->0: topological", is_topological(res, 4, edges));
        const std::vector<std::size_t> want{3, 2, 1, 0};
        check_true("chain 3->2->1->0: exact order", res.order == want);
    }
    // No edges at all: every strip, index order.
    {
        const auto res = compute_strip_order(3, {});
        const std::vector<std::size_t> want{0, 1, 2};
        check_true("no edges: index order, nothing dropped",
                   res.order == want && res.dropped.empty());
    }
}

// --- 2. Diamonds ------------------------------------------------------------
void test_diamond() {
    // 0 feeds 1 and 2; both feed 3. (The engine only mints one output per
    // strip — D5 — but the sort must stay correct for the general graph.)
    {
        const Edges edges{{1, 3}, {0, 1}, {2, 3}, {0, 2}};
        const auto res = compute_strip_order(4, edges);
        check_true("diamond: topological", is_topological(res, 4, edges));
        check_true("diamond: nothing dropped", res.dropped.empty());
        const std::vector<std::size_t> want{0, 1, 2, 3};
        check_true("diamond: exact order (smallest ready index first)",
                   res.order == want);
    }
    // Diamond pointing at the low index: 3 splits to 1/2, both feed 0.
    {
        const Edges edges{{3, 1}, {3, 2}, {1, 0}, {2, 0}};
        const auto res = compute_strip_order(4, edges);
        check_true("reverse diamond: topological", is_topological(res, 4, edges));
        const std::vector<std::size_t> want{3, 1, 2, 0};
        check_true("reverse diamond: exact order", res.order == want);
    }
}

// --- 3. Cycles --------------------------------------------------------------
void test_cycles() {
    // Two-strip cycle plus a bystander. The victim is the smallest-index
    // remaining node (0), so the dropped edge is deterministically the one
    // into it: 1->0.
    {
        const Edges edges{{0, 1}, {1, 0}};
        const auto res = compute_strip_order(3, edges);
        check_true("2-cycle: exactly one edge dropped", res.dropped.size() == 1);
        check_true("2-cycle: the edge into the smallest node (1->0)",
                   res.dropped.size() == 1 && res.dropped[0] == Edge{1, 0});
        check_true("2-cycle: remainder still topological",
                   is_topological(res, 3, edges));
        // The bystander is the only ready node, so it lands first — the cycle
        // is only broken once nothing else can move.
        const std::vector<std::size_t> want{2, 0, 1};
        check_true("2-cycle: bystander strip 2 still ordered", res.order == want);

        // Determinism: the same graph drops the same edge and yields the
        // same order, every time.
        const auto again = compute_strip_order(3, edges);
        check_true("2-cycle: deterministic across runs",
                   again.order == res.order && again.dropped == res.dropped);
    }
    // A chain feeding a cycle: 0->1, 1->2, 2->1. The chain part must survive
    // intact — only the edge that closes the loop (2->1) goes.
    {
        const Edges edges{{0, 1}, {1, 2}, {2, 1}};
        const auto res = compute_strip_order(3, edges);
        check_true("chain into cycle: exactly one edge dropped",
                   res.dropped.size() == 1);
        check_true("chain into cycle: the loop-closing edge (2->1)",
                   res.dropped.size() == 1 && res.dropped[0] == Edge{2, 1});
        const std::vector<std::size_t> want{0, 1, 2};
        check_true("chain into cycle: chain order preserved", res.order == want);
    }
    // Three-strip cycle 1->2->3->1 with independent strips around it: the
    // rest of the desk keeps its order, one drop opens the ring.
    {
        const Edges edges{{1, 2}, {2, 3}, {3, 1}, {0, 4}};
        const auto res = compute_strip_order(5, edges);
        check_true("3-cycle amid a desk: exactly one edge dropped",
                   res.dropped.size() == 1);
        check_true("3-cycle amid a desk: the edge into the smallest cycle node (3->1)",
                   res.dropped.size() == 1 && res.dropped[0] == Edge{3, 1});
        check_true("3-cycle amid a desk: remainder topological",
                   is_topological(res, 5, edges));
    }
}

// --- 4. Self-loops ----------------------------------------------------------
void test_self_loop() {
    const Edges edges{{1, 1}, {0, 2}};
    const auto res = compute_strip_order(3, edges);
    check_true("self-loop: dropped", res.dropped.size() == 1 &&
                                     res.dropped[0] == Edge{1, 1});
    check_true("self-loop: everything still ordered",
               is_topological(res, 3, edges));
}

// --- 5. Monitor exclusion ---------------------------------------------------
void test_monitor_excluded() {
    // Strip 2 is the Monitor. It must be absent from the order, and edges
    // touching it — even ones that would otherwise close a cycle through it —
    // are ignored rather than sorted or reported as drops.
    {
        const Edges edges{{0, 1}, {3, 2}, {2, 0}};
        const auto res = compute_strip_order(4, edges, 2);
        check_true("monitor: absent from the order",
                   std::find(res.order.begin(), res.order.end(),
                             std::size_t{2}) == res.order.end());
        check_true("monitor: order covers everyone else once",
                   is_topological(res, 4, edges, 2));
        check_true("monitor: its edges are ignored, not dropped",
                   res.dropped.empty());
    }
    // A real cycle among the others still drops with Monitor excluded.
    {
        const Edges edges{{0, 1}, {1, 0}, {3, 2}};
        const auto res = compute_strip_order(4, edges, 2);
        check_true("monitor + cycle: cycle edge still dropped (1->0)",
                   res.dropped.size() == 1 && res.dropped[0] == Edge{1, 0});
        check_true("monitor + cycle: remainder topological",
                   is_topological(res, 4, edges, 2));
    }
    // Out-of-range edges are ignored quietly.
    {
        const Edges edges{{0, 7}, {9, 1}, {0, 1}};
        const auto res = compute_strip_order(2, edges);
        const std::vector<std::size_t> want{0, 1};
        check_true("out-of-range edges: ignored", res.order == want &&
                                                  res.dropped.empty());
    }
}

} // namespace

int main() {
    std::printf("== compute_strip_order: chains ==\n");
    test_chains();
    std::printf("\n== compute_strip_order: diamonds ==\n");
    test_diamond();
    std::printf("\n== compute_strip_order: cycles ==\n");
    test_cycles();
    std::printf("\n== compute_strip_order: self-loops ==\n");
    test_self_loop();
    std::printf("\n== compute_strip_order: monitor exclusion ==\n");
    test_monitor_excluded();

    std::printf("\n%s (%d failure%s)\n",
                g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
