// ============================================================================
// project_state.cpp — see project_state.hpp.
// ============================================================================
#include "liveplay/core/project_state.hpp"
#include "liveplay/logger.hpp"
#include "liveplay/meta/metadata.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <functional>
#include <future>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace liveplay::core {

namespace {
inline std::string id_to_string(const audio::CueId& id)          { return id.value; }
inline std::string id_to_string(const audio::MixerChannelId& id) { return id.value; }
inline std::string id_to_string(const audio::DeviceId& id)       { return id.value; }

// Convert a Unix timestamp (seconds since epoch) to an ISO 8601 UTC string.
inline std::string unix_ts_to_iso(std::int64_t unix_sec) {
    const std::time_t t = static_cast<std::time_t>(unix_sec);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm_buf);
    return std::string{buf};
}

// Return the current time as an ISO 8601 UTC string.
inline std::string now_iso() {
    const auto now = std::chrono::system_clock::now();
    return unix_ts_to_iso(
        static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count()));
}

// Read lastModified from a document, tolerating both ISO string and legacy
// Unix-timestamp integer formats. Returns "" if the field is absent or
// has an unexpected type.
inline std::string read_last_modified(const json& doc) {
    if (!doc.contains("lastModified")) return "";
    const auto& lm = doc["lastModified"];
    if (lm.is_string())          return lm.get<std::string>();
    if (lm.is_number_integer())  return unix_ts_to_iso(lm.get<std::int64_t>());
    if (lm.is_number_unsigned()) return unix_ts_to_iso(
                                     static_cast<std::int64_t>(lm.get<std::uint64_t>()));
    return "";
}

// ---------------------------------------------------------------------------
// Project document validation + repair
// ---------------------------------------------------------------------------

// Walk an items array recursively, counting every UUID occurrence.
void count_uuids_in_items(const json& items,
                          std::unordered_map<std::string, int>& counts) {
    if (!items.is_array()) return;
    for (const auto& it : items) {
        if (!it.is_object()) continue;
        const std::string uuid = it.value("uuid", std::string{});
        if (!uuid.empty()) ++counts[uuid];
        if (it.value("type", std::string{}) == "group" && it.contains("children"))
            count_uuids_in_items(it["children"], counts);
    }
}

// Remove duplicate-UUID entries from `items` (keep first occurrence).
// `seen` is the set of already-accepted UUIDs (pre-populated from other arrays
// if needed). Returns the number of items removed.
int remove_duplicate_items(json& items, std::unordered_set<std::string>& seen) {
    if (!items.is_array()) return 0;
    int removed = 0;
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        auto& it = items[i];
        if (!it.is_object()) continue;
        const std::string uuid = it.value("uuid", std::string{});
        if (!uuid.empty()) {
            if (seen.count(uuid)) {
                items.erase(items.begin() + i);
                ++removed;
                continue;
            }
            seen.insert(uuid);
        }
        // Recurse into groups for duplicates within children.
        if (it.value("type", std::string{}) == "group" && it.contains("children"))
            removed += remove_duplicate_items(it["children"], seen);
    }
    return removed;
}

// Inspect `doc` for known corruption patterns. Repairs in place and returns
// a RepairInfo describing what was fixed (empty if nothing needed repair).
RepairInfo detect_and_repair(json& doc) {
    RepairInfo info;

    // ---- 1. lastModified stored as Unix integer instead of ISO string ----
    if (doc.contains("lastModified") && !doc["lastModified"].is_string()) {
        doc["lastModified"] = read_last_modified(doc);
        info.repaired = true;
        info.issues.push_back(
            "lastModified was stored as a number; converted to ISO 8601 string");
    }

    // ---- 2. Duplicate UUIDs in the items array ----
    if (doc.contains("items") && doc["items"].is_array()) {
        std::unordered_set<std::string> seen;
        const int removed = remove_duplicate_items(doc["items"], seen);
        if (removed > 0) {
            info.repaired = true;
            info.issues.push_back(
                "Removed " + std::to_string(removed) +
                " duplicate item(s) from the playlist");
        }
    }

    // ---- 3. Duplicate UUIDs in cartOnlyItems ----
    if (doc.contains("cartOnlyItems") && doc["cartOnlyItems"].is_array()) {
        // Build the already-seen set from items so cross-array dupes are caught.
        std::unordered_set<std::string> seen;
        if (doc.contains("items")) {
            std::unordered_map<std::string, int> counts;
            count_uuids_in_items(doc["items"], counts);
            for (auto& [u, _] : counts) seen.insert(u);
        }
        const int removed = remove_duplicate_items(doc["cartOnlyItems"], seen);
        if (removed > 0) {
            info.repaired = true;
            info.issues.push_back(
                "Removed " + std::to_string(removed) +
                " duplicate item(s) from the cart");
        }
    }

    return info;
}

} // namespace

// ADL-visible to_json overloads — must be in liveplay::core (not anonymous namespace)
// so nlohmann's adl_serializer can find them for push_back / operator= conversions.
void to_json(json& j, const CueMeta& m) {
    j = json{
        {"id",                 m.id.value},
        {"display_name",       m.display_name},
        {"file_path",          util::path_to_utf8(m.file_path)},
        {"artist",             m.artist},
        {"title",              m.title},
        {"duration_sec",       m.duration_seconds},
        {"gain_db",            m.gain_db},
        {"fade_in_ms",         m.fade_in_ms.count()},
        {"fade_out_ms",        m.fade_out_ms.count()},
        {"ltc_enabled",        m.ltc_enabled},
        {"ltc_fps",            m.ltc_frame_rate_index},
        {"ltc_offset_ns",      static_cast<long long>(m.ltc_offset_ns.count())},
        {"ltc_start_timecode", m.ltc_start_timecode},
    };
}

void to_json(json& j, const MixerChannelMeta& m) {
    j = json{
        {"id",           m.id.value},
        {"display_name", m.display_name},
        {"gain_db",      m.gain_db},
        {"muted",        m.muted},
        {"soloed",       m.soloed},
    };
}

void to_json(json& j, const RouteSendV2& r) {
    j = json{
        {"source_channel",    r.source_channel},
        {"destination_mixer", r.destination_mixer.value},
        {"gain_db",           r.gain_db},
    };
}

void to_json(json& j, const MixerToMasterV2& r) {
    j = json{
        {"mixer",           r.mixer.value},
        {"master_channel",  r.master_channel},
        {"gain_db",         r.gain_db},
    };
}

void to_json(json& j, const MasterAssignment& a) {
    j = json{
        {"master_channel", a.master_channel},
        {"device",         a.device.value},
        {"hw_channel",     a.hw_channel},
    };
}

namespace {
audio::LTCFrameRate fps_index_to_rate(int idx) noexcept {
    switch (idx) {
        case 0: return audio::LTCFrameRate::Fps24;
        case 1: return audio::LTCFrameRate::Fps25;
        case 2: return audio::LTCFrameRate::Fps2997_NDF;
        case 3: return audio::LTCFrameRate::Fps2997_DF;
        default: return audio::LTCFrameRate::Fps30;
    }
}

// Convert a "HH:MM:SS:FF" (or "HH:MM:SS;FF" drop-frame) SMPTE string and a
// frame-rate index into a nanosecond offset suitable for LTCGenerator::configure().
// The offset is the timecode value at playhead position zero.
std::chrono::nanoseconds parse_smpte_timecode_to_ns(const std::string& tc,
                                                     int fps_index) noexcept {
    // Integer fps used for frame counting; real fps used for time conversion.
    static constexpr int    kFpsInt[]  = {24, 25, 30, 30, 30};
    static constexpr double kFpsReal[] = {24.0, 25.0,
                                          30000.0 / 1001.0,   // 29.97 NDF
                                          30000.0 / 1001.0,   // 29.97 DF
                                          30.0};
    const int   idx     = std::clamp(fps_index, 0, 4);
    const int   fps_int = kFpsInt[idx];
    const double fps    = kFpsReal[idx];

    int hh = 0, mm = 0, ss = 0, ff = 0;
    // Try both ':' separator (NDF) and ';' separator (DF convention).
    if (std::sscanf(tc.c_str(), "%d:%d:%d:%d", &hh, &mm, &ss, &ff) < 4)
        std::sscanf(tc.c_str(), "%d:%d:%d;%d", &hh, &mm, &ss, &ff);

    hh = std::clamp(hh, 0, 23);
    mm = std::clamp(mm, 0, 59);
    ss = std::clamp(ss, 0, 59);
    ff = std::clamp(ff, 0, fps_int - 1);

    const long long total_frames =
        static_cast<long long>(hh) * 3600LL * fps_int +
        static_cast<long long>(mm) *   60LL * fps_int +
        static_cast<long long>(ss)           * fps_int +
        static_cast<long long>(ff);

    const double seconds = static_cast<double>(total_frames) / fps;
    return std::chrono::nanoseconds{static_cast<long long>(seconds * 1e9)};
}

} // namespace

// ---------------------------------------------------------------------------

ProjectState::ProjectState(audio::AudioEngine& engine) : engine_(engine) {
    document_ = default_empty_document();
    start_sequencer();
}

ProjectState::~ProjectState() {
    stop_sequencer();
    // Make sure any in-flight async mirror finishes before the engine is
    // torn down — otherwise the worker would dereference dangling state.
    {
        std::lock_guard lock{mirror_mutex_};
        if (load_thread_.joinable()) load_thread_.join();
    }

    // Tear down preview infrastructure on shutdown so the audio device gets
    // released cleanly.
    if (!preview_cue_.empty()) {
        engine_.stop(preview_cue_);
        engine_.unload_cue(preview_cue_);
    }
    if (!preview_mixer_.empty()) {
        engine_.remove_mixer_channel(preview_mixer_);
    }
    if (!preview_device_.empty()) {
        engine_.close_device(preview_device_);
    }
}

