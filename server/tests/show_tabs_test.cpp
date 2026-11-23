// ============================================================================
// show_tabs_test.cpp - standalone assertions for playlist tabs and cart pages.
// ----------------------------------------------------------------------------
// No test framework: each check prints PASS/FAIL and the binary exits non-zero
// if anything failed. The rules pinned here are the ones a show depends on: an
// old show means what it always meant, and running order stays inside a tab
// unless that tab says otherwise.
// ============================================================================
#include "liveplay/core/show_tabs.hpp"

#include <cstdio>
#include <string>

using namespace liveplay::core::tabs;
using nlohmann::json;

static int failures = 0;
static void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "   ", detail.c_str());
    if (!ok) ++failures;
}

static json cue(const char* uuid, const char* tab = nullptr) {
    json c{{"uuid", uuid}, {"type", "audio"}};
    if (tab) c["tabId"] = tab;
    return c;
}

int main() {
    // ---- A show from before tabs -------------------------------------------
    json old_show{{"items", json::array({cue("a"), cue("b")})}};
    json copy = old_show;
    normalise(copy);
    check("normalising a show with no tabs changes nothing", copy == old_show);
    check("next in an untabbed show is the next item", next_in_tab(old_show, "a").uuid == "b");
    check("the last item has no next", next_in_tab(old_show, "b").uuid.empty());
    check("an untabbed show has one page at base 0", slot_of(old_show, 0, 15) == 15);
    check("...of 16 carts", !slot_of(old_show, 0, 16).has_value());
    check("...and no second page", !slot_of(old_show, 1, 0).has_value());

    // ---- Running order inside tabs -----------------------------------------
    json show{
        {"playlistTabs", json::array({
            {{"id", "t1"}, {"name", "One"}, {"atEnd", "wrap"}},
            {{"id", "t2"}, {"name", "Two"}, {"atEnd", "continue"}},
            {{"id", "t3"}, {"name", "Empty"}, {"atEnd", "wrap"}},
            {{"id", "t4"}, {"name", "Four"}, {"atEnd", "stop"}},
        })},
        {"items", json::array({cue("a"), cue("b", "t1"), cue("c", "t2"), cue("d", "t2"),
                               cue("e", "t4"), cue("f", "gone")})},
    };
    check("an item with no tabId is on the first tab",
          root_tab_id(show["items"][0], show["playlistTabs"]) == "t1");
    check("an item naming a deleted tab is on the first tab",
          root_tab_id(show["items"][5], show["playlistTabs"]) == "t1");
    check("next stays in the tab", next_in_tab(show, "c").uuid == "d");
    check("items of the tab need not be adjacent", next_in_tab(show, "b").uuid == "f");
    const auto wrap = next_in_tab(show, "f");
    check("a wrap tab has no next at its end", wrap.uuid.empty() && wrap.at_end == "wrap");
    const auto cont = next_in_tab(show, "d");
    check("a continue tab runs into the next tab with cues, skipping an empty one",
          cont.uuid == "e" && cont.at_end == "continue", cont.uuid);
    const auto stop = next_in_tab(show, "e");
    check("a stop tab ends there", stop.uuid.empty() && stop.at_end == "stop");
    check("first playable of a tab", first_playable_in_tab(show, "t2") == "c");
    check("an empty tab has none", first_playable_in_tab(show, "t3").empty());

    json grouped{
        {"playlistTabs", json::array({{{"id", "t1"}}, {{"id", "t2"}}})},
        {"items", json::array({
            {{"uuid", "g"}, {"type", "group"}, {"tabId", "t2"},
             {"children", json::array({cue("g1"), cue("g2")})}},
        })},
    };
    check("a tab's audio includes its groups' cues",
          audio_uuids_in_tab(grouped, "t2") == std::vector<std::string>({"g1", "g2"}));

    // ---- Tab targets -------------------------------------------------------
    check("a tab target is recognised", is_tab_target("tab:abc") && tab_id_of_target("tab:abc") == "abc");
    check("an ordinary uuid is not one", !is_tab_target("abc-123"));

    // ---- Cleaning ----------------------------------------------------------
    const json tabs = clean_playlist_tabs(json::array({
        {{"id", "x"}, {"name", "X"}, {"atEnd", "sideways"}},
        {{"id", "x"}, {"name", "Duplicate"}},
        {{"name", "No id"}},
        "not a tab",
    }));
    check("bad tabs are dropped", tabs.size() == 1);
    check("an unknown end rule becomes wrap", tabs[0].value("atEnd", "") == "wrap");

    const json pages = clean_cart_pages(json::array({
        {{"id", "p1"}, {"size", 2}},
        {{"id", "p2"}, {"size", 999}, {"slotBase", 0}},
        {{"id", "p3"}, {"size", 8}, {"slotBase", 64}},
        {{"id", "p4"}, {"slotBase", 13}},
        {{"id", "p5"}, {"slotBase", 64}},
    }));
    check("every page survives", pages.size() == 5, std::to_string(pages.size()));
    check("a size is clamped up", pages[0].value("size", 0) == kMinCartsPerPage);
    check("a size is clamped down", pages[1].value("size", 0) == kMaxCartsPerPage);
    std::set<int> bases;
    bool aligned = true;
    for (const auto& p : pages) {
        const int base = p.value("slotBase", -1);
        bases.insert(base);
        aligned = aligned && base >= 0 && base % kCartSlotsPerPage == 0;
    }
    check("no two pages share a block of slots", bases.size() == pages.size());
    check("every base is aligned", aligned);
    check("a good base is kept", pages[2].value("slotBase", -1) == 64);

    json paged{{"cartPages", json::array({
        {{"id", "b"}, {"size", 8}, {"slotBase", 128}},
        {{"id", "a"}, {"size", 16}, {"slotBase", 0}},
    })}};
    check("page,cart uses the page's own base", slot_of(paged, 0, 3) == 131);
    check("order is the list order, not the base", slot_of(paged, 1, 3) == 3);
    check("a cart past the page's size doesn't exist", !slot_of(paged, 0, 8).has_value());

    json empties{{"playlistTabs", json::array()}, {"cartPages", json::array()}};
    normalise(empties);
    check("empty lists are removed", !empties.contains("playlistTabs") && !empties.contains("cartPages"));

    if (failures) std::printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
