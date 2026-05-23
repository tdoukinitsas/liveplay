// ============================================================================
// project_state.cpp — see project_state.hpp.
// ============================================================================
#include "liveplay/core/project_state.hpp"
#include "liveplay/logger.hpp"
#include "liveplay/meta/metadata.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>

namespace liveplay::core {

namespace {
inline std::string id_to_string(const audio::CueId& id)          { return id.value; }
inline std::string id_to_string(const audio::MixerChannelId& id) { return id.value; }
inline std::string id_to_string(const audio::DeviceId& id)       { return id.value; }
} // namespace

// ADL-visible to_json overloads — must be in liveplay::core (not anonymous namespace)
// so nlohmann's adl_serializer can find them for push_back / operator= conversions.
void to_json(json& j, const CueMeta& m) {
    j = json{
        {"id",            m.id.value},
        {"display_name",  m.display_name},
        {"file_path",     util::path_to_utf8(m.file_path)},
        {"artist",        m.artist},
        {"title",         m.title},
        {"duration_sec",  m.duration_seconds},
        {"gain_db",       m.gain_db},
        {"fade_in_ms",    m.fade_in_ms.count()},
        {"fade_out_ms",   m.fade_out_ms.count()},
        {"ltc_enabled",   m.ltc_enabled},
        {"ltc_fps",       m.ltc_frame_rate_index},
        {"ltc_offset_ns", static_cast<long long>(m.ltc_offset_ns.count())},
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

} // namespace

// ---------------------------------------------------------------------------

ProjectState::ProjectState(audio::AudioEngine& engine) : engine_(engine) {
    document_ = default_empty_document();
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

    // Load any new items.
    for (auto& [uuid, path] : wanted) {
        if (item_uuid_to_cue_.find(uuid) != item_uuid_to_cue_.end()) continue;
        const auto cue_id = engine_.load_cue(path);
        if (cue_id.empty()) {
            Logger::warn("ProjectState: failed to load audio item uuid='{}' path='{}'",
                         uuid, util::path_to_utf8(path));
            continue;
        }
        item_uuid_to_cue_.emplace(uuid, cue_id);

        // Populate CueMeta so /api/cues stays consistent.
        CueMeta meta;
        meta.id           = cue_id;
        meta.file_path    = path;
        const auto md     = meta::read_metadata(path);
        meta.display_name = md.title.empty() ? util::path_to_utf8(path.filename()) : md.title;
        meta.artist       = md.artist;
        meta.title        = md.title;
        meta.duration_seconds =
            static_cast<double>(md.duration.count()) / 1000.0;
        cues_.emplace(cue_id.value, std::move(meta));
    }

    // Apply per-item audio properties (gain/fade/etc.) to the engine.
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
            if (item.contains("fadeOutDuration") &&
                item["fadeOutDuration"].is_number()) {
                cue->set_fade_out(std::chrono::milliseconds{
                    static_cast<long long>(item["fadeOutDuration"].get<double>() * 1000.0)});
            }
        });
}

// ---------------------------------------------------------------------------
// Cue mutations (control thread)
// ---------------------------------------------------------------------------
audio::CueId ProjectState::add_cue_from_file(const std::filesystem::path& file,
                                             std::string display_name) {
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
            doc["lastModified"] = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
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
json ProjectState::full_document() const {
    std::lock_guard lock{mutex_};
    json out = document_;
    // Decorate audio items with their server-side cue_id so the client can
    // map UI → engine commands without a separate lookup pass.
    std::function<void(json&)> annotate;
    annotate = [&](json& arr) {
        if (!arr.is_array()) return;
        for (auto& it : arr) {
            if (!it.is_object()) continue;
            const std::string uuid = it.value("uuid", std::string{});
            if (it.value("type", std::string{}) == "audio") {
                auto found = item_uuid_to_cue_.find(uuid);
                if (found != item_uuid_to_cue_.end()) {
                    it["cueId"] = found->second.value;
                }
            } else if (it.value("type", std::string{}) == "group" &&
                       it.contains("children")) {
                annotate(it["children"]);
            }
        }
    };
    if (out.contains("items"))         annotate(out["items"]);
    if (out.contains("cartOnlyItems")) annotate(out["cartOnlyItems"]);

    // Attach a minimal "server" block so the client can read project file
    // path, available engine cues, etc. without a separate fetch.
    out["server"] = json{
        {"projectFilePath", util::path_to_utf8(project_file_path_)},
        {"mediaRoot",       util::path_to_utf8(media_root_)},
    };
    return out;
}

bool ProjectState::replace_full_document(const json& doc) {
    if (!doc.is_object()) return false;
    std::lock_guard lock{mutex_};
    // Unload everything; mirror_items_to_engine_locked rebuilds.
    for (auto& [_, id] : item_uuid_to_cue_) engine_.unload_cue(id);
    item_uuid_to_cue_.clear();
    cues_.clear();
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
    mirror_items_to_engine_locked();
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

    {
        std::lock_guard lock{mutex_};
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
        if (it != item_uuid_to_cue_.end()) return it->second;
    }
    return {};
}

bool ProjectState::update_item(const std::string& uuid, const json& patch) {
    if (!patch.is_object()) return false;
    bool touched = false;
    {
        std::lock_guard lock{mutex_};
        for_each_item(document_,
            [&](json& it, const std::string& /*parent*/) {
                if (it.value("uuid", std::string{}) != uuid) return;
                for (auto& [k, v] : patch.items()) {
                    if (k == "uuid") continue;   // can't repath via patch
                    it[k] = v;
                }
                touched = true;
            });
        if (touched) mirror_items_to_engine_locked();
    }
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
    std::lock_guard lock{mutex_};
    if (!document_.contains("settings") || !document_["settings"].is_object()) {
        document_["settings"] = json::object();
    }
    for (auto& [k, v] : patch.items()) document_["settings"][k] = v;
    // settings changes may require re-routing (default device, preview device).
    // For now we just record them; routing wiring comes in the next phase.
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
        c["ltc_enabled"]   = false;
        c["ltc_fps"]       = 4;
        c["ltc_offset_ns"] = 0;
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
        std::lock_guard lock{mutex_};
        // Unload any previously-loaded engine cues so we start clean.
        for (auto& [_, id] : item_uuid_to_cue_) engine_.unload_cue(id);
        item_uuid_to_cue_.clear();
        cues_.clear();
        mixers_.clear();
        item_routes_.clear();
        mixer_routes_.clear();
        master_assignments_.clear();

        document_ = doc_in;
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

        mirror_items_to_engine_locked();
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
            m.ltc_enabled = c.value("ltc_enabled", false);
            m.ltc_frame_rate_index = c.value("ltc_fps", 4);
            m.ltc_offset_ns = std::chrono::nanoseconds{c.value("ltc_offset_ns", (long long)0)};
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

} // namespace liveplay::core