void ProjectState::start_async_mirror() {
    // Wait for any prior background mirror to finish before launching a new
    // one — overlapping mirrors against the same engine state would race.
    std::lock_guard mirror_lock{mirror_mutex_};
    if (load_thread_.joinable()) load_thread_.join();

    loading_audio_.store(true, std::memory_order_release);
    load_progress_loaded_.store(0, std::memory_order_release);
    load_progress_total_.store(0, std::memory_order_release);

    load_thread_ = std::thread([this] {
        try {
            // Phase 1: snapshot what we need to load under a brief lock.
            std::unordered_map<std::string, std::filesystem::path> wanted;
            std::unordered_set<std::string> cart_uuids;
            // LTC: per-item settings snapshotted here, device opened between Phase 2/3.
            struct LtcItemSnap { bool enabled; std::string timecode; int fps_index; };
            std::unordered_map<std::string, LtcItemSnap> ltc_snaps;
            std::string ltc_device_name;
            std::unordered_map<std::string, std::filesystem::path> actually_wanted;
            {
                std::lock_guard lock{mutex_};
                json& doc = document_;
                for_each_item(doc, [&](json& item, const std::string&) {
                    if (item.value("type", std::string{}) != "audio") return;
                    const std::string uuid = item.value("uuid", std::string{});
                    if (uuid.empty()) return;
                    std::string path_str;
                    if (item.contains("mediaServerPath") && item["mediaServerPath"].is_string()) {
                        path_str = item["mediaServerPath"].get<std::string>();
                    }
                    if (path_str.empty() && item.contains("mediaPath") &&
                        item["mediaPath"].is_string()) {
                        const std::string folder = doc.value("folderPath", std::string{});
                        if (!folder.empty()) {
                            path_str = folder;
                            if (!path_str.empty() && path_str.back() != '/' &&
                                path_str.back() != '\\') path_str += '/';
                        }
                        path_str += item["mediaPath"].get<std::string>();
                    }
                    if (!path_str.empty()) {
                        wanted.emplace(uuid, util::utf8_to_path(path_str));
                    }
                    // Snapshot LTC settings for this item.
                    ltc_snaps[uuid] = LtcItemSnap{
                        item.value("ltcEnabled",        false),
                        item.value("ltcStartTimecode",  std::string{"00:00:00:00"}),
                        item.value("ltcFrameRate",       4),
                    };
                });
                if (doc.contains("cartItems") && doc["cartItems"].is_array()) {
                    for (const auto& c : doc["cartItems"]) {
                        if (c.is_object()) {
                            const std::string u = c.value("itemUuid", std::string{});
                            if (!u.empty()) cart_uuids.insert(u);
                        }
                    }
                }
                // Snapshot the project-level LTC output device name.
                if (doc.contains("settings") && doc["settings"].is_object()) {
                    const auto& s = doc["settings"];
                    if (s.contains("ltcDevice") && s["ltcDevice"].is_string())
                        ltc_device_name = s["ltcDevice"].get<std::string>();
                }

                // Unload missing cues
                for (auto it = item_uuid_to_cue_.begin(); it != item_uuid_to_cue_.end();) {
                    if (wanted.find(it->first) == wanted.end()) {
                        engine_.unload_cue(it->second);
                        cues_.erase(it->second.value);
                        it = item_uuid_to_cue_.erase(it);
                    } else {
                        ++it;
                    }
                }

                // Filter to only new items that haven't been loaded yet
                for (auto& [u, p] : wanted) {
                    if (item_uuid_to_cue_.find(u) == item_uuid_to_cue_.end()) {
                        actually_wanted.emplace(u, p);
                    }
                }
            }

            // Phase 2: parallel decoder init. NO project mutex — load_cue_no_route
            // only takes the engine's own internal lock. /api/project,
            // /api/cues, /api/project/progress all stay responsive while
            // we're here. The OS file I/O is what dominates anyway.
            load_progress_total_.store(actually_wanted.size(), std::memory_order_release);
            load_progress_loaded_.store(0, std::memory_order_release);

            const unsigned hw = std::thread::hardware_concurrency();
            const std::size_t concurrency = (hw <= 1) ? 1u : static_cast<std::size_t>(hw - 1);
            Logger::info("ProjectState: async-mirroring {} items ({} workers).",
                         actually_wanted.size(), concurrency);

            struct Loaded {
                std::string uuid;
                std::filesystem::path path;
                audio::CueId cue_id;
            };
            std::vector<std::future<Loaded>> in_flight;
            std::vector<Loaded> done;
            done.reserve(actually_wanted.size());
            auto drain_one = [&]() {
                if (in_flight.empty()) return;
                done.push_back(in_flight.front().get());
                in_flight.erase(in_flight.begin());
                load_progress_loaded_.fetch_add(1, std::memory_order_release);
            };
            for (auto& [uuid, path] : actually_wanted) {
                if (in_flight.size() >= concurrency) drain_one();
                in_flight.push_back(std::async(std::launch::async,
                    [this, u = uuid, p = path]() -> Loaded {
                        return { u, p, engine_.load_cue_no_route(p) };
                    }));
            }
            while (!in_flight.empty()) drain_one();

            // Phase 2.5: ensure the LTC output device is open and has a mixer
            // allocated BEFORE we take the mutex again in Phase 3. Doing it here
            // (no lock held) avoids a deadlock because ensure_device_routing()
            // acquires mutex_ internally.
            {
                bool any_ltc_enabled = false;
                for (auto& [_, ls] : ltc_snaps)
                    if (ls.enabled) { any_ltc_enabled = true; break; }
                if (any_ltc_enabled && !ltc_device_name.empty())
                    ensure_device_routing(ltc_device_name);
            }

            // Phase 3: register results + metadata under lock. Cheap because
            // the heavy I/O is already done — this is just hashtable inserts
            // and a single routing rebuild.
            {
                std::lock_guard lock{mutex_};
                for (auto& l : done) {
                    if (l.cue_id.empty()) {
                        Logger::warn("ProjectState: load failed uuid='{}'", l.uuid);
                        continue;
                    }
                    item_uuid_to_cue_.emplace(l.uuid, l.cue_id);

                    CueMeta meta;
                    meta.id           = l.cue_id;
                    meta.file_path    = l.path;
                    const auto md     = meta::read_metadata(l.path);
                    meta.display_name = md.title.empty()
                                          ? util::path_to_utf8(l.path.filename())
                                          : md.title;
                    meta.artist       = md.artist;
                    meta.title        = md.title;
                    meta.duration_seconds =
                        static_cast<double>(md.duration.count()) / 1000.0;
                    cues_.emplace(l.cue_id.value, std::move(meta));
                }

                // Apply per-item audio properties to the engine cues we just
                // registered (including LTC settings).
                for_each_item(document_,
                    [&](json& it, const std::string&) {
                        if (it.value("type", std::string{}) != "audio") return;
                        const std::string uuid = it.value("uuid", std::string{});
                        auto cit = item_uuid_to_cue_.find(uuid);
                        if (cit == item_uuid_to_cue_.end()) return;
                        auto* cue = engine_.find_cue(cit->second);
                        if (!cue) return;
                        if (it.contains("volume") && it["volume"].is_number()) {
                            const float lin = it["volume"].get<float>();
                            const float db  = (lin <= 0.0001f) ? -120.0f :
                                                20.0f * std::log10(lin);
                            cue->set_gain_db(db);
                        }
                        if (it.contains("playFade") && it["playFade"].is_number()) {
                            cue->set_fade_in(std::chrono::milliseconds{
                                static_cast<long long>(it["playFade"].get<double>() * 1000.0)});
                        }
                        if (it.contains("fadeOutDuration") && it["fadeOutDuration"].is_number()) {
                            cue->set_fade_out(std::chrono::milliseconds{
                                static_cast<long long>(it["fadeOutDuration"].get<double>() * 1000.0)});
                        }
                        if (it.contains("outPoint") && it["outPoint"].is_number()) {
                            cue->set_out_point_seconds(it["outPoint"].get<double>());
                        }

                        // LTC: configure on the PlaybackItem and route its
                        // synthetic channel to the LTC device mixer (which was
                        // opened in Phase 2.5 and is now in device_routings_).
                        auto ls_it = ltc_snaps.find(uuid);
                        if (ls_it != ltc_snaps.end() && ls_it->second.enabled) {
                            const auto& ls = ls_it->second;
                            const auto offset = parse_smpte_timecode_to_ns(ls.timecode, ls.fps_index);
                            cue->set_ltc_enabled(true);
                            cue->set_ltc_frame_rate(fps_index_to_rate(ls.fps_index));
                            cue->set_ltc_offset(offset);
                            // Persist into CueMeta so /api/cues reflects the setting.
                            auto cm_it = cues_.find(cit->second.value);
                            if (cm_it != cues_.end()) {
                                cm_it->second.ltc_enabled           = true;
                                cm_it->second.ltc_frame_rate_index  = ls.fps_index;
                                cm_it->second.ltc_offset_ns         = offset;
                                cm_it->second.ltc_start_timecode    = ls.timecode;
                            }
                            // Route the LTC synthetic channel to the LTC device mixer.
                            if (!ltc_device_name.empty()) {
                                auto dr_it = device_routings_.find(ltc_device_name);
                                if (dr_it != device_routings_.end()) {
                                    const auto ltc_ch = static_cast<audio::ChannelIndex>(
                                        cue->source_channel_count() - 1);
                                    engine_.route_item_source_to_mixer(
                                        cit->second, ltc_ch, dr_it->second.mixer, 0.0f);
                                }
                            }
                        }
                    });

                // Now that properties like ltc_enabled are applied, establish
                // default routing. This ensures LTC channels aren't mistakenly
                // routed to the Main mixer.
                engine_.ensure_default_routing();
            }

            // Phase 4: prime cart cues (also unlocked — engine handles its own).
            std::vector<std::future<void>> prime_futures;
            for (const auto& uuid : cart_uuids) {
                audio::CueId cue;
                {
                    std::lock_guard lock{mutex_};
                    auto it = item_uuid_to_cue_.find(uuid);
                    if (it == item_uuid_to_cue_.end()) continue;
                    cue = it->second;
                }
                prime_futures.push_back(std::async(std::launch::async,
                    [this, cue]() {
                        if (auto* pi = engine_.find_cue(cue)) pi->prime(2.0);
                    }));
            }
            for (auto& f : prime_futures) f.get();
            if (!cart_uuids.empty()) {
                Logger::info("ProjectState: primed {} cart cue(s).", cart_uuids.size());
            }
        } catch (const std::exception& e) {
            Logger::error("async mirror threw: {}", e.what());
        }
        loading_audio_.store(false, std::memory_order_release);
    });
}

void ProjectState::reset() {
    std::lock_guard lock{mutex_};
    cues_.clear();
    mixers_.clear();
    item_routes_.clear();
    mixer_routes_.clear();
    master_assignments_.clear();
    item_uuid_to_cue_.clear();
    project_name_ = "Untitled";
    project_file_path_.clear();
    document_ = default_empty_document();
    apply_to_engine_locked();
}

json ProjectState::default_empty_document() {
    // Mirror the client-side `Project` interface defaults so a fresh server
    // session looks identical to what `createNewProject` would have produced
    // on the client side. Field names are camelCase to match the client.
    return json{
        {"name",          "Untitled"},
        {"version",       "2.0.0"},
        {"folderPath",    ""},
        {"items",         json::array()},
        {"cartItems",     json::array()},
        {"cartSlotKeys",  json::object()},
        {"playbackKeys",  json::object()},
        {"cartOnlyItems", json::array()},
        {"theme",         json{{"mode", "dark"}, {"accentColor", "#DA1E28"}}},
        {"settings",      json{
            {"defaultOutputDevice", nullptr},
            {"previewDevice",       nullptr},
            {"ltcDevice",           nullptr},
        }},
        {"createdAt",     ""},
        {"lastModified",  ""},
    };
}

