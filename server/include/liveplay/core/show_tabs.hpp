#pragma once
// ============================================================================
// show_tabs.hpp - playlist tabs and cart pages in a show document (#74).
// ----------------------------------------------------------------------------
// Both are additions a show that doesn't use them never sees:
//
//   * Playlist tabs: `playlistTabs` lists them, and a top-level item says which
//     one it is on with `tabId`. `items` stays one flat list in tab order, so a
//     cue's index, every `goto-index`, and the index API all mean what they did.
//     An item with no tabId (or an unknown one) is on the first tab.
//
//   * Cart pages: `cartPages` lists them, and each owns a block of the existing
//     cart slot numbers starting at its `slotBase`. A page's slots are
//     slotBase + 0..size-1. The base never changes when pages are reordered, so
//     bindings don't move, and a show from before pages is simply page one at
//     base 0.
//
// A cart slot can also start a whole playlist tab, by binding to the target
// "tab:<tabId>" in place of an item uuid (discussion #61).
//
// Every limit is here, and clients are told them, so they change in one place.
// ============================================================================
#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace liveplay::core::tabs {

using json = nlohmann::json;

inline constexpr int kMaxPlaylistTabs     = 100;
inline constexpr int kMaxCartPages        = 100;
inline constexpr int kMinCartsPerPage     = 4;
inline constexpr int kMaxCartsPerPage     = 64;
inline constexpr int kDefaultCartsPerPage = 16;
// How many slot numbers a page reserves, whatever its current size, so a page
// can grow without colliding with the next one.
inline constexpr int kCartSlotsPerPage = kMaxCartsPerPage;
inline constexpr int kCartSlotCount    = kMaxCartPages * kCartSlotsPerPage;

inline constexpr const char* kTabTargetPrefix = "tab:";

inline json limits() {
    return json{
        {"maxPlaylistTabs", kMaxPlaylistTabs},
        {"maxCartPages", kMaxCartPages},
        {"minCartsPerPage", kMinCartsPerPage},
        {"maxCartsPerPage", kMaxCartsPerPage},
        {"defaultCartsPerPage", kDefaultCartsPerPage},
        {"cartSlotsPerPage", kCartSlotsPerPage},
    };
}

inline bool is_tab_target(const std::string& uuid) {
    return uuid.rfind(kTabTargetPrefix, 0) == 0;
}
inline std::string tab_id_of_target(const std::string& uuid) {
    return is_tab_target(uuid) ? uuid.substr(std::char_traits<char>::length(kTabTargetPrefix))
                               : std::string{};
}

