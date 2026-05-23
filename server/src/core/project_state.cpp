// ============================================================================
// project_state.cpp — see project_state.hpp.
// ============================================================================
#include "liveplay/core/project_state.hpp"
#include "liveplay/logger.hpp"
#include "liveplay/meta/metadata.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <fstream>

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

ProjectState::ProjectState(audio::AudioEngine& engine) : engine_(engine) {}

void ProjectState::reset() {
    std::lock_guard lock{mutex_};
    cues_.clear();
    mixers_.clear();
    item_routes_.clear();
    mixer_routes_.clear();
    master_assignments_.clear();
    project_name_ = "Untitled";
    apply_to_engine_locked();
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
                          ? (md.title.empty() ? file.filename().string() : md.title)
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
    try {
        std::ofstream f{path};
        if (!f) return false;
        f << to_json().dump(2);
        return true;
    } catch (const std::exception& ex) {
        Logger::error("ProjectState::save failed: {}", ex.what());
        return false;
    }
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
    json doc = doc_in;
    if (is_legacy_document(doc)) {
        Logger::info("ProjectState: detected legacy 1.x document, upgrading.");
        doc = upgrade_legacy_document(doc);
    }

    std::lock_guard lock{mutex_};
    cues_.clear();
    mixers_.clear();
    item_routes_.clear();
    mixer_routes_.clear();
    master_assignments_.clear();

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
            Logger::error("ProjectState::load: cannot open '{}'", path.string());
            return false;
        }
        json doc;
        f >> doc;
        return load_from_json(doc);
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