bool ProjectState::is_client_document(const json& doc) const {
    // Client-format heuristics: camelCase top-level fields that are unique
    // to the Electron client's `Project` interface and not present in the
    // server's snake_case schema_version 2 format.
    if (!doc.is_object()) return false;
    if (doc.contains("items") && doc["items"].is_array()) {
        // Confirm one item has uuid/type/displayName (camelCase) — that
        // distinguishes from any other "items" field we might add later.
        for (const auto& it : doc["items"]) {
            if (it.is_object() && it.contains("uuid") && it.contains("type")) {
                return true;
            }
        }
    }
    if (doc.contains("cartItems") || doc.contains("cartSlotKeys") ||
        doc.contains("cartOnlyItems")) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Document walker — visits every item (audio + group) in the document, depth
// first. Visits the item itself then recurses into group children.
// ---------------------------------------------------------------------------
void ProjectState::for_each_item(json& doc,
                                 const std::function<void(json&, const std::string&)>& visit) {
    std::function<void(json&, const std::string&)> walk;
    walk = [&](json& arr, const std::string& parent_uuid) {
        if (!arr.is_array()) return;
        for (auto& it : arr) {
            if (!it.is_object()) continue;
            visit(it, parent_uuid);
            if (it.value("type", std::string{}) == "group" &&
                it.contains("children") && it["children"].is_array()) {
                walk(it["children"], it.value("uuid", std::string{}));
            }
        }
    };
    if (doc.contains("items") && doc["items"].is_array()) {
        walk(doc["items"], "");
    }
    // cartOnlyItems are flat (no groups inside) but use the same walker for
    // consistency.
    if (doc.contains("cartOnlyItems") && doc["cartOnlyItems"].is_array()) {
        for (auto& it : doc["cartOnlyItems"]) {
            if (it.is_object()) visit(it, "");
        }
    }
}

// ---------------------------------------------------------------------------
// Mirror every audio item in document_ onto the engine + cue tables. Called
// after load and after a full-document replace. Existing engine cues that
// no longer have a matching item are unloaded.
// ---------------------------------------------------------------------------
void ProjectState::mirror_items_to_engine_locked() {
    // Collect uuid → file_path for current document.
    std::unordered_map<std::string, std::filesystem::path> wanted;
    for_each_item(document_,
        [&](json& item, const std::string& /*parent*/) {
            if (item.value("type", std::string{}) != "audio") return;
            const std::string uuid = item.value("uuid", std::string{});
            if (uuid.empty()) return;

            // Resolve file path: prefer mediaServerPath, else folderPath/mediaPath.
            std::string path_str;
            if (item.contains("mediaServerPath") && item["mediaServerPath"].is_string()) {
                path_str = item["mediaServerPath"].get<std::string>();
            }
            if (path_str.empty() && item.contains("mediaPath") &&
                item["mediaPath"].is_string()) {
                const std::string media_path = item["mediaPath"].get<std::string>();
                const std::string folder     = document_.value("folderPath", std::string{});
                if (!folder.empty()) {
                    path_str = folder;
                    if (!path_str.empty() && path_str.back() != '/' && path_str.back() != '\\') {
                        path_str += '/';
                    }
                    path_str += media_path;
                } else {
                    path_str = media_path;
                }
            }
            if (path_str.empty()) return;
            wanted.emplace(uuid, util::utf8_to_path(path_str));
        });

    // Unload any engine cues whose item is gone.
    for (auto it = item_uuid_to_cue_.begin(); it != item_uuid_to_cue_.end();) {
        if (wanted.find(it->first) == wanted.end()) {
            engine_.unload_cue(it->second);
            cues_.erase(it->second.value);
            it = item_uuid_to_cue_.erase(it);
        } else {
            ++it;
        }
    }

    // Gather the set of cart slot bindings so we can prioritise priming
    // those items (cart cues need to be hot — they can be triggered at any
    // moment by a hotkey or MIDI).
    std::unordered_set<std::string> cart_uuids;
    if (document_.contains("cartItems") && document_["cartItems"].is_array()) {
        for (const auto& c : document_["cartItems"]) {
            if (c.is_object()) {
                const std::string u = c.value("itemUuid", std::string{});
                if (!u.empty()) cart_uuids.insert(u);
            }
        }
    }

    // Build the list of new items to load (skip already-loaded). We do
    // metadata + decoder init in parallel because both are I/O bound.
    struct LoadJob {
        std::string uuid;
        std::filesystem::path path;
    };
    std::vector<LoadJob> jobs;
    for (auto& [uuid, path] : wanted) {
        if (item_uuid_to_cue_.find(uuid) == item_uuid_to_cue_.end()) {
            jobs.push_back({uuid, path});
        }
    }

    if (!jobs.empty()) {
        load_progress_total_.store(jobs.size(), std::memory_order_release);
        load_progress_loaded_.store(0, std::memory_order_release);
        // Use every CPU thread except one (leave one core for the OS/UI). On
        // single-core machines stay at 1; on hardware_concurrency() returning
        // 0 (rare) fall back to 1 as well.
        const unsigned hw = std::thread::hardware_concurrency();
        const std::size_t concurrency = (hw <= 1) ? 1u : static_cast<std::size_t>(hw - 1);
        Logger::info("ProjectState: bulk-loading {} audio items ({} parallel workers).",
                     jobs.size(), concurrency);
        std::vector<std::future<std::pair<std::string, audio::CueId>>> futures;
        futures.reserve(jobs.size());

        // Issue all jobs but cap in-flight concurrency by waiting once we
        // hit the limit. This keeps memory pressure / fd usage bounded for
        // very large projects.
        std::vector<std::pair<std::string, audio::CueId>> done;
        done.reserve(jobs.size());

        auto drain_one = [&]() {
            if (futures.empty()) return;
            done.push_back(futures.front().get());
            futures.erase(futures.begin());
            load_progress_loaded_.fetch_add(1, std::memory_order_release);
        };

        for (const auto& job : jobs) {
            if (futures.size() >= concurrency) drain_one();
            futures.push_back(std::async(std::launch::async,
                [this, job]() -> std::pair<std::string, audio::CueId> {
                    const auto cue_id = engine_.load_cue_no_route(job.path);
                    return {job.uuid, cue_id};
                }));
        }
        while (!futures.empty()) drain_one();

        // Register results sequentially (cues_ access is single-threaded under
        // the lock the caller holds).
        for (auto& [uuid, cue_id] : done) {
            if (cue_id.empty()) {
                Logger::warn("ProjectState: failed to load item uuid='{}'", uuid);
                continue;
            }
            item_uuid_to_cue_.emplace(uuid, cue_id);

            const auto file_path = wanted[uuid];
            CueMeta meta;
            meta.id           = cue_id;
            meta.file_path    = file_path;
            const auto md     = meta::read_metadata(file_path);
            meta.display_name = md.title.empty()
                                  ? util::path_to_utf8(file_path.filename())
                                  : md.title;
            meta.artist       = md.artist;
            meta.title        = md.title;
            meta.duration_seconds =
                static_cast<double>(md.duration.count()) / 1000.0;
            cues_.emplace(cue_id.value, std::move(meta));
        }

        // Prime cart cues in parallel so their first hit is glitch-free.
        // Non-cart items are primed on-demand at play time.
        std::vector<std::future<void>> prime_futures;
        for (const auto& uuid : cart_uuids) {
            auto it = item_uuid_to_cue_.find(uuid);
            if (it == item_uuid_to_cue_.end()) continue;
            auto* pi = engine_.find_cue(it->second);
            if (!pi) continue;
            prime_futures.push_back(std::async(std::launch::async,
                [pi]() { pi->prime(2.0); }));
        }
        for (auto& f : prime_futures) f.get();
        if (!cart_uuids.empty()) {
            Logger::info("ProjectState: primed {} cart cue(s).",
                         cart_uuids.size());
        }
    }

    // Apply per-item audio properties (gain/fade/in-out/LTC/etc.) to the engine.
    // NOTE: LTC *device routing* is handled separately in apply_ltc_device_routing()
    // which is called by callers after they release mutex_ (because
    // ensure_device_routing() also needs to acquire mutex_).
    for_each_item(document_,
        [&](json& item, const std::string& /*parent*/) {
            if (item.value("type", std::string{}) != "audio") return;
            const std::string uuid = item.value("uuid", std::string{});
            auto it = item_uuid_to_cue_.find(uuid);
            if (it == item_uuid_to_cue_.end()) return;
            auto* cue = engine_.find_cue(it->second);
            if (!cue) return;

            // volume: 0..2 linear (matches the client). Engine takes dB.
            if (item.contains("volume") && item["volume"].is_number()) {
                const float lin = item["volume"].get<float>();
                const float db  = (lin <= 0.0001f) ? -120.0f :
                                    20.0f * std::log10(lin);
                cue->set_gain_db(db);
            }
            if (item.contains("playFade") && item["playFade"].is_number()) {
                cue->set_fade_in(std::chrono::milliseconds{
                    static_cast<long long>(item["playFade"].get<double>() * 1000.0)});
            }
            // Manual-stop fade-out: the UI's "STOP FADE OUT" slider writes to
            // `stopFade`, which is also used by the sequencer to begin fading
            // before natural end. We expose the larger of the two as the
            // PlaybackItem's fade_out_duration so the stop button (and global
            // stop) honour whichever value the user actually configured —
            // without breaking legacy projects that only set fadeOutDuration.
            {
                double stop_fade_sec = 0.0;
                double fade_out_dur  = 0.0;
                if (item.contains("stopFade") && item["stopFade"].is_number())
                    stop_fade_sec = item["stopFade"].get<double>();
                if (item.contains("fadeOutDuration") && item["fadeOutDuration"].is_number())
                    fade_out_dur = item["fadeOutDuration"].get<double>();
                const double effective = std::max(stop_fade_sec, fade_out_dur);
                cue->set_fade_out(std::chrono::milliseconds{
                    static_cast<long long>(effective * 1000.0)});
            }
            // outPoint: when set (> 0), engine fades out as the playhead reaches
            // that time instead of running to the file end.
            if (item.contains("outPoint") && item["outPoint"].is_number()) {
                cue->set_out_point_seconds(item["outPoint"].get<double>());
            } else {
                cue->set_out_point_seconds(0.0);  // disabled
            }

            // LTC: configure enabled/rate/offset on the PlaybackItem.
            // Routing of the synthetic LTC channel to the ltcDevice is done
            // AFTER this function returns (via apply_ltc_device_routing()).
            const bool ltc_on = item.value("ltcEnabled", false);
            const std::string tc_str = item.value("ltcStartTimecode",
                                                   std::string{"00:00:00:00"});
            const int fps_idx = item.value("ltcFrameRate", 4);
            cue->set_ltc_enabled(ltc_on);
            if (ltc_on) {
                const auto offset = parse_smpte_timecode_to_ns(tc_str, fps_idx);
                cue->set_ltc_frame_rate(fps_index_to_rate(fps_idx));
                cue->set_ltc_offset(offset);
                auto cm_it = cues_.find(it->second.value);
                if (cm_it != cues_.end()) {
                    cm_it->second.ltc_enabled          = true;
                    cm_it->second.ltc_frame_rate_index = fps_idx;
                    cm_it->second.ltc_offset_ns        = offset;
                    cm_it->second.ltc_start_timecode   = tc_str;
                }
            }
        });

    // Now that every cue is in items_ and properties like ltc_enabled are
    // configured, establish default routing ONCE.
    engine_.ensure_default_routing();
}

// ---------------------------------------------------------------------------
// Cue mutations (control thread)
// ---------------------------------------------------------------------------
audio::CueId ProjectState::add_cue_from_file(const std::filesystem::path& file,
                                             std::string display_name) {
    // De-dupe: if a cue is already loaded for this file path (typical case
    // is the project mirror loading every item up front), reuse it instead
    // of creating a parallel engine cue. Without this, the legacy
    // ServerHowl path creates a *second* cue for every project item, and
    // play(cue_id) on that orphan bypasses ProjectState::play_item — which
    // is what carries duckingBehavior / inPoint / fades / endBehavior into
    // the engine. The user-visible symptom is in/out points and Up Next
    // not firing.
    {
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(file, ec);
        const auto& want = ec ? file : canonical;
        std::lock_guard lock{mutex_};
        for (auto& [id, c] : cues_) {
            std::error_code ec2;
            const auto have = std::filesystem::weakly_canonical(c.file_path, ec2);
            const auto& cmp = ec2 ? c.file_path : have;
            if (cmp == want) return audio::CueId{id};
        }
    }
    const auto cue_id = engine_.load_cue(file);
    if (cue_id.empty()) return {};

    // Populate artist/title/duration via TagLib (best-effort; never fatal).
    const auto md = meta::read_metadata(file);

    std::lock_guard lock{mutex_};
    CueMeta meta;
    meta.id           = cue_id;
    meta.display_name = display_name.empty()
                          ? (md.title.empty() ? util::path_to_utf8(file.filename()) : md.title)
                          : std::move(display_name);
    meta.file_path    = file;
    meta.artist       = md.artist;
    meta.title        = md.title;
    meta.duration_seconds = static_cast<double>(md.duration.count()) / 1000.0;
    cues_.emplace(cue_id.value, std::move(meta));
    return cue_id;
}

void ProjectState::remove_cue(const audio::CueId& id) {
    // Don't unload a cue that belongs to a project item — multiple
    // ServerHowl instances on the client may share it (add_cue_from_file
    // dedupes by path), and removing it would yank audio out from under
    // a sibling that's still using it.
    {
        std::lock_guard lock{mutex_};
        for (const auto& [_, cue_id] : item_uuid_to_cue_) {
            if (cue_id.value == id.value) return;
        }
    }
    engine_.unload_cue(id);
    std::lock_guard lock{mutex_};
    cues_.erase(id.value);
    item_routes_.erase(std::remove_if(item_routes_.begin(), item_routes_.end(),
                                       [&](const RouteSendV2& /*r*/){
                                           // can't easily tell which cue this belonged to without
                                           // tagging — for now we just drop nothing here. The engine
                                           // already dropped the cue's routes when unload_cue ran.
                                           return false;
                                       }),
                       item_routes_.end());
}

void ProjectState::rename_cue(const audio::CueId& id, std::string new_name) {
    std::lock_guard lock{mutex_};
    auto it = cues_.find(id.value);
    if (it == cues_.end()) return;
    it->second.display_name = std::move(new_name);
}

void ProjectState::set_cue_gain_db(const audio::CueId& id, float db) {
    if (auto* item = engine_.find_cue(id)) item->set_gain_db(db);
    std::lock_guard lock{mutex_};
    auto it = cues_.find(id.value);
    if (it != cues_.end()) it->second.gain_db = db;
}

void ProjectState::set_cue_fade_in(const audio::CueId& id, std::chrono::milliseconds d) {
    if (auto* item = engine_.find_cue(id)) item->set_fade_in(d);
    std::lock_guard lock{mutex_};
    auto it = cues_.find(id.value);
    if (it != cues_.end()) it->second.fade_in_ms = d;
}

void ProjectState::set_cue_fade_out(const audio::CueId& id, std::chrono::milliseconds d) {
    if (auto* item = engine_.find_cue(id)) item->set_fade_out(d);
    std::lock_guard lock{mutex_};
    auto it = cues_.find(id.value);
    if (it != cues_.end()) it->second.fade_out_ms = d;
}

void ProjectState::set_cue_ltc(const audio::CueId& id, bool enabled, int fps_index,
                                std::chrono::nanoseconds offset) {
    if (auto* item = engine_.find_cue(id)) {
        item->set_ltc_enabled(enabled);
        item->set_ltc_frame_rate(fps_index_to_rate(fps_index));
        item->set_ltc_offset(offset);
    }
    std::lock_guard lock{mutex_};
    auto it = cues_.find(id.value);
    if (it == cues_.end()) return;
    it->second.ltc_enabled = enabled;
    it->second.ltc_frame_rate_index = fps_index;
    it->second.ltc_offset_ns = offset;
}

// ---------------------------------------------------------------------------
// LTC device routing — called from outside the mutex so ensure_device_routing
// (which acquires the mutex itself) can safely do its work.
// ---------------------------------------------------------------------------
void ProjectState::apply_ltc_device_routing() {
    // 1. Under a brief lock, gather: the configured LTC device name and the
    //    list of LTC-enabled cues with their LTC channel index.
    std::string ltc_device;
    std::vector<std::pair<audio::CueId, audio::ChannelIndex>> ltc_routes;
    {
        std::lock_guard lock{mutex_};
        if (document_.contains("settings") && document_["settings"].is_object()) {
            const auto& s = document_["settings"];
            if (s.contains("ltcDevice") && s["ltcDevice"].is_string())
                ltc_device = s["ltcDevice"].get<std::string>();
        }
        if (!ltc_device.empty()) {
            for (auto& [uuid, cue_id] : item_uuid_to_cue_) {
                auto* pi = engine_.find_cue(cue_id);
                if (!pi || !pi->desc().ltc_enabled) continue;
                // The LTC synthetic channel is always the last source channel.
                const auto ltc_ch = static_cast<audio::ChannelIndex>(
                    pi->source_channel_count() - 1);
                ltc_routes.push_back({cue_id, ltc_ch});
            }
        }
    }

    if (ltc_device.empty() || ltc_routes.empty()) return;

    // 2. Ensure the LTC device is open and has a mixer (acquires/releases mutex
    //    internally — safe because we're not holding mutex_ here).
    const auto ltc_mixer = ensure_device_routing(ltc_device);
    if (ltc_mixer.empty()) return;

    // 3. Route each LTC channel to the LTC device mixer (engine ops; no mutex
    //    needed — the engine has its own independent synchronisation).
    for (auto& [cue_id, ltc_ch] : ltc_routes)
        engine_.route_item_source_to_mixer(cue_id, ltc_ch, ltc_mixer, 0.0f);
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------
std::vector<CueMeta> ProjectState::list_cues() const {
    std::lock_guard lock{mutex_};
    std::vector<CueMeta> out;
    out.reserve(cues_.size());
    for (auto& [_, c] : cues_) out.push_back(c);
    return out;
}

std::optional<CueMeta> ProjectState::find_cue(const audio::CueId& id) const {
    std::lock_guard lock{mutex_};
    auto it = cues_.find(id.value);
    if (it == cues_.end()) return std::nullopt;
    return it->second;
}

std::vector<MixerChannelMeta> ProjectState::list_mixer_channels() const {
    std::lock_guard lock{mutex_};
    std::vector<MixerChannelMeta> out;
    out.reserve(mixers_.size());
    for (auto& [_, m] : mixers_) out.push_back(m);
    return out;
}

std::filesystem::path ProjectState::media_root() const {
    std::lock_guard lock{mutex_};
    return media_root_;
}

void ProjectState::set_media_root(std::filesystem::path p) {
    std::lock_guard lock{mutex_};
    media_root_ = std::move(p);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
json ProjectState::to_json() const {
    std::lock_guard lock{mutex_};
    json j;
    j["schema_version"] = 2;
    j["project_name"]   = project_name_;
    j["media_root"]     = util::path_to_utf8(media_root_);

    json cues_arr = json::array();
    for (auto& [_, c] : cues_) cues_arr.push_back(c);
    j["cues"] = std::move(cues_arr);

    json mixers_arr = json::array();
    for (auto& [_, m] : mixers_) mixers_arr.push_back(m);
    j["mixer_channels"] = std::move(mixers_arr);

    j["item_routes"]    = item_routes_;
    j["mixer_routes"]   = mixer_routes_;
    j["master_assignments"] = master_assignments_;
    return j;
}

bool ProjectState::save(const std::filesystem::path& path) const {
    // Persist the full client-shaped document — this is what the Electron
    // client expects to read back. Server-only tables (mixer routing,
    // engine state) live on the side and don't get written to disk here;
    // they're rebuilt from the document on next load.
    try {
        std::ofstream f{path};
        if (!f) {
            Logger::error("ProjectState::save: cannot open '{}' for writing",
                          util::path_to_utf8(path));
            return false;
        }
        json doc;
        {
            std::lock_guard lock{mutex_};
            doc = document_;
            doc["lastModified"] = now_iso();
        }
        f << doc.dump(2);
        return true;
    } catch (const std::exception& ex) {
        Logger::error("ProjectState::save failed: {}", ex.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Full-document accessors
// ---------------------------------------------------------------------------
namespace {
// Walk an items array (or a group's children) and decorate each audio
// item with its engine cueId. Recurses into groups. Caller holds the
// project mutex.
void annotate_items_with_cue_ids(
    json& arr,
    const std::unordered_map<std::string, audio::CueId>& item_uuid_to_cue) {
    if (!arr.is_array()) return;
    for (auto& it : arr) {
        if (!it.is_object()) continue;
        const std::string uuid = it.value("uuid", std::string{});
        if (it.value("type", std::string{}) == "audio") {
            auto found = item_uuid_to_cue.find(uuid);
            if (found != item_uuid_to_cue.end()) {
                it["cueId"] = found->second.value;
            }
        } else if (it.value("type", std::string{}) == "group" &&
                   it.contains("children")) {
            annotate_items_with_cue_ids(it["children"], item_uuid_to_cue);
        }
    }
}
} // namespace

json ProjectState::full_document() const {
    std::lock_guard lock{mutex_};
    json out = document_;
    if (out.contains("items"))         annotate_items_with_cue_ids(out["items"],         item_uuid_to_cue_);
    if (out.contains("cartOnlyItems")) annotate_items_with_cue_ids(out["cartOnlyItems"], item_uuid_to_cue_);

    // Attach a minimal "server" block so the client can read project file
    // path, available engine cues, etc. without a separate fetch.
    out["server"] = json{
        {"projectFilePath", util::path_to_utf8(project_file_path_)},
        {"mediaRoot",       util::path_to_utf8(media_root_)},
        {"audioLoading",    loading_audio_.load(std::memory_order_acquire)},
        {"audioLoaded",     load_progress_loaded_.load(std::memory_order_acquire)},
        {"audioTotal",      load_progress_total_.load(std::memory_order_acquire)},
    };
    return out;
}

json ProjectState::header_document() const {
    std::lock_guard lock{mutex_};
    // Cart-only items carry waveform data is already lazy, but the array
    // itself is small relative to the full playlist. We DO include it
    // because the cart slots reference these items and the workspace
    // can't paint cart buttons without them.
    json cart_only = document_.value("cartOnlyItems", json::array());
    annotate_items_with_cue_ids(cart_only, item_uuid_to_cue_);

    const auto& items = document_.value("items", json::array());
    const std::size_t item_count = items.is_array() ? items.size() : 0;

    return json{
        {"name",         document_.value("name", "")},
        {"version",      document_.value("version", "")},
        {"folderPath",   document_.value("folderPath", "")},
        {"createdAt",    document_.value("createdAt", "")},
        {"lastModified", read_last_modified(document_)},
        {"theme",        document_.value("theme",         json::object())},
        {"settings",     document_.value("settings",      json::object())},
        {"cartItems",    document_.value("cartItems",     json::array())},
        {"cartSlotKeys", document_.value("cartSlotKeys",  json::object())},
        {"playbackKeys", document_.value("playbackKeys",  json::object())},
        {"cartOnlyItems", std::move(cart_only)},
        {"itemCount",    item_count},
        // "Open" means a real project landed — either it has items or it
        // was loaded/saved from disk. A fresh server has a default name
        // "Untitled" but no file path and no items, so the welcome screen
        // should still show New/Open.
        {"hasOpenProject", item_count > 0 ||
                           !project_file_path_.empty()},
        {"server", json{
            {"projectFilePath", util::path_to_utf8(project_file_path_)},
            {"mediaRoot",       util::path_to_utf8(media_root_)},
            {"audioLoading",    loading_audio_.load(std::memory_order_acquire)},
            {"audioLoaded",     load_progress_loaded_.load(std::memory_order_acquire)},
            {"audioTotal",      load_progress_total_.load(std::memory_order_acquire)},
        }},
    };
}

json ProjectState::items_page(std::size_t offset, std::size_t limit) const {
    std::lock_guard lock{mutex_};
    const auto& items = document_.value("items", json::array());
    const std::size_t total = items.is_array() ? items.size() : 0;
    if (offset > total) offset = total;
    const std::size_t end = (limit > total - offset) ? total : offset + limit;

    json page = json::array();
    if (items.is_array()) {
        for (std::size_t i = offset; i < end; ++i) {
            page.push_back(items[i]);
        }
    }
    annotate_items_with_cue_ids(page, item_uuid_to_cue_);

    return json{
        {"offset", offset},
        {"limit",  limit},
        {"total",  total},
        {"items",  std::move(page)},
    };
}

bool ProjectState::replace_full_document(const json& doc) {
    if (!doc.is_object()) return false;
    {
        std::lock_guard lock{mutex_};
        document_ = doc;
        if (!document_.contains("settings")) {
            document_["settings"] = json{
                {"defaultOutputDevice", nullptr},
                {"previewDevice",       nullptr},
                {"ltcDevice",           nullptr},
            };
        }
        if (!document_.contains("theme")) {
            document_["theme"] = json{{"mode", "dark"}, {"accentColor", "#DA1E28"}};
        }
        project_name_ = document_.value("name", std::string{"Untitled"});
    }
    // Kick off the engine mirror asynchronously — matches load_from_json's
    // path so the PUT /api/project/document handler doesn't block on cue
    // decode for large projects. start_async_mirror() takes mutex_ itself,
    // so it must run after the lock above is released.
    start_async_mirror();
    // Route LTC channels to the LTC device (also acquires mutex_ internally).
    apply_ltc_device_routing();
    return true;
}

std::filesystem::path ProjectState::project_file_path() const {
    std::lock_guard lock{mutex_};
    return project_file_path_;
}
void ProjectState::set_project_file_path(std::filesystem::path p) {
    std::lock_guard lock{mutex_};
    project_file_path_ = std::move(p);
}

// ---------------------------------------------------------------------------
// Item CRUD — operates on the document_ tree and mirrors audio items to
// the engine.
// ---------------------------------------------------------------------------
audio::CueId ProjectState::add_item(const json& item, const std::string& parent_uuid) {
    if (!item.is_object()) return {};
    const std::string uuid = item.value("uuid", std::string{});
    if (uuid.empty()) return {};

    audio::CueId result;
    {
        std::lock_guard lock{mutex_};

        // Reject duplicates — the same UUID must not appear twice in the document.
        bool already_exists = false;
        for_each_item(document_, [&](json& it, const std::string&) {
            if (it.value("uuid", std::string{}) == uuid) already_exists = true;
        });
        if (already_exists) {
            Logger::warn("add_item: uuid '{}' already exists, ignoring duplicate", uuid);
            auto it = item_uuid_to_cue_.find(uuid);
            return it != item_uuid_to_cue_.end() ? it->second : audio::CueId{};
        }

        if (parent_uuid.empty()) {
            if (!document_.contains("items") || !document_["items"].is_array()) {
                document_["items"] = json::array();
            }
            document_["items"].push_back(item);
        } else {
            // Find the parent group and append.
            bool found = false;
            for_each_item(document_,
                [&](json& it, const std::string& /*parent*/) {
                    if (found) return;
                    if (it.value("uuid", std::string{}) == parent_uuid &&
                        it.value("type", std::string{}) == "group") {
                        if (!it.contains("children") || !it["children"].is_array()) {
                            it["children"] = json::array();
                        }
                        it["children"].push_back(item);
                        found = true;
                    }
                });
            if (!found) {
                Logger::warn("add_item: parent_uuid '{}' not found, appending to root",
                             parent_uuid);
                document_["items"].push_back(item);
            }
        }
        mirror_items_to_engine_locked();
        auto it = item_uuid_to_cue_.find(uuid);
        if (it != item_uuid_to_cue_.end()) result = it->second;
    }
    // Route any LTC-enabled items to the LTC device (after releasing mutex_).
    apply_ltc_device_routing();
    return result;
}

bool ProjectState::update_item(const std::string& uuid, const json& patch) {
    if (!patch.is_object()) return false;
    bool touched            = false;
    bool media_path_changed = false;
    bool ltc_changed        = false;
    json* updated_item = nullptr;
    {
        std::lock_guard lock{mutex_};
        for_each_item(document_,
            [&](json& it, const std::string& /*parent*/) {
                if (it.value("uuid", std::string{}) != uuid) return;
                for (auto& [k, v] : patch.items()) {
                    if (k == "uuid") continue;
                    if (k == "mediaPath" || k == "mediaServerPath" ||
                        k == "mediaFileName") {
                        if (!it.contains(k) || it[k] != v)
                            media_path_changed = true;
                    }
                    if (k == "ltcEnabled" || k == "ltcStartTimecode" ||
                        k == "ltcFrameRate") {
                        if (!it.contains(k) || it[k] != v)
                            ltc_changed = true;
                    }
                    it[k] = v;
                }
                touched = true;
                updated_item = &it;
            });
        if (touched && media_path_changed) {
            mirror_items_to_engine_locked();
        } else if (touched && updated_item) {
            // Cheap path: apply audio-engine-visible properties to the
            // existing PlaybackItem without a full mirror walk.
            auto cit = item_uuid_to_cue_.find(uuid);
            if (cit != item_uuid_to_cue_.end()) {
                if (auto* cue = engine_.find_cue(cit->second)) {
                    const json& it = *updated_item;
                    if (it.contains("volume") && it["volume"].is_number()) {
                        const float lin = it["volume"].get<float>();
                        const float db  = (lin <= 0.0001f) ? -120.0f :
                                            20.0f * std::log10(lin);
                        cue->set_gain_db(db);
                    }
                    if (it.contains("playFade") && it["playFade"].is_number()) {
                        cue->set_fade_in(std::chrono::milliseconds{
                            static_cast<long long>(it["playFade"].get<double>() * 1000.0)});
                    }
                    // Same max(stopFade, fadeOutDuration) rule as
                    // mirror_items_to_engine_locked() — keep them in sync.
                    {
                        double stop_fade_sec = 0.0;
                        double fade_out_dur  = 0.0;
                        if (it.contains("stopFade") && it["stopFade"].is_number())
                            stop_fade_sec = it["stopFade"].get<double>();
                        if (it.contains("fadeOutDuration") && it["fadeOutDuration"].is_number())
                            fade_out_dur = it["fadeOutDuration"].get<double>();
                        const double effective = std::max(stop_fade_sec, fade_out_dur);
                        cue->set_fade_out(std::chrono::milliseconds{
                            static_cast<long long>(effective * 1000.0)});
                    }
                    if (it.contains("outPoint") && it["outPoint"].is_number()) {
                        cue->set_out_point_seconds(it["outPoint"].get<double>());
                    }
                    // LTC property update: configure the PlaybackItem in-place.
                    if (ltc_changed) {
                        const bool ltc_on = it.value("ltcEnabled", false);
                        const std::string tc_str = it.value("ltcStartTimecode",
                                                            std::string{"00:00:00:00"});
                        const int fps_idx = it.value("ltcFrameRate", 4);
                        cue->set_ltc_enabled(ltc_on);
                        if (ltc_on) {
                            const auto offset = parse_smpte_timecode_to_ns(tc_str, fps_idx);
                            cue->set_ltc_frame_rate(fps_index_to_rate(fps_idx));
                            cue->set_ltc_offset(offset);
                            auto cm_it = cues_.find(cit->second.value);
                            if (cm_it != cues_.end()) {
                                cm_it->second.ltc_enabled          = true;
                                cm_it->second.ltc_frame_rate_index = fps_idx;
                                cm_it->second.ltc_offset_ns        = offset;
                                cm_it->second.ltc_start_timecode   = tc_str;
                            }
                        } else {
                            auto cm_it = cues_.find(cit->second.value);
                            if (cm_it != cues_.end())
                                cm_it->second.ltc_enabled = false;
                        }
                    }
                }
            }
        }
    }
    // Route (or re-route) the LTC channel after releasing mutex_ so
    // ensure_device_routing() can safely acquire it.
    if (touched && ltc_changed)
        apply_ltc_device_routing();
    return touched;
}

bool ProjectState::remove_item(const std::string& uuid) {
    bool removed = false;
    std::function<bool(json&)> erase_from;
    erase_from = [&](json& arr) -> bool {
        if (!arr.is_array()) return false;
        for (auto it = arr.begin(); it != arr.end(); ++it) {
            if (!it->is_object()) continue;
            if (it->value("uuid", std::string{}) == uuid) {
                arr.erase(it);
                return true;
            }
            if (it->value("type", std::string{}) == "group" &&
                it->contains("children")) {
                if (erase_from((*it)["children"])) return true;
            }
        }
        return false;
    };
    {
        std::lock_guard lock{mutex_};
        if (document_.contains("items")) {
            removed = erase_from(document_["items"]);
        }
        if (!removed && document_.contains("cartOnlyItems")) {
            removed = erase_from(document_["cartOnlyItems"]);
        }
        // Clean up cart bindings that reference this uuid.
        if (document_.contains("cartItems") && document_["cartItems"].is_array()) {
            auto& arr = document_["cartItems"];
            arr.erase(std::remove_if(arr.begin(), arr.end(),
                                     [&](const json& c){
                                         return c.value("itemUuid", std::string{}) == uuid;
                                     }),
                      arr.end());
        }
        if (removed) mirror_items_to_engine_locked();
    }
    return removed;
}

bool ProjectState::reorder_items(const std::vector<std::string>& uuids,
                                 const std::string& parent_uuid) {
    std::lock_guard lock{mutex_};
    json* target = nullptr;
    if (parent_uuid.empty()) {
        if (document_.contains("items") && document_["items"].is_array()) {
            target = &document_["items"];
        }
    } else {
        for_each_item(document_,
            [&](json& it, const std::string& /*parent*/) {
                if (target) return;
                if (it.value("uuid", std::string{}) == parent_uuid &&
                    it.value("type", std::string{}) == "group" &&
                    it.contains("children") && it["children"].is_array()) {
                    target = &it["children"];
                }
            });
    }
    if (!target) return false;

    // Pull items out into a uuid → json map, then rebuild in the requested
    // order. Items whose uuid isn't in `uuids` are appended at the end so we
    // never lose data on a partial reorder.
    std::unordered_map<std::string, json> by_uuid;
    for (auto& it : *target) {
        if (it.is_object()) {
            by_uuid.emplace(it.value("uuid", std::string{}), std::move(it));
        }
    }
    json rebuilt = json::array();
    for (const auto& u : uuids) {
        auto found = by_uuid.find(u);
        if (found != by_uuid.end()) {
            rebuilt.push_back(std::move(found->second));
            by_uuid.erase(found);
        }
    }
    for (auto& [_, v] : by_uuid) rebuilt.push_back(std::move(v));
    *target = std::move(rebuilt);
    return true;
}

std::optional<std::filesystem::path>
ProjectState::resolve_item_path(const std::string& uuid) const {
    std::lock_guard lock{mutex_};
    std::optional<std::filesystem::path> out;
    // Walk a copy-free view: we're const, so for_each_item takes a non-const
    // json — work around with a const_cast since we only read inside the
    // lambda. The lambda mutates nothing.
    json& doc_ref = const_cast<json&>(document_);
    for_each_item(doc_ref,
        [&](json& item, const std::string& /*parent*/) {
            if (out) return;
            if (item.value("uuid", std::string{}) != uuid) return;
            std::string path_str;
            if (item.contains("mediaServerPath") &&
                item["mediaServerPath"].is_string()) {
                path_str = item["mediaServerPath"].get<std::string>();
            }
            if (path_str.empty() && item.contains("mediaPath") &&
                item["mediaPath"].is_string()) {
                const std::string folder =
                    doc_ref.value("folderPath", std::string{});
                if (!folder.empty()) {
                    path_str = folder;
                    if (!path_str.empty() && path_str.back() != '/' &&
                        path_str.back() != '\\') path_str += '/';
                }
                path_str += item["mediaPath"].get<std::string>();
            }
            if (!path_str.empty()) out = util::utf8_to_path(path_str);
        });
    return out;
}

std::optional<audio::CueId>
ProjectState::item_to_cue_id(const std::string& uuid) const {
    std::lock_guard lock{mutex_};
    auto it = item_uuid_to_cue_.find(uuid);
    if (it == item_uuid_to_cue_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string>
ProjectState::cue_to_item_uuid(const audio::CueId& id) const {
    std::lock_guard lock{mutex_};
    for (const auto& [uuid, cue_id] : item_uuid_to_cue_) {
        if (cue_id.value == id.value) return uuid;
    }
    return std::nullopt;
}

std::string ProjectState::resolve_next_item_locked(const std::string& current_uuid) const {
    // The lookup is read-only; we need a mutable json& to satisfy for_each_item
    // (which itself only reads). const_cast is safe here for the same reason
    // as in resolve_item_path().
    json& doc = const_cast<json&>(document_);

    // 1) Find the current item + its endBehavior.
    std::string end_action;
    std::string end_target_uuid;
    std::string parent_uuid;
    bool found = false;
    for_each_item(doc,
        [&](json& it, const std::string& parent) {
            if (found) return;
            if (it.value("uuid", std::string{}) != current_uuid) return;
            found = true;
            parent_uuid = parent;
            if (it.contains("endBehavior") && it["endBehavior"].is_object()) {
                const auto& eb = it["endBehavior"];
                end_action      = eb.value("action", std::string{"nothing"});
                end_target_uuid = eb.value("targetUuid", std::string{});
            }
        });
    if (!found) return {};

    if (end_action == "goto-item" && !end_target_uuid.empty()) {
        return end_target_uuid;
    }
    if (end_action == "loop" || end_action == "nothing" || end_action.empty()) {
        return {};   // either replay self or stop; nothing new to prime
    }
    if (end_action != "next") return {};

    // 2) "next" — return the sibling immediately following `current_uuid` in
    // the same level (top or inside a group). Look at the parent's children
    // array; if `current_uuid` is the last, fall back to nothing.
    auto find_next_in_array = [&](json& arr) -> std::string {
        if (!arr.is_array()) return {};
        for (std::size_t i = 0; i + 1 < arr.size(); ++i) {
            if (arr[i].is_object() &&
                arr[i].value("uuid", std::string{}) == current_uuid) {
                const auto& next = arr[i + 1];
                if (next.is_object()) return next.value("uuid", std::string{});
            }
        }
        return {};
    };

    if (parent_uuid.empty()) {
        // Top-level.
        if (doc.contains("items")) {
            const auto next = find_next_in_array(doc["items"]);
            if (!next.empty()) return next;
        }
        return {};
    }

    // Inside a group — find the group, then look in its children.
    std::string result;
    for_each_item(doc,
        [&](json& it, const std::string& /*p*/) {
            if (!result.empty()) return;
            if (it.value("uuid", std::string{}) != parent_uuid) return;
            if (it.value("type", std::string{}) != "group") return;
            if (!it.contains("children")) return;
            result = find_next_in_array(it["children"]);
        });
    return result;
}

// ---------------------------------------------------------------------------
// Item-level transport with ducking + in/out point semantics.
// ---------------------------------------------------------------------------
bool ProjectState::play_item(const std::string& uuid) {
    // Snapshot everything we need under the lock, then release before
    // touching the engine (engine calls take their own locks).
    std::string  ducking_mode  = "stop-all";
    float        duck_level    = 0.2f;
    double       in_point      = 0.0;
    double       out_point     = 0.0;
    double       fade_out_dur  = 1.0;
    double       crossfade_sec = 0.0;
    double       stop_fade_sec = 0.0;
    std::string  device_override;
    std::string  start_behavior_action;
    std::string  start_behavior_target_uuid;
    std::string  end_behavior_action;
    audio::CueId target_cue;
    std::vector<audio::CueId> other_cues;

    std::vector<ScheduledCustomAction> custom_actions_snapshot;
    {
        std::lock_guard lock{mutex_};
        auto cue_it = item_uuid_to_cue_.find(uuid);
        if (cue_it == item_uuid_to_cue_.end()) return false;
        target_cue = cue_it->second;

        // Locate the item JSON to read its behavior fields.
        json* found = nullptr;
        json& doc = document_;
        std::function<void(json&)> walk;
        walk = [&](json& arr) {
            if (found || !arr.is_array()) return;
            for (auto& it : arr) {
                if (found) return;
                if (!it.is_object()) continue;
                if (it.value("uuid", std::string{}) == uuid) { found = &it; return; }
                if (it.value("type", std::string{}) == "group" &&
                    it.contains("children")) walk(it["children"]);
            }
        };
        if (doc.contains("items"))              walk(doc["items"]);
        if (!found && doc.contains("cartOnlyItems")) walk(doc["cartOnlyItems"]);

        if (found) {
            in_point      = found->value("inPoint",         0.0);
            out_point     = found->value("outPoint",         0.0);
            fade_out_dur  = found->value("fadeOutDuration",  1.0);
            crossfade_sec = found->value("crossFade",        0.0);
            stop_fade_sec = found->value("stopFade",         0.0);
            if (found->contains("duckingBehavior") &&
                (*found)["duckingBehavior"].is_object()) {
                const auto& dk = (*found)["duckingBehavior"];
                ducking_mode = dk.value("mode",      std::string{"stop-all"});
                duck_level   = dk.value("duckLevel", 0.2f);
            }
            if (found->contains("deviceOverride") &&
                (*found)["deviceOverride"].is_string()) {
                device_override = (*found)["deviceOverride"].get<std::string>();
            }
            if (found->contains("startBehavior") &&
                (*found)["startBehavior"].is_object()) {
                const auto& sb = (*found)["startBehavior"];
                start_behavior_action      = sb.value("action",     std::string{});
                start_behavior_target_uuid = sb.value("targetUuid", std::string{});
            }
            if (found->contains("endBehavior") &&
                (*found)["endBehavior"].is_object()) {
                const auto& eb = (*found)["endBehavior"];
                end_behavior_action = eb.value("action", std::string{});
            }
            // Snapshot custom actions for the sequencer to dispatch.
            if (found->contains("customActions") &&
                (*found)["customActions"].is_array()) {
                for (const auto& ca : (*found)["customActions"]) {
                    if (!ca.is_object() || !ca.contains("action")) continue;
                    ScheduledCustomAction sca;
                    sca.time_point = ca.value("timePoint", 0.0);
                    sca.action     = ca["action"];
                    custom_actions_snapshot.push_back(std::move(sca));
                }
            }
        }

        // Collect every cue id except this one.
        for (auto& [_, c] : cues_) {
            if (c.id == target_cue) continue;
            other_cues.push_back(c.id);
        }
    }

    // Snapshot original gains for duck-others before applying ducking.
    std::vector<DuckedEntry> ducks_made;

    // Apply ducking to other cues before triggering the new one.
    if (ducking_mode == "stop-all") {
        const auto fade_ms = std::chrono::milliseconds{
            static_cast<long long>(std::max(fade_out_dur, 0.0) * 1000.0)};
        for (auto& cid : other_cues) {
            if (auto* pi = engine_.find_cue(cid)) {
                const auto prev_fade = pi->desc().fade_out_duration;
                pi->set_fade_out(fade_ms);
                pi->stop();
                pi->set_fade_out(prev_fade);
            }
        }
    } else if (ducking_mode == "duck-others") {
        const float lin = std::clamp(duck_level, 0.0f, 1.0f);
        const float db  = (lin <= 0.0001f) ? -120.0f : 20.0f * std::log10(lin);
        for (auto& cid : other_cues) {
            if (auto* pi = engine_.find_cue(cid)) {
                ducks_made.push_back({cid, pi->gain_db()});
                pi->set_gain_db(db);
            }
        }
    }
    // "no-ducking" → do nothing.

    // Apply per-cue device routing right before play.
    if (!device_override.empty()) {
        const auto mixer = ensure_device_routing(device_override);
        if (!mixer.empty()) {
            route_cue_to_mixer(target_cue, mixer);
        } else {
            engine_.ensure_default_routing();
        }
    } else {
        std::vector<audio::MixerChannelId> override_mixers;
        {
            std::lock_guard lock{mutex_};
            for (auto& [_, r] : device_routings_) override_mixers.push_back(r.mixer);
        }
        if (auto* pi = engine_.find_cue(target_cue)) {
            const auto src_count = pi->source_channel_count();
            for (auto& m : override_mixers) {
                for (audio::ChannelIndex c = 0; c < src_count; ++c) {
                    engine_.unroute_item_source_from_mixer(target_cue, c, m);
                }
            }
        }
        engine_.ensure_default_routing();
    }

    // Re-establish LTC device routing after any audio routing changes above
    // may have disrupted it (unrouting all channels clears LTC device routes).
    apply_ltc_device_routing();

    // Look up file duration for sequencer scheduling (from CueMeta).
    double file_duration = 0.0;
    {
        std::lock_guard lock{mutex_};
        auto cm_it = cues_.find(target_cue.value);
        if (cm_it != cues_.end()) file_duration = cm_it->second.duration_seconds;
    }

    // Prime the target around the configured in-point.
    if (auto* pi = engine_.find_cue(target_cue)) {
        pi->set_out_point_seconds(out_point > 0.0 ? out_point : 0.0);
        // Configure engine-level seamless looping based on endBehavior. Doing
        // the loop inside the audio thread (decoder seek + playhead reset on
        // EOF/out-point) avoids the Stopped→Playing flap that the broadcast
        // thread used to observe between the natural-end and the sequencer's
        // re-trigger — that flap caused the client UI to drop the cue from
        // "currently playing" and grey out its stop button mid-loop.
        pi->set_loop(end_behavior_action == "loop", in_point);
        pi->prime(2.0, in_point);
    }
    engine_.play(target_cue);

    // Register with the sequencer so it can handle end-behaviour, crossfade,
    // and stop-fade autonomously — even when the client is disconnected.
    {
        const double effective_end = (out_point > 0.0) ? out_point : file_duration;
        SequencedItem si;
        si.uuid          = uuid;
        si.cue_id        = target_cue;
        si.crossfade_sec = crossfade_sec;
        si.stop_fade_sec = stop_fade_sec;
        si.effective_end = effective_end;
        si.ducked        = std::move(ducks_made);
        si.custom_actions = std::move(custom_actions_snapshot);

        std::lock_guard slock{sequencer_mutex_};
        // Remove any prior sequencer entry for this item (re-play case).
        sequenced_items_.erase(
            std::remove_if(sequenced_items_.begin(), sequenced_items_.end(),
                           [&](const SequencedItem& x){ return x.uuid == uuid; }),
            sequenced_items_.end());
        sequenced_items_.push_back(std::move(si));
    }

    // Best-effort: warm the *next* cue so the first read is glitch-free.
    std::string next_uuid;
    {
        std::lock_guard lock{mutex_};
        next_uuid = resolve_next_item_locked(uuid);
    }
    if (!next_uuid.empty()) {
        std::optional<audio::CueId> next_cue;
        {
            std::lock_guard lock{mutex_};
            auto it = item_uuid_to_cue_.find(next_uuid);
            if (it != item_uuid_to_cue_.end()) next_cue = it->second;
        }
        if (next_cue) {
            std::thread([this, cue = *next_cue]() {
                if (auto* pi = engine_.find_cue(cue)) pi->prime(2.0);
            }).detach();
        }
    }

    // Handle start-behaviour immediately.
    if (start_behavior_action == "stop" && !start_behavior_target_uuid.empty()) {
        stop_item(start_behavior_target_uuid);
    } else if (start_behavior_action == "play" && !start_behavior_target_uuid.empty()) {
        play_item(start_behavior_target_uuid);
    }

    return true;
}

bool ProjectState::stop_item(const std::string& uuid) {
    audio::CueId cue;
    {
        std::lock_guard lock{mutex_};
        auto it = item_uuid_to_cue_.find(uuid);
        if (it == item_uuid_to_cue_.end()) return false;
        cue = it->second;
    }

    // Remove from sequencer and restore any ducked gains.
    {
        std::lock_guard slock{sequencer_mutex_};
        auto it = std::find_if(sequenced_items_.begin(), sequenced_items_.end(),
                               [&](const SequencedItem& si){ return si.uuid == uuid; });
        if (it != sequenced_items_.end()) {
            for (auto& dk : it->ducked) {
                if (auto* pi = engine_.find_cue(dk.cue_id))
                    pi->set_gain_db(dk.original_gain_db);
            }
            sequenced_items_.erase(it);
        }
    }

    engine_.stop(cue);
    return true;
}

void ProjectState::set_next_item_override(const std::string& uuid) {
    std::lock_guard lock{mutex_};
    next_item_override_ = uuid;
}

std::string ProjectState::next_item_override() const {
    std::lock_guard lock{mutex_};
    return next_item_override_;
}

// Dispatch by item type: audio → play_item; group → walk startBehavior
// (play-first plays the first child recursively; play-all triggers every
// child). Mirrors the client's triggerGroup() so auto-next / Up Next
// override / goto-item behave consistently when the target is a group.
bool ProjectState::trigger_item(const std::string& uuid) {
    // Look up the item's type and (for groups) startBehavior + children.
    std::string type;
    std::string start_action;
    std::vector<std::string> child_uuids;
    {
        std::lock_guard lock{mutex_};
        json* found = nullptr;
        json& doc = document_;
        std::function<void(json&)> walk;
        walk = [&](json& arr) {
            if (found || !arr.is_array()) return;
            for (auto& it : arr) {
                if (found) return;
                if (!it.is_object()) continue;
                if (it.value("uuid", std::string{}) == uuid) { found = &it; return; }
                if (it.value("type", std::string{}) == "group" &&
                    it.contains("children")) walk(it["children"]);
            }
        };
        if (doc.contains("items"))              walk(doc["items"]);
        if (!found && doc.contains("cartOnlyItems")) walk(doc["cartOnlyItems"]);
        if (!found) return false;

        type = found->value("type", std::string{});
        if (type == "group") {
            if (found->contains("startBehavior") &&
                (*found)["startBehavior"].is_object()) {
                start_action = (*found)["startBehavior"]
                                   .value("action", std::string{"play-first"});
            } else {
                start_action = "play-first";
            }
            if (found->contains("children") && (*found)["children"].is_array()) {
                for (auto& c : (*found)["children"]) {
                    if (c.is_object()) {
                        auto u = c.value("uuid", std::string{});
                        if (!u.empty()) child_uuids.push_back(std::move(u));
                    }
                }
            }
        }
    }

    if (type == "audio") {
        return play_item(uuid);
    }
    if (type == "group") {
        if (child_uuids.empty()) return false;
        if (start_action == "play-all") {
            bool any = false;
            for (const auto& cu : child_uuids) {
                if (trigger_item(cu)) any = true;
            }
            return any;
        }
        // Default / "play-first": trigger only the first child.
        return trigger_item(child_uuids.front());
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-device routing — each cue with a `deviceOverride` is wired through a
// dedicated mixer + pair of master channels into that specific output
// device. Items without an override fall through to the engine's default
// Main mixer (master channels 0/1, default device).
// ---------------------------------------------------------------------------
audio::MixerChannelId
ProjectState::ensure_device_routing(const std::string& device_name) {
    if (device_name.empty()) return {};
    {
        std::lock_guard lock{mutex_};
        auto it = device_routings_.find(device_name);
        if (it != device_routings_.end()) return it->second.mixer;
    }

    // Open device + allocate masters + create mixer (all engine APIs are
    // independently locked, so we don't hold our own mutex during them).
    const auto dev = engine_.open_device_by_name(device_name, 2);
    if (dev.empty()) {
        Logger::warn("ensure_device_routing: could not open '{}'", device_name);
        return {};
    }

    audio::MasterChannelIndex master_l;
    audio::MasterChannelIndex master_r;
    {
        std::lock_guard lock{mutex_};
        master_l = next_override_master_;
        master_r = next_override_master_ + 1;
        next_override_master_ += 2;
    }

    const auto mixer = engine_.create_mixer_channel(
        "Output: " + device_name);
    engine_.assign_master_to_device(master_l, dev, 0);
    engine_.assign_master_to_device(master_r, dev, 1);
    engine_.route_mixer_to_master(mixer, master_l);
    engine_.route_mixer_to_master(mixer, master_r);

    {
        std::lock_guard lock{mutex_};
        device_routings_[device_name] = DeviceRouting{
            dev, mixer, master_l, master_r,
        };
    }
    Logger::info("ensure_device_routing: '{}' → mixer '{}' (masters {}/{})",
                 device_name, mixer.value, master_l, master_r);
    return mixer;
}

void ProjectState::route_cue_to_mixer(const audio::CueId& cue,
                                      const audio::MixerChannelId& mixer) {
    auto* pi = engine_.find_cue(cue);
    if (!pi) return;
    const auto src_count = pi->source_channel_count();

    // Drop any prior item-to-mixer routes (incl. Main) by walking every
    // known mixer — the engine doesn't have a "list routes for cue" API, so
    // we unroute against each mixer we know about. Cheap because the
    // unroute is a no-op when no route exists.
    std::vector<audio::MixerChannelId> known_mixers;
    {
        std::lock_guard lock{mutex_};
        for (auto& [_, m] : mixers_) known_mixers.push_back(m.id);
        for (auto& [_, r] : device_routings_) known_mixers.push_back(r.mixer);
    }
    for (const auto& m : known_mixers) {
        for (audio::ChannelIndex c = 0; c < src_count; ++c) {
            engine_.unroute_item_source_from_mixer(cue, c, m);
        }
    }
    for (audio::ChannelIndex c = 0; c < std::min<audio::ChannelCount>(2, src_count); ++c) {
        engine_.route_item_source_to_mixer(cue, c, mixer);
    }
}

// ---------------------------------------------------------------------------
// Preview routing — independent playback of a cue through the configured
// preview device, used for DJ-style pre-listening. The infrastructure
// (device + mixer + master assignments) is set up lazily on first preview
// and reused for subsequent ones.
// ---------------------------------------------------------------------------
namespace {
// Master channels reserved for preview output. Picked from the tail of the
// 32-channel master bus so they don't collide with project routing.
constexpr audio::MasterChannelIndex kPreviewMasterL = 30;
constexpr audio::MasterChannelIndex kPreviewMasterR = 31;
}  // namespace

bool ProjectState::start_preview(const std::string& item_uuid) {
    if (item_uuid.empty()) return false;

    // 1. Resolve the source file path under the lock.
    std::filesystem::path file_path;
    double in_point = 0.0;
    std::string preview_device_name;
    {
        std::lock_guard lock{mutex_};
        // path
        for_each_item(document_,
            [&](json& it, const std::string&) {
                if (it.value("uuid", std::string{}) != item_uuid) return;
                std::string s;
                if (it.contains("mediaServerPath") && it["mediaServerPath"].is_string()) {
                    s = it["mediaServerPath"].get<std::string>();
                }
                if (s.empty() && it.contains("mediaPath") && it["mediaPath"].is_string()) {
                    const std::string folder = document_.value("folderPath", std::string{});
                    if (!folder.empty()) {
                        s = folder;
                        if (s.back() != '/' && s.back() != '\\') s += '/';
                    }
                    s += it["mediaPath"].get<std::string>();
                }
                if (!s.empty()) file_path = util::utf8_to_path(s);
                in_point = it.value("inPoint", 0.0);
            });
        // settings.previewDevice
        if (document_.contains("settings") && document_["settings"].is_object()) {
            const auto& s = document_["settings"];
            if (s.contains("previewDevice") && s["previewDevice"].is_string()) {
                preview_device_name = s["previewDevice"].get<std::string>();
            }
        }
    }
    if (file_path.empty()) {
        Logger::warn("preview: item '{}' has no resolvable file path", item_uuid);
        return false;
    }

    // 2. Tear down any in-flight preview cleanly. Keep the mixer + device
    // open if we have them — they're reused below.
    {
        audio::CueId prev_cue;
        {
            std::lock_guard lock{mutex_};
            prev_cue = preview_cue_;
            preview_cue_ = audio::CueId{};
            preview_item_uuid_.clear();
        }
        if (!prev_cue.empty()) {
            engine_.stop(prev_cue);
            engine_.unload_cue(prev_cue);
        }
    }

    // 3. Ensure preview infrastructure (device open + mixer + master wiring).
    audio::MixerChannelId preview_mixer;
    {
        // If the user changed the preview device since our last setup,
        // close the old one and start fresh.
        std::string current_name;
        audio::DeviceId current_device;
        audio::MixerChannelId current_mixer;
        {
            std::lock_guard lock{mutex_};
            current_name   = preview_device_name_;
            current_device = preview_device_;
            current_mixer  = preview_mixer_;
        }

        if (preview_device_name.empty()) {
            Logger::warn("preview: no preview device configured in settings");
            return false;
        }

        if (current_name != preview_device_name && !current_device.empty()) {
            // Close old preview device + mixer.
            engine_.close_device(current_device);
            if (!current_mixer.empty()) engine_.remove_mixer_channel(current_mixer);
            std::lock_guard lock{mutex_};
            preview_device_ = audio::DeviceId{};
            preview_mixer_  = audio::MixerChannelId{};
            preview_device_name_.clear();
        }

        if (preview_device_.empty()) {
            // Open the device, create a dedicated "Preview" mixer, wire it.
            const auto dev = engine_.open_device_by_name(preview_device_name, 2);
            if (dev.empty()) {
                Logger::warn("preview: could not open device '{}'", preview_device_name);
                return false;
            }
            const auto mixer = engine_.create_mixer_channel("Preview");
            engine_.assign_master_to_device(kPreviewMasterL, dev, 0);
            engine_.assign_master_to_device(kPreviewMasterR, dev, 1);
            engine_.route_mixer_to_master(mixer, kPreviewMasterL);
            engine_.route_mixer_to_master(mixer, kPreviewMasterR);
            {
                std::lock_guard lock{mutex_};
                preview_device_      = dev;
                preview_mixer_       = mixer;
                preview_device_name_ = preview_device_name;
                preview_mixer = mixer;
            }
        } else {
            preview_mixer = preview_mixer_;
        }
    }

    // 4. Load the file as a fresh engine cue, route it to the preview mixer
    // ONLY (no auto-routing to Main). prime + play.
    const auto cue_id = engine_.load_cue_no_route(file_path);
    if (cue_id.empty()) return false;

    auto* pi = engine_.find_cue(cue_id);
    if (pi) {
        engine_.route_item_source_to_mixer(cue_id, 0, preview_mixer);
        if (pi->source_channel_count() >= 2) {
            engine_.route_item_source_to_mixer(cue_id, 1, preview_mixer);
        }
        pi->prime(2.0, in_point);
    }
    engine_.play(cue_id);

    {
        std::lock_guard lock{mutex_};
        preview_cue_       = cue_id;
        preview_item_uuid_ = item_uuid;
    }
    Logger::info("preview: started for item '{}' on '{}'", item_uuid, preview_device_name);
    return true;
}

bool ProjectState::stop_preview() {
    audio::CueId cue;
    {
        std::lock_guard lock{mutex_};
        cue = preview_cue_;
        preview_cue_ = audio::CueId{};
        preview_item_uuid_.clear();
    }
    if (cue.empty()) return false;
    engine_.stop(cue);
    engine_.unload_cue(cue);
    Logger::info("preview: stopped");
    return true;
}

std::string ProjectState::current_preview_item_uuid() const {
    std::lock_guard lock{mutex_};
    return preview_item_uuid_;
}

audio::CueId ProjectState::current_preview_cue_id() const {
    std::lock_guard lock{mutex_};
    return preview_cue_;
}

// ---------------------------------------------------------------------------
// Cart slot bindings
// ---------------------------------------------------------------------------
bool ProjectState::set_cart_slot(int slot, const std::string& item_uuid) {
    if (slot < 0 || slot >= 64) return false;
    std::lock_guard lock{mutex_};
    if (!document_.contains("cartItems") || !document_["cartItems"].is_array()) {
        document_["cartItems"] = json::array();
    }
    auto& arr = document_["cartItems"];
    // Remove any existing binding for this slot.
    arr.erase(std::remove_if(arr.begin(), arr.end(),
                             [&](const json& c){
                                 return c.value("slot", -1) == slot;
                             }),
              arr.end());
    arr.push_back(json{
        {"slot",     slot},
        {"itemUuid", item_uuid},
        {"index",    json::array({-1, slot})},
    });
    return true;
}

bool ProjectState::clear_cart_slot(int slot) {
    std::lock_guard lock{mutex_};
    if (!document_.contains("cartItems") || !document_["cartItems"].is_array()) {
        return false;
    }
    auto& arr = document_["cartItems"];
    const auto before = arr.size();
    arr.erase(std::remove_if(arr.begin(), arr.end(),
                             [&](const json& c){
                                 return c.value("slot", -1) == slot;
                             }),
              arr.end());
    return arr.size() != before;
}

// ---------------------------------------------------------------------------
// Theme + settings patches
// ---------------------------------------------------------------------------
bool ProjectState::patch_theme(const json& patch) {
    if (!patch.is_object()) return false;
    std::lock_guard lock{mutex_};
    if (!document_.contains("theme") || !document_["theme"].is_object()) {
        document_["theme"] = json::object();
    }
    for (auto& [k, v] : patch.items()) document_["theme"][k] = v;
    return true;
}

bool ProjectState::patch_settings(const json& patch) {
    if (!patch.is_object()) return false;
    bool ltc_device_changed = false;
    {
        std::lock_guard lock{mutex_};
        if (!document_.contains("settings") || !document_["settings"].is_object()) {
            document_["settings"] = json::object();
        }
        for (auto& [k, v] : patch.items()) {
            if (k == "ltcDevice") ltc_device_changed = true;
            document_["settings"][k] = v;
        }
    }
    // settings.ltcDevice toggled / changed: open the device, build its mixer,
    // and route every LTC-enabled cue's synthetic channel into it. Without
    // this, LTC was silent until the user replayed an item (which is when
    // play_item happens to call apply_ltc_device_routing() too).
    if (ltc_device_changed) apply_ltc_device_routing();
    return true;
}

bool ProjectState::is_legacy_document(const json& doc) const {
    // Heuristic: v2 always has schema_version >= 2.
    if (doc.contains("schema_version") &&
        doc["schema_version"].is_number() &&
        doc["schema_version"].get<int>() >= 2) {
        return false;
    }
    // Anything else with a `carts` or `playlist` array we treat as 1.x.
    return doc.contains("carts") || doc.contains("playlist") || doc.contains("cues_legacy");
}

json ProjectState::upgrade_legacy_document(const json& legacy) const {
    // Conservative translator: build a v2 doc that mirrors 1.x semantics —
    // each cue routes its source channels straight to the default device's
    // hardware channels 0 and 1 (stereo). Mixer channels are auto-created
    // per-cue so individual fades/gains still apply.
    json out;
    out["schema_version"] = 2;
    out["project_name"]   = legacy.value("name", "Untitled (upgraded)");
    out["media_root"]     = legacy.value("media_root", media_root_.string());
    out["cues"]              = json::array();
    out["mixer_channels"]    = json::array();
    out["item_routes"]       = json::array();
    out["mixer_routes"]      = json::array();
    out["master_assignments"]= json::array();

    auto add_cue = [&](const json& src) {
        json c;
        c["id"]            = src.value("id", "");
        c["display_name"]  = src.value("name", src.value("title", "Cue"));
        c["file_path"]     = src.value("path", src.value("file", ""));
        c["artist"]        = src.value("artist", "");
        c["title"]         = src.value("title", "");
        c["duration_sec"]  = src.value("duration", 0.0);
        c["gain_db"]       = src.value("gain_db", src.value("volume_db", 0.0));
        c["fade_in_ms"]    = src.value("fade_in_ms",  static_cast<long long>(0));
        c["fade_out_ms"]   = src.value("fade_out_ms", static_cast<long long>(0));
        c["ltc_enabled"]        = false;
        c["ltc_fps"]            = 4;
        c["ltc_offset_ns"]      = 0;
        c["ltc_start_timecode"] = "00:00:00:00";
        out["cues"].push_back(std::move(c));
    };
    if (legacy.contains("carts") && legacy["carts"].is_array()) {
        for (auto& cart : legacy["carts"]) add_cue(cart);
    }
    if (legacy.contains("playlist") && legacy["playlist"].is_array()) {
        for (auto& it : legacy["playlist"]) add_cue(it);
    }
    if (legacy.contains("cues_legacy") && legacy["cues_legacy"].is_array()) {
        for (auto& it : legacy["cues_legacy"]) add_cue(it);
    }

    // Default device → stereo master channels 0 and 1. The engine fills in the
    // actual DeviceId on apply_to_engine_locked() because we don't know it
    // until a device is opened.
    json a0{{"master_channel", 0}, {"device", ""}, {"hw_channel", 0}};
    json a1{{"master_channel", 1}, {"device", ""}, {"hw_channel", 1}};
    out["master_assignments"].push_back(a0);
    out["master_assignments"].push_back(a1);
    return out;
}

bool ProjectState::load_from_json(const json& doc_in) {
    // The .liveplay format the Electron client writes today is camelCase and
    // hierarchical (items → groups → audio items). Detect that flavour and
    // store the full document for the client to read back via /api/project.
    // The engine-facing tables (cues_/mixers_/routes_) get populated by
    // mirror_items_to_engine_locked() so audio playback works as before.
    if (is_client_document(doc_in)) {
        // Run repair before taking the lock — it's pure document transformation.
        json doc_repaired = doc_in;
        RepairInfo repair = detect_and_repair(doc_repaired);
        if (repair.repaired) {
            Logger::warn("ProjectState::load_from_json: project repaired ({} issue(s)).",
                         repair.issues.size());
            for (const auto& issue : repair.issues)
                Logger::warn("  - {}", issue);
        }

        {
            std::lock_guard lock{mutex_};
            // Unload any previously-loaded engine cues so we start clean.
            for (auto& [_, id] : item_uuid_to_cue_) engine_.unload_cue(id);
            item_uuid_to_cue_.clear();
            cues_.clear();
            mixers_.clear();
            item_routes_.clear();
            mixer_routes_.clear();
            master_assignments_.clear();

            document_ = std::move(doc_repaired);
            // Ensure required top-level keys exist (migrate older client saves).
            if (!document_.contains("settings") || !document_["settings"].is_object()) {
                document_["settings"] = json{
                    {"defaultOutputDevice", nullptr},
                    {"previewDevice",       nullptr},
                    {"ltcDevice",           nullptr},
                };
            }
            if (!document_.contains("cartOnlyItems") ||
                !document_["cartOnlyItems"].is_array()) {
                document_["cartOnlyItems"] = json::array();
            }
            if (!document_.contains("theme") || !document_["theme"].is_object()) {
                document_["theme"] = json{{"mode", "dark"}, {"accentColor", "#DA1E28"}};
            }
            project_name_ = document_.value("name", std::string{"Untitled"});
            pending_repair_info_ = std::move(repair);
        }

        // Audio mirroring happens off-thread so the client can render the
        // project immediately. Items not yet loaded into the engine will
        // simply fail play() until ready (rare in practice — by the time the
        // user clicks anything, the first batch is usually done).
        start_async_mirror();
        return true;
    }

    // Otherwise: assume server's snake_case schema (current behaviour).
    json doc = doc_in;
    if (is_legacy_document(doc)) {
        Logger::info("ProjectState: detected legacy 1.x document, upgrading.");
        doc = upgrade_legacy_document(doc);
    }

    std::lock_guard lock{mutex_};
    for (auto& [_, id] : item_uuid_to_cue_) engine_.unload_cue(id);
    item_uuid_to_cue_.clear();
    cues_.clear();
    mixers_.clear();
    item_routes_.clear();
    mixer_routes_.clear();
    master_assignments_.clear();
    document_ = default_empty_document();

    project_name_ = doc.value("project_name", std::string{"Untitled"});
    if (doc.contains("media_root") && doc["media_root"].is_string()) {
        media_root_ = util::utf8_to_path(doc["media_root"].get<std::string>());
    }

    if (doc.contains("cues") && doc["cues"].is_array()) {
        for (auto& c : doc["cues"]) {
            CueMeta m;
            m.id = audio::CueId{c.value("id", std::string{})};
            m.display_name     = c.value("display_name", "");
            m.file_path        = util::utf8_to_path(c.value("file_path", std::string{}));
            m.artist           = c.value("artist", "");
            m.title            = c.value("title", "");
            m.duration_seconds = c.value("duration_sec", 0.0);
            m.gain_db          = c.value("gain_db", 0.0f);
            m.fade_in_ms  = std::chrono::milliseconds{c.value("fade_in_ms",  (long long)0)};
            m.fade_out_ms = std::chrono::milliseconds{c.value("fade_out_ms", (long long)0)};
            m.ltc_enabled          = c.value("ltc_enabled",        false);
            m.ltc_frame_rate_index = c.value("ltc_fps",            4);
            m.ltc_offset_ns        = std::chrono::nanoseconds{c.value("ltc_offset_ns", (long long)0)};
            m.ltc_start_timecode   = c.value("ltc_start_timecode", std::string{"00:00:00:00"});
            cues_.emplace(m.id.value, std::move(m));
        }
    }
    if (doc.contains("mixer_channels") && doc["mixer_channels"].is_array()) {
        for (auto& m : doc["mixer_channels"]) {
            MixerChannelMeta mm;
            mm.id           = audio::MixerChannelId{m.value("id", std::string{})};
            mm.display_name = m.value("display_name", "");
            mm.gain_db      = m.value("gain_db", 0.0f);
            mm.muted        = m.value("muted",   false);
            mm.soloed       = m.value("soloed",  false);
            mixers_.emplace(mm.id.value, std::move(mm));
        }
    }
    if (doc.contains("item_routes") && doc["item_routes"].is_array()) {
        for (auto& r : doc["item_routes"]) {
            item_routes_.push_back(RouteSendV2{
                r.value("source_channel", (audio::ChannelIndex)0),
                audio::MixerChannelId{r.value("destination_mixer", std::string{})},
                r.value("gain_db", 0.0f),
            });
        }
    }
    if (doc.contains("mixer_routes") && doc["mixer_routes"].is_array()) {
        for (auto& r : doc["mixer_routes"]) {
            mixer_routes_.push_back(MixerToMasterV2{
                audio::MixerChannelId{r.value("mixer", std::string{})},
                r.value("master_channel", (audio::MasterChannelIndex)0),
                r.value("gain_db", 0.0f),
            });
        }
    }
    if (doc.contains("master_assignments") && doc["master_assignments"].is_array()) {
        for (auto& a : doc["master_assignments"]) {
            master_assignments_.push_back(MasterAssignment{
                a.value("master_channel", (audio::MasterChannelIndex)0),
                audio::DeviceId{a.value("device", std::string{})},
                a.value("hw_channel", (audio::ChannelIndex)0),
            });
        }
    }
    apply_to_engine_locked();
    return true;
}

bool ProjectState::load(const std::filesystem::path& path) {
    try {
        std::ifstream f{path};
        if (!f) {
            Logger::error("ProjectState::load: cannot open '{}'", util::path_to_utf8(path));
            return false;
        }
        json doc;
        f >> doc;
        const bool ok = load_from_json(doc);
        if (ok) {
            set_project_file_path(path);
            // Default the folderPath in the document to the directory the file
            // sits in (clients use this to resolve mediaPath).
            std::lock_guard lock{mutex_};
            if (document_.value("folderPath", std::string{}).empty() && path.has_parent_path()) {
                document_["folderPath"] = util::path_to_utf8(path.parent_path());
            }
        }
        return ok;
    } catch (const std::exception& ex) {
        Logger::error("ProjectState::load failed: {}", ex.what());
        return false;
    }
}

RepairInfo ProjectState::consume_repair_info() {
    std::lock_guard lock{mutex_};
    RepairInfo result;
    std::swap(result, pending_repair_info_);
    return result;
}

RepairInfo ProjectState::repair_project() {
    json doc;
    {
        std::lock_guard lock{mutex_};
        doc = document_;
    }
    RepairInfo info = detect_and_repair(doc);
    if (info.repaired) {
        std::lock_guard lock{mutex_};
        document_ = std::move(doc);
        Logger::info("ProjectState::repair_project: {} issue(s) repaired.", info.issues.size());
    }
    return info;
}

void ProjectState::apply_to_engine_locked() {
    // 1) Load every cue's file into the engine. We accept that this can fail
    //    for missing files; the entry stays in the project for the user to
    //    relocate later.
    for (auto& [_, c] : cues_) {
        const auto id = engine_.load_cue(c.file_path, c.id);
        if (!id.empty()) {
            engine_.find_cue(id)->set_gain_db(c.gain_db);
            engine_.find_cue(id)->set_fade_in (c.fade_in_ms);
            engine_.find_cue(id)->set_fade_out(c.fade_out_ms);
            if (c.ltc_enabled) {
                engine_.find_cue(id)->set_ltc_enabled(true);
                engine_.find_cue(id)->set_ltc_frame_rate(fps_index_to_rate(c.ltc_frame_rate_index));
                engine_.find_cue(id)->set_ltc_offset(c.ltc_offset_ns);
            }
        }
    }
    // 2) Create mixer channels and apply their gain.
    for (auto& [_, mm] : mixers_) {
        auto created = engine_.create_mixer_channel(mm.display_name);
        // (Engine assigns a fresh id; we keep ours as the canonical one. For
        //  full round-trip we'd thread the requested id through engine — left
        //  as a follow-up so the document/engine ids stay in sync.)
        mm.id = created;
        if (auto* m = engine_.find_mixer_channel(created)) {
            m->set_gain_db(mm.gain_db);
            m->set_mute (mm.muted);
            m->set_solo (mm.soloed);
        }
    }
    // 3) Re-apply routes.
    for (auto& r : item_routes_) {
        engine_.route_item_source_to_mixer(audio::CueId{}, r.source_channel,
                                           r.destination_mixer, r.gain_db);
    }
    for (auto& r : mixer_routes_) {
        engine_.route_mixer_to_master(r.mixer, r.master_channel, r.gain_db);
    }
    for (auto& a : master_assignments_) {
        // If the document didn't specify a device (e.g. upgraded legacy
        // project), the assignment is deferred — the control server can
        // bind it to the default device on first open.
        if (!a.device.empty()) {
            engine_.assign_master_to_device(a.master_channel, a.device, a.hw_channel);
        }
    }
}

// ---------------------------------------------------------------------------
// Sequencer — server-side auto-advance, crossfade, ducking restore
// ---------------------------------------------------------------------------
void ProjectState::start_sequencer() {
    sequencer_running_.store(true, std::memory_order_release);
    sequencer_thread_ = std::thread([this]{ sequencer_loop(); });
}

void ProjectState::stop_sequencer() {
    sequencer_running_.store(false, std::memory_order_release);
    if (sequencer_thread_.joinable()) sequencer_thread_.join();
}

void ProjectState::sequencer_loop() {
    using namespace std::chrono_literals;

    while (sequencer_running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(50ms);
        if (!sequencer_running_.load(std::memory_order_acquire)) break;

        struct PendingAction {
            SequencedItem item;
            enum class Kind {
                NaturalEnd,    // take_natural_end() returned true
                Crossfade,     // start next + fade out current
                BeginStopFade, // begin fading out (item stays until Stopped)
                StopFadeEnded, // stop-fade complete → fire end behavior
                Cleanup,       // cue gone; restore ducking, no end behavior
                CustomAction,  // fire one of the item's customActions
            } kind;
            json custom_action;  // populated only for Kind::CustomAction
        };
        std::vector<PendingAction> pending;

        {
            std::lock_guard slock{sequencer_mutex_};
            for (auto& si : sequenced_items_) {
                auto* pi = engine_.find_cue(si.cue_id);
                if (!pi) {
                    pending.push_back({si, PendingAction::Kind::Cleanup});
                    continue;
                }

                // Natural end takes priority over all timing checks.
                if (pi->take_natural_end()) {
                    pending.push_back({si, PendingAction::Kind::NaturalEnd});
                    continue;
                }

                // Stop-fade completed (transport settled to Stopped).
                const auto ts = pi->stats().transport;
                if (ts == audio::TransportState::Stopped && si.stop_fade_triggered) {
                    pending.push_back({si, PendingAction::Kind::StopFadeEnded});
                    continue;
                }

                const double pos = pi->stats().playhead_seconds;

                // Custom-action dispatch: any action whose time_point we've
                // crossed fires now. Snapshot the action JSON into pending so
                // we can execute it outside the lock.
                for (auto& sca : si.custom_actions) {
                    if (sca.triggered) continue;
                    if (pos < sca.time_point) continue;
                    sca.triggered = true;
                    pending.push_back({si, PendingAction::Kind::CustomAction,
                                       sca.action});
                }

                // Timing-based triggers only apply when we know the duration.
                if (si.effective_end <= 0.0) continue;
                const double remaining = si.effective_end - pos;

                if (!si.crossfade_triggered && si.crossfade_sec > 0.0 &&
                    remaining <= si.crossfade_sec && remaining > 0.0) {
                    si.crossfade_triggered = true;
                    pending.push_back({si, PendingAction::Kind::Crossfade});
                } else if (!si.stop_fade_triggered && si.stop_fade_sec > 0.0 &&
                           si.crossfade_sec <= 0.0 &&
                           remaining <= si.stop_fade_sec && remaining > 0.0) {
                    si.stop_fade_triggered = true;
                    pending.push_back({si, PendingAction::Kind::BeginStopFade});
                }
            }

            // Remove terminal items while the lock is held.
            for (const auto& p : pending) {
                const bool terminal =
                    p.kind == PendingAction::Kind::NaturalEnd  ||
                    p.kind == PendingAction::Kind::Crossfade   ||
                    p.kind == PendingAction::Kind::StopFadeEnded ||
                    p.kind == PendingAction::Kind::Cleanup;
                if (terminal) {
                    sequenced_items_.erase(
                        std::remove_if(sequenced_items_.begin(), sequenced_items_.end(),
                            [&](const SequencedItem& x){ return x.uuid == p.item.uuid; }),
                        sequenced_items_.end());
                }
                // BeginStopFade items stay until they reach the Stopped state.
            }
        }

        // Execute pending actions with the sequencer lock released.
        for (const auto& p : pending) {
            switch (p.kind) {
            case PendingAction::Kind::NaturalEnd:
            case PendingAction::Kind::StopFadeEnded:
                handle_item_ended(p.item);
                break;

            case PendingAction::Kind::Crossfade: {
                // Restore ducked gains so the new cue's ducking applies fresh.
                for (const auto& dk : p.item.ducked) {
                    if (auto* pi = engine_.find_cue(dk.cue_id))
                        pi->set_gain_db(dk.original_gain_db);
                }
                // Fade out the old cue over the crossfade window.
                if (auto* pi = engine_.find_cue(p.item.cue_id)) {
                    pi->stop_with_fade(std::chrono::milliseconds{
                        static_cast<long long>(p.item.crossfade_sec * 1000.0)});
                }
                // Start the next cue (it will register itself with the sequencer).
                // Honour user-set Up Next override, same as handle_item_ended.
                std::string next_uuid;
                {
                    std::lock_guard lock{mutex_};
                    if (!next_item_override_.empty()) {
                        next_uuid = std::move(next_item_override_);
                        next_item_override_.clear();
                    } else {
                        next_uuid = resolve_next_item_locked(p.item.uuid);
                    }
                }
                if (!next_uuid.empty()) trigger_item(next_uuid);
                break;
            }

            case PendingAction::Kind::BeginStopFade:
                if (auto* pi = engine_.find_cue(p.item.cue_id)) {
                    pi->stop_with_fade(std::chrono::milliseconds{
                        static_cast<long long>(p.item.stop_fade_sec * 1000.0)});
                }
                break;

            case PendingAction::Kind::Cleanup:
                for (const auto& dk : p.item.ducked) {
                    if (auto* pi = engine_.find_cue(dk.cue_id))
                        pi->set_gain_db(dk.original_gain_db);
                }
                break;

            case PendingAction::Kind::CustomAction:
                execute_custom_action(p.custom_action);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Custom-action dispatcher — fired by the sequencer when an item's playhead
// crosses a customAction.timePoint. Server-side action types are executed
// directly; http-request is fanned out via the broadcast hook (which the
// control server wires up so a connected client performs the actual fetch).
// ---------------------------------------------------------------------------
void ProjectState::execute_custom_action(const json& action) {
    if (!action.is_object()) return;
    const std::string type = action.value("type", "");

    if (type == "play-item") {
        const auto u = action.value("uuid", std::string{});
        if (!u.empty()) trigger_item(u);
    }
    else if (type == "play-index") {
        // index is an array path through the items tree. Resolve under lock.
        std::vector<int> idx;
        if (action.contains("index") && action["index"].is_array()) {
            for (const auto& v : action["index"]) {
                if (v.is_number_integer()) idx.push_back(v.get<int>());
            }
        }
        std::string target_uuid;
        {
            std::lock_guard lock{mutex_};
            const json* arr = document_.contains("items") ? &document_["items"] : nullptr;
            const json* current = nullptr;
            for (std::size_t depth = 0; depth < idx.size() && arr && arr->is_array(); ++depth) {
                const int i = idx[depth];
                if (i < 0 || i >= static_cast<int>(arr->size())) { current = nullptr; break; }
                current = &(*arr)[i];
                if (depth + 1 < idx.size()) {
                    if (current->value("type", std::string{}) == "group" &&
                        current->contains("children")) {
                        arr = &(*current)["children"];
                    } else { current = nullptr; break; }
                }
            }
            if (current && current->is_object())
                target_uuid = current->value("uuid", std::string{});
        }
        if (!target_uuid.empty()) trigger_item(target_uuid);
    }
    else if (type == "stop-all") {
        engine_.stop_all(std::chrono::milliseconds{0});
    }
    else if (type == "http-request") {
        // Hand off to whoever subscribed via set_external_action_handler.
        // The control server wires this to a doc_patch broadcast so a
        // connected client executes the fetch — keeping server free of an
        // HTTP client dependency.
        std::function<void(const json&)> handler;
        {
            std::lock_guard lock{mutex_};
            handler = external_action_handler_;
        }
        if (handler) {
            try { handler(action); } catch (...) {}
        } else {
            Logger::warn("custom action http-request: no handler installed");
        }
    }
    else {
        Logger::warn("custom action: unknown type '{}'", type);
    }
}

void ProjectState::set_external_action_handler(std::function<void(const json&)> h) {
    std::lock_guard lock{mutex_};
    external_action_handler_ = std::move(h);
}

void ProjectState::handle_item_ended(const SequencedItem& item) {
    // Ensure the engine explicitly transitions the transport state and 
    // triggers a cue_state broadcast so the client UI updates.
    engine_.stop(item.cue_id);

    // Restore ducked gains first so the next item starts with clean levels.
    for (const auto& dk : item.ducked) {
        if (auto* pi = engine_.find_cue(dk.cue_id))
            pi->set_gain_db(dk.original_gain_db);
    }

    // Read end-behaviour from the document.
    std::string end_action;
    std::string target_uuid;
    int         target_index = -1;
    {
        std::lock_guard lock{mutex_};
        json* found = nullptr;
        const std::string& uuid = item.uuid;
        std::function<void(json&)> walk;
        walk = [&](json& arr) {
            if (found || !arr.is_array()) return;
            for (auto& it : arr) {
                if (found) return;
                if (!it.is_object()) continue;
                if (it.value("uuid", std::string{}) == uuid) { found = &it; return; }
                if (it.value("type", std::string{}) == "group" &&
                    it.contains("children")) walk(it["children"]);
            }
        };
        json& doc = document_;
        if (doc.contains("items"))              walk(doc["items"]);
        if (!found && doc.contains("cartOnlyItems")) walk(doc["cartOnlyItems"]);

        if (found && found->contains("endBehavior") &&
            (*found)["endBehavior"].is_object()) {
            const auto& eb = (*found)["endBehavior"];
            end_action   = eb.value("action",      std::string{});
            target_uuid  = eb.value("targetUuid",  std::string{});
            target_index = eb.value("targetIndex", -1);
        }
    }

    if (end_action == "loop") {
        play_item(item.uuid);
    } else if (end_action == "next") {
        std::string next_uuid;
        {
            std::lock_guard lock{mutex_};
            // User-set override wins; consume it.
            if (!next_item_override_.empty()) {
                next_uuid = std::move(next_item_override_);
                next_item_override_.clear();
            } else {
                next_uuid = resolve_next_item_locked(item.uuid);
            }
        }
        if (!next_uuid.empty()) trigger_item(next_uuid);
    } else if (end_action == "goto-item" && !target_uuid.empty()) {
        trigger_item(target_uuid);
    } else if (end_action == "goto-index" && target_index >= 0) {
        std::string idx_uuid;
        {
            std::lock_guard lock{mutex_};
            if (document_.contains("items") && document_["items"].is_array()) {
                const auto& arr = document_["items"];
                if (target_index < static_cast<int>(arr.size()))
                    idx_uuid = arr[target_index].value("uuid", std::string{});
            }
        }
        if (!idx_uuid.empty()) trigger_item(idx_uuid);
    }
    // "nothing" or unrecognized → do nothing.
}

} // namespace liveplay::core