namespace detail {
inline std::string text(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
}  // namespace detail

// Well-formed tabs: a unique string id, a name, an optional colour, and what
// happens after the last cue (`wrap` unless it says `continue` or `stop`).
inline json clean_playlist_tabs(const json& tabs) {
    json clean = json::array();
    if (!tabs.is_array()) return clean;
    std::set<std::string> seen;
    for (const auto& t : tabs) {
        if (static_cast<int>(clean.size()) >= kMaxPlaylistTabs) break;
        if (!t.is_object()) continue;
        const std::string id = detail::text(t, "id");
        if (id.empty() || !seen.insert(id).second) continue;
        std::string at_end = detail::text(t, "atEnd");
        if (at_end != "continue" && at_end != "stop") at_end = "wrap";
        json entry{{"id", id}, {"name", detail::text(t, "name")}, {"atEnd", at_end}};
        if (const auto color = detail::text(t, "color"); !color.empty()) entry["color"] = color;
        clean.push_back(std::move(entry));
    }
    return clean;
}

// Well-formed pages. A base that is missing, misaligned, out of range or already
// taken gets the lowest free one, so two pages can never share a slot.
inline json clean_cart_pages(const json& pages) {
    json clean = json::array();
    if (!pages.is_array()) return clean;
    std::set<std::string> seen;
    std::set<int> taken;
    const auto valid_base = [](const json& p) -> std::optional<int> {
        const auto it = p.find("slotBase");
        if (it == p.end() || !it->is_number_integer()) return std::nullopt;
        const int base = it->get<int>();
        if (base < 0 || base >= kCartSlotCount || base % kCartSlotsPerPage != 0) return std::nullopt;
        return base;
    };
    // Valid bases claim theirs first, so a bad entry early in the list can't
    // take the number a good one later on is already using.
    for (const auto& p : pages) {
        if (!p.is_object()) continue;
        if (const auto base = valid_base(p)) taken.insert(*base);
    }
    std::set<int> placed;
    for (const auto& p : pages) {
        if (static_cast<int>(clean.size()) >= kMaxCartPages) break;
        if (!p.is_object()) continue;
        const std::string id = detail::text(p, "id");
        if (id.empty() || !seen.insert(id).second) continue;
        int base = valid_base(p).value_or(-1);
        if (base < 0 || placed.count(base)) {
            base = 0;
            while (taken.count(base) || placed.count(base)) base += kCartSlotsPerPage;
            if (base >= kCartSlotCount) continue;
        }
        placed.insert(base);
        int size = kDefaultCartsPerPage;
        if (const auto it = p.find("size"); it != p.end() && it->is_number_integer()) {
            size = std::clamp(it->get<int>(), kMinCartsPerPage, kMaxCartsPerPage);
        }
        json entry{{"id", id}, {"name", detail::text(p, "name")}, {"size", size}, {"slotBase", base}};
        if (const auto color = detail::text(p, "color"); !color.empty()) entry["color"] = color;
        clean.push_back(std::move(entry));
    }
    return clean;
}

// Tidies both lists in place. An empty list is removed, so a show with no tabs
// keeps the shape it has always had.
inline void normalise(json& doc) {
    if (!doc.is_object()) return;
    for (const char* key : {"playlistTabs", "cartPages"}) {
        if (!doc.contains(key)) continue;
        json clean = std::string_view{key} == "playlistTabs" ? clean_playlist_tabs(doc[key])
                                                             : clean_cart_pages(doc[key]);
        if (clean.empty()) doc.erase(key);
        else doc[key] = std::move(clean);
    }
}

inline const json* tab_list(const json& doc) {
    const auto it = doc.find("playlistTabs");
    return it != doc.end() && it->is_array() && !it->empty() ? &*it : nullptr;
}

// The tab a top-level item is on.
inline std::string root_tab_id(const json& item, const json& tabs) {
    const std::string first = detail::text(tabs.front(), "id");
    if (!item.is_object()) return first;
    const std::string id = detail::text(item, "tabId");
    if (id.empty()) return first;
    for (const auto& t : tabs) {
        if (detail::text(t, "id") == id) return id;
    }
    return first;
}

struct NextInTab {
    std::string uuid;       // what follows in running order; empty if nothing
    std::string tab_id;     // the tab the item is on (empty in an untabbed show)
    std::string at_end;     // that tab's rule, when `uuid` ran off its end
};

// What follows a top-level item, keeping to its tab. Off the end of a tab, a
// `continue` tab runs into the next tab that has anything on it.
inline NextInTab next_in_tab(const json& doc, const std::string& uuid) {
    NextInTab out;
    const auto items_it = doc.find("items");
    if (items_it == doc.end() || !items_it->is_array()) return out;
    const json& items = *items_it;

    std::size_t at = items.size();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].is_object() && detail::text(items[i], "uuid") == uuid) {
            at = i;
            break;
        }
    }
    if (at == items.size()) return out;

    const json* tabs = tab_list(doc);
    if (!tabs) {
        if (at + 1 < items.size() && items[at + 1].is_object()) {
            out.uuid = detail::text(items[at + 1], "uuid");
        }
        return out;
    }

    out.tab_id = root_tab_id(items[at], *tabs);
    for (std::size_t j = at + 1; j < items.size(); ++j) {
        if (items[j].is_object() && root_tab_id(items[j], *tabs) == out.tab_id) {
            out.uuid = detail::text(items[j], "uuid");
            return out;
        }
    }

    std::size_t tab_index = 0;
    for (std::size_t t = 0; t < tabs->size(); ++t) {
        if (detail::text((*tabs)[t], "id") == out.tab_id) tab_index = t;
    }
    out.at_end = detail::text((*tabs)[tab_index], "atEnd");
    if (out.at_end == "continue") {
        for (std::size_t t = tab_index + 1; t < tabs->size(); ++t) {
            const std::string next_tab = detail::text((*tabs)[t], "id");
            for (const auto& it : items) {
                if (it.is_object() && root_tab_id(it, *tabs) == next_tab) {
                    out.uuid = detail::text(it, "uuid");
                    return out;
                }
            }
        }
    }
    return out;
}

// Every audio item on a tab, in order, groups included.
inline std::vector<std::string> audio_uuids_in_tab(const json& doc, const std::string& tab_id) {
    std::vector<std::string> out;
    const auto items_it = doc.find("items");
    if (items_it == doc.end() || !items_it->is_array()) return out;
    const json* tabs = tab_list(doc);
    std::function<void(const json&)> walk = [&](const json& it) {
        if (!it.is_object()) return;
        if (detail::text(it, "type") == "audio") {
            out.push_back(detail::text(it, "uuid"));
        } else if (const auto c = it.find("children"); c != it.end() && c->is_array()) {
            for (const auto& child : *c) walk(child);
        }
    };
    for (const auto& it : *items_it) {
        // In a show with no tabs, everything is on the one implicit tab.
        if (!tabs || root_tab_id(it, *tabs) == tab_id) walk(it);
    }
    return out;
}

inline std::string first_playable_in_tab(const json& doc, const std::string& tab_id) {
    const auto all = audio_uuids_in_tab(doc, tab_id);
    return all.empty() ? std::string{} : all.front();
}

// The show's pages, or the one implicit page of a show that has never had any.
inline json pages_or_default(const json& doc) {
    const auto it = doc.find("cartPages");
    if (it != doc.end() && it->is_array() && !it->empty()) return *it;
    return json::array({json{{"id", "default"}, {"name", ""},
                             {"size", kDefaultCartsPerPage}, {"slotBase", 0}}});
}

// The slot number of `page`,`cart`, both zero-based, or nothing if there is no
// such cart on that page.
inline std::optional<int> slot_of(const json& doc, int page, int cart) {
    const json pages = pages_or_default(doc);
    if (page < 0 || page >= static_cast<int>(pages.size())) return std::nullopt;
    const json& p = pages[static_cast<std::size_t>(page)];
    if (cart < 0 || cart >= p.value("size", kDefaultCartsPerPage)) return std::nullopt;
    return p.value("slotBase", 0) + cart;
}

}  // namespace liveplay::core::tabs
