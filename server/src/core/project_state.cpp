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
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace liveplay::core {

namespace {
inline std::string id_to_string(const audio::CueId& id)          { return id.value; }
inline std::string id_to_string(const audio::MixerChannelId& id) { return id.value; }
inline std::string id_to_string(const audio::DeviceId& id)       { return id.value; }

// Type-safe JSON field read. Unlike nlohmann's .value<T>()/.at().get<T>(),
// this never throws on a wrong-typed (or null) field — it returns `def`
// instead. Malformed project fields (e.g. a number where a string is
// expected) would otherwise throw nlohmann::type_error uncaught through the
// network layer. 
template <class T>
T json_get_or(const nlohmann::json& j, const char* key, const T& def) noexcept {
    try {
        if (auto it = j.find(key); it != j.end() && !it->is_null())
            return it->get<T>();
    } catch (...) {}
    return def;
}

// An item's two fade-outs, from its document fields. One owner for the rule,
// because three mirror paths each carried their own copy and one had drifted.
//
//   end fade  (EOF / out-point) = max(stopFade, fadeOutDuration) -- unchanged
//             from 2.4, so an existing show ends every cue exactly as before.
//   stop fade (the Stop button) = manualStopFade when the item has one (#56),
//             else the same legacy max -- so a show that relied on stopFade to
//             make its Stop button fade keeps doing so until someone sets the
//             new field.
void apply_item_fade_outs(audio::PlaybackItem& cue, const nlohmann::json& item) {
    const double stop_fade = std::max(0.0, json_get_or(item, "stopFade", 0.0));
    const double fade_out  = std::max(0.0, json_get_or(item, "fadeOutDuration", 0.0));
    const double end_fade  = std::max(stop_fade, fade_out);
    const auto to_ms = [](double sec) {
        return std::chrono::milliseconds{static_cast<long long>(sec * 1000.0)};
    };
    cue.set_fade_out(to_ms(end_fade));
    if (auto it = item.find("manualStopFade"); it != item.end() && it->is_number())
        cue.set_stop_fade(to_ms(std::max(0.0, it->get<double>())));
}

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
// Media path resolution
// ---------------------------------------------------------------------------
// An audio item can carry two file references:
//   • mediaPath       — RELATIVE to the project folder, e.g. "media/foo.mp3".
//                       Portable: it stays valid even after the whole project
//                       (the .liveplay file plus its media/ folder) is moved,
//                       because folderPath is rewritten to the file's real
//                       location on load.
//   • mediaServerPath — an ABSOLUTE path captured at import time. Handy while
//                       the project hasn't moved, but goes stale the instant
//                       the project is relocated.
//
// We therefore prefer the relative form whenever it actually points at a file
// on disk, and only fall back to the absolute mediaServerPath when the relative
// form is absent or missing. That makes moved projects — and legacy v1 projects
// (relative mediaPath, but a folderPath baked to the old location) — resolve
// correctly, while still honouring genuine out-of-folder references.
inline std::filesystem::path resolve_media_path(const json& item,
                                                const std::string& folder) {
    std::string relative;
    if (item.contains("mediaPath") && item["mediaPath"].is_string()) {
        const std::string media_path = item["mediaPath"].get<std::string>();
        if (!media_path.empty()) {
            if (!folder.empty()) {
                relative = folder;
                if (relative.back() != '/' && relative.back() != '\\')
                    relative += '/';
            }
            relative += media_path;
        }
    }
    std::string server;
    if (item.contains("mediaServerPath") && item["mediaServerPath"].is_string())
        server = item["mediaServerPath"].get<std::string>();

    std::error_code ec;
    if (!relative.empty()) {
        const auto rel_path = util::utf8_to_path(relative);
        if (std::filesystem::exists(rel_path, ec)) return rel_path;
    }
    if (!server.empty()) {
        const auto srv_path = util::utf8_to_path(server);
        if (std::filesystem::exists(srv_path, ec)) return srv_path;
    }
    // Neither file exists. Return the portable relative form when we have one
    // (so the item still carries a sensible path to relocate later); otherwise
    // the absolute one.
    if (!relative.empty()) return util::utf8_to_path(relative);
    return server.empty() ? std::filesystem::path{} : util::utf8_to_path(server);
}

// Rewrite every audio item to reference its media RELATIVE to the project
// folder whenever the file actually lives inside that folder. The portable
// "media/<file>" mediaPath becomes the canonical reference and the absolute,
// import-time mediaServerPath is dropped, so a saved project never strands its
// media when moved. Genuine out-of-folder references (media that doesn't live
// under the project folder) are left untouched, absolute path and all.
inline void relativize_media_paths(json& doc) {
    const std::string folder = doc.value("folderPath", std::string{});
    if (folder.empty()) return;
    std::error_code ec;
    const auto folder_base =
        std::filesystem::weakly_canonical(util::utf8_to_path(folder), ec);
    const std::filesystem::path base = ec ? util::utf8_to_path(folder) : folder_base;

    // Rewrite a single absolute path that lives inside the project folder to
    // its portable "subdir/file" relative form. Returns nullopt when the path
    // is empty or genuinely outside the folder (left untouched by the caller).
    const auto relativize_inside = [&](const std::string& p)
            -> std::optional<std::string> {
        if (p.empty()) return std::nullopt;
        std::error_code ecc;
        const auto abs = std::filesystem::weakly_canonical(util::utf8_to_path(p), ecc);
        const auto target = ecc ? util::utf8_to_path(p) : abs;
        std::error_code ecr;
        const auto rel = std::filesystem::relative(target, base, ecr);
        if (ecr || rel.empty() || *rel.begin() == std::filesystem::path(".."))
            return std::nullopt;
        std::string rel_utf8 = util::path_to_utf8(rel);
        std::replace(rel_utf8.begin(), rel_utf8.end(), '\\', '/');
        return rel_utf8;
    };

    const std::function<void(json&)> visit = [&](json& item) {
        if (!item.is_object()) return;
        if (item.value("type", std::string{}) == "audio") {
            const auto resolved = resolve_media_path(item, folder);
            if (!resolved.empty()) {
                std::error_code ec2;
                const auto resolved_canon = std::filesystem::weakly_canonical(resolved, ec2);
                const auto abs = ec2 ? resolved : resolved_canon;
                std::error_code ec3;
                const auto rel = std::filesystem::relative(abs, base, ec3);
                // Inside the project folder iff the relative path exists and its
                // first component isn't ".." (i.e. it doesn't climb back out).
                if (!ec3 && !rel.empty() && *rel.begin() != std::filesystem::path("..")) {
                    std::string rel_utf8 = util::path_to_utf8(rel);
                    std::replace(rel_utf8.begin(), rel_utf8.end(), '\\', '/');
                    item["mediaPath"] = rel_utf8;
                    item.erase("mediaServerPath");
                }
            }
            // Normalise the waveform sidecar path to the portable relative form
            // too, so a project moved to a new folder keeps resolving its
            // waveforms. Absolute paths that point outside the folder are left
            // as-is (genuine external reference).
            if (item.contains("waveformPath") && item["waveformPath"].is_string()) {
                if (auto relwf = relativize_inside(item["waveformPath"].get<std::string>()))
                    item["waveformPath"] = *relwf;
            }
        }
        if (item.value("type", std::string{}) == "group" &&
            item.contains("children") && item["children"].is_array()) {
            for (auto& ch : item["children"]) visit(ch);
        }
    };
    if (doc.contains("items") && doc["items"].is_array())
        for (auto& it : doc["items"]) visit(it);
    if (doc.contains("cartOnlyItems") && doc["cartOnlyItems"].is_array())
        for (auto& it : doc["cartOnlyItems"]) visit(it);
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

// ---------------------------------------------------------------------------
// Output Target loudness standards.
// The server is the single authority on these numbers. The client reads them
// back through settings["outputTargetLevels"] embedded in every full_document()
// / header_document() response and in the settings_patched broadcast.
// ---------------------------------------------------------------------------
struct OutputTargetLevels {
    float blue_below;            // meter reads blue below this value
    float green_min;             // green zone start
    float green_max;             // green zone end
    float yellow_min;            // yellow zone start (== green_max)
    float yellow_max;            // yellow zone end (== red threshold == hard limit)
    float limiter_ceiling_db;    // brickwall limiter ceiling for this platform
    float auto_volume_target_db; // target for the auto-volume normalise feature
    const char* meter_unit;      // preferred unit: "LUFS" | "dBFS" | "dBTP" | "RMS"
    const char* waveform_color;  // CSS hex color for the properties-panel waveform
};

// All zone boundaries are in the same unit as meter_unit for that platform.
static const std::unordered_map<std::string, OutputTargetLevels> kOutputTargets {
    // EBU R128 — integrated loudness target -23 LUFS, max TP -1 dBTP
    {"ebu-r128",  {-28.0f, -28.0f, -20.0f, -20.0f, -1.0f,  -1.0f,  -23.0f, "LUFS", "#00e676"}},
    // Streaming (Spotify, Apple Music, YouTube) — target ~ -14 LUFS
    {"streaming", {-19.0f, -19.0f, -11.0f, -11.0f, -1.0f,  -1.0f,  -14.0f, "LUFS", "#00e676"}},
    // Radio broadcast (EBU R128 S1 / ITU BS.1770-4 radio) — target -16 LUFS
    {"radio",     {-21.0f, -21.0f, -13.0f, -13.0f, -1.0f,  -1.0f,  -16.0f, "LUFS", "#00e676"}},
    // Netflix (OAPP — Operational Audio Practice for Post) — -27 LUFS, TP -2 dBTP
    {"netflix",   {-32.0f, -32.0f, -24.0f, -24.0f, -2.0f,  -2.0f,  -27.0f, "LUFS", "#00e676"}},
    // Live / Digital Console — dBFS peaks, green comfort at -9 dBFS peak (-18 RMS)
    {"live",      {-24.0f, -24.0f, -9.0f,  -9.0f,  -0.1f,  -0.1f,  -18.0f, "dBFS", "#00e676"}},
};

static json compute_output_target_levels(const json& settings) {
    const std::string target = settings.value("outputTarget", std::string{"ebu-r128"});
    auto it = kOutputTargets.find(target);
    if (it == kOutputTargets.end()) it = kOutputTargets.find("ebu-r128");
    const auto& lv = it->second;
    return json{
        {"blueBelow",          lv.blue_below},
        {"greenMin",           lv.green_min},
        {"greenMax",           lv.green_max},
        {"yellowMin",          lv.yellow_min},
        {"yellowMax",          lv.yellow_max},
        {"redAbove",           lv.yellow_max},
        {"limiterCeilingDb",   lv.limiter_ceiling_db},
        {"autoVolumeTargetDb", lv.auto_volume_target_db},
        {"meterUnit",          lv.meter_unit},
        {"waveformColor",      lv.waveform_color},
    };
}

// Resolve the project's meter ballistics from settings.meterBallistics
// (preset id) / settings.meterBallisticsCustom ({attackMs, releaseMs,
// rmsWindowMs}, used when the preset id is "custom"). Unknown or absent
// values fall back to the engine default (digital-ppm feel).
static audio::MeterBallistics meter_ballistics_from_settings(const json& settings) {
    const std::string preset =
        settings.value("meterBallistics", std::string{"digital-ppm"});
    if (preset == "custom" &&
        settings.contains("meterBallisticsCustom") &&
        settings["meterBallisticsCustom"].is_object()) {
        const auto& c = settings["meterBallisticsCustom"];
        audio::MeterBallistics b;
        b.attack_ms     = std::clamp(c.value("attackMs",    1.0f),   0.0f, 5000.0f);
        b.release_ms    = std::clamp(c.value("releaseMs",   300.0f), 0.0f, 10000.0f);
        b.rms_window_ms = std::clamp(c.value("rmsWindowMs", 300.0f), 1.0f, 10000.0f);
        return b;
    }
    return audio::meter_ballistics_from_preset(preset)
        .value_or(audio::MeterBallistics{});
}

// The unit this PROJECT implies, which since U4 is the only thing the document
// still has to say about metering: settings.meterMode moved to the person, so
// what is left is the output target's recommended unit. A show mastered for
// EBU R128 still wants loudness computed even when nobody has opened a meter,
// because the limiter ceiling and the target levels are the show's, and an
// operator who has expressed no preference should see the unit the show is
// being made in. Mirrors the client's useOutputTarget fallback.
static std::string project_meter_mode(const json& settings) {
    return compute_output_target_levels(settings)
        .value("meterUnit", std::string{"LUFS"});
}

// ---------------------------------------------------------------------------
// The settings registry
// ---------------------------------------------------------------------------
// Every key `document_["settings"]` may carry, with the type and range that
// make it valid. Before this table existed, patch_settings() looped over the
// incoming patch and wrote each key straight into the show document, so any
// client could persist any key of any type into a .liveplay and only a
// handful of them had server meaning. The document is the portable artifact —
// it should not be an open bag.
//
// Unknown keys are DROPPED, not rejected, and that is deliberate. The client
// watches its whole settings object and PATCHes all of it back whenever any
// part changes (useProject.ts), and the object it holds is the one
// full_document() decorated with the derived outputTargetLevels. Answering a
// stray key with 400 would therefore fail every settings edit in the app.
// Dropping buys the guarantee that actually matters — the document only ever
// contains registered keys — without breaking that round trip.
//
// Numbers clamp rather than drop, matching merge_bus_dsp()'s convention for
// every other client-supplied number in this file.
enum class SettingKind {
    Bool,
    Enum,          // string, one of `allowed`
    Number,        // double, clamped to [min, max]
    Integer,       // whole number, clamped to [min, max]
    StringOrNull,  // free string, or null to clear
    Ballistics,    // the meterBallisticsCustom object
    Derived,       // computed server-side; never accepted from a client
    Relocated,     // moved to a user profile (U4); read on load, never stored
};

struct SettingSpec {
    SettingKind                   kind;
    double                        min = 0.0;
    double                        max = 0.0;
    std::vector<std::string_view> allowed{};
};

// Function-local static: avoids any static-init-order dependency on
// kOutputTargets, which shares this translation unit.
static const std::unordered_map<std::string, SettingSpec>& settings_registry() {
    static const std::unordered_map<std::string, SettingSpec> kRegistry = {
        // --- Audio / metering -------------------------------------------
        // Where this show's timecode goes, as a LOGICAL output name — the
        // same vocabulary a bus uses, bound to hardware by the machine's
        // output map (D38). It replaced ltcDevice, which named a sound card
        // in a document meant to travel.
        {"ltcOutput",                     {SettingKind::StringOrNull}},
        {"outputTarget",                  {SettingKind::Enum, 0, 0,
                                           {"ebu-r128", "streaming", "radio", "netflix", "live"}}},
        {"meterBallistics",               {SettingKind::Enum, 0, 0,
                                           {"digital-ppm", "ppm-i", "ppm-ii", "vu", "instant",
                                            "custom"}}},
        {"meterBallisticsCustom",         {SettingKind::Ballistics}},
        {"disableLimiter",                {SettingKind::Bool}},

        // --- Playback ----------------------------------------------------
        {"defaultTransitionMode",         {SettingKind::Enum, 0, 0,
                                           {"crossfade", "start-next"}}},
        {"autoCueNextWithoutEndBehavior", {SettingKind::Bool}},
        // 60 s is well past any musical fade and still finite, so a typo'd
        // value cannot wedge Stop-All into an unstoppable ramp.
        {"stopAllFadeMs",                 {SettingKind::Integer, 0.0, 60'000.0}},
        // Absent = true: Stop All is a panic button and silences the preview
        // audition too. False lets an operator keep pre-listening through a
        // stop-all (#60).
        {"stopAllStopsPreview",           {SettingKind::Bool}},
        {"disableAutoVolumeAndTrim",      {SettingKind::Bool}},
        {"disableSilenceWarning",         {SettingKind::Bool}},

        // --- UI / project ------------------------------------------------
        {"autoSave",                      {SettingKind::Bool}},
        // Matches normalizeIndexDisplayStart() on the client: a non-negative
        // whole number. The ceiling is arbitrary but keeps the displayed cue
        // number inside a sane column width.
        {"indexDisplayStart",             {SettingKind::Integer, 0.0, 1'000'000.0}},

        // --- Legacy, still honoured on the way in ------------------------
        // All three migrate at load and are erased from the document there —
        // the first two onto buses, ltcDevice onto ltcOutput. They stay
        // registered so a pre-2.5 client patching one is handled by the
        // migration rather than silently dropped; patch_settings() rewrites
        // ltcDevice to ltcOutput on the way in, so the live key keeps exactly
        // one writer.
        {"defaultOutputDevice",           {SettingKind::StringOrNull}},
        {"previewDevice",                 {SettingKind::StringOrNull}},
        {"ltcDevice",                     {SettingKind::StringOrNull}},

        // --- Relocated to the person (U4) ---------------------------------
        // These two belong to whoever is looking at the screen, not to the
        // show: which unit a meter is drawn in, and whether the playlist
        // chases the playing cue. They live in a user profile now
        // (user_prefs.hpp). Registered rather than deleted so the drop is
        // explained instead of reading as "not a known setting" — a 2.4
        // document still carries them, and a 2.4 client still patches them.
        {"meterMode",                     {SettingKind::Relocated}},
        {"uiScrollToPlaying",             {SettingKind::Relocated}},

        // --- Derived ------------------------------------------------------
        // compute_output_target_levels() owns this. full_document() injects a
        // fresh copy on every read, so the client always has it and hands it
        // straight back; accepting it is what used to persist a stale copy
        // into the saved document.
        {"outputTargetLevels",            {SettingKind::Derived}},
    };
    return kRegistry;
}

// Validate one key against the registry. Returns the value to persist, or
// nullopt to drop it — with `why` describing the reason for the log line.
static std::optional<json> validate_setting(const std::string& key,
                                            const json&        value,
                                            std::string&       why) {
    const auto& reg = settings_registry();
    const auto  it  = reg.find(key);
    if (it == reg.end()) {
        why = "not a known setting";
        return std::nullopt;
    }
    const SettingSpec& spec = it->second;

    switch (spec.kind) {
    case SettingKind::Derived:
        why = "computed server-side";
        return std::nullopt;

    case SettingKind::Relocated:
        why = "moved to user preferences — PATCH /api/prefs";
        return std::nullopt;

    // Every accepted branch wraps the value explicitly: nlohmann's greedy
    // converting constructor means `return value;` for an lvalue json does not
    // resolve to optional<json>.
    case SettingKind::Bool:
        if (!value.is_boolean()) { why = "expected a boolean"; return std::nullopt; }
        return std::optional<json>{value};

    case SettingKind::StringOrNull:
        if (value.is_null() || value.is_string()) return std::optional<json>{value};
        why = "expected a string or null";
        return std::nullopt;

    case SettingKind::Enum: {
        if (!value.is_string()) { why = "expected a string"; return std::nullopt; }
        const auto s = value.get<std::string>();
        for (const auto& a : spec.allowed)
            if (s == a) return std::optional<json>{value};
        why = "not one of the accepted values";
        return std::nullopt;
    }

    case SettingKind::Number: {
        if (!value.is_number()) { why = "expected a number"; return std::nullopt; }
        return std::optional<json>{json(std::clamp(value.get<double>(), spec.min, spec.max))};
    }

    case SettingKind::Integer: {
        if (!value.is_number()) { why = "expected a number"; return std::nullopt; }
        const double c = std::clamp(value.get<double>(), spec.min, spec.max);
        return std::optional<json>{json(static_cast<long long>(std::trunc(c)))};
    }

    case SettingKind::Ballistics: {
        if (!value.is_object()) { why = "expected an object"; return std::nullopt; }
        // Same clamps meter_ballistics_from_settings() applies when reading,
        // hoisted to the write so the stored value and the applied value
        // cannot disagree. Unrecognised sub-keys are dropped with the rest.
        json out = json::object();
        out["attackMs"]    = std::clamp(value.value("attackMs",    1.0),   0.0, 5'000.0);
        out["releaseMs"]   = std::clamp(value.value("releaseMs",   300.0), 0.0, 10'000.0);
        out["rmsWindowMs"] = std::clamp(value.value("rmsWindowMs", 300.0), 1.0, 10'000.0);
        return std::optional<json>{std::move(out)};
    }
    }
    why = "unhandled setting kind";
    return std::nullopt;
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
        {"pfl",          m.pfl},
    };
}

void to_json(json& j, const RouteSendV2& r) {
    j = json{
        {"source_channel",    r.source_channel},
        {"destination_mixer", r.destination_mixer.value},
        {"gain_db",           r.gain_db},
        {"lane",              r.lane},
    };
}

void to_json(json& j, const MixerToMasterV2& r) {
    j = json{
        {"mixer",           r.mixer.value},
        {"master_channel",  r.master_channel},
        {"gain_db",         r.gain_db},
        {"lane",            r.lane},
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

ProjectState::ProjectState(audio::AudioEngine& engine, OutputMap& outputs)
    : engine_(engine), outputs_(outputs) {
    {
        std::lock_guard lock{mutex_};
        document_ = default_empty_document();
        // A server with no project open still has a desk: the default master
        // and preview buses (D35), so /api/buses and the mixer never look at
        // an empty rail and the house pair is wired from the start.
        load_buses_locked();
        write_buses_to_document_locked();
        pending_bus_migration_ = {};
    }
    materialise_buses();
    start_sequencer();
    // Background decoder for single-item adds/media swaps (#43).
    loader_thread_ = std::thread([this] { loader_loop(); });
}

ProjectState::~ProjectState() {
    stop_sequencer();
    stop_loaders();
    // Make sure any in-flight async mirror finishes before the engine is
    // torn down — otherwise the worker would dereference dangling state.
    {
        std::lock_guard lock{mirror_mutex_};
        if (load_thread_.joinable()) load_thread_.join();
    }

    // Tear down preview infrastructure on shutdown so the audio device gets
    // released cleanly.
    // The strip and the device belong to the preview bus now, and are torn
    // down with every other bus routing; only the auditioned cue is ours.
    if (!preview_cue_.empty()) {
        engine_.stop(preview_cue_);
        engine_.unload_cue(preview_cue_);
    }
}

// ---------------------------------------------------------------------------
// Single-item async audio load (#43)
// ---------------------------------------------------------------------------
// add_item() and update_item() must return promptly and must not hold mutex_
// across an audio decode: one large or network-mounted file used to stall every
// other request (play_item, stop, state, WS/HTTP handlers) for the whole decode.
//
// The split: under mutex_ we reserve the CueId and register a placeholder
// CueMeta (so the cue is immediately visible to find_cue / list_cues /
// item_to_cue_id and callers get a usable id synchronously), then queue the
// decode. The loader thread decodes with no ProjectState lock held and takes
// mutex_ again only for the cheap publish step.
// ---------------------------------------------------------------------------
audio::CueId ProjectState::begin_item_load_locked(const std::string& uuid,
                                                  const std::filesystem::path& path,
                                                  const json& item) {
    if (uuid.empty() || path.empty()) return {};

    const audio::CueId cue_id{engine_.new_cue_id()};

    // Placeholder metadata: the real artist/title/duration arrive with the
    // decode. Seed the duration from the document so the sequencer has
    // something sane if the item is fired before the load lands.
    CueMeta meta;
    meta.id        = cue_id;
    meta.file_path = path;
    meta.display_name = item.value("displayName", std::string{});
    if (meta.display_name.empty())
        meta.display_name = util::path_to_utf8(path.filename());
    meta.duration_seconds = json_get_or(item, "duration", 0.0);
    cues_.emplace(cue_id.value, std::move(meta));
    item_uuid_to_cue_[uuid] = cue_id;

    {
        std::lock_guard qlock{loader_mutex_};
        load_queue_.push_back(LoadRequest{uuid, cue_id, path});
        pending_load_uuids_.insert(uuid);
    }
    loader_cv_.notify_one();
    return cue_id;
}

void ProjectState::loader_loop() {
    for (;;) {
        LoadRequest req;
        {
            std::unique_lock qlock{loader_mutex_};
            loader_cv_.wait(qlock, [this] {
                return loaders_stop_ || !load_queue_.empty();
            });
            // On shutdown, drop whatever is still queued: those cues are about
            // to be torn down anyway, and the process shouldn't wait on them.
            if (loaders_stop_) return;
            req = std::move(load_queue_.front());
            load_queue_.pop_front();
        }

        // Guard the whole task — an exception escaping here would terminate the
        // process, and a decode touches the filesystem (network shares, removable
        // media) where anything can go wrong.
        try {
            // The expensive part: decoder init + metadata read, NO lock held.
            const auto loaded_id = engine_.load_cue_no_route(req.path, req.cue_id);
            const auto md        = meta::read_metadata(req.path);

            bool  publish   = false;
            bool  is_cart   = false;
            json  item_snap;
            {
                std::lock_guard lock{mutex_};
                // The item may have been removed, or its media swapped again,
                // while we were decoding. Either way this cue is now an orphan.
                auto it = item_uuid_to_cue_.find(req.uuid);
                const bool still_wanted =
                    it != item_uuid_to_cue_.end() && it->second == req.cue_id;

                if (!still_wanted || loaded_id.empty()) {
                    if (!loaded_id.empty()) engine_.unload_cue(loaded_id);
                    cues_.erase(req.cue_id.value);
                    if (!still_wanted) {
                        Logger::info("ProjectState: dropped stale load for uuid='{}'",
                                     req.uuid);
                    } else {
                        // Decode failed: drop the placeholder mapping too, so the
                        // item reads as "not loaded" rather than silently dead.
                        item_uuid_to_cue_.erase(req.uuid);
                        Logger::warn("ProjectState: failed to load item uuid='{}' ('{}')",
                                     req.uuid, util::path_to_utf8(req.path));
                    }
                } else {
                    auto cm_it = cues_.find(req.cue_id.value);
                    if (cm_it != cues_.end()) {
                        auto& meta = cm_it->second;
                        if (!md.title.empty()) meta.display_name = md.title;
                        meta.artist = md.artist;
                        meta.title  = md.title;
                        if (md.duration.count() > 0) {
                            meta.duration_seconds =
                                static_cast<double>(md.duration.count()) / 1000.0;
                        }
                    }
                    // Re-read the document node: the operator may have changed
                    // volume/fades/out point while the decode was running.
                    for_each_item(document_, [&](json& it2, const std::string&) {
                        if (it2.value("uuid", std::string{}) == req.uuid)
                            item_snap = it2;
                    });
                    if (item_snap.is_object())
                        apply_item_properties_locked(item_snap, req.cue_id);
                    // Cart-bound cues get primed below — they can be fired by a
                    // hotkey/MIDI at any moment and must be hot.
                    if (document_.contains("cartItems") &&
                        document_["cartItems"].is_array()) {
                        for (const auto& c : document_["cartItems"]) {
                            if (c.is_object() &&
                                c.value("itemUuid", std::string{}) == req.uuid) {
                                is_cart = true;
                                break;
                            }
                        }
                    }
                    publish = true;
                }
            }

            if (publish) {
                // Routing needs no ProjectState lock; apply_ltc_output_routing()
                // takes mutex_ itself, so it must run unlocked.
                engine_.ensure_default_routing();
                apply_ltc_output_routing();
                if (is_cart) {
                    if (auto* pi = engine_.find_cue(req.cue_id)) pi->prime();
                }
                Logger::info("ProjectState: loaded item uuid='{}' cue='{}'",
                             req.uuid, req.cue_id.value);
            }
        } catch (const std::exception& e) {
            Logger::error("ProjectState loader: uuid='{}' threw: {}", req.uuid, e.what());
        } catch (...) {
            Logger::error("ProjectState loader: uuid='{}' threw (unknown).", req.uuid);
        }

        // Release anyone waiting on this specific item (see wait_for_item_load).
        {
            std::lock_guard qlock{loader_mutex_};
            pending_load_uuids_.erase(req.uuid);
        }
        loader_done_cv_.notify_all();
    }
}

void ProjectState::stop_loaders() {
    {
        std::lock_guard qlock{loader_mutex_};
        loaders_stop_ = true;
    }
    loader_cv_.notify_all();
    if (loader_thread_.joinable()) loader_thread_.join();
    // Nothing will ever complete now — release any wait_for_item_load() caller.
    {
        std::lock_guard qlock{loader_mutex_};
        load_queue_.clear();
        pending_load_uuids_.clear();
    }
    loader_done_cv_.notify_all();
}

bool ProjectState::wait_for_item_load(const std::string& uuid,
                                      std::chrono::milliseconds timeout) {
    std::unique_lock qlock{loader_mutex_};
    if (pending_load_uuids_.find(uuid) == pending_load_uuids_.end()) return true;
    Logger::info("ProjectState: waiting for '{}' to finish loading", uuid);
    return loader_done_cv_.wait_for(qlock, timeout, [this, &uuid] {
        return pending_load_uuids_.find(uuid) == pending_load_uuids_.end();
    });
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
        // Items this pass actually loaded into the engine. They come out of
        // Phase 3 wired to the engine's default routing, which is not where
        // their bus says they belong — see the reroute at the bottom.
        std::vector<std::string> newly_loaded;
        try {
            // Phase 1: snapshot what we need to load under a brief lock.
            std::unordered_map<std::string, std::filesystem::path> wanted;
            std::unordered_set<std::string> cart_uuids;
            // LTC: per-item settings snapshotted here, the output resolved and
            // its feed opened between Phase 2/3.
            struct LtcItemSnap { bool enabled; std::string timecode; int fps_index; };
            std::unordered_map<std::string, LtcItemSnap> ltc_snaps;
            std::string ltc_output_name;
            std::unordered_map<std::string, std::filesystem::path> actually_wanted;
            {
                std::lock_guard lock{mutex_};
                json& doc = document_;
                for_each_item(doc, [&](json& item, const std::string&) {
                    if (item.value("type", std::string{}) != "audio") return;
                    const std::string uuid = item.value("uuid", std::string{});
                    if (uuid.empty()) return;
                    auto path = resolve_media_path(
                        item, doc.value("folderPath", std::string{}));
                    if (!path.empty()) {
                        wanted.emplace(uuid, std::move(path));
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
                // Snapshot the project-level LTC output name (D38: logical, not
                // a device — the machine's output map says what it means here).
                if (doc.contains("settings") && doc["settings"].is_object()) {
                    const auto& s = doc["settings"];
                    if (s.contains("ltcOutput") && s["ltcOutput"].is_string())
                        ltc_output_name = s["ltcOutput"].get<std::string>();
                }

                // Unload missing cues
                for (auto it = item_uuid_to_cue_.begin(); it != item_uuid_to_cue_.end();) {
                    if (wanted.find(it->first) == wanted.end()) {
                        engine_.unload_cue(it->second);
                        cues_.erase(it->second.value);
                        forget_primed_cue_locked(it->second);
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

            // Phase 2.5: resolve the LTC output and open its feed BEFORE we take
            // the mutex again in Phase 3. Doing it here (no lock held) avoids a
            // deadlock, because ensure_ltc_routing() acquires mutex_ internally.
            {
                bool any_ltc_enabled = false;
                for (auto& [_, ls] : ltc_snaps)
                    if (ls.enabled) { any_ltc_enabled = true; break; }
                if (any_ltc_enabled && !ltc_output_name.empty())
                    ensure_ltc_routing(ltc_output_name);
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
                    newly_loaded.push_back(l.uuid);

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
                        apply_item_fade_outs(*cue, it);
                        if (it.contains("outPoint") && it["outPoint"].is_number()) {
                            cue->set_out_point_seconds(it["outPoint"].get<double>());
                        }

                        // LTC: configure on the PlaybackItem and route its
                        // synthetic channel to the LTC strip (which Phase 2.5
                        // opened, and which ltc_routing_ now describes).
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
                            // Route the LTC synthetic channel to the LTC strip.
                            // Inactive means the output resolved to nothing, so
                            // there is nowhere to send timecode and the cue
                            // simply plays without it.
                            if (ltc_routing_.active) {
                                const auto ltc_ch = static_cast<audio::ChannelIndex>(
                                    cue->source_channel_count() - 1);
                                engine_.route_item_source_to_mixer(
                                    cit->second, ltc_ch, ltc_routing_.mixer, 0.0f);
                            }
                        }
                    });

                // Now that properties like ltc_enabled are applied, establish
                // default routing. This ensures LTC channels aren't mistakenly
                // routed to the Main mixer.
                engine_.ensure_default_routing();
            }

            // Phase 4: prime cart cues (also unlocked — engine handles its own).
            //
            // ONCE PER CUE, not once per mirror. Priming seeks the decoder and
            // decodes two seconds, and this ran for every cart binding every
            // time the mirror did — which is every save, because the client
            // round-trips the whole document. Sixteen cart cues meant sixteen
            // std::async threads all seeking and decoding at once, on a machine
            // that was in the middle of a show, for no benefit: the cue was
            // already primed and nothing had changed.
            //
            // A cue is dropped from the set when it is unloaded, so a genuinely
            // new or replaced cue still gets primed.
            std::vector<std::future<void>> prime_futures;
            for (const auto& uuid : cart_uuids) {
                audio::CueId cue;
                {
                    std::lock_guard lock{mutex_};
                    auto it = item_uuid_to_cue_.find(uuid);
                    if (it == item_uuid_to_cue_.end()) continue;
                    if (!primed_cues_.insert(it->second.value).second) continue;
                    cue = it->second;
                }
                prime_futures.push_back(std::async(std::launch::async,
                    [this, cue]() {
                        if (auto* pi = engine_.find_cue(cue)) pi->prime();
                    }));
            }
            for (auto& f : prime_futures) f.get();
            if (!prime_futures.empty()) {
                Logger::info("ProjectState: primed {} cart cue(s).", prime_futures.size());
            }
        } catch (const std::exception& e) {
            Logger::error("async mirror threw: {}", e.what());
        }
        // Apply the output-target brickwall ceiling configured for this project.
        //
        // ONLY WHAT CHANGED, field by field. This block runs at the end of
        // every mirror, and the mirror runs on every save — and re-applying an
        // unchanged ceiling was never the no-op it looked like. The engine's
        // set_master_ceiling_db() reconfigures the master limiters, and
        // Limiter::configure() zeroes the lookahead delay line and snaps the
        // gain envelope — five milliseconds of hard silence punched into the
        // house output, on both masters, on every save. That was the save-time
        // pop the seam detector finally caught: a step the size of the
        // programme material, at a block boundary, only when a save carried a
        // document. The meter setters walk every strip and reset meter state
        // the same way; cheaper, but just as pointless when nothing moved.
        //
        // patch_settings() applies these live when the operator changes them
        // and keeps applied_engine_settings_ in step, so the save that follows
        // a settings edit does not apply the same value a second time.
        {
            json settings_snap;
            {
                std::lock_guard lock{mutex_};
                settings_snap = document_.value("settings", json::object());
            }
            const auto levels = compute_output_target_levels(settings_snap);
            AppliedEngineSettings next;
            next.ceiling_db      = levels.value("limiterCeilingDb", -0.3f);
            next.limiter_enabled = !settings_snap.value("disableLimiter", false);
            next.ballistics      = meter_ballistics_from_settings(settings_snap);
            // Computed rather than applied here: this block already owns the
            // diffing against applied_engine_settings_, so it asks the gate
            // what it wants and folds the answer into the same comparison.
            std::tie(next.true_peak, next.loudness) = meter_gate_for(settings_snap);

            std::lock_guard alock{applied_engine_settings_mutex_};
            const auto& prev = applied_engine_settings_;
            if (!prev || prev->ceiling_db != next.ceiling_db)
                engine_.set_master_ceiling_db(next.ceiling_db);
            if (!prev || prev->limiter_enabled != next.limiter_enabled)
                engine_.set_limiter_enabled(next.limiter_enabled);
            if (!prev ||
                prev->ballistics.attack_ms     != next.ballistics.attack_ms ||
                prev->ballistics.release_ms    != next.ballistics.release_ms ||
                prev->ballistics.rms_window_ms != next.ballistics.rms_window_ms)
                engine_.set_meter_ballistics(next.ballistics);
            if (!prev || prev->true_peak != next.true_peak)
                engine_.set_true_peak_metering(next.true_peak);
            if (!prev || prev->loudness != next.loudness)
                engine_.set_loudness_metering(next.loudness);
            applied_engine_settings_ = next;
        }
        // Honour the project's default output device: re-pin every non-override
        // cue from Main (the OS default device, where ensure_default_routing()
        // above parked them) to the selected device. Previously this ran only
        // when the user changed the setting, so on load the project's chosen
        // device was ignored until re-selected. (#30)
        apply_default_device_routing();

        // Bus assignment is the last word on where an item goes, so it is
        // applied last — after ensure_default_routing() and the legacy
        // default-device pass have both had their say.
        //
        // Until this ran, an item carrying a busId sat on the engine's default
        // routing from the moment the project opened until the first time it
        // was played, because only play_item() consulted resolve_item_bus().
        // That is audible before a single GO: PFL and the bus meters were
        // reading the wrong strip, and an item assigned to a bus with its own
        // output was still wired to the house pair.
        //
        // Only the items this pass loaded. An ordinary save re-runs the mirror
        // with nothing new to load, and re-patching every cue in the project on
        // every save is exactly the churn §0.7 warns about — items already in
        // the engine keep the routing their bus assignment gave them, which
        // assign_item_bus() maintains through reroute_items_to_buses().
        if (!newly_loaded.empty()) {
            try {
                reroute_items_to_buses(newly_loaded);
            } catch (const std::exception& e) {
                Logger::error("async mirror: bus routing failed: {}", e.what());
            }
        }
        loading_audio_.store(false, std::memory_order_release);
    });
}

void ProjectState::reset() {
    // Drop queued single-item loads and let any in-flight decode finish before
    // we clear the tables (#43). A load that published after the reset would
    // resurrect a cue belonging to the project we just closed.
    {
        std::unique_lock qlock{loader_mutex_};
        for (const auto& req : load_queue_) pending_load_uuids_.erase(req.uuid);
        load_queue_.clear();
        loader_done_cv_.wait_for(qlock, std::chrono::seconds{20}, [this] {
            return pending_load_uuids_.empty();
        });
    }
    // Release anyone waiting on a load we just cancelled.
    loader_done_cv_.notify_all();

    // Quiesce any in-flight async mirror BEFORE taking mutex_. The mirror
    // worker acquires mutex_ in its phases, so joining it while we held the
    // lock would deadlock. mirror_mutex_ also serialises us against a
    // concurrent start_async_mirror(). Without this, a half-finished mirror
    // would repopulate cues_/item_uuid_to_cue_ right after we clear them and
    // leave dangling engine cues — the source of the crash when the next
    // project is opened.
    {
        std::lock_guard mirror_lock{mirror_mutex_};
        if (load_thread_.joinable()) load_thread_.join();
    }
    loading_audio_.store(false, std::memory_order_release);
    load_progress_loaded_.store(0, std::memory_order_release);
    load_progress_total_.store(0, std::memory_order_release);

    // Drop the sequencer's tracking list so its 50 ms loop stops dereferencing
    // cues we're about to unload (auto-advance / crossfade against a project
    // we've just closed).
    {
        std::lock_guard slock{sequencer_mutex_};
        sequenced_items_.clear();
    }
    next_item_override_.clear(); next_item_override_manual_ = false;

    std::unique_lock lock{mutex_};

    // Stop and unload every engine cue. Clearing the bookkeeping maps alone is
    // not enough — the PlaybackItems live in the engine and keep playing until
    // explicitly unloaded, which is why a closed project kept making sound.
    engine_.stop_all();
    for (auto& [_, id] : item_uuid_to_cue_) engine_.unload_cue(id);

    // Tear down any active preview cue too (its decoder outlives the maps).
    const bool had_preview = !preview_cue_.empty();
    if (had_preview) {
        engine_.stop(preview_cue_);
        engine_.unload_cue(preview_cue_);
        preview_cue_ = {};
    }
    preview_item_uuid_.clear();
    for (const auto& id : retired_preview_cues_) engine_.unload_cue(id);
    retired_preview_cues_.clear();
    auto preview_stopped_cb = had_preview ? preview_stopped_broadcaster_
                                          : std::function<void()>{};

    cues_.clear();
    mixers_.clear();
    item_routes_.clear();
    mixer_routes_.clear();
    master_assignments_.clear();
    item_uuid_to_cue_.clear();
    primed_cues_.clear();
    release_device_routings_locked();
    // The LTC feed goes too, but materialise_buses() below is what does it —
    // it owns the master-pair rewind, and the feed holds one of those pairs.
    // The outgoing project's strips are NOT torn down here: materialise_buses()
    // below does that, outside the lock, as it does on every load — and
    // creates the default desk in their place.
    buses_.clear();
    // The selection and the trigger-order stamps belong to the project that is
    // going away; carrying them into the next one would leave control surfaces
    // pointing at uuids that no longer exist. Show Mode and the locale are
    // operator preferences, not project data, so they survive the reset.
    selected_item_uuid_.clear();
    item_trigger_seq_.clear();
    project_name_ = "Untitled";
    project_file_path_.clear();
    document_ = default_empty_document();
    // A brand-new project still has a master and a preview bus (D35), so
    // /api/buses and the mixer are never looking at an empty desk.
    load_buses_locked();
    write_buses_to_document_locked();
    // The defaults are not a migration: nothing was rewritten on anyone's
    // behalf, so the next load reports only what IT had to do.
    pending_bus_migration_ = {};
    apply_to_engine_locked();
    lock.unlock();
    if (preview_stopped_cb) preview_stopped_cb();
    // Strips for the default desk, with the house pair wired (round-1
    // finding 7: a closed project used to leave the rail empty until the
    // next load). Outside the lock: every engine call takes its own.
    materialise_buses();
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
        // No "theme" (U4): a colour scheme belongs to whoever is looking at
        // the screen, so a blank document has nothing to say about one.
        // No device names at all: where audio goes is the master bus's
        // output, pre-listen goes to the preview bus, timecode goes to
        // ltcOutput, and the binding from a logical output to hardware
        // belongs to the machine, not the show. D21's last exception closed
        // with D38.
        {"settings",      json{
            {"ltcOutput",           nullptr},
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
    // Rule out the two formats that are definitely not client documents first,
    // so the `items` test below can be a plain shape check rather than a
    // content one. Anything carrying schema_version speaks the server's own
    // snake_case schema; anything carrying the 1.x collections is legacy (the
    // same keys is_legacy_document() looks for).
    if (doc.contains("schema_version")) return false;
    if (doc.contains("carts") || doc.contains("playlist") ||
        doc.contains("cues_legacy")) {
        return false;
    }
    // An `items` ARRAY is the client format, empty or not. Requiring a
    // populated item here meant a brand-new project — `items: []`, no carts —
    // fell through to the legacy branch and had its whole document replaced by
    // the default, silently discarding name, theme and settings.
    if (doc.contains("items") && doc["items"].is_array()) return true;
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

            // Resolve file path: prefer relative folderPath/mediaPath (portable),
            // fall back to the absolute mediaServerPath. See resolve_media_path().
            auto path = resolve_media_path(
                item, document_.value("folderPath", std::string{}));
            if (path.empty()) return;
            wanted.emplace(uuid, std::move(path));
        });

    // Unload any engine cues whose item is gone.
    for (auto it = item_uuid_to_cue_.begin(); it != item_uuid_to_cue_.end();) {
        if (wanted.find(it->first) == wanted.end()) {
            engine_.unload_cue(it->second);
            cues_.erase(it->second.value);
            forget_primed_cue_locked(it->second);
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
                [pi]() { pi->prime(); }));
        }
        for (auto& f : prime_futures) f.get();
        if (!cart_uuids.empty()) {
            Logger::info("ProjectState: primed {} cart cue(s).",
                         cart_uuids.size());
        }
    }

    // Apply per-item audio properties (gain/fade/in-out/LTC/etc.) to the engine.
    // NOTE: LTC *output routing* is handled separately in apply_ltc_output_routing()
    // which is called by callers after they release mutex_ (because
    // ensure_ltc_routing() also needs to acquire mutex_).
    for_each_item(document_,
        [&](json& item, const std::string& /*parent*/) {
            if (item.value("type", std::string{}) != "audio") return;
            const std::string uuid = item.value("uuid", std::string{});
            auto it = item_uuid_to_cue_.find(uuid);
            if (it == item_uuid_to_cue_.end()) return;
            apply_item_properties_locked(item, it->second);
        });

    // Now that every cue is in items_ and properties like ltc_enabled are
    // configured, establish default routing ONCE.
    engine_.ensure_default_routing();
}

// ---------------------------------------------------------------------------
// Push one document item's engine-visible properties onto its PlaybackItem.
// Caller holds mutex_. No-op when the cue isn't in the engine (yet) — that is
// the normal state for an item whose background decode is still in flight; the
// loader calls this again once the cue exists.
// ---------------------------------------------------------------------------
void ProjectState::apply_item_properties_locked(const json& item,
                                                const audio::CueId& cue_id) {
    auto* cue = engine_.find_cue(cue_id);
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
    apply_item_fade_outs(*cue, item);
    // outPoint: when set (> 0), engine fades out as the playhead reaches
    // that time instead of running to the file end.
    if (item.contains("outPoint") && item["outPoint"].is_number()) {
        cue->set_out_point_seconds(item["outPoint"].get<double>());
    } else {
        cue->set_out_point_seconds(0.0);  // disabled
    }

    // LTC: configure enabled/rate/offset on the PlaybackItem.
    // Routing of the synthetic LTC channel to the ltcOutput is done by the
    // caller after it releases mutex_ (via apply_ltc_output_routing()).
    const bool ltc_on = item.value("ltcEnabled", false);
    const std::string tc_str = item.value("ltcStartTimecode",
                                           std::string{"00:00:00:00"});
    const int fps_idx = item.value("ltcFrameRate", 4);
    cue->set_ltc_enabled(ltc_on);
    if (ltc_on) {
        const auto offset = parse_smpte_timecode_to_ns(tc_str, fps_idx);
        cue->set_ltc_frame_rate(fps_index_to_rate(fps_idx));
        cue->set_ltc_offset(offset);
        auto cm_it = cues_.find(cue_id.value);
        if (cm_it != cues_.end()) {
            cm_it->second.ltc_enabled          = true;
            cm_it->second.ltc_frame_rate_index = fps_idx;
            cm_it->second.ltc_offset_ns        = offset;
            cm_it->second.ltc_start_timecode   = tc_str;
        }
    }
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
// LTC output routing (D38)
//
// Timecode goes to a LOGICAL output, resolved by exactly the rule every bus
// uses: the output map wins; an unmapped name is a device name only if that
// device is actually present; otherwise nothing. Called from outside the mutex
// so ensure_ltc_routing (which acquires it) can safely do its work.
// ---------------------------------------------------------------------------
void ProjectState::apply_ltc_output_routing() {
    // 1. Under a brief lock, gather: the configured LTC output name and the
    //    list of LTC-enabled cues with their LTC channel index.
    std::string ltc_output;
    std::vector<std::pair<audio::CueId, audio::ChannelIndex>> ltc_routes;
    {
        std::lock_guard lock{mutex_};
        if (document_.contains("settings") && document_["settings"].is_object()) {
            const auto& s = document_["settings"];
            if (s.contains("ltcOutput") && s["ltcOutput"].is_string())
                ltc_output = s["ltcOutput"].get<std::string>();
        }
        if (!ltc_output.empty()) {
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

    // The output going away (or emptying) has to take the feed with it, or a
    // show that switches timecode off keeps a strip assigned to the interface
    // and keeps sending to it. Done before the early return for that reason.
    if (ltc_output.empty()) {
        std::lock_guard lock{mutex_};
        release_ltc_routing_locked();
        return;
    }
    if (ltc_routes.empty()) return;

    // 2. Resolve the name and make sure the feed exists (acquires/releases
    //    mutex_ internally — safe because we're not holding it here).
    const auto ltc_mixer = ensure_ltc_routing(ltc_output);
    if (ltc_mixer.empty()) return;

    // 3. Route each LTC channel to the LTC strip (engine ops; no mutex
    //    needed — the engine has its own independent synchronisation).
    for (auto& [cue_id, ltc_ch] : ltc_routes)
        engine_.route_item_source_to_mixer(cue_id, ltc_ch, ltc_mixer, 0.0f);
}

audio::MixerChannelId ProjectState::ensure_ltc_routing(const std::string& output_name) {
    if (output_name.empty()) {
        std::lock_guard lock{mutex_};
        release_ltc_routing_locked();
        return {};
    }

    // What this machine says the name means. `false` withholds the default
    // device deliberately, and it is the whole point of the unit: Main Out
    // unmapped IS the default device, and the default device is the house.
    // Timecode in the house is a squeal over the programme — the same class of
    // accident as PFL in the house, which is why the preview bus passes false
    // too. An LTC output this venue cannot provide is silent.
    const auto channels = resolve_output_channels(output_name, false);

    {
        std::lock_guard lock{mutex_};
        if (channels.empty()) {
            if (ltc_routing_.active) {
                Logger::warn("LTC output '{}' is no longer available on this machine — "
                             "timecode is silent", output_name);
            } else {
                Logger::warn("LTC output '{}' is not in the output map and names no "
                             "present device, so timecode is silent — map it, or point "
                             "settings.ltcOutput somewhere else", output_name);
            }
            release_ltc_routing_locked();
            return {};
        }
        // Already wired exactly here: leave it alone. Tearing a running feed
        // down and rebuilding it would gap the timecode a receiver is locked
        // to, and every save re-materialises the desk.
        if (ltc_routing_.active && ltc_routing_.output_name == output_name &&
            ltc_routing_.wired.size() == channels.size() &&
            std::equal(ltc_routing_.wired.begin(), ltc_routing_.wired.end(),
                       channels.begin(),
                       [](const OutputMap::Channel& a, const OutputMap::Channel& b) {
                           return a.device == b.device && a.hw_channel == b.hw_channel;
                       })) {
            return ltc_routing_.mixer;
        }
        // Anything else — a different name, or the same name now meaning
        // different hardware — is a move, so the old feed goes first.
        release_ltc_routing_locked();
    }

    // Open the device and build the strip with the lock released: every engine
    // API takes its own, and ensure_device_routing's ordering lesson applies
    // here too — create the strip before reserving the pair, so a refused
    // strip costs nothing to unwind.
    const auto dev = engine_.open_device_by_name(channels.front().device, 2);
    if (dev.empty()) {
        Logger::warn("LTC output '{}': could not open device '{}'; timecode is silent",
                     output_name, channels.front().device);
        return {};
    }
    const auto mixer = engine_.create_mixer_channel("LTC: " + output_name);
    if (mixer.empty()) {
        Logger::error("LTC output '{}': no mixer strip available; timecode is silent",
                      output_name);
        return {};
    }

    audio::MasterChannelIndex master_l = 0;
    audio::MasterChannelIndex master_r = 0;
    bool exhausted = false;
    {
        std::lock_guard lock{mutex_};
        if (!allocate_master_pair_locked(master_l, master_r)) {
            Logger::error("LTC output '{}': out of master channels (next={}, bus_width={}); "
                          "timecode is silent", output_name, next_override_master_,
                          engine_.config().master_channels);
            exhausted = true;   // unwound outside the lock, as the engine takes its own
        }
    }
    if (exhausted) {
        engine_.remove_mixer_channel(mixer);
        return {};
    }

    // Assign each master lane to the hardware channel the map named. A
    // one-channel mapping is honoured as one channel: a dedicated mono
    // timecode feed is the normal way an interface carries LTC, and putting it
    // on a second output the map did not ask for would waste a channel that
    // may well be carrying programme.
    engine_.assign_master_to_device(master_l, dev, channels[0].hw_channel);
    engine_.route_mixer_to_master(mixer, master_l, 0.0f, 0);
    if (channels.size() > 1) {
        engine_.assign_master_to_device(master_r, dev, channels[1].hw_channel);
        engine_.route_mixer_to_master(mixer, master_r, 0.0f, 1);
    }

    {
        std::lock_guard lock{mutex_};
        ltc_routing_ = LtcRouting{output_name, channels, dev, mixer,
                                  master_l, master_r, true};
    }
    Logger::info("LTC output '{}' → strip '{}' on device '{}' channel(s) {}{}",
                 output_name, mixer.value, channels.front().device,
                 channels[0].hw_channel,
                 channels.size() > 1 ? "/" + std::to_string(channels[1].hw_channel) : "");
    return mixer;
}

void ProjectState::release_ltc_routing_locked() {
    if (!ltc_routing_.active) return;
    // remove_mixer_channel() drops the strip's mixer->master sends and any
    // item->mixer sends aimed at it, so only the master->device assignments
    // need clearing explicitly — the same contract release_device_routings_-
    // locked() relies on.
    engine_.remove_mixer_channel(ltc_routing_.mixer);
    engine_.clear_master_assignment(ltc_routing_.master_l);
    if (ltc_routing_.wired.size() > 1)
        engine_.clear_master_assignment(ltc_routing_.master_r);
    release_master_pair_locked(ltc_routing_.master_l);
    Logger::debug("released the LTC feed for output '{}'", ltc_routing_.output_name);
    ltc_routing_ = LtcRouting{};
}

// ---------------------------------------------------------------------------
// Default output device routing — re-route all cues that have no per-item
// deviceOverride to the newly selected defaultOutputDevice. Called from
// patch_settings() whenever that key changes. Pattern mirrors
// apply_ltc_output_routing(): gather data under lock, then do engine ops
// outside the lock so ensure_device_routing() can safely acquire mutex_.
// ---------------------------------------------------------------------------
void ProjectState::apply_default_device_routing() {
    std::string device_name;
    std::vector<audio::CueId> non_override_cues;
    {
        std::lock_guard lock{mutex_};
        if (document_.contains("settings") && document_["settings"].is_object()) {
            const auto& s = document_["settings"];
            if (s.contains("defaultOutputDevice") && s["defaultOutputDevice"].is_string())
                device_name = s["defaultOutputDevice"].get<std::string>();
        }
        if (!device_name.empty()) {
            for_each_item(document_,
                [&](json& item, const std::string&) {
                    const std::string uuid = item.value("uuid", std::string{});
                    if (uuid.empty()) return;
                    // Skip items with a per-item device override.
                    if (item.contains("deviceOverride") &&
                        item["deviceOverride"].is_string() &&
                        !item["deviceOverride"].get<std::string>().empty()) return;
                    auto it = item_uuid_to_cue_.find(uuid);
                    if (it != item_uuid_to_cue_.end())
                        non_override_cues.push_back(it->second);
                });
        }
    }
    if (device_name.empty() || non_override_cues.empty()) return;

    const auto mixer = ensure_device_routing(device_name);
    if (mixer.empty()) return;

    for (const auto& cue_id : non_override_cues)
        route_cue_to_mixer(cue_id, mixer);

    // route_cue_to_mixer() clears every source route (incl. the LTC synthetic
    // channel), so re-establish LTC output routing for any LTC-enabled cues we
    // just re-pinned to the default device.
    apply_ltc_output_routing();

    Logger::info("apply_default_device_routing: routed {} cue(s) to '{}'",
                 non_override_cues.size(), device_name);
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

void ProjectState::update_media_root_from_folder_locked() {
    // The project folder is the single source of truth for where media lives.
    // Anchoring media_root_ to "<folderPath>/media" is what makes uploads and
    // server-side copies land inside the project folder (portable) instead of
    // the server's working directory — the old default that left imports
    // stranded where playback couldn't find them (PLAY: ?).
    const std::string folder = document_.value("folderPath", std::string{});
    if (folder.empty()) return;
    media_root_ = util::utf8_to_path(folder) / "media";
}

void ProjectState::reanchor_folder_path_locked() {
    // No file yet — a new project that has been given a folder but not written.
    // The client's folderPath is all there is, so leave it alone.
    if (project_file_path_.empty() || !project_file_path_.has_parent_path()) {
        update_media_root_from_folder_locked();
        return;
    }
    document_["folderPath"] = util::path_to_utf8(project_file_path_.parent_path());
    update_media_root_from_folder_locked();
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
        json doc;
        {
            std::lock_guard lock{mutex_};
            doc = document_;
            doc["lastModified"] = now_iso();
        }
        // The folder we are saving INTO is the project folder, whatever the
        // in-memory document says — the same rule load() applies in reverse.
        // relativize_media_paths() measures against this, so on a Save As the
        // media that stayed behind is correctly seen as outside the new folder
        // and keeps its absolute path instead of being rewritten to a
        // "media/..." that would not exist there.
        if (path.has_parent_path())
            doc["folderPath"] = util::path_to_utf8(path.parent_path());

        // Persist media as relative paths whenever it lives in the project
        // folder, so the saved file stays portable across moves. Covers items
        // imported this session (which carry an absolute mediaServerPath).
        relativize_media_paths(doc);

        // ...and then the folder itself goes. It is an absolute path on THIS
        // machine, and a show file has no business naming one: mail the project
        // to a colleague, unzip a .lpa anywhere, or just move the folder, and
        // the stored value is wrong while the file's own location is right.
        // load() has always overwritten it on the way in for exactly that
        // reason, so nothing reads what we were writing here.
        doc.erase("folderPath");

        // ...and so do the four values that belong to the PERSON (U4). A
        // document is a thing you mail to a colleague, and every one of these
        // travelled with it: opening someone else's show changed your colours
        // and silently reassigned your transport keys. They live in a user
        // profile now (user_prefs.hpp) or, with no accounts configured, in the
        // client's own machine store.
        //
        // Dropping them here rather than merely ignoring them on read is the
        // half that makes R1 true — leaving a stale copy in the file would
        // give every one of these two writers again the moment anything else
        // learned to read it. A 2.4 file still opens: load() reads these as
        // the seed for a profile that has none, which is the migration.
        //
        // cartSlotKeys deliberately STAYS. A cart wall is the show's layout,
        // not a personal preference — the slot that fires the door slam is a
        // property of this production, and it must be the same slot for
        // whoever is standing at the desk tonight.
        doc.erase("theme");
        doc.erase("playbackKeys");
        if (doc.contains("settings") && doc["settings"].is_object()) {
            doc["settings"].erase("meterMode");
            doc["settings"].erase("uiScrollToPlaying");
        }

        // Atomic write: serialise to a sibling temp file, verify the stream is
        // healthy, then rename it over the target. A write error, disk-full, or
        // crash therefore never truncates or corrupts the previous good file —
        // the documented "preserve previous state on failure" contract. 
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f{tmp, std::ios::binary | std::ios::trunc};
            if (!f) {
                Logger::error("ProjectState::save: cannot open '{}' for writing",
                              util::path_to_utf8(tmp));
                return false;
            }
            f << doc.dump(2);
            f.flush();
            f.close();
            if (!f.good()) {
                Logger::error("ProjectState::save: failed writing '{}' — "
                              "previous file left untouched",
                              util::path_to_utf8(tmp));
                std::error_code rm_ec;
                std::filesystem::remove(tmp, rm_ec);
                return false;
            }
        }

        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            Logger::error("ProjectState::save: rename '{}' -> '{}' failed: {} — "
                          "previous file left untouched",
                          util::path_to_utf8(tmp), util::path_to_utf8(path),
                          ec.message());
            std::error_code rm_ec;
            std::filesystem::remove(tmp, rm_ec);
            return false;
        }
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
    // Inject computed output-target levels so the client never needs to
    // hardcode platform loudness values.
    if (!out.contains("settings") || !out["settings"].is_object())
        out["settings"] = json::object();
    out["settings"]["outputTargetLevels"] = compute_output_target_levels(out["settings"]);

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

    // Inject computed output-target levels into the settings copy we send to the
    // client so it never has to hardcode platform loudness standards.
    json settings_out = document_.value("settings", json::object());
    settings_out["outputTargetLevels"] = compute_output_target_levels(settings_out);

    return json{
        {"name",         document_.value("name", "")},
        {"version",      document_.value("version", "")},
        {"folderPath",   document_.value("folderPath", "")},
        {"createdAt",    document_.value("createdAt", "")},
        {"lastModified", read_last_modified(document_)},
        {"theme",        document_.value("theme",         json::object())},
        {"settings",     std::move(settings_out)},
        {"cartItems",    document_.value("cartItems",     json::array())},
        {"cartSlotKeys", document_.value("cartSlotKeys",  json::object())},
        {"playbackKeys", document_.value("playbackKeys",  json::object())},
        {"cartOnlyItems", std::move(cart_only)},
        {"itemCount",    item_count},
        // The bus-schema version the loaded document is at. The client mirrors
        // it and hands it back on every whole-document save, which is how
        // replace_full_document() tells a round trip of this project from a
        // pre-bus project being pushed over the top of it (D11).
        {"busSchema",    document_.value("busSchema", kBusSchemaVersion)},
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

namespace {

// The document's declared bus-schema version, 0 when it does not declare one.
int bus_schema_of(const json& doc) {
    if (!doc.contains("busSchema") || !doc["busSchema"].is_number()) return 0;
    return doc["busSchema"].get<int>();
}

}  // namespace

bool ProjectState::replace_full_document(const json& doc) {
    if (!doc.is_object()) return false;
    bool buses_unchanged = false;
    {
        std::lock_guard lock{mutex_};
        // What the buses looked like before this document landed, so an
        // ordinary save can be told apart from a genuine project change.
        const json buses_before = document_.contains("buses") ? document_["buses"] : json{};
        // A document that does not mention buses is not asserting that there
        // are none — the client round-trips the project on every ordinary save
        // and does not carry the bus list, so taking absence literally wiped
        // every bus the moment anything else was edited. Buses are mutated
        // through their own endpoints; an absent key means "unchanged".
        //
        // But only for a document that says it comes from the bus era. A
        // round trip of a bus-era project declares `busSchema`; a pre-bus
        // project PUT over a loaded one does not, and must bring its own
        // routing rather than inherit the outgoing project's buses (D11).
        // Nothing is sniffed out of the content — the version is the signal.
        //
        // Any bus-era schema carries (D34: the rule is `>= 1`, not "the
        // current version") — a round trip from a client that has not yet
        // learned about roles still omits `buses` for the same reason.
        json carried_buses;
        if (!doc.contains("buses") && document_.contains("buses") &&
            bus_schema_of(doc) >= 1) {
            carried_buses = document_["buses"];
        }
        document_ = doc;
        if (!carried_buses.is_null()) document_["buses"] = std::move(carried_buses);
        if (!document_.contains("settings")) {
            // No defaultOutputDevice — load_buses_locked() migrates that key
            // onto the master bus and erases it, so re-injecting it here would
            // resurrect the field the migration exists to remove.
            document_["settings"] = json{
                {"ltcOutput",           nullptr},
            };
        }
        // No default theme is injected any more (U4). Filling one in made
        // every loaded document look like it was carrying a colour scheme,
        // which meant legacy_user_prefs() always had something to hand a
        // profile — and a person's one chance to inherit their real theme from
        // a 2.4 file was spent seeding them the default instead.
        project_name_ = document_.value("name", std::string{"Untitled"});
        // A document that names its own folder is asserting where it lives, and
        // is believed: that is how a brand-new project arrives — pushed with
        // the folder the operator picked, before any file has been written to
        // it. Only when it names none do we fall back to the open file's
        // location, which is the one thing on this machine that cannot be
        // stale. (A document loaded from disk was corrected on the way in, so
        // by the time a client round-trips it the two already agree.)
        if (document_.value("folderPath", std::string{}).empty())
            reanchor_folder_path_locked();
        else
            update_media_root_from_folder_locked();
        // The incoming document carries its own buses. Without this, buses_
        // would still describe the outgoing document and every assignment in
        // the new one would resolve against stale definitions.
        load_buses_locked();
        write_buses_to_document_locked();
        // Nothing about the buses moved, and every one of them already has a
        // strip: there is nothing to rebuild.
        //
        // The client round-trips the whole project on every ordinary save, so
        // without this check a single property edit tore down every bus strip
        // and wired it up again — re-opening audio devices, dropping the audio
        // running through those buses for the length of the rebuild, and
        // logging a full materialise each time. Saving a project should not
        // interrupt what is playing through it.
        buses_unchanged = !buses_before.is_null() && buses_before == document_["buses"];
        if (buses_unchanged) {
            for (const auto& b : buses_) {
                const auto it = bus_routings_.find(b.id);
                if (it == bus_routings_.end() || it->second.mixer.empty()) {
                    buses_unchanged = false;
                    break;
                }
            }
        }
    }
    // Rebuild the engine strips for the new document's buses. Outside the lock.
    //
    // Loud on purpose. This tears down every strip and wires it up again,
    // which reopens audio devices and drops whatever is playing through them —
    // so on an ordinary save it is a bug, not a step. If this line appears
    // while a show is running, the round trip through load_buses_locked() and
    // write_buses_to_document_locked() is not exact and something in the bus
    // JSON is changing shape on the way through.
    if (!buses_unchanged) {
        Logger::warn("replace_full_document: bus definitions changed shape — "
                     "rebuilding every strip. On an ordinary save this is a bug; "
                     "see BUS_ARCHITECTURE.md §0.7.");
        materialise_buses();
    }
    // Kick off the engine mirror asynchronously — matches load_from_json's
    // path so the PUT /api/project/document handler doesn't block on cue
    // decode for large projects. start_async_mirror() takes mutex_ itself,
    // so it must run after the lock above is released.
    start_async_mirror();
    // Route LTC channels to the LTC output (also acquires mutex_ internally).
    apply_ltc_output_routing();
    return true;
}

std::filesystem::path ProjectState::project_file_path() const {
    std::lock_guard lock{mutex_};
    return project_file_path_;
}
void ProjectState::set_project_file_path(std::filesystem::path p) {
    std::lock_guard lock{mutex_};
    project_file_path_ = std::move(p);
    // Learning where the file lives IS learning the project folder — the two
    // cannot disagree. Before this, a Save As adopted the new path but left
    // folderPath (and with it media_root_) pointing at the old folder, so the
    // next import landed in the project the operator had just saved away from.
    reanchor_folder_path_locked();
}

ProjectState::PlaybackSnapshot ProjectState::current_playback_snapshot() const {
    PlaybackSnapshot snap;
    std::lock_guard lock{mutex_};
    snap.project_file = util::path_to_utf8(project_file_path_);
    for (auto& [uuid, cue_id] : item_uuid_to_cue_) {
        auto* pi = engine_.find_cue(cue_id);
        if (!pi) continue;
        const auto st = pi->stats();
        if (st.transport == audio::TransportState::Playing ||
            st.transport == audio::TransportState::FadingIn) {
            snap.item_uuid    = uuid;
            snap.position_sec = st.playhead_seconds;
            break;
        }
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Item CRUD — operates on the document_ tree and mirrors audio items to
// the engine.
// ---------------------------------------------------------------------------
audio::CueId ProjectState::add_item(const json& item, const std::string& parent_uuid,
                                    bool cart_only) {
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

        if (cart_only) {
            // Cart-bound cue: lives in the separate cartOnlyItems array so it
            // is mirrored to the engine (cart hotkeys can trigger it) without
            // ever appearing in the playlist tree.
            if (!document_.contains("cartOnlyItems") ||
                !document_["cartOnlyItems"].is_array()) {
                document_["cartOnlyItems"] = json::array();
            }
            document_["cartOnlyItems"].push_back(item);
        } else if (parent_uuid.empty()) {
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
        // Queue the audio load(s) for what we just inserted instead of decoding
        // inline: mirror_items_to_engine_locked() decodes while holding mutex_,
        // so adding one big or network-mounted file stalled every other request
        // for the whole decode (#43). Walking the document (rather than just the
        // new node) covers a group's children too, and picks up any earlier item
        // that still has no cue — the same set the mirror would have loaded.
        for_each_item(document_, [&](json& it, const std::string&) {
            if (it.value("type", std::string{}) != "audio") return;
            const std::string u = it.value("uuid", std::string{});
            if (u.empty()) return;
            if (item_uuid_to_cue_.find(u) != item_uuid_to_cue_.end()) return;
            auto path = resolve_media_path(
                it, document_.value("folderPath", std::string{}));
            if (path.empty()) return;
            begin_item_load_locked(u, path, it);
        });
        auto it = item_uuid_to_cue_.find(uuid);
        if (it != item_uuid_to_cue_.end()) result = it->second;
    }
    // Route any LTC-enabled items to the LTC output (after releasing mutex_).
    // The loader repeats this once the decode lands — this call covers items
    // that were already loaded.
    apply_ltc_output_routing();
    return result;
}

bool ProjectState::update_item(const std::string& uuid, const json& patch) {
    if (!patch.is_object()) return false;
    bool touched            = false;
    bool media_path_changed = false;
    bool ltc_changed        = false;
    bool bus_changed        = false;
    json* updated_item = nullptr;
    // Items whose routing this edit moves: the item itself, or — when a group's
    // assignment changes — every audio descendant that inherits from it.
    std::vector<std::string> rerouted;

    // Captured under mutex_, applied to the engine loop state and the sequencer
    // snapshot AFTER the lock is released — so out point / crossfade / stop-fade /
    // end-behaviour edits take effect on an already-playing cue without a replay,
    // mirroring how the engine's raw out-point already updates live.
    bool         have_seq_cue     = false;
    audio::CueId seq_cue;
    double       seq_in_point     = 0.0;
    double       seq_out_point    = 0.0;
    double       seq_crossfade    = 0.0;
    double       seq_stop_fade    = 0.0;
    double       seq_file_duration = 0.0;
    bool         seq_sn_enabled   = false;
    double       seq_sn_time      = 0.0;
    bool         seq_sn_fade_out  = false;
    double       seq_fade_out_dur = 1.0;
    std::string  seq_end_action;
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
                    if (k == "busId") {
                        if (!it.contains(k) || it[k] != v) bus_changed = true;
                    }
                    it[k] = v;
                }
                touched = true;
                updated_item = &it;
            });
        if (touched && media_path_changed && updated_item &&
            updated_item->value("type", std::string{}) == "audio") {
            // The media file behind this item changed: retire the old cue and
            // queue a fresh decode on the loader thread. Doing it inline (via
            // mirror_items_to_engine_locked) meant the decode ran under mutex_
            // and blocked every other request for its duration (#43).
            auto old = item_uuid_to_cue_.find(uuid);
            if (old != item_uuid_to_cue_.end()) {
                engine_.unload_cue(old->second);
                cues_.erase(old->second.value);
                forget_primed_cue_locked(old->second);
                item_uuid_to_cue_.erase(old);
            }
            auto path = resolve_media_path(
                *updated_item, document_.value("folderPath", std::string{}));
            if (!path.empty())
                begin_item_load_locked(uuid, path, *updated_item);
        } else if (touched && media_path_changed) {
            // Media change on something that isn't a loadable audio item —
            // fall back to the full mirror (cheap: nothing to decode).
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
                    apply_item_fade_outs(*cue, it);
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

        // Collect who this assignment moves. An item carries its own routing;
        // a group carries it for every descendant that doesn't override it, so
        // re-assigning a group has to move all of them. resolve_item_bus works
        // the inheritance out per item, so this only needs the candidates.
        if (touched && bus_changed && updated_item) {
            if (updated_item->value("type", std::string{}) == "group") {
                std::function<void(const json&)> walk = [&](const json& arr) {
                    if (!arr.is_array()) return;
                    for (const auto& child : arr) {
                        if (!child.is_object()) continue;
                        if (child.value("type", std::string{}) == "audio") {
                            const auto u = child.value("uuid", std::string{});
                            if (!u.empty()) rerouted.push_back(u);
                        } else if (child.value("type", std::string{}) == "group" &&
                                   child.contains("children")) {
                            walk(child["children"]);
                        }
                    }
                };
                if (updated_item->contains("children")) walk((*updated_item)["children"]);
            } else {
                rerouted.push_back(uuid);
            }
        }

        // Snapshot the sequencing-relevant fields so the playing cue's
        // auto-advance/crossfade/stop-fade/loop timing can be refreshed live
        // (done below, after releasing mutex_, to keep the existing lock order).
        if (touched && updated_item) {
            auto cit = item_uuid_to_cue_.find(uuid);
            if (cit != item_uuid_to_cue_.end()) {
                have_seq_cue  = true;
                seq_cue       = cit->second;
                const json& it = *updated_item;
                seq_in_point  = json_get_or(it, "inPoint",  0.0);
                seq_out_point = json_get_or(it, "outPoint", 0.0);
                seq_crossfade = json_get_or(it, "crossFade", 0.0);
                seq_stop_fade = json_get_or(it, "stopFade",  0.0);
                seq_sn_enabled   = json_get_or(it, "startNextEnabled", false);
                seq_sn_time      = json_get_or(it, "startNextTime",    0.0);
                seq_sn_fade_out  = json_get_or(it, "startNextFadeOut", false);
                seq_fade_out_dur = json_get_or(it, "fadeOutDuration",  1.0);
                if (it.contains("endBehavior") && it["endBehavior"].is_object())
                    seq_end_action = json_get_or(it["endBehavior"], "action", std::string{});
                auto cm_it = cues_.find(cit->second.value);
                if (cm_it != cues_.end())
                    seq_file_duration = cm_it->second.duration_seconds;
            }
        }
    }

    // Live-apply loop + sequencer timing for an already-playing cue. The engine
    // out-point / fades were updated under the lock above; here we keep the
    // audio-thread loop state and the sequencer snapshot in sync so changes to
    // out point / crossfade / stop-fade / end-behaviour take effect immediately
    // (the sequencer loop only updates a matching entry, so this is a no-op when
    // the item isn't currently sequenced).
    if (have_seq_cue) {
        const bool looping = (seq_end_action == "loop");
        if (auto* pi = engine_.find_cue(seq_cue))
            pi->set_loop(looping, seq_in_point);
        const double effective_end = looping
            ? 0.0
            : ((seq_out_point > 0.0) ? seq_out_point : seq_file_duration);
        // Same arming rules as play_item: Start Next supersedes crossfade,
        // and neither applies to a looping cue.
        const bool sn_on = !looping && seq_sn_enabled && seq_sn_time > 0.0;
        std::lock_guard slock{sequencer_mutex_};
        for (auto& si : sequenced_items_) {
            if (si.uuid != uuid) continue;
            si.crossfade_sec = (looping || sn_on) ? 0.0 : seq_crossfade;
            si.stop_fade_sec = looping ? 0.0 : seq_stop_fade;
            si.start_next_time     = sn_on ? seq_sn_time : 0.0;
            si.start_next_fade_sec = (sn_on && seq_sn_fade_out)
                                         ? seq_fade_out_dur : 0.0;
            si.effective_end = effective_end;
            break;
        }
    }

    // A bus assignment now takes effect on the cue immediately, rather than
    // waiting for the next time it happens to be fired. It re-establishes LTC
    // itself, so this runs before the LTC branch and makes it redundant.
    if (!rerouted.empty()) {
        reroute_items_to_buses(rerouted);
        Logger::info("update_item: '{}' bus changed — re-routed {} cue(s) live",
                     uuid, rerouted.size());
    }

    // Route (or re-route) the LTC channel after releasing mutex_ so
    // ensure_ltc_routing() can safely acquire it.
    if (touched && ltc_changed && rerouted.empty())
        apply_ltc_output_routing();
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
            auto path = resolve_media_path(
                item, doc_ref.value("folderPath", std::string{}));
            if (!path.empty()) out = std::move(path);
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

// Resolve an index path (array of child indices) to a uuid. Mirrors the
// client's findItemByIndex: start at the top-level `items`, and at each level
// descend into the selected item's `children` when it is a group.
std::string ProjectState::resolve_index_path_locked(const std::vector<int>& path) const {
    if (path.empty()) return {};
    if (!document_.contains("items") || !document_["items"].is_array()) return {};

    const json* arr     = &document_["items"];
    const json* current = nullptr;
    for (int idx : path) {
        if (!arr || !arr->is_array()) return {};
        if (idx < 0 || idx >= static_cast<int>(arr->size())) return {};
        current = &(*arr)[static_cast<std::size_t>(idx)];
        if (!current->is_object()) return {};
        if (current->value("type", std::string{}) == "group" &&
            current->contains("children") && (*current)["children"].is_array()) {
            arr = &(*current)["children"];
        } else {
            arr = nullptr;   // leaf — any remaining path components are invalid
        }
    }
    return current ? current->value("uuid", std::string{}) : std::string{};
}

std::string ProjectState::first_playable_item_uuid_locked() const {
    if (!document_.contains("items") || !document_["items"].is_array()) return {};
    std::string result;
    std::function<void(const json&)> walk = [&](const json& arr) {
        if (!result.empty() || !arr.is_array()) return;
        for (const auto& it : arr) {
            if (!result.empty()) return;
            if (!it.is_object()) continue;
            if (it.value("type", std::string{}) == "audio") {
                result = it.value("uuid", std::string{});
                return;
            }
            if (it.value("type", std::string{}) == "group" && it.contains("children"))
                walk(it["children"]);
        }
    };
    walk(document_["items"]);
    return result;
}

std::vector<std::string> ProjectState::flat_item_uuids_locked() const {
    std::vector<std::string> out;
    if (!document_.contains("items") || !document_["items"].is_array()) return out;
    std::function<void(const json&)> walk = [&](const json& arr) {
        if (!arr.is_array()) return;
        for (const auto& it : arr) {
            if (!it.is_object()) continue;
            const auto uuid = it.value("uuid", std::string{});
            if (!uuid.empty()) out.push_back(uuid);
            if (it.value("type", std::string{}) == "group" && it.contains("children"))
                walk(it["children"]);
        }
    };
    walk(document_["items"]);
    return out;
}

// Public thread-safe wrapper: resolve an index path to an item uuid.
std::string ProjectState::item_uuid_by_index(const std::vector<int>& path) const {
    std::lock_guard lock{mutex_};
    return resolve_index_path_locked(path);
}

// ---------------------------------------------------------------------------
// Item-level transport with ducking + in/out point semantics.
// ---------------------------------------------------------------------------
bool ProjectState::play_item(const std::string& uuid,
                             double fade_in_override_sec,
                             const audio::CueId& exclude_from_ducking) {
  // Guard the whole body: a malformed item field must never throw uncaught
  // into the network layer. 
  try {
    // If this item was added moments ago its decode may still be running on the
    // loader thread (#43). Wait for THAT item only — every other request stays
    // responsive — and bound the wait so a wedged network share can't hang the
    // caller. Returns immediately when nothing is pending, which is the norm.
    wait_for_item_load(uuid, std::chrono::seconds{20});

    // Snapshot everything we need under the lock, then release before
    // touching the engine (engine calls take their own locks).
    std::string  ducking_mode  = "stop-all";
    float        duck_level    = 0.2f;
    double       in_point      = 0.0;
    double       out_point     = 0.0;
    double       fade_out_dur  = 1.0;
    double       crossfade_sec = 0.0;
    double       stop_fade_sec = 0.0;
    bool         start_next_enabled  = false;
    double       start_next_time     = 0.0;
    bool         start_next_fade_out = false;
    std::string  device_override;
    std::string  start_behavior_action;
    std::string  start_behavior_target_uuid;
    std::string  end_behavior_action;
    std::string  end_behavior_target_uuid;
    std::vector<int> end_behavior_target_index;
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
            in_point      = json_get_or(*found, "inPoint",         0.0);
            out_point     = json_get_or(*found, "outPoint",         0.0);
            fade_out_dur  = json_get_or(*found, "fadeOutDuration",  1.0);
            crossfade_sec = json_get_or(*found, "crossFade",        0.0);
            stop_fade_sec = json_get_or(*found, "stopFade",         0.0);
            start_next_enabled  = json_get_or(*found, "startNextEnabled",  false);
            start_next_time     = json_get_or(*found, "startNextTime",     0.0);
            start_next_fade_out = json_get_or(*found, "startNextFadeOut",  false);
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
                end_behavior_action      = eb.value("action",     std::string{});
                end_behavior_target_uuid = eb.value("targetUuid", std::string{});
                if (eb.contains("targetIndex") && eb["targetIndex"].is_array()) {
                    for (const auto& v : eb["targetIndex"]) {
                        if (v.is_number_integer())
                            end_behavior_target_index.push_back(v.get<int>());
                    }
                }
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
            // Crossfade: don't touch the outgoing cue — the sequencer already
            // started its engine-owned fade-out. Stopping it here would (since
            // it's FadingOut) route through stop_now() and hard-cut it.
            if (!exclude_from_ducking.empty() && cid == exclude_from_ducking) continue;
            if (auto* pi = engine_.find_cue(cid)) pi->stop(fade_ms);
        }
    } else if (ducking_mode == "duck-others") {
        const float lin = std::clamp(duck_level, 0.0f, 1.0f);
        const float db  = (lin <= 0.0001f) ? -120.0f : 20.0f * std::log10(lin);
        for (auto& cid : other_cues) {
            if (!exclude_from_ducking.empty() && cid == exclude_from_ducking) continue;
            if (auto* pi = engine_.find_cue(cid)) {
                ducks_made.push_back({cid, pi->gain_db()});
                pi->set_gain_db(db);
            }
        }
    }
    // "no-ducking" → do nothing.

    // Bus assignment is the whole of an item's routing. Everything resolves to
    // a bus — the master bus when nothing along the chain says otherwise — and the bus
    // decides where the audio goes. Where a project used to name an output
    // device, that is now the master bus's output, set on load.
    audio::MixerChannelId bus_mixer;
    {
        std::lock_guard lock{mutex_};
        bus_mixer = mixer_for_bus(resolve_item_bus(uuid));
    }
    if (!bus_mixer.empty()) {
        route_cue_to_mixer(target_cue, bus_mixer);
    } else if (!device_override.empty()) {
        const auto mixer = ensure_device_routing(device_override);
        if (!mixer.empty()) {
            route_cue_to_mixer(target_cue, mixer);
        } else {
            engine_.ensure_default_routing();
        }
    } else {
        // No per-item override: honour the project's default output device if
        // one is set, otherwise fall back to the engine's Main → platform-
        // default routing. Without this, a project that selected a non-default
        // output device was ignored at play time, because ensure_default_-
        // routing() re-pinned the cue to Main (the OS default device). (#30)
        std::string default_device;
        {
            std::lock_guard lock{mutex_};
            if (document_.contains("settings") && document_["settings"].is_object()) {
                const auto& s = document_["settings"];
                if (s.contains("defaultOutputDevice") && s["defaultOutputDevice"].is_string())
                    default_device = s["defaultOutputDevice"].get<std::string>();
            }
        }
        audio::MixerChannelId default_mixer;
        if (!default_device.empty()) default_mixer = ensure_device_routing(default_device);
        if (!default_mixer.empty()) {
            // route_cue_to_mixer() clears every prior route (incl. Main) first,
            // so the cue plays ONLY on the selected device.
            route_cue_to_mixer(target_cue, default_mixer);
        } else {
            // Drop any stale per-device override routes, then Main-route.
            engine_.unroute_item_from_all_mixers(target_cue);
            engine_.ensure_default_routing();
        }
    }

    // Re-establish LTC output routing after any audio routing changes above
    // may have disrupted it (unrouting all channels clears LTC routes too).
    apply_ltc_output_routing();

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
        pi->prime(audio::kPrimeSeconds, in_point);

        // Crossfade-in: fade the incoming cue up over the crossfade window
        // instead of using its own play-fade. play() captures the fade
        // duration synchronously inside start_fade(), so restoring the stored
        // value immediately after doesn't disturb the in-flight fade.
        const bool override_fade = fade_in_override_sec >= 0.0;
        const auto saved_fade_in = pi->desc().fade_in_duration;
        if (override_fade) {
            pi->set_fade_in(std::chrono::milliseconds{
                static_cast<long long>(fade_in_override_sec * 1000.0)});
        }
        pi->play();
        if (override_fade) pi->set_fade_in(saved_fade_in);
    } else {
        engine_.play(target_cue);  // logs the "no cue" warning path
    }

    // Stamp the item with a monotonic trigger sequence. Control surfaces read
    // this back from the state summary to answer "what did the operator fire
    // LAST?" — with several cues on air (a bed under a stinger, say), document
    // order is the wrong answer for a "currently playing" display.
    {
        std::lock_guard lock{mutex_};
        item_trigger_seq_[uuid] = ++trigger_seq_counter_;
    }

    // Register with the sequencer so it can handle end-behaviour, crossfade,
    // and stop-fade autonomously — even when the client is disconnected.
    {
        const bool looping = (end_behavior_action == "loop");
        // A looping cue plays forever (the audio thread seeks back to the
        // in-point on EOF/out-point). It must NOT arm the timing-based
        // auto-advance: otherwise, as the playhead nears effective_end, the
        // sequencer's crossfade/stop-fade triggers would fire and start the
        // *next* item — which is exactly the "loop behaves like play-next" bug.
        // effective_end = 0 disables all timing triggers while still letting
        // custom actions fire (they're checked before the effective_end gate).
        const double effective_end = looping
            ? 0.0
            : ((out_point > 0.0) ? out_point : file_duration);
        // Start Next supersedes crossfade — both would start the next item,
        // so arming them together would double-trigger it.
        const bool start_next_on =
            !looping && start_next_enabled && start_next_time > 0.0;
        SequencedItem si;
        si.uuid          = uuid;
        si.cue_id        = target_cue;
        si.crossfade_sec = (looping || start_next_on) ? 0.0 : crossfade_sec;
        si.stop_fade_sec = looping ? 0.0 : stop_fade_sec;
        si.start_next_time     = start_next_on ? start_next_time : 0.0;
        si.start_next_fade_sec = (start_next_on && start_next_fade_out)
                                     ? fade_out_dur : 0.0;
        si.effective_end = effective_end;
        si.end_action        = end_behavior_action;
        si.goto_target_uuid  = end_behavior_target_uuid;
        si.goto_target_index = end_behavior_target_index;
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
                if (auto* pi = engine_.find_cue(cue)) pi->prime();
            }).detach();
        }
    }

    // Playback just moved somewhere the server's own guess didn't predict, so
    // drop a stale auto-arming — the clients then fall back to the "Up Next"
    // derived from what is actually on air (e.g. the group child that follows
    // the one we just started) instead of showing the item armed before the
    // operator jumped. An arming the OPERATOR made is left alone: they chose it
    // deliberately and GO must still honour it.
    // Cart-only items are excluded: firing an SFX pad is not "moving the
    // playlist", and blanking the arming there would leave GO with nothing to
    // fire until the next cue ended.
    {
        bool stale = false;
        {
            std::lock_guard lock{mutex_};
            if (!next_item_override_.empty() &&
                !next_item_override_manual_ &&
                next_item_override_ != uuid) {
                std::function<bool(const json&)> in_playlist = [&](const json& arr) -> bool {
                    if (!arr.is_array()) return false;
                    for (const auto& it : arr) {
                        if (!it.is_object()) continue;
                        if (it.value("uuid", std::string{}) == uuid) return true;
                        if (it.value("type", std::string{}) == "group" &&
                            it.contains("children") && in_playlist(it["children"])) return true;
                    }
                    return false;
                };
                stale = document_.contains("items") && in_playlist(document_["items"]);
            }
        }
        if (stale) set_next_item_override("", /*manual=*/false);
    }

    // Handle start-behaviour immediately.
    if (start_behavior_action == "stop" && !start_behavior_target_uuid.empty()) {
        stop_item(start_behavior_target_uuid);
    } else if (start_behavior_action == "play" && !start_behavior_target_uuid.empty()) {
        play_item(start_behavior_target_uuid);
    }

    return true;
  } catch (const std::exception& e) {
    Logger::error("ProjectState::play_item('{}') failed: {}", uuid, e.what());
    return false;
  }
}

bool ProjectState::stop_item(const std::string& uuid, std::optional<long long> fade_ms) {
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

    if (fade_ms) engine_.stop(cue, std::chrono::milliseconds{std::max<long long>(0, *fade_ms)});
    else         engine_.stop(cue);

    // Server-authoritative "Up Next" arming: a manual stop of a cue with no end
    // behaviour advances the arming to the next sibling (but never wraps at the
    // end of the list — the operator is holding the show). (#28)
    arm_next_after_stop(uuid, /*was_manual=*/true);
    return true;
}

void ProjectState::stop_all_cues(std::optional<long long> fade_ms) {
    long long resolved_ms;
    if (fade_ms.has_value()) {
        resolved_ms = std::max<long long>(0, *fade_ms);
    } else {
        // Project-wide default fade for the Stop All button (default 1000 ms).
        long long setting_ms = 1000;
        {
            std::lock_guard lock{mutex_};
            if (document_.contains("settings") && document_["settings"].is_object()) {
                const auto& s = document_["settings"];
                if (s.contains("stopAllFadeMs") && s["stopAllFadeMs"].is_number())
                    setting_ms = static_cast<long long>(s["stopAllFadeMs"].get<double>());
            }
        }
        resolved_ms = std::max<long long>(0, setting_ms);
    }
    const auto fade = std::chrono::milliseconds{resolved_ms};
    bool stops_preview = true;
    audio::CueId preview;
    {
        std::lock_guard lock{mutex_};
        if (document_.contains("settings") && document_["settings"].is_object())
            stops_preview = document_["settings"].value("stopAllStopsPreview", true);
        preview = preview_cue_;
    }
    // Global fade always wins over any per-track fade-out (force_fade = true).
    // The audition is left out here either way: when it goes too it is faded
    // by stop_preview_if below, which is what also clears the preview state
    // and tells every client — the engine fade alone left the card stuck on
    // "previewing" (#60).
    engine_.stop_all(fade, /*force_fade=*/true, preview);
    if (stops_preview && !preview.empty()) stop_preview_if(preview, fade);
}

void ProjectState::set_next_item_override(const std::string& uuid, bool manual) {
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard lock{mutex_};
        // Re-arming the same uuid still needs to record the (possibly stronger)
        // provenance — an operator confirming the server's guess makes it
        // sticky — but must not re-broadcast.
        const bool same = (next_item_override_ == uuid);
        next_item_override_        = uuid;
        next_item_override_manual_ = uuid.empty() ? false : manual;
        if (same) return;
        cb = next_item_broadcaster_;
    }
    // Fan the change out to every connected client (server- or client-
    // initiated), outside the lock so the broadcast can't deadlock on mutex_.
    if (cb) cb(uuid);
}

void ProjectState::set_preview_stopped_broadcaster(std::function<void()> cb) {
    std::lock_guard lock{mutex_};
    preview_stopped_broadcaster_ = std::move(cb);
}

void ProjectState::set_next_item_broadcaster(std::function<void(const std::string&)> cb) {
    std::lock_guard lock{mutex_};
    next_item_broadcaster_ = std::move(cb);
}

std::string ProjectState::next_item_override() const {
    std::lock_guard lock{mutex_};
    return next_item_override_;
}

// ---------------------------------------------------------------------------
// Show-control surface: selection, Show Mode and locale. Server-owned so every
// client and control surface renders the same operator state. Each setter
// broadcasts outside the lock (the broadcaster re-enters the network layer,
// which must never happen while holding mutex_) and only when the value
// actually changed, so a client echoing a patch back can't loop.
// ---------------------------------------------------------------------------
void ProjectState::set_ui_state_broadcaster(std::function<void(const json&)> cb) {
    std::lock_guard lock{mutex_};
    ui_state_broadcaster_ = std::move(cb);
}

std::string ProjectState::selected_item_uuid() const {
    std::lock_guard lock{mutex_};
    return selected_item_uuid_;
}

bool ProjectState::set_selected_item(const std::string& uuid) {
    std::function<void(const json&)> cb;
    {
        std::lock_guard lock{mutex_};
        if (selected_item_uuid_ == uuid) return false;
        selected_item_uuid_ = uuid;
        cb = ui_state_broadcaster_;
    }
    if (cb) cb(json{{"type", "doc_patch"}, {"op", "selection_changed"}, {"itemUuid", uuid}});
    return true;
}

std::string ProjectState::step_selection(int delta,
                                         const std::vector<std::string>& anchor_candidates) {
    std::string target;
    {
        std::lock_guard lock{mutex_};
        const auto flat = flat_item_uuids_locked();
        if (flat.empty()) return {};

        auto it = std::find(flat.begin(), flat.end(), selected_item_uuid_);
        bool have_position = !selected_item_uuid_.empty() && it != flat.end();

        // Nothing selected (or a selection that no longer exists — the item was
        // deleted by another client). If the caller handed us anchors — the
        // items currently playing — step from the furthest one down the
        // playlist: an operator who has been firing cues without touching the
        // selection means "carry on from what I'm hearing", not "jump back to
        // the top of the show".
        if (!have_position && !anchor_candidates.empty()) {
            for (const auto& uuid : anchor_candidates) {
                auto anchor = std::find(flat.begin(), flat.end(), uuid);
                if (anchor == flat.end()) continue;
                if (!have_position || anchor > it) { it = anchor; have_position = true; }
            }
        }

        if (!have_position) {
            // No selection and nothing playing: start at the top.
            target = flat.front();
        } else {
            const auto idx = static_cast<long long>(std::distance(flat.begin(), it));
            const auto last = static_cast<long long>(flat.size()) - 1;
            // Clamp at both ends — the same "stop at the edge" behaviour as the
            // client's arrow keys. Wrapping would risk a blind operator holding
            // a button and silently landing back at the top of the show.
            target = flat[static_cast<std::size_t>(std::clamp(idx + delta, 0LL, last))];
        }
    }
    set_selected_item(target);
    return target;
}

bool ProjectState::show_mode() const {
    std::lock_guard lock{mutex_};
    return show_mode_;
}

void ProjectState::set_show_mode(bool enabled) {
    std::function<void(const json&)> cb;
    {
        std::lock_guard lock{mutex_};
        if (show_mode_ == enabled) return;
        show_mode_ = enabled;
        cb = ui_state_broadcaster_;
    }
    if (cb) cb(json{{"type", "doc_patch"}, {"op", "show_mode_changed"}, {"enabled", enabled}});
}

bool ProjectState::toggle_show_mode() {
    bool result;
    std::function<void(const json&)> cb;
    {
        std::lock_guard lock{mutex_};
        show_mode_ = !show_mode_;
        result = show_mode_;
        cb = ui_state_broadcaster_;
    }
    if (cb) cb(json{{"type", "doc_patch"}, {"op", "show_mode_changed"}, {"enabled", result}});
    return result;
}

std::string ProjectState::default_ui_locale() const {
    std::lock_guard lock{mutex_};
    return default_ui_locale_;
}

void ProjectState::set_default_ui_locale(const std::string& code) {
    std::lock_guard lock{mutex_};
    if (code.empty()) return;
    default_ui_locale_ = code;
}

// ---------------------------------------------------------------------------
// External-control surface (Bitfocus Companion, custom remotes).
// ---------------------------------------------------------------------------
static const char* transport_state_name(audio::TransportState t) {
    switch (t) {
        case audio::TransportState::Playing:   return "playing";
        case audio::TransportState::FadingIn:  return "fading_in";
        case audio::TransportState::FadingOut: return "fading_out";
        case audio::TransportState::Paused:    return "paused";
        default:                               return "stopped";
    }
}

json ProjectState::state_summary() const {
    // Doc-derived facts are snapshotted under mutex_ first; engine transport
    // is read afterwards (engine calls take their own locks) and the auto
    // "Up Next" derivation re-acquires mutex_ at the end. Never call into the
    // engine while holding mutex_ from here — play_item does the same dance.
    struct ItemFacts {
        std::string      name;
        std::string      color;            // "#RRGGBB" as authored in the client
        std::string      type;             // "audio" | "group" | "action"
        double           duration  = 0.0;  // full-file seconds (0 = unknown)
        double           in_point  = 0.0;
        double           out_point = 0.0;  // 0 = play to end
        std::vector<int> index;            // playlist index path; empty for cart-only items
    };
    std::unordered_map<std::string, ItemFacts> facts;
    // uuid → (cue, duration from decode metadata)
    std::vector<std::tuple<std::string, audio::CueId, double>> cue_pairs;
    std::string override_uuid;
    std::string selected_uuid;
    bool        show_mode_now = false;
    std::string locale_now;
    std::unordered_map<std::string, long long> trigger_seq;
    json project_block;
    json cart_bindings = json::array();

    {
        std::lock_guard lock{mutex_};

        const std::size_t item_count =
            (document_.contains("items") && document_["items"].is_array())
                ? document_["items"].size() : 0;
        project_block = json{
            {"name",           document_.value("name", "")},
            {"itemCount",      item_count},
            {"hasOpenProject", item_count > 0 || !project_file_path_.empty()},
            {"audioLoading",   loading_audio_.load(std::memory_order_acquire)},
        };

        // Walk the playlist tree recording every item's display facts and its
        // index path — the same path /api/transport/play_index accepts.
        std::function<void(const json&, std::vector<int>&)> walk;
        walk = [&](const json& arr, std::vector<int>& path) {
            if (!arr.is_array()) return;
            for (int i = 0; i < static_cast<int>(arr.size()); ++i) {
                const auto& it = arr[i];
                if (!it.is_object()) continue;
                path.push_back(i);
                const std::string uuid = it.value("uuid", std::string{});
                if (!uuid.empty()) {
                    ItemFacts f;
                    f.name      = it.value("displayName", std::string{});
                    f.color     = it.value("color", std::string{});
                    f.type      = it.value("type",  std::string{});
                    f.duration  = it.value("duration", 0.0);
                    f.in_point  = it.value("inPoint",  0.0);
                    f.out_point = it.value("outPoint", 0.0);
                    f.index     = path;
                    facts.emplace(uuid, std::move(f));
                }
                if (it.value("type", std::string{}) == "group" &&
                    it.contains("children")) {
                    walk(it["children"], path);
                }
                path.pop_back();
            }
        };
        std::vector<int> path;
        if (document_.contains("items")) walk(document_["items"], path);

        // Cart-only items live outside the playlist tree — record their names
        // so cart bindings resolve, but with no index path.
        if (document_.contains("cartOnlyItems") && document_["cartOnlyItems"].is_array()) {
            for (const auto& it : document_["cartOnlyItems"]) {
                if (!it.is_object()) continue;
                const std::string uuid = it.value("uuid", std::string{});
                if (uuid.empty() || facts.count(uuid)) continue;
                ItemFacts f;
                f.name      = it.value("displayName", std::string{});
                f.color     = it.value("color", std::string{});
                f.type      = it.value("type",  std::string{});
                f.duration  = it.value("duration", 0.0);
                f.in_point  = it.value("inPoint",  0.0);
                f.out_point = it.value("outPoint", 0.0);
                facts.emplace(uuid, std::move(f));
            }
        }

        cue_pairs.reserve(item_uuid_to_cue_.size());
        for (const auto& [uuid, cue] : item_uuid_to_cue_) {
            double duration = 0.0;
            if (auto it = cues_.find(cue.value); it != cues_.end())
                duration = it->second.duration_seconds;
            cue_pairs.emplace_back(uuid, cue, duration);
        }

        override_uuid = next_item_override_;
        selected_uuid = selected_item_uuid_;
        show_mode_now = show_mode_;
        locale_now    = default_ui_locale_;
        trigger_seq   = item_trigger_seq_;

        if (document_.contains("cartItems") && document_["cartItems"].is_array()) {
            for (const auto& c : document_["cartItems"]) {
                if (c.is_object()) cart_bindings.push_back(c);
            }
        }
    }

    // Engine pass: which cues are on air, and where are their playheads.
    struct OnAir {
        std::string           uuid;
        std::string           cue_id;
        audio::TransportState transport;
        double                playhead = 0.0;
        double                duration = 0.0;
    };
    std::vector<OnAir> on_air;
    for (const auto& [uuid, cue, duration] : cue_pairs) {
        auto* pi = engine_.find_cue(cue);
        if (!pi) continue;
        const auto s = pi->stats();
        if (s.transport == audio::TransportState::Stopped) continue;
        on_air.push_back(OnAir{uuid, cue.value, s.transport, s.playhead_seconds, duration});
    }
    // Document order (index path, lexicographic); cart-only items last.
    std::sort(on_air.begin(), on_air.end(), [&](const OnAir& a, const OnAir& b) {
        const auto& ia = facts[a.uuid].index;
        const auto& ib = facts[b.uuid].index;
        if (ia.empty() != ib.empty()) return ib.empty();
        return ia < ib;
    });

    json playing = json::array();
    for (const auto& e : on_air) {
        const auto& f = facts[e.uuid];
        // Effective bounds honour inPoint / outPoint trims the same way the
        // client's transport display does.
        const double duration  = (f.duration > 0.0) ? f.duration : e.duration;
        const double end       = (f.out_point > f.in_point) ? f.out_point
                                : (duration > 0.0 ? duration : e.playhead);
        const double elapsed   = std::max(0.0, e.playhead - f.in_point);
        const double eff_dur   = std::max(0.0, end - f.in_point);
        json entry{
            {"itemUuid",     e.uuid},
            {"cueId",        e.cue_id},
            {"name",         f.name},
            {"color",        f.color},
            {"transport",    transport_state_name(e.transport)},
            {"paused",       e.transport == audio::TransportState::Paused},
            {"playheadSec",  e.playhead},
            {"elapsedSec",   elapsed},
            {"durationSec",  eff_dur},
            {"remainingSec", std::max(0.0, eff_dur - elapsed)},
        };
        if (!f.index.empty()) entry["index"] = f.index;
        // Firing order, so a control surface can show the cue the operator
        // triggered last rather than the topmost one in the playlist.
        if (auto ts = trigger_seq.find(e.uuid); ts != trigger_seq.end())
            entry["triggerSeq"] = ts->second;
        playing.push_back(std::move(entry));
    }

    // Effective "Up Next": user override first, else derived from the
    // currently-playing item's endBehavior (client GO-button parity).
    std::string next_uuid   = override_uuid;
    std::string next_source = next_uuid.empty() ? "" : "override";
    if (next_uuid.empty() && !on_air.empty()) {
        std::lock_guard lock{mutex_};
        for (const auto& e : on_air) {
            const auto n = resolve_next_item_locked(e.uuid);
            if (!n.empty()) { next_uuid = n; next_source = "auto"; break; }
        }
    }
    json next = nullptr;
    if (!next_uuid.empty()) {
        next = json{{"itemUuid", next_uuid}, {"source", next_source}};
        if (auto it = facts.find(next_uuid); it != facts.end()) {
            next["name"]  = it->second.name;
            next["color"] = it->second.color;
            next["type"]  = it->second.type;
            if (!it->second.index.empty()) next["index"] = it->second.index;
        }
    }

    // Selected playlist item. Reported even when the uuid has gone stale (the
    // item was deleted by another client) so a surface can tell "selection
    // points nowhere" apart from "nothing is selected".
    json selection = nullptr;
    if (!selected_uuid.empty()) {
        selection = json{{"itemUuid", selected_uuid}};
        if (auto it = facts.find(selected_uuid); it != facts.end()) {
            selection["name"]  = it->second.name;
            selection["color"] = it->second.color;
            selection["type"]  = it->second.type;
            if (!it->second.index.empty()) selection["index"] = it->second.index;
        }
        selection["onAir"] = std::any_of(on_air.begin(), on_air.end(),
                                         [&](const OnAir& e){ return e.uuid == selected_uuid; });
    }

    // Cart bindings, decorated with the bound item's name, colour + live state.
    json cart = json::array();
    for (const auto& c : cart_bindings) {
        const int slot = c.value("slot", -1);
        const std::string uuid = c.value("itemUuid", std::string{});
        if (slot < 0 || uuid.empty()) continue;
        json entry{{"slot", slot}, {"itemUuid", uuid}};
        if (auto it = facts.find(uuid); it != facts.end()) {
            entry["name"]  = it->second.name;
            entry["color"] = it->second.color;
        }
        entry["playing"] = std::any_of(on_air.begin(), on_air.end(),
                                       [&](const OnAir& e){ return e.uuid == uuid; });
        cart.push_back(std::move(entry));
    }

    // Buses: names, levels and flags only, for Companion-style control
    // surfaces (D23: meters ride the separate `meters` WS broadcast, not
    // this snapshot). Deliberately compact — no `dsp` block, no
    // `itemUuids`; a controller wants "what is it called and what state is
    // it in", not the mixer's internals.
    json buses_arr = json::array();
    for (const auto& b : list_buses()) {
        const char* kind = b.def.output_kind == BusOutputKind::Bus ? "bus" : "output";
        json entry{
            {"id",      b.def.id},
            {"name",    b.def.display_name},
            {"color",   b.def.color},
            {"order",   b.def.order},
            {"width",   b.def.width},
            {"gainDb",  b.def.gain_db},
            {"mute",    b.def.muted},
            {"pfl",     b.pfl},
            {"bound",   b.bound},
            {"master",  b.def.master},
            {"preview", b.def.preview},
            {"masters", b.masters ? json{b.masters->first, b.masters->second} : json{}},
            {"output",  json{{"type", kind}, {"target", b.def.output_target}}},
        };
        if (b.def.preview) entry["monoCheck"] = b.mono_check;
        buses_arr.push_back(std::move(entry));
    }

    return json{
        {"buses",   std::move(buses_arr)},
        {"project", std::move(project_block)},
        {"playing", std::move(playing)},
        {"next",    std::move(next)},
        {"selection", std::move(selection)},
        {"ui", json{
            {"showMode", show_mode_now},
            {"locale",   locale_now},
        }},
        {"master", json{
            {"gainDb",         engine_.master_gain_db()},
            {"limiterEnabled", engine_.limiter_enabled()},
        }},
        {"cart",    std::move(cart)},
        {"preview", json{
            {"active",   !current_preview_item_uuid().empty()},
            {"itemUuid", current_preview_item_uuid()},
        }},
    };
}

std::string ProjectState::go() {
    std::string target;
    {
        std::lock_guard lock{mutex_};
        target = next_item_override_;
    }
    const bool from_override = !target.empty();

    if (target.empty()) {
        // No override armed — derive from the currently-playing item's
        // endBehavior, the same fallback the client's GO button uses.
        std::vector<std::pair<std::string, audio::CueId>> pairs;
        {
            std::lock_guard lock{mutex_};
            pairs.reserve(item_uuid_to_cue_.size());
            for (const auto& [u, c] : item_uuid_to_cue_) pairs.emplace_back(u, c);
        }
        std::vector<std::string> on_air;
        for (const auto& [u, c] : pairs) {
            if (auto* pi = engine_.find_cue(c)) {
                if (pi->stats().transport != audio::TransportState::Stopped)
                    on_air.push_back(u);
            }
        }
        {
            std::lock_guard lock{mutex_};
            for (const auto& u : on_air) {
                const auto n = resolve_next_item_locked(u);
                if (!n.empty()) { target = n; break; }
            }
        }
    }

    if (target.empty()) return {};
    if (!trigger_item(target)) return {};
    // Consume the override only after a successful trigger so a GO against a
    // not-yet-loaded item doesn't silently disarm the operator's choice.
    if (from_override) set_next_item_override("");
    return target;
}

std::string ProjectState::cart_slot_item_uuid(int slot) const {
    std::lock_guard lock{mutex_};
    if (!document_.contains("cartItems") || !document_["cartItems"].is_array())
        return {};
    for (const auto& c : document_["cartItems"]) {
        if (c.is_object() && c.value("slot", -1) == slot)
            return c.value("itemUuid", std::string{});
    }
    return {};
}

// Dispatch by item type: audio → play_item; group → walk startBehavior
// (play-first plays the first child recursively; play-all triggers every
// child). Mirrors the client's triggerGroup() so auto-next / Up Next
// override / goto-item behave consistently when the target is a group.
bool ProjectState::trigger_item(const std::string& uuid,
                                double fade_in_override_sec,
                                const audio::CueId& exclude_from_ducking) {
  // Guard the whole body: a malformed item field must never throw uncaught
  // into the network layer. 
  try {
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
        return play_item(uuid, fade_in_override_sec, exclude_from_ducking);
    }
    if (type == "group") {
        if (child_uuids.empty()) return false;
        // A group that was itself armed as "Up Next" is being started now, so
        // the arming is spent. play_item() below only ever sees the CHILD uuid,
        // so it can't recognise the group and would leave the arming standing —
        // which then blocked the group's 2nd child from being armed when its
        // 1st finished.
        {
            bool armed_here = false;
            {
                std::lock_guard lock{mutex_};
                armed_here = (next_item_override_ == uuid);
            }
            if (armed_here) set_next_item_override("", /*manual=*/false);
        }
        if (start_action == "play-all") {
            bool any = false;
            for (const auto& cu : child_uuids) {
                if (trigger_item(cu, fade_in_override_sec, exclude_from_ducking)) any = true;
            }
            return any;
        }
        // Default / "play-first": trigger only the first child.
        return trigger_item(child_uuids.front(), fade_in_override_sec, exclude_from_ducking);
    }
    return false;
  } catch (const std::exception& e) {
    Logger::error("ProjectState::trigger_item('{}') failed: {}", uuid, e.what());
    return false;
  }
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

    // Create the strip before reserving the master pair. A refused strip then
    // costs nothing to unwind, whereas reserving first would strand the pair —
    // next_override_master_ only ever moves upward within a project.
    const auto mixer = engine_.create_mixer_channel(
        "Output: " + device_name);
    if (mixer.empty()) {
        Logger::error("ensure_device_routing: no mixer strip available for '{}'; "
                      "item will use default routing", device_name);
        return {};
    }

    audio::MasterChannelIndex master_l = 0;
    audio::MasterChannelIndex master_r = 0;
    bool exhausted = false;
    {
        std::lock_guard lock{mutex_};
        // Bound-check master allocation. Each distinct device override consumes
        // a pair of master channels growing upward from next_override_master_,
        // while the top two channels of the bus are reserved for preview.
        // Never allocate into or past that reserve — doing so would collide with
        // preview output or run past the engine's master bus width.
        if (!allocate_master_pair_locked(master_l, master_r)) {
            Logger::error(
                "ensure_device_routing: out of master channels for '{}' "
                "(next={}, bus_width={}); item will use default routing "
                "instead of a dedicated device master",
                device_name, next_override_master_,
                engine_.config().master_channels);
            exhausted = true;
        }
    }
    if (exhausted) {
        engine_.remove_mixer_channel(mixer);   // engine takes its own lock
        return {};
    }

    engine_.assign_master_to_device(master_l, dev, 0);
    engine_.assign_master_to_device(master_r, dev, 1);
    engine_.route_mixer_to_master(mixer, master_l, 0.0f, 0);   // strip L lane
    engine_.route_mixer_to_master(mixer, master_r, 0.0f, 1);   // strip R lane

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

void ProjectState::reroute_items_to_buses(const std::vector<std::string>& item_uuids) {
    // Resolve everything under the lock, route with it released: route_cue_to_-
    // mixer calls into the engine, which takes its own.
    std::vector<std::pair<audio::CueId, audio::MixerChannelId>> moves;
    {
        std::lock_guard lock{mutex_};
        for (const auto& uuid : item_uuids) {
            const auto cit = item_uuid_to_cue_.find(uuid);
            if (cit == item_uuid_to_cue_.end()) continue;   // not loaded; play_item will route it
            const auto mixer = mixer_for_bus(resolve_item_bus(uuid));
            if (mixer.empty()) continue;
            moves.emplace_back(cit->second, mixer);
        }
    }

    // Moving a cue between buses is a re-patch, not a fade. It is seamless in
    // the ordinary case — the samples simply arrive at the master through a
    // different accumulator, at the same gain — and audibly abrupt only when
    // the two buses differ in level, mute or output, which is precisely the
    // change the operator just asked for.
    for (const auto& [cue, mixer] : moves) route_cue_to_mixer(cue, mixer);

    // route_cue_to_mixer drops every item->mixer route this cue had, the
    // synthetic LTC channel included, so timecode has to be re-established or
    // re-assigning the bus of an LTC-enabled cue would silently kill its
    // output. Same call play_item makes straight after routing.
    if (!moves.empty()) apply_ltc_output_routing();
}

void ProjectState::route_cue_to_mixer(const audio::CueId& cue,
                                      const audio::MixerChannelId& mixer) {
    auto* pi = engine_.find_cue(cue);
    if (!pi) return;
    const auto src_count = pi->source_channel_count();
    // The LTC synthetic channel is always the LAST source channel — it must
    // never feed the audible mixer (it gets its own output routing from
    // apply_ltc_output_routing()). Only the real audio channels route here.
    const audio::ChannelCount audio_count =
        (pi->desc().ltc_enabled && src_count > 0) ? src_count - 1 : src_count;

    // Drop ALL prior item-to-mixer routes for this cue — including the engine's
    // auto-created "Main" mixer, which ProjectState does NOT track by id. The
    // previous implementation only unrouted from mixers we knew about (mixers_
    // + device_routings_), so a cue pinned to a specific output device stayed
    // routed to Main → the platform-default device as well and played out of
    // both. (The LTC synthetic channel is re-established by the
    // apply_ltc_output_routing() call that follows routing in play_item.)
    engine_.unroute_item_from_all_mixers(cue);

    if (audio_count == 1) {
        // Mono cue: fan the single channel across both strip lanes (centre).
        engine_.route_item_source_to_mixer(cue, 0, mixer, 0.0f,
                                           audio::kAllMixerLanes);
    } else {
        // Stereo (or wider): L → lane 0, R → lane 1, preserving the image.
        for (audio::ChannelIndex c = 0;
             c < std::min<audio::ChannelCount>(audio::kMixerLanes, audio_count); ++c) {
            engine_.route_item_source_to_mixer(cue, c, mixer, 0.0f, c);
        }
    }
}

// ---------------------------------------------------------------------------
// Preview routing — independent playback of a cue through the configured
// preview device, used for DJ-style pre-listening. The infrastructure
// (device + mixer + master assignments) is set up lazily on first preview
// and reused for subsequent ones.
// ---------------------------------------------------------------------------
// Unwire every per-device override routing and release its master pair.
//
// These live only in device_routings_ — the engine knows about the mixer strip
// and the master assignments, but nothing else tracks them. Dropping the map
// alone would strand the strips and permanently consume master channels:
// next_override_master_ only ever moves upward, so switching projects enough
// times in one session exhausts the bus and every subsequent device override
// silently falls back to default routing.
//
// Devices themselves are left open on purpose. Reopening one is the expensive
// part (which is why preview caches its device), and the engine reuses open
// devices by name, so nothing is gained by closing them here.
void ProjectState::release_device_routings_locked() {
    for (auto& [name, dr] : device_routings_) {
        // remove_mixer_channel() also drops the strip's mixer->master sends and
        // any item->mixer sends pointing at it, so only the master->device
        // assignments need clearing explicitly.
        engine_.remove_mixer_channel(dr.mixer);
        engine_.clear_master_assignment(dr.master_l);
        engine_.clear_master_assignment(dr.master_r);
    }
    if (!device_routings_.empty()) {
        Logger::debug("released {} device-override routing(s)", device_routings_.size());
    }
    device_routings_.clear();
    next_override_master_ = kFirstOverrideMaster;
}

// ---------------------------------------------------------------------------
// Buses
//
// A bus is the user-facing name for an engine mixer strip. Items and groups
// carry a busId and nothing else about routing; the bus decides where the
// audio goes. Definitions live in document_["buses"] and are materialised onto
// engine strips whenever a project loads.
// ---------------------------------------------------------------------------
namespace {
} // namespace

// Declared in the header: the control server serialises buses too.
void merge_bus_dsp(const json& src, BusDsp& out) {
    if (!src.is_object()) return;
    out.eq_enabled  = src.value("eqEnabled",  out.eq_enabled);
    out.dyn_enabled = src.value("dynEnabled", out.dyn_enabled);
    const auto filter = [](const json& f, BusFilter& o) {
        if (!f.is_object()) return;
        o.freq_hz = f.value("freq", o.freq_hz);
        o.q       = std::clamp(f.value("q", o.q), 0.1f, 40.0f);
    };
    if (src.contains("hpf")) filter(src["hpf"], out.hpf);
    if (src.contains("lpf")) filter(src["lpf"], out.lpf);
    if (src.contains("gate") && src["gate"].is_object()) {
        const auto& g = src["gate"];
        auto& o = out.gate;
        out.gate_on   = g.value("on", out.gate_on);
        o.threshold_db = std::clamp(g.value("threshold", o.threshold_db), -80.0f, 0.0f);
        o.ratio        = std::clamp(g.value("ratio",     o.ratio),         1.0f, 20.0f);
        o.range_db     = std::clamp(g.value("range",     o.range_db),    -80.0f, 0.0f);
        o.attack_ms    = std::clamp(g.value("attack",    o.attack_ms),     0.1f, 100.0f);
        o.hold_ms      = std::clamp(g.value("hold",      o.hold_ms),       0.0f, 1000.0f);
        o.release_ms   = std::clamp(g.value("release",   o.release_ms),    5.0f, 5000.0f);
    }
    if (src.contains("comp") && src["comp"].is_object()) {
        const auto& k = src["comp"];
        auto& o = out.comp;
        out.comp_on   = k.value("on", out.comp_on);
        // Ranges match the surface's knobs. The ratio runs to 60:1 rather than
        // the gate's 20 because the top of this control is meant to be a
        // limiter setting, not a heavier compressor.
        o.threshold_db = std::clamp(k.value("threshold", o.threshold_db), -60.0f, 0.0f);
        o.ratio        = std::clamp(k.value("ratio",     o.ratio),          1.0f, 60.0f);
        o.makeup_db    = std::clamp(k.value("makeup",    o.makeup_db),    -12.0f, 24.0f);
        o.attack_ms    = std::clamp(k.value("attack",    o.attack_ms),      0.1f, 300.0f);
        o.knee_db      = std::clamp(k.value("knee",      o.knee_db),        0.0f, 24.0f);
        o.release_ms   = std::clamp(k.value("release",   o.release_ms),     5.0f, 5000.0f);
    }
    if (src.contains("width") && src["width"].is_object()) {
        const auto& w = src["width"];
        auto& o = out.width;
        // 2.0 is the conventional top of a width control: past it the phantom
        // centre is so far down that lead material sounds hollow, and the mono
        // sum starts losing it altogether.
        o.width        = std::clamp(w.value("width",      o.width),        0.0f, 2.0f);
        o.bass_mono_hz = std::clamp(w.value("bassMonoHz", o.bass_mono_hz), 20.0f, 500.0f);
        o.bass_mono_q  = std::clamp(w.value("bassMonoQ",  o.bass_mono_q),   0.1f, 4.0f);
    }
    if (src.contains("eq") && src["eq"].is_array()) {
        const auto& arr = src["eq"];
        for (std::size_t i = 0; i < kBusEqBands && i < arr.size(); ++i) {
            if (!arr[i].is_object()) continue;
            auto& b = out.eq[i];
            b.freq_hz = arr[i].value("freq", b.freq_hz);
            b.gain_db = std::clamp(arr[i].value("gain", b.gain_db), -24.0f, 24.0f);
            b.q       = std::clamp(arr[i].value("q", b.q), 0.1f, 40.0f);
            // Only the outer bands can be shelves. Refused here rather than
            // ignored downstream, so what the document says and what the desk
            // does cannot drift apart.
            b.shelf   = (i == 0 || i == kBusEqBands - 1) &&
                        arr[i].value("shelf", b.shelf);
            // Past 2 a shelf overshoots into a resonant peak at the corner,
            // which is not what the control means; the engine clamps too.
            b.slope   = std::clamp(arr[i].value("slope", b.slope), 0.1f, 2.0f);
        }
    }
}

json bus_dsp_to_json(const BusDsp& d) {
    json eq = json::array();
    for (const auto& b : d.eq) {
        eq.push_back(json{{"freq",  b.freq_hz}, {"gain",  b.gain_db}, {"q", b.q},
                          {"shelf", b.shelf},   {"slope", b.slope}});
    }
    return json{
        {"eqEnabled",  d.eq_enabled},
        {"dynEnabled", d.dyn_enabled},
        {"hpf", json{{"freq", d.hpf.freq_hz}, {"q", d.hpf.q}}},
        {"lpf", json{{"freq", d.lpf.freq_hz}, {"q", d.lpf.q}}},
        {"eq",  std::move(eq)},
        {"gate", json{
            {"on",        d.gate_on},
            {"threshold", d.gate.threshold_db},
            {"ratio",     d.gate.ratio},
            {"range",     d.gate.range_db},
            {"attack",    d.gate.attack_ms},
            {"hold",      d.gate.hold_ms},
            {"release",   d.gate.release_ms},
        }},
        {"comp", json{
            {"on",        d.comp_on},
            {"threshold", d.comp.threshold_db},
            {"ratio",     d.comp.ratio},
            {"makeup",    d.comp.makeup_db},
            {"attack",    d.comp.attack_ms},
            {"knee",      d.comp.knee_db},
            {"release",   d.comp.release_ms},
        }},
        {"width", json{
            {"width",      d.width.width},
            {"bassMonoHz", d.width.bass_mono_hz},
            {"bassMonoQ",  d.width.bass_mono_q},
        }},
    };
}

namespace {

// Human-readable, stable-ish bus id derived from a name, so the document stays
// legible instead of carrying opaque uuids. Uniqueness is the caller's problem.
std::string bus_id_from_name(const std::string& name) {
    std::string id;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) id += static_cast<char>(std::tolower(c));
        else if (!id.empty() && id.back() != '-')        id += '-';
    }
    while (!id.empty() && id.back() == '-') id.pop_back();
    return id.empty() ? std::string{"bus"} : id;
}
}  // namespace

// Parse an output spec as the API and the document carry it. "master" is the
// retired kind (D25): accepted for one release and mapped to a bus→bus send
// into the master-role bus, whose id the caller supplies because at load time
// the roles are settled after every bus has been read.
namespace {
struct ParsedOutput {
    BusOutputKind kind = BusOutputKind::Bus;
    std::string   target;
    bool          legacy_master = false;   // the word "master" was used
};
ParsedOutput parse_output_spec(const json& out) {
    ParsedOutput p;
    const auto kind = out.value("type", std::string{"master"});
    p.target = out.value("target", std::string{});
    if (kind == "bus")         { p.kind = BusOutputKind::Bus;    return p; }
    if (kind == "output")      { p.kind = BusOutputKind::Output; return p; }
    // "master", or anything unrecognised: the old default.
    p.kind          = BusOutputKind::Bus;
    p.target.clear();
    p.legacy_master = true;
    return p;
}
}  // namespace

const BusDef* ProjectState::master_bus_locked() const {
    for (const auto& b : buses_) if (b.master) return &b;
    return nullptr;
}

const BusDef* ProjectState::preview_bus_locked() const {
    for (const auto& b : buses_) if (b.preview) return &b;
    return nullptr;
}

std::string ProjectState::master_bus_id_locked() const {
    const BusDef* b = master_bus_locked();
    return b ? b->id : std::string{};
}

std::string ProjectState::preview_bus_id_locked() const {
    const BusDef* b = preview_bus_locked();
    return b ? b->id : std::string{};
}

void ProjectState::load_buses_locked() {
    // What this load has to invent on the operator's behalf (D12). Reset on
    // every load so an already-migrated document reports nothing.
    BusMigrationSummary summary;
    // A document with no `buses` key predates buses entirely: nothing in it
    // says where anything goes, so everything in it lands on the master bus
    // (D1). One with a key but a schema below 2 comes from round 1: it has
    // buses but no roles, and may carry the retired "master" output kind.
    const bool had_buses_key = document_.contains("buses") &&
                               document_["buses"].is_array();
    int schema = 0;
    if (document_.contains("busSchema") && document_["busSchema"].is_number())
        schema = document_["busSchema"].get<int>();

    buses_.clear();
    // The bus list is about to change, so anything previously reported as
    // unknown is worth reporting again if it is still unknown afterwards.
    warned_unknown_buses_.clear();
    std::unordered_set<std::string> seen;
    // Buses whose output was written as the retired "master" kind, or left
    // out entirely (which meant the same thing). Settled once the master
    // role is known.
    std::unordered_set<std::string> legacy_master_kind;

    if (had_buses_key) {
        for (const auto& b : document_["buses"]) {
            if (!b.is_object()) continue;
            BusDef d;
            d.id = b.value("id", std::string{});
            if (d.id.empty() || seen.count(d.id)) continue;   // drop junk / dupes
            d.display_name = b.value("name", d.id);
            d.color        = b.value("color", std::string{});
            d.order        = b.value("order", 0);
            d.width        = std::clamp(b.value("width", 2), 1, 2);
            d.gain_db      = b.value("gainDb", 0.0f);
            d.muted        = b.value("mute", false);
            d.pan          = std::clamp(b.value("pan", 0.0f), -1.0f, 1.0f);
            d.master       = b.value("master", false);
            d.preview      = b.value("preview", false);
            if (b.contains("dsp")) merge_bus_dsp(b["dsp"], d.dsp);
            ParsedOutput out;
            if (b.contains("output") && b["output"].is_object()) {
                out = parse_output_spec(b["output"]);
            } else {
                out.legacy_master = true;
            }
            d.output_kind   = out.kind;
            d.output_target = out.target;
            if (out.legacy_master) legacy_master_kind.insert(d.id);
            seen.insert(d.id);
            buses_.push_back(std::move(d));
        }
    }

    const auto find = [&](const std::string& id) -> BusDef* {
        for (auto& b : buses_) if (b.id == id) return &b;
        return nullptr;
    };
    const auto by_order = [](const BusDef& a, const BusDef& b) { return a.order < b.order; };

    // ---- Roles (D24 / D34 / D35) ----------------------------------------
    // A round-1 document has no role flags; its "main" and "monitor" buses
    // ARE the holders, and get the new stock names only if they still carry
    // the old ones — a renamed bus stays renamed.
    if (had_buses_key && schema < kBusSchemaVersion) {
        if (BusDef* m = find("main")) {
            if (!m->master) { m->master = true; summary.roles_migrated = true; }
            if (m->display_name == "Main") m->display_name = "Master";
        }
        if (BusDef* p = find("monitor")) {
            if (!p->preview) { p->preview = true; summary.roles_migrated = true; }
            if (p->display_name == "Monitor") p->display_name = "Preview";
        }
    }
    // Exactly one of each, never the same bus. Duplicates can only come from
    // a hand-edited document; the first by order keeps the role. A bus
    // claiming both keeps master — the house matters more than the phones —
    // and a preview holder is found or made below.
    std::stable_sort(buses_.begin(), buses_.end(), by_order);
    {
        bool have_master = false;
        for (auto& b : buses_) {
            if (!b.master) continue;
            if (have_master) {
                Logger::warn("bus '{}' also claims the master role; '{}' keeps it",
                             b.display_name, master_bus_locked()->display_name);
                b.master = false;
                summary.roles_migrated = true;
            }
            have_master = true;
            if (b.preview) {
                Logger::warn("bus '{}' claims both roles; it keeps master", b.display_name);
                b.preview = false;
                summary.roles_migrated = true;
            }
        }
        bool have_preview = false;
        for (auto& b : buses_) {
            if (!b.preview) continue;
            if (have_preview) {
                Logger::warn("bus '{}' also claims the preview role; '{}' keeps it",
                             b.display_name, preview_bus_locked()->display_name);
                b.preview = false;
                summary.roles_migrated = true;
            }
            have_preview = true;
        }
    }
    // No master holder: the first Output-kind bus by order is promoted, else
    // one is synthesised. A document with no buses at all is not being
    // migrated — it is getting the defaults — so only a document that brought
    // buses and lacked a holder counts.
    bool master_synthesised = false;
    if (!master_bus_locked()) {
        BusDef* promoted = nullptr;
        for (auto& b : buses_) {
            if (b.preview || b.output_kind != BusOutputKind::Output) continue;
            promoted = &b;
            break;
        }
        if (promoted) {
            promoted->master = true;
            summary.roles_migrated = true;
            Logger::warn("no bus carries the master role; promoting '{}' (it sends to '{}')",
                         promoted->display_name, promoted->output_target);
        } else {
            BusDef d;
            d.id            = kMasterBusId;
            for (int n = 2; find(d.id); ++n) d.id = std::string{kMasterBusId} + "-" + std::to_string(n);
            d.display_name  = "Master";
            d.order         = kMasterBusOrder;
            d.master        = true;
            d.output_kind   = BusOutputKind::Output;
            d.output_target = kMainOutputName;
            buses_.push_back(std::move(d));
            master_synthesised = true;
            if (had_buses_key) {
                summary.roles_migrated = true;
                Logger::warn("no bus carries the master role and none sends to an output; "
                             "synthesised '{}' -> '{}'", buses_.back().display_name, kMainOutputName);
            }
        }
    }
    if (!preview_bus_locked()) {
        BusDef d;
        d.id            = kPreviewBusId;
        for (int n = 2; find(d.id); ++n) d.id = std::string{kPreviewBusId} + "-" + std::to_string(n);
        d.display_name  = "Preview";
        d.order         = kPreviewBusOrder;
        d.preview       = true;
        d.output_kind   = BusOutputKind::Output;
        d.output_target = kPreviewOutputName;
        buses_.push_back(std::move(d));
        if (had_buses_key) {
            summary.roles_migrated = true;
            Logger::warn("no bus carries the preview role; synthesised '{}' -> '{}'",
                         buses_.back().display_name, kPreviewOutputName);
        }
    }
    const std::string master_id  = master_bus_id_locked();
    const std::string preview_id = preview_bus_id_locked();

    // ---- settings.defaultOutputDevice → the master bus -------------------
    // A project that named a default output device gets it moved onto the
    // master bus, as a logical output. Because an unmapped name resolves back
    // to a device of the same name, the audio keeps coming out of the same
    // place — but the project no longer decides which hardware that is, which
    // is the point: output binding belongs to the machine, not the show.
    // Only when the master bus does not already say where it goes.
    if (document_.contains("settings") && document_["settings"].is_object()) {
        auto& settings = document_["settings"];
        if (settings.contains("defaultOutputDevice") &&
            settings["defaultOutputDevice"].is_string()) {
            const auto device = settings["defaultOutputDevice"].get<std::string>();
            settings.erase("defaultOutputDevice");
            if (!device.empty()) {
                // A master that was the old Master kind, or one synthesised
                // just now for a pre-bus document (which is exactly where a
                // defaultOutputDevice comes from): the device name becomes
                // its target, so audio keeps coming out of the same place.
                if (BusDef* m = find(master_id);
                    m && (legacy_master_kind.count(m->id) || master_synthesised)) {
                    m->output_kind   = BusOutputKind::Output;
                    m->output_target = device;
                    legacy_master_kind.erase(m->id);
                    summary.main_output_migrated = true;
                    Logger::warn("migrated settings.defaultOutputDevice '{}' onto the master bus",
                                 device);
                }
            }
        }
    }

    // ---- The retired "master" output kind (D25) ---------------------------
    // A user bus that went "to master" now feeds the master bus as a bus→bus
    // send; the master bus itself, which used to BE the master, sends to the
    // built-in Main Out. Both are lossless: the audio lands where it did.
    for (auto& b : buses_) {
        if (!legacy_master_kind.count(b.id)) continue;
        if (b.id == master_id) {
            b.output_kind   = BusOutputKind::Output;
            b.output_target = kMainOutputName;
        } else {
            b.output_kind   = BusOutputKind::Bus;
            b.output_target = master_id;
        }
        // Only a document from the bus era is being migrated; the defaults
        // for a pre-bus or empty document are just the defaults.
        if (had_buses_key && schema < kBusSchemaVersion) summary.roles_migrated = true;
    }
    // A bus→bus send with no target meant "to master" too (the API never
    // writes one; a hand-edited document might).
    for (auto& b : buses_) {
        if (b.id != master_id && b.output_kind == BusOutputKind::Bus && b.output_target.empty())
            b.output_target = master_id;
    }

    // ---- settings.previewDevice → the preview bus (D28) ------------------
    // The one device name the document still carried. It becomes the preview
    // bus's target unless that target is already mapped — a real mapping is
    // the portable answer — and the key goes either way, so nothing reads it
    // after load. A device that is not on this machine leaves the preview
    // bus valid and silent, as any unresolvable preview target does.
    if (document_.contains("settings") && document_["settings"].is_object()) {
        auto& settings = document_["settings"];
        if (settings.contains("previewDevice")) {
            std::string device;
            if (settings["previewDevice"].is_string())
                device = settings["previewDevice"].get<std::string>();
            settings.erase("previewDevice");
            if (!device.empty()) {
                if (BusDef* pv = find(preview_id); pv && !outputs_.has(pv->output_target)) {
                    pv->output_kind   = BusOutputKind::Output;
                    pv->output_target = device;
                    summary.preview_device_migrated = 1;
                    Logger::warn("migrated settings.previewDevice '{}' onto the preview bus", device);
                }
            }
        }
    }

    // ---- settings.ltcDevice → settings.ltcOutput (D38) --------------------
    // The last device name a portable document carried (D21's one deliberate
    // exception). It becomes a LOGICAL output name, which is the same string
    // it always was — so a machine that really has that interface keeps
    // working with no operator action, because an unmapped name that names a
    // present device still resolves to it. What changes is the machine that
    // does NOT have it: timecode now goes nowhere instead of being handed to
    // open_device_by_name(), which falls back to the DEFAULT device and put
    // an LTC squeal into the house at every venue without the interface.
    //
    // An ltcOutput already in the document wins — the document has been here
    // before, or an operator chose an output by name — and ltcDevice goes
    // either way, so nothing reads it after load.
    if (document_.contains("settings") && document_["settings"].is_object()) {
        auto& settings = document_["settings"];
        if (settings.contains("ltcDevice")) {
            std::string device;
            if (settings["ltcDevice"].is_string())
                device = settings["ltcDevice"].get<std::string>();
            settings.erase("ltcDevice");
            const bool have_output =
                settings.contains("ltcOutput") && settings["ltcOutput"].is_string() &&
                !settings["ltcOutput"].get<std::string>().empty();
            if (!device.empty() && !have_output) {
                settings["ltcOutput"]      = device;
                summary.ltc_device_migrated = 1;
                Logger::warn("migrated settings.ltcDevice '{}' to settings.ltcOutput — "
                             "map that name in the output map to make it portable", device);
            }
        }
    }

    // ---- Validation (D25) ---------------------------------------------------
    // The rules the API enforces, applied to what came off disk. Each fix is
    // the conservative one: the role holders go to the built-in outputs,
    // feeders of the preview bus go to the house rather than into the phones.
    if (BusDef* m = find(master_id); m && m->output_kind != BusOutputKind::Output) {
        Logger::warn("the master bus '{}' must send to an output; moving it to '{}'",
                     m->display_name, kMainOutputName);
        m->output_kind   = BusOutputKind::Output;
        m->output_target = kMainOutputName;
        summary.roles_migrated = true;
    }
    if (BusDef* pv = find(preview_id); pv && pv->output_kind != BusOutputKind::Output) {
        Logger::warn("the preview bus '{}' must send to an output; moving it to '{}'",
                     pv->display_name, kPreviewOutputName);
        pv->output_kind   = BusOutputKind::Output;
        pv->output_target = kPreviewOutputName;
        summary.roles_migrated = true;
    }
    for (auto& b : buses_) {
        if (b.output_kind != BusOutputKind::Bus || b.output_target != preview_id) continue;
        Logger::warn("bus '{}' fed the preview bus, which nothing may feed; re-routed to the master bus",
                     b.display_name);
        b.output_target = master_id;
        summary.roles_migrated = true;
    }

    migrate_device_overrides_locked(summary);

    // Items that end up on the master bus because the document never said
    // otherwise. Only counted for a document that predates buses: one that
    // carries a `buses` key and leaves an item unassigned is expressing a
    // choice, not being migrated. Counted after the deviceOverride pass so
    // items that just gained a real bus are not reported as having fallen
    // back.
    if (!had_buses_key) {
        for_each_item(document_, [&](json& item, const std::string&) {
            if (item.value("type", std::string{}) != "audio") return;
            if (item.contains("busId") && item["busId"].is_string() &&
                !item["busId"].get<std::string>().empty()) {
                return;
            }
            ++summary.items_to_main;
        });
        if (summary.items_to_main > 0) {
            Logger::warn("legacy project loaded: it carries no bus assignments, "
                         "so all {} item(s) play through the master bus.", summary.items_to_main);
        }
    }

    std::stable_sort(buses_.begin(), buses_.end(), by_order);

    if (summary.roles_migrated) {
        Logger::warn("bus roles settled for this document: master = '{}', preview = '{}'",
                     master_bus_locked()->display_name, preview_bus_locked()->display_name);
    }

    // Not a bus fact, counted here because this is the one function every load
    // path runs and the summary it produces is the one thing the operator is
    // shown. The values are LEFT IN the document — they are the seed a user
    // profile reads on first sign-in, and a client with no profile at all
    // still displays them — but save() drops them, so the file quietly changes
    // shape and the operator is entitled to know that before it happens (U4).
    summary.user_prefs_migrated = count_legacy_user_prefs_locked();
    if (summary.user_prefs_migrated > 0) {
        Logger::warn("this document still carries {} value(s) that belong to the person "
                     "rather than the show (theme / playbackKeys / meterMode / "
                     "uiScrollToPlaying); they will be read once and then dropped on save",
                     summary.user_prefs_migrated);
    }

    // Surfaced by the endpoint that triggered the load and broadcast to every
    // other connected client (D12). Overwritten, not accumulated: the summary
    // describes the document that is loaded right now.
    pending_bus_migration_ = summary;
}

// The four values U4 moved out of the document, as they stand in whatever is
// loaded. Caller holds mutex_.
json ProjectState::legacy_user_prefs_locked() const {
    json out = json::object();
    if (document_.contains("theme") && document_["theme"].is_object() &&
        !document_["theme"].empty())
        out["theme"] = document_["theme"];
    // An empty keymap object is not a keymap. Handing one over would count as
    // "something to seed" and consume a person's one migration on nothing.
    if (document_.contains("playbackKeys") && document_["playbackKeys"].is_object() &&
        !document_["playbackKeys"].empty())
        out["playbackKeys"] = document_["playbackKeys"];
    if (document_.contains("settings") && document_["settings"].is_object()) {
        const json& s = document_["settings"];
        if (s.contains("meterMode"))         out["meterMode"]         = s["meterMode"];
        if (s.contains("uiScrollToPlaying")) out["uiScrollToPlaying"] = s["uiScrollToPlaying"];
    }
    return out;
}

int ProjectState::count_legacy_user_prefs_locked() const {
    // Deliberately counts the same keys legacy_user_prefs_locked() hands out,
    // so what the operator is told matches what actually gets migrated. If
    // these two ever disagree, the banner is lying about one of them.
    return static_cast<int>(legacy_user_prefs_locked().size());
}

json ProjectState::legacy_user_prefs() const {
    std::lock_guard lock{mutex_};
    return legacy_user_prefs_locked();
}

// Convert the legacy per-item `deviceOverride` into real buses.
//
// Before buses existed, "play this cue out of the other sound card" was a
// device name written on the item. Each distinct device becomes one bus whose
// output targets that name; because an unmapped logical name resolves back to
// a device of the same name (see OutputMap::resolve), the audio lands exactly
// where it did before. The field is then dropped — a project should carry one
// routing concept, not two.
void ProjectState::migrate_device_overrides_locked(BusMigrationSummary& summary) {
    std::unordered_map<std::string, std::string> device_to_bus;
    int migrated = 0;

    for_each_item(document_, [&](json& item, const std::string&) {
        if (!item.contains("deviceOverride")) return;
        std::string device;
        if (item["deviceOverride"].is_string()) device = item["deviceOverride"].get<std::string>();
        item.erase("deviceOverride");
        if (device.empty()) return;

        // An explicit bus assignment already says where this goes; the legacy
        // field is just stale, so drop it and leave the assignment alone.
        if (item.contains("busId") && item["busId"].is_string() &&
            !item["busId"].get<std::string>().empty()) {
            return;
        }

        auto it = device_to_bus.find(device);
        if (it == device_to_bus.end()) {
            const std::string base = bus_id_from_name(device);
            std::string       id   = base;
            for (int n = 2; ; ++n) {
                bool taken = false;
                for (const auto& b : buses_) if (b.id == id) { taken = true; break; }
                if (!taken) break;
                id = base + "-" + std::to_string(n);
            }
            BusDef d;
            d.id            = id;
            d.display_name  = device;
            d.width         = 2;
            d.output_kind   = BusOutputKind::Output;
            d.output_target = device;
            int max_order = 0;
            for (const auto& b : buses_)
                if (!b.master && !b.preview) max_order = std::max(max_order, b.order);
            d.order = max_order + 1;
            buses_.push_back(d);
            it = device_to_bus.emplace(device, id).first;
        }
        item["busId"] = it->second;
        ++migrated;
    });

    if (migrated > 0) {
        summary.buses_from_device_override = static_cast<int>(device_to_bus.size());
        // Warn, not info: a routing concept was rewritten under the operator.
        Logger::warn("migrated {} item(s) from deviceOverride onto {} bus(es)",
                     migrated, device_to_bus.size());
    }
}

void ProjectState::write_buses_to_document_locked() {
    json arr = json::array();
    for (const auto& b : buses_) {
        const char* kind = b.output_kind == BusOutputKind::Bus ? "bus" : "output";
        arr.push_back(json{
            {"id",      b.id},
            {"name",    b.display_name},
            {"color",   b.color},
            {"order",   b.order},
            {"width",   b.width},
            {"gainDb",  b.gain_db},
            {"mute",    b.muted},
            {"pan",     b.pan},
            {"master",  b.master},
            {"preview", b.preview},
            {"dsp",     bus_dsp_to_json(b.dsp)},
            {"output",  json{{"type", kind}, {"target", b.output_target}}},
        });
    }
    document_["buses"] = std::move(arr);
    // Version marker for the bus era, deliberately top-level: document_["buses"]
    // stays a bare array so replace_full_document()'s before/after comparison
    // keeps matching on an ordinary save. Nesting the version inside it would
    // change that shape and re-materialise every strip on every save.
    document_["busSchema"] = kBusSchemaVersion;
}

bool ProjectState::allocate_master_pair_locked(audio::MasterChannelIndex& l,
                                               audio::MasterChannelIndex& r) {
    // Reuse a pair handed back by a deleted or rewired bus before growing the
    // monotonic counter, so churn doesn't march into the preview reserve.
    if (!free_master_pairs_.empty()) {
        l = free_master_pairs_.back();
        r = l + 1;
        free_master_pairs_.pop_back();
        return true;
    }
    const audio::MasterChannelIndex bus_width     = engine_.config().master_channels;
    const audio::MasterChannelIndex reserved_base = audio::preview_master_base(bus_width);
    if (next_override_master_ + 1 >= reserved_base) return false;
    l = next_override_master_;
    r = next_override_master_ + 1;
    next_override_master_ += 2;
    return true;
}

void ProjectState::release_master_pair_locked(audio::MasterChannelIndex l) {
    free_master_pairs_.push_back(l);
}

void ProjectState::unwire_bus(BusRouting& routing) {
    if (routing.mixer.empty()) return;
    routing.wired_channels.clear();
    if (!routing.wired_bus_target.empty()) {
        // A bus→bus edge lives entirely on the source strip and holds no
        // master pair of its own, so taking it down is one call. Dropped
        // before anything else so a bus that has just changed kind stops
        // feeding its old destination.
        engine_.unroute_mixer_to_mixer(routing.mixer);
        routing.wired_bus_target.clear();
        if (!routing.reserved_pair && !routing.has_masters) return;
    }
    if (routing.reserved_pair || routing.house_pair) {
        // The preview bus's pair comes back off the strip and the device, but
        // never goes into the pool — it belongs to the headphone output, and
        // handing it to the next direct-out bus would put that bus in the
        // operator's ears. The master bus's house pair (0/1) likewise: it is
        // the engine's own pair, and ensure_default_routing() re-assigns it
        // to the first open device if nothing claims it again.
        engine_.unroute_mixer_from_master(routing.mixer, routing.master_l);
        engine_.unroute_mixer_from_master(routing.mixer, routing.master_r);
        engine_.clear_master_assignment(routing.master_l);
        engine_.clear_master_assignment(routing.master_r);
        routing.reserved_pair = false;
        routing.house_pair    = false;
    } else if (routing.has_masters) {
        engine_.unroute_mixer_from_master(routing.mixer, routing.master_l);
        engine_.unroute_mixer_from_master(routing.mixer, routing.master_r);
        engine_.clear_master_assignment(routing.master_l);
        engine_.clear_master_assignment(routing.master_r);
        std::lock_guard lock{mutex_};
        release_master_pair_locked(routing.master_l);
        routing.has_masters = false;
    }
}

std::size_t ProjectState::rewire_buses_for_output_map() {
    std::vector<BusDef>                         defs;
    std::unordered_map<std::string, BusRouting> routings;
    {
        std::lock_guard lock{mutex_};
        defs     = buses_;
        routings = bus_routings_;
    }

    const auto same = [](const std::vector<OutputMap::Channel>& a,
                         const std::vector<OutputMap::Channel>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].device != b[i].device || a[i].hw_channel != b[i].hw_channel) return false;
        }
        return true;
    };

    // The device list may be why the operator is here.
    refresh_device_cache();

    std::size_t moved = 0;
    for (const auto& bus : defs) {
        // Bus-kind buses never consult the map, and resolving their target
        // (a bus id) would hit the identity fallback and look like a change
        // on every save.
        if (bus.output_kind != BusOutputKind::Output) continue;

        auto it = routings.find(bus.id);
        if (it == routings.end() || it->second.mixer.empty()) continue;

        // A bus that failed to wire earlier has no recorded resolution, so it
        // compares as changed and gets a retry — which is what you want after
        // the operator has just fixed the map.
        //
        // Asked exactly the way it was wired. Comparing against the plain map
        // would use OutputMap's identity fallback, which no bus gets any more —
        // so every save of the output map would tear a working feed down and
        // build it again. The preview bus is the same call with the default
        // device withheld, which is its one difference.
        const auto resolved = resolve_output_channels(bus.output_target, !bus.preview);
        if (same(resolved, it->second.wired_channels)) continue;

        BusRouting routing = it->second;
        unwire_bus(routing);
        wire_bus(bus, routing);
        {
            std::lock_guard lock{mutex_};
            bus_routings_[bus.id] = routing;
        }
        ++moved;
        Logger::info("output map changed: re-wired bus '{}' -> '{}'",
                     bus.display_name, bus.output_target);
    }
    return moved;
}

audio::StripDspParams ProjectState::dsp_params_for(const BusDef& bus) const {
    audio::StripDspParams p;
    // Parked is out of circuit. A filter left at the end of its travel should
    // be a genuine passthrough, not a 20 Hz section still bending phase across
    // the bottom of the band — which is what "off" has to mean when the knob
    // itself is the only control.
    p.hpf.freq_hz = bus.dsp.hpf.freq_hz > 0.0f ? bus.dsp.hpf.freq_hz : kHpfParkedHz;
    p.hpf.q       = bus.dsp.hpf.q;
    p.hpf.enabled = p.hpf.freq_hz > kHpfParkedHz;

    p.lpf.freq_hz = bus.dsp.lpf.freq_hz > 0.0f ? bus.dsp.lpf.freq_hz : kLpfParkedHz;
    p.lpf.q       = bus.dsp.lpf.q;
    p.lpf.enabled = p.lpf.freq_hz < kLpfParkedHz;

    // A band at 0 dB is an identity whatever its Q, so it is left out of
    // circuit rather than run to achieve nothing. ChannelDsp checks the gain
    // again on its side; this keeps the two honest about the same rule.
    //
    // A bypassed section takes every band out while leaving the parameters
    // untouched, so switching it back in restores exactly what was there.
    for (std::size_t i = 0; i < kBusEqBands && i < audio::kEqBands; ++i) {
        const auto& src = bus.dsp.eq[i];
        auto&       dst = p.eq[i];
        dst.freq_hz = src.freq_hz;
        dst.gain_db = src.gain_db;
        dst.q       = src.q;
        dst.slope   = src.slope;
        // Outer bands only, and which end follows from which band it is —
        // the low band shelves the bottom, the high band the top.
        dst.shelf     = src.shelf && (i == 0 || i + 1 == kBusEqBands);
        dst.low_shelf = i == 0;
        dst.enabled = bus.dsp.eq_enabled && src.gain_db != 0.0f;
    }

    // The gate needs both switches: its own, and the dynamics section's
    // bypass. A ratio of 1 is also a no-op, so it is treated as off rather
    // than run to multiply by one.
    p.gate.enabled      = bus.dsp.dyn_enabled && bus.dsp.gate_on &&
                          bus.dsp.gate.ratio > 1.0f && bus.dsp.gate.range_db < 0.0f;
    p.gate.threshold_db = bus.dsp.gate.threshold_db;
    p.gate.ratio        = bus.dsp.gate.ratio;
    p.gate.range_db     = bus.dsp.gate.range_db;
    p.gate.attack_ms    = bus.dsp.gate.attack_ms;
    p.gate.hold_ms      = bus.dsp.gate.hold_ms;
    p.gate.release_ms   = bus.dsp.gate.release_ms;

    // Same pair of switches for the compressor. A ratio of 1 is a no-op here
    // too — but only if the makeup is also zero, because ratio 1 with makeup is
    // a legitimate way to use this as a plain gain stage, and switching it out
    // from under the operator would silently lose the level they set.
    p.comp.enabled      = bus.dsp.dyn_enabled && bus.dsp.comp_on &&
                          (bus.dsp.comp.ratio > 1.0f || bus.dsp.comp.makeup_db != 0.0f);
    p.comp.threshold_db = bus.dsp.comp.threshold_db;
    p.comp.ratio        = bus.dsp.comp.ratio;
    p.comp.knee_db      = bus.dsp.comp.knee_db;
    p.comp.attack_ms    = bus.dsp.comp.attack_ms;
    p.comp.release_ms   = bus.dsp.comp.release_ms;
    p.comp.makeup_db    = bus.dsp.comp.makeup_db;

    // Width is meaningless on a mono bus — there is no second lane to matrix
    // against — so it is forced to identity rather than left for the render
    // thread to skip. That way a bus narrowed to mono and then rebuilt as mono
    // does not come back wide if it is ever widened again.
    //
    // No dyn_enabled here: the dynamics bypass is a bypass of the two dynamics
    // processors, and width is neither. Its own parked position is its bypass.
    if (bus.width >= 2) {
        p.width.width        = bus.dsp.width.width;
        p.width.bass_mono_hz = bus.dsp.width.bass_mono_hz;
        p.width.bass_mono_q  = bus.dsp.width.bass_mono_q;
    }

    // The mono-sum audition, which is the preview strip's width forced to 0.
    // It overrides whatever width the preview bus is carrying, because the
    // operator pressing MONO wants mono and not "mono times whatever was
    // already set". Last, so nothing above can undo it.
    if (bus.preview && monitor_mono_.load(std::memory_order_relaxed)) {
        p.width.width = 0.0f;
    }
    return p;
}

bool ProjectState::set_monitor_mono(bool on) {
    BusDef def;
    audio::MixerChannelId mixer;
    {
        std::lock_guard lock{mutex_};
        const BusDef* pv = preview_bus_locked();
        if (!pv) return false;
        def   = *pv;
        mixer = mixer_for_bus(pv->id);
    }
    if (mixer.empty()) return false;
    // Set before rebuilding the parameters, since dsp_params_for reads it.
    monitor_mono_.store(on, std::memory_order_relaxed);
    engine_.set_mixer_dsp(mixer, dsp_params_for(def));
    return true;
}

bool ProjectState::set_bus_dsp_live(const std::string& id, const json& dsp) {
    BusDef def;
    audio::MixerChannelId mixer;
    {
        std::lock_guard lock{mutex_};
        const auto bit = std::find_if(buses_.begin(), buses_.end(),
                                      [&](const BusDef& b) { return b.id == id; });
        if (bit == buses_.end()) return false;
        def   = *bit;             // a copy: this is the in-gesture value
        mixer = mixer_for_bus(id);
    }
    if (mixer.empty()) return false;

    // Deliberately not written back to buses_, exactly as pan is not. patch_bus
    // persists the settled value and re-applies the same coefficients.
    merge_bus_dsp(dsp, def.dsp);
    engine_.set_mixer_dsp(mixer, dsp_params_for(def));
    return true;
}

bool ProjectState::set_bus_pan_live(const std::string& id, float pan) {
    BusDef     def;
    BusRouting routing;
    {
        std::lock_guard lock{mutex_};
        const auto bit = std::find_if(buses_.begin(), buses_.end(),
                                      [&](const BusDef& b) { return b.id == id; });
        if (bit == buses_.end()) return false;
        def = *bit;
        const auto rit = bus_routings_.find(id);
        if (rit == bus_routings_.end()) return false;
        routing = rit->second;
    }
    // Deliberately not written back to buses_: this is the in-gesture value.
    // patch_bus persists it on settle and will re-apply the same gains.
    def.pan = std::clamp(pan, -1.0f, 1.0f);
    apply_bus_pan(def, routing);
    return true;
}

audio::MixerChannelId ProjectState::resolve_bus_strip(
        const std::string& bus_id,
        const std::unordered_map<std::string, BusRouting>* strips,
        int* width_out,
        bool* preview_out) const {
    if (bus_id.empty()) return {};
    std::lock_guard lock{mutex_};
    const auto bit = std::find_if(buses_.begin(), buses_.end(),
                                  [&](const BusDef& b) { return b.id == bus_id; });
    if (bit == buses_.end()) return {};
    if (width_out)   *width_out   = bit->width;
    if (preview_out) *preview_out = bit->preview;
    const auto& table = strips ? *strips : bus_routings_;
    const auto  rit   = table.find(bus_id);
    return rit == table.end() ? audio::MixerChannelId{} : rit->second.mixer;
}

std::vector<audio::AudioEngine::MixerLaneGain>
ProjectState::bus_to_bus_lane_gains(const BusDef& src, int dst_width) {
    // Exactly the laws the mixer→master sends use (D8), with the destination
    // strip's lanes standing in for the master pair: a mono source is PANNED
    // across the destination's two lanes on the constant-power law, a stereo
    // source is BALANCED lane-for-lane, and a stereo source arriving at a mono
    // destination folds at kDefaultDownmixDb. No new law.
    using LG = audio::AudioEngine::MixerLaneGain;
    if (src.width >= 2 && dst_width < 2) {
        return {LG{0, 0, audio::kDefaultDownmixDb}, LG{1, 0, audio::kDefaultDownmixDb}};
    }
    if (src.width < 2 && dst_width < 2) {
        // One lane into one lane: nothing to place, so nothing to trim.
        return {LG{0, 0, 0.0f}};
    }
    const auto g = src.width >= 2 ? audio::balance_gains_db(src.pan)
                                  : audio::pan_gains_db(src.pan);
    const audio::ChannelIndex lane_r = src.width >= 2 ? 1 : 0;
    return {LG{0, 0, g.left}, LG{lane_r, 1, g.right}};
}

void ProjectState::apply_bus_pan(const BusDef& bus, const BusRouting& routing,
                                 const std::unordered_map<std::string, BusRouting>* strips) {
    if (routing.mixer.empty()) return;

    // The strip needs its own copy: the PFL tap is taken upstream of the sends
    // below, so it places the signal itself to stay post-pan. Without this a
    // panned mono bus would sit dead centre in the phones.
    engine_.set_mixer_pan(routing.mixer, bus.pan);

    // One knob, two laws. A mono bus is PANNED — lane 0 is placed between the
    // destination's lanes on the constant-power law (§2.5.3). A stereo bus is
    // BALANCED — its own two lanes are trimmed against each other, and that
    // law only ever attenuates, so correcting a lopsided mix cannot push the
    // loud side up into the limiter. Both end up as the same two send gains.
    const auto g = bus.width >= 2 ? audio::balance_gains_db(bus.pan)
                                  : audio::pan_gains_db(bus.pan);

    // ...and two source lanes, or one twice. A stereo bus keeps its own L and R
    // and only has their gains trimmed; a mono bus sends lane 0 to both sides,
    // which is what "placing" it means when there is only one of it.
    const audio::ChannelIndex lane_l = 0;
    const audio::ChannelIndex lane_r = bus.width >= 2 ? 1 : 0;

    if (bus.output_kind == BusOutputKind::Bus) {
        // Nothing was wired — an unknown or unresolvable destination — so
        // there is no send to move. wire_bus has already said why.
        if (routing.wired_bus_target.empty()) return;
        int dst_width = 2;
        const auto dst = resolve_bus_strip(routing.wired_bus_target, strips, &dst_width);
        if (dst.empty()) return;
        // Routing again replaces the send in place (D5), exactly as
        // route_mixer_to_master does — so a pan drag re-issues gains rather
        // than tearing the edge down and dropping audio.
        engine_.route_mixer_to_mixer(routing.mixer, dst,
                                     bus_to_bus_lane_gains(bus, dst_width));
        return;
    }

    // Output-kind, on a pool pair or the house pair. The preview bus's
    // reserved pair is wired lane-for-lane by wire_preview_bus and does not
    // pan: it holds no pair in this sense, so it returns here with only the
    // strip's own pan set (which is what places its PFL taps).
    if (bus.output_kind != BusOutputKind::Output) return;
    if (!routing.has_masters && !routing.house_pair) return;
    if (outputs_.resolve(bus.output_target).size() < 2) return;

    engine_.route_mixer_to_master(routing.mixer, routing.master_l, g.left,  lane_l);
    engine_.route_mixer_to_master(routing.mixer, routing.master_r, g.right, lane_r);
}

void ProjectState::wire_bus(const BusDef& bus, BusRouting& routing,
                            const std::unordered_map<std::string, BusRouting>* strips) {
    if (routing.mixer.empty()) return;

    // Only the Output branch below consults the map; anything else is wired
    // from nothing the map can change, so it records no resolution.
    routing.wired_channels.clear();
    routing.wired_bus_target.clear();

    if (bus.output_kind == BusOutputKind::Bus) {
        // The API refuses an illegal or looping destination before it is ever
        // stored (D6), so anything that gets here is either legal or came off
        // disk in a document nobody validated. A destination that no longer
        // exists leaves the bus valid and silent rather than failing the load,
        // and a loop that was hand-edited into the document is dropped at
        // topology build, so neither can reach the render thread.
        int  dst_width   = 2;
        bool dst_preview = false;
        const auto dst = resolve_bus_strip(bus.output_target, strips, &dst_width, &dst_preview);
        if (dst_preview) {
            // Only a document that was hand-edited gets here: the API refuses
            // the preview bus as a destination (D25) — feeding it would put
            // the house mix in the operator's headphones.
            Logger::warn("bus '{}': output bus '{}' is the preview bus and cannot be "
                         "fed; leaving it silent", bus.display_name, bus.output_target);
            return;
        }
        if (dst.empty()) {
            Logger::warn("bus '{}': output bus '{}' does not exist or has no strip; "
                         "leaving it silent", bus.display_name, bus.output_target);
            return;
        }
        routing.wired_bus_target = bus.output_target;
        // One call for both widths: the lane gains carry the pan/balance law
        // and the 2→1 fold (D8). At centre a stereo bus is unity on both
        // lanes — the straight-through wiring "to master" used to hard-code.
        apply_bus_pan(bus, routing, strips);
        Logger::info("bus '{}' -> bus '{}'", bus.display_name, bus.output_target);
        return;
    }

    // The preview bus is wired differently enough to be its own function:
    // the reserved master pair, and no fallback to the default device, ever.
    if (bus.preview) {
        wire_preview_bus(bus, routing);
        return;
    }

    // The master bus is the house (D27): its output sits on masters 0/1,
    // which the engine's own default routing, the house meters, the seam
    // detector and the clock-device rule (D15) all read. Never a pool pair.
    // Claimed before resolving, like the preview bus's reserved pair, so an
    // unresolvable target still reports the pair it owns rather than none.
    if (bus.master) {
        routing.master_l   = 0;
        routing.master_r   = 1;
        routing.house_pair = true;
        if (routing.has_masters) {
            // It held a pool pair as an ordinary bus before the role landed
            // on it; that pair goes back.
            std::lock_guard lock{mutex_};
            release_master_pair_locked(routing.master_l);
            routing.has_masters = false;
        }
    }

    // What "FOH" means on this machine. Main Out unmapped is the default
    // device; any other unmapped name is a device name only if that device is
    // present, and otherwise nothing — see resolve_output_channels() for why
    // silence rather than the default device is the safe answer.
    const auto channels = resolve_output_channels(bus.output_target, true);
    if (channels.empty()) {
        Logger::warn("bus '{}': output '{}' is not in the output map and names no present "
                     "device, so it is silent — map it or point the bus somewhere else",
                     bus.display_name, bus.output_target);
        return;
    }
    routing.wired_channels = channels;

    if (!routing.has_masters && !routing.house_pair) {
        std::lock_guard lock{mutex_};
        if (!allocate_master_pair_locked(routing.master_l, routing.master_r)) {
            Logger::error("bus '{}': out of master channels; leaving it silent",
                          bus.display_name);
            return;
        }
        routing.has_masters = true;
    }

    // One master channel per physical output channel, up to the stereo cap.
    const std::size_t used = std::min<std::size_t>(channels.size(), 2);
    const audio::MasterChannelIndex masters[2] = {routing.master_l, routing.master_r};
    for (std::size_t i = 0; i < used; ++i) {
        const auto dev = engine_.open_device_by_name(channels[i].device, 2);
        if (dev.empty()) {
            Logger::warn("bus '{}': could not open device '{}'",
                         bus.display_name, channels[i].device);
            continue;
        }
        engine_.assign_master_to_device(masters[i], dev, channels[i].hw_channel);
    }

    if (bus.width >= 2 && used >= 2) {
        // Balance rides on these two sends, exactly as pan does on the mono
        // branch below. At centre it is unity on both, which is the
        // straight-through wiring this used to hard-code.
        apply_bus_pan(bus, routing);
    } else if (bus.width >= 2) {
        // Stereo bus folded into a mono output: both lanes at the pan law.
        engine_.route_mixer_to_master(routing.mixer, routing.master_l,
                                      audio::kDefaultDownmixDb, 0);
        engine_.route_mixer_to_master(routing.mixer, routing.master_l,
                                      audio::kDefaultDownmixDb, 1);
    } else if (used >= 2) {
        // Mono bus placed across a stereo output by the same pan law.
        apply_bus_pan(bus, routing);
    } else {
        engine_.route_mixer_to_master(routing.mixer, routing.master_l, 0.0f, 0);
    }

    Logger::info("bus '{}' -> output '{}' (masters {}/{}{}{})",
                 bus.display_name, bus.output_target,
                 routing.master_l, routing.master_r,
                 routing.house_pair ? ", the house pair" : "",
                 outputs_.has(bus.output_target)          ? ""
                 : bus.output_target == kMainOutputName   ? ", unmapped: default device"
                                                          : ", unmapped: name used as device");
}

std::vector<OutputMap::Channel> ProjectState::resolve_output_channels(
        const std::string& logical_name, bool allow_default_device) const {
    // A real mapping wins. That is the portable answer, and what the whole
    // logical-output model exists to make the normal case.
    if (outputs_.has(logical_name)) return outputs_.resolve(logical_name);
    // An empty name is nothing, and the built-in Preview Out is silence
    // unmapped (D26) whichever bus asks.
    if (logical_name.empty() || logical_name == kPreviewOutputName) return {};
    // Main Out unmapped is the platform default device — an empty device name
    // is what open_device_by_name() opens as the default — so a fresh install
    // makes sound with no configuration. Withheld from the preview bus, where
    // reaching the default device means the house, and the house is where PFL
    // must never arrive.
    if (logical_name == kMainOutputName)
        return allow_default_device ? std::vector<OutputMap::Channel>{
                   OutputMap::Channel{"", 0}, OutputMap::Channel{"", 1}}
                                    : std::vector<OutputMap::Channel>{};
    // Otherwise a device name — which is what a strip pick and the legacy
    // deviceOverride / previewDevice migrations produce — but ONLY if that
    // device is actually here.
    //
    // This is the clause that stops a travelling show going to the wrong
    // place. OutputMap::resolve()'s identity fallback would hand a name this
    // machine cannot match to open_device_by_name(), which falls back to the
    // DEFAULT device: a sub-mix bus in a project opened at another venue used
    // to land in the house rather than going quiet. It is the same rule the
    // preview bus has always had, now applied to every hardware output.
    {
        std::lock_guard lock{mutex_};
        if (!device_present_locked(logical_name)) return {};
    }
    return {OutputMap::Channel{logical_name, 0}, OutputMap::Channel{logical_name, 1}};
}

void ProjectState::wire_preview_bus(const BusDef& bus, BusRouting& routing) {
    // The preview bus owns the master pair the engine reserves at the top of
    // the bus, so PFL from the mixer and pre-listen from the playlist arrive
    // in the same headphones, under the same fader, on the same meter. It is
    // never drawn from the general pool — allocate_master_pair_locked stops
    // below it precisely so nothing else can land here.
    const audio::MasterChannelIndex l =
        audio::preview_master_base(engine_.config().master_channels);
    if (routing.has_masters) {
        // It held a pool pair as an ordinary bus before the role landed on
        // it; that pair goes back.
        std::lock_guard lock{mutex_};
        release_master_pair_locked(routing.master_l);
        routing.has_masters = false;
    }
    routing.master_l      = l;
    routing.master_r      = l + 1;
    routing.has_masters   = false;   // not ours to pool
    routing.reserved_pair = true;

    const auto channels = resolve_output_channels(bus.output_target, false);
    if (channels.empty()) {
        // Valid and silent, per §7.5. The default device is withheld here even
        // for Main Out — see resolve_output_channels.
        Logger::warn("bus '{}' (preview): no headphone output on this machine "
                     "('{}' is not in the output map and names no present device), "
                     "so PFL and pre-listen are silent",
                     bus.display_name, bus.output_target);
        return;
    }
    routing.wired_channels = channels;

    const audio::MasterChannelIndex masters[2] = {routing.master_l, routing.master_r};
    const std::size_t used = std::min<std::size_t>(channels.size(), 2);
    for (std::size_t i = 0; i < used; ++i) {
        const auto dev = engine_.open_device_by_name(channels[i].device, 2);
        if (dev.empty()) {
            Logger::warn("bus '{}' (preview): could not open device '{}'",
                         bus.display_name, channels[i].device);
            continue;
        }
        engine_.assign_master_to_device(masters[i], dev, channels[i].hw_channel);
    }

    if (used >= 2) {
        engine_.route_mixer_to_master(routing.mixer, routing.master_l, 0.0f, 0);
        engine_.route_mixer_to_master(routing.mixer, routing.master_r, 0.0f, 1);
    } else {
        // A one-channel headphone output: fold both lanes into it rather than
        // dropping the right-hand side of everything being auditioned.
        engine_.route_mixer_to_master(routing.mixer, routing.master_l,
                                      audio::kDefaultDownmixDb, 0);
        engine_.route_mixer_to_master(routing.mixer, routing.master_l,
                                      audio::kDefaultDownmixDb, 1);
    }

    Logger::info("bus '{}' (preview) -> '{}' on the reserved pair (masters {}/{}){}",
                 bus.display_name, channels[0].device, routing.master_l, routing.master_r,
                 outputs_.has(bus.output_target) ? "" : ", unmapped: name used as device");
}

void ProjectState::refresh_device_cache() {
    // Enumeration opens a backend context, so it runs here — control thread,
    // no locks held — and the result is what list_buses() consults.
    std::vector<std::string> names;
    for (const auto& d : engine_.enumerate_devices()) names.push_back(d.display_name);
    std::lock_guard lock{mutex_};
    known_devices_ = std::move(names);
}

bool ProjectState::device_present_locked(const std::string& name) const {
    if (name.empty()) return false;
    return std::find(known_devices_.begin(), known_devices_.end(), name) != known_devices_.end();
}

void ProjectState::materialise_buses() {
    std::vector<BusDef>                         defs;
    std::unordered_map<std::string, BusRouting> previous;
    {
        std::lock_guard lock{mutex_};
        defs = buses_;
        // Copy rather than move, and leave bus_routings_ in place. Everything
        // below runs with mutex_ released — it makes engine calls and can open
        // devices — so emptying the table here would publish a state where
        // every bus exists but reports no strip. A GET /api/buses landing in
        // that window returned exactly that, and the mixer, which hides buses
        // without a strip, showed an empty rail until it was remounted. The
        // table is swapped once, at the end, instead.
        previous = bus_routings_;
        // A fresh project starts from a clean pool: every pair the outgoing
        // one held is about to be torn down wholesale.
        //
        // The allocator is rewound with it. Clearing the pool alone abandoned
        // every pair the outgoing project held — open enough projects in one
        // session and the counter walks up into the preview reserve and the
        // next direct-out bus has nowhere to go. Same leak §1.2 described for
        // device_routings_, and visible here as the pair index climbing by two
        // on every reload.
        //
        // The LTC feed holds a pair out of that same pool, so it has to come
        // down BEFORE the rewind — otherwise the allocator hands its channels
        // to a bus while timecode is still assigned to them, and the LTC
        // squeal appears in that bus's output. Whoever asks for it next
        // rebuilds it: every caller here is followed by an
        // apply_ltc_output_routing(), or by the mirror's Phase 2.5, which is
        // the same call under another name.
        release_ltc_routing_locked();
        free_master_pairs_.clear();
        next_override_master_ = kFirstOverrideMaster;
    }

    // Strips from the outgoing project are ours to clean up; nothing else
    // tracks them, exactly as with device_routings_. The reserved pair and
    // the house pair come off the device too: the incoming project's holders
    // assign them afresh, and a stale assignment would keep the old device
    // in the operator's ears, or as the clock, until they did.
    for (auto& [_, r] : previous) {
        if (!r.mixer.empty()) engine_.remove_mixer_channel(r.mixer);
        if (r.has_masters || r.reserved_pair || r.house_pair) {
            engine_.clear_master_assignment(r.master_l);
            engine_.clear_master_assignment(r.master_r);
        }
    }

    // `bound` for a bus naming a device is decided against this list (D26).
    refresh_device_cache();

    // Two passes. A Bus-kind bus is wired to another bus's STRIP, and the
    // document says nothing about the order buses appear in, so every strip
    // has to exist before any edge is drawn — otherwise a bus that feeds one
    // defined below it would resolve to nothing and come up silent.
    std::unordered_map<std::string, BusRouting> created;
    for (const auto& b : defs) {
        BusRouting r;
        r.mixer = engine_.create_mixer_channel(b.display_name);
        if (r.mixer.empty()) {
            Logger::error("materialise_buses: no strip available for bus '{}'", b.display_name);
            continue;
        }
        if (auto* m = engine_.find_mixer_channel(r.mixer)) {
            m->set_gain_db(b.gain_db);
            m->set_mute(b.muted);
            // Width so PFL can place this strip in the monitor; never PFL,
            // because a project does not carry what the operator is listening
            // to and a stale flag would be signal in the phones with nothing
            // on screen to explain it.
            m->set_width(static_cast<audio::ChannelCount>(b.width));
            m->set_pan(b.pan);
            m->set_pfl(false);
            m->dsp().set_params(dsp_params_for(b));
        }
        created[b.id] = r;
    }

    // Pass two: every strip now exists, so a bus→bus edge can resolve. The
    // strips built above are handed to wire_bus explicitly — bus_routings_
    // still names the outgoing project's strips, which are already gone.
    for (const auto& b : defs) {
        auto it = created.find(b.id);
        if (it == created.end()) continue;
        wire_bus(b, it->second, &created);
        // Everything PFL'd taps the preview strip. Nominated after wiring so
        // the engine never sees a monitor that is not yet connected to
        // anything. The master strip is where the engine parks a cue that
        // has no route (D27).
        if (b.preview) engine_.set_monitor_mixer(it->second.mixer);
        if (b.master)  engine_.set_master_mixer(it->second.mixer);
    }

    {
        // One swap: the table goes straight from the outgoing routings to the
        // new ones and is never observed empty in between. Readers in the gap
        // above see the old strip ids, which are stale for a moment rather
        // than absent — a meter reads silent and self-corrects, where a
        // missing id made the whole bus disappear.
        std::lock_guard lock{mutex_};
        bus_routings_ = std::move(created);
    }
    Logger::info("materialise_buses: {} bus(es) live", defs.size());
}

// ---------------------------------------------------------------------------
// Bus mutation. Each of these updates document_["buses"] so the change is
// saved, and touches only the affected strip so unrelated buses keep playing.
// ---------------------------------------------------------------------------
ProjectState::PatchBusResult ProjectState::validate_bus_output_locked(
        const std::string& source_id,
        BusOutputKind kind,
        const std::string& target) const {
    const auto find = [&](const std::string& id) -> const BusDef* {
        for (const auto& b : buses_) if (b.id == id) return &b;
        return nullptr;
    };
    // The role holders send to outputs (D25). The preview bus carries PFL:
    // sending it anywhere but hardware would put every PFL'd channel
    // somewhere the audience can hear it — one click, live, which is the
    // failure mode PFL was chosen over solo to avoid (§2.4). The master bus
    // IS the house; a house that feeds a bus is not a house.
    if (const BusDef* src = source_id.empty() ? nullptr : find(source_id)) {
        if (kind == BusOutputKind::Bus) {
            if (src->preview) return PatchBusResult::RefusedPreviewToBus;
            if (src->master)  return PatchBusResult::RefusedMasterToBus;
        }
    }
    if (kind != BusOutputKind::Bus) return PatchBusResult::Ok;

    const BusDef* dst = find(target);
    if (!dst) return PatchBusResult::UnknownTarget;
    // D25: nothing may feed the preview bus. The master bus may be fed — that
    // is how a sub-mix reaches the house.
    if (dst->preview) return PatchBusResult::IllegalTarget;

    // Walk the output chain forward from the destination. If it comes back to
    // the source, this edge would close the loop. A bus routed into itself
    // falls out of the same walk on its first step, which is why self is not a
    // separate case. The hop cap makes a cycle already present in a
    // hand-edited document terminate rather than spin.
    const std::string* cur = &target;
    for (std::size_t hops = 0; hops <= buses_.size(); ++hops) {
        if (!source_id.empty() && *cur == source_id) return PatchBusResult::Cycle;
        const BusDef* b = find(*cur);
        if (!b || b->output_kind != BusOutputKind::Bus || b->output_target.empty())
            return PatchBusResult::Ok;
        cur = &b->output_target;
    }
    // Only reachable if the stored graph already loops, which the API cannot
    // produce. Refusing to add to it is the safe answer.
    Logger::warn("validate_bus_output: the stored bus graph already loops at '{}'", target);
    return PatchBusResult::Cycle;
}

bool ProjectState::bus_reaches_hardware_locked(const std::string& bus_id) const {
    // D10: walk the output chain to its terminal and ask whether THAT reaches
    // hardware. A submix is bound exactly when the bus it feeds is.
    std::string cur = bus_id;
    for (std::size_t hops = 0; hops <= buses_.size(); ++hops) {
        const BusDef* def = nullptr;
        for (const auto& b : buses_) if (b.id == cur) { def = &b; break; }
        if (!def) return false;                 // a destination that went away
        if (def->output_kind == BusOutputKind::Output) {
            // The preview bus keeps the strict rule: bound only when it
            // actually resolved to channels, because it never falls back to
            // the default device. Every other bus is bound when its target is
            // mapped, is the built-in Main Out (the default device), or names
            // a device that is present (D26).
            if (def->preview) {
                const auto rit = bus_routings_.find(def->id);
                return rit != bus_routings_.end() && !rit->second.wired_channels.empty();
            }
            return outputs_.has(def->output_target) ||
                   def->output_target == kMainOutputName ||
                   device_present_locked(def->output_target);
        }
        if (def->output_target.empty()) return false;
        cur = def->output_target;
    }
    // Hop-capped rather than visited-set: the cap costs nothing and answers
    // the same question. The API refuses cycles, so reaching this means the
    // document was hand-edited — and a loop reaches no hardware.
    Logger::warn("bus '{}': its output chain loops, so it reaches no output", bus_id);
    return false;
}

std::optional<BusDef> ProjectState::create_bus(const json& spec, PatchBusResult* why) {
    if (why) *why = PatchBusResult::Ok;
    BusDef d;
    d.display_name = spec.value("name", std::string{"Bus"});
    d.color        = spec.value("color", std::string{});
    d.width        = std::clamp(spec.value("width", 2), 1, 2);
    d.gain_db      = spec.value("gainDb", 0.0f);
    d.muted        = spec.value("mute", false);
    d.pan          = std::clamp(spec.value("pan", 0.0f), -1.0f, 1.0f);
    ParsedOutput out;
    if (spec.contains("output") && spec["output"].is_object()) {
        out = parse_output_spec(spec["output"]);
    } else {
        out.legacy_master = true;   // no output = into the house, as ever
    }
    d.output_kind   = out.kind;
    d.output_target = out.target;

    {
        std::lock_guard lock{mutex_};
        // The retired kind (D25): "master" means a bus→bus send into the bus
        // carrying the master role. Warned, because a controller still
        // sending it is working against the round-1 protocol.
        if (out.legacy_master) {
            d.output_target = master_bus_id_locked();
            if (spec.contains("output")) {
                Logger::warn("create_bus: output.type \"master\" is retired; routing '{}' to "
                             "the master bus '{}' instead", d.display_name, d.output_target);
            }
        }
        // Validated before the bus exists, so a refused output leaves nothing
        // behind. The source id is empty because there is no source yet —
        // which also means a new bus cannot close a loop: nothing points at it.
        const auto v = validate_bus_output_locked({}, d.output_kind, d.output_target);
        if (v != PatchBusResult::Ok) {
            if (why) *why = v;
            Logger::warn("create_bus: refusing '{}' -> bus '{}'",
                         d.display_name, d.output_target);
            return std::nullopt;
        }
        const std::string base = bus_id_from_name(d.display_name);
        std::string       id   = base;
        for (int n = 2; ; ++n) {
            bool taken = false;
            for (const auto& b : buses_) if (b.id == id) { taken = true; break; }
            if (!taken) break;
            id = base + "-" + std::to_string(n);
        }
        d.id = id;
        // New buses land at the end of the rail; the role holders are pinned
        // on the surface and deliberately sort last.
        int max_order = 0;
        for (const auto& b : buses_)
            if (!b.master && !b.preview) max_order = std::max(max_order, b.order);
        d.order = spec.value("order", max_order + 1);
        buses_.push_back(d);
        std::stable_sort(buses_.begin(), buses_.end(),
                         [](const BusDef& a, const BusDef& b) { return a.order < b.order; });
        write_buses_to_document_locked();
    }

    BusRouting r;
    r.mixer = engine_.create_mixer_channel(d.display_name);
    if (r.mixer.empty()) {
        Logger::error("create_bus: no strip available for '{}'", d.display_name);
        std::lock_guard lock{mutex_};
        buses_.erase(std::remove_if(buses_.begin(), buses_.end(),
                                    [&](const BusDef& b) { return b.id == d.id; }),
                     buses_.end());
        write_buses_to_document_locked();
        return std::nullopt;
    }
    if (auto* m = engine_.find_mixer_channel(r.mixer)) {
        m->set_gain_db(d.gain_db);
        m->set_mute(d.muted);
        m->set_width(static_cast<audio::ChannelCount>(d.width));
        m->set_pan(d.pan);
        m->dsp().set_params(dsp_params_for(d));
    }
    if (d.output_kind == BusOutputKind::Output) refresh_device_cache();
    wire_bus(d, r);
    {
        std::lock_guard lock{mutex_};
        bus_routings_[d.id] = r;
    }
    Logger::info("create_bus: '{}' ({})", d.display_name, d.id);
    return d;
}

ProjectState::PatchBusResult ProjectState::patch_bus(const std::string& id,
                                                     const json& patch,
                                                     std::string* materialised_output) {
    if (materialised_output) materialised_output->clear();
    std::string to_materialise;
    // The whole patch is parsed and checked BEFORE anything is mutated, so a
    // refused patch leaves the bus exactly as it was rather than half-applied
    // — and so a rejected edge is never stored, never persisted, and never
    // reaches the engine. Every bus→bus rule (D6/D25) lives in
    // validate_bus_output_locked; the role rules (D24) are here.
    bool          output_patched = false;
    bool          output_legacy  = false;
    BusOutputKind new_kind       = BusOutputKind::Bus;
    std::string   new_target;
    if (patch.contains("output") && patch["output"].is_object()) {
        const auto out = parse_output_spec(patch["output"]);
        output_patched = true;
        output_legacy  = out.legacy_master;
        new_kind       = out.kind;
        new_target     = out.target;
    }
    // Roles: {master:true} / {preview:true} move the role here; false is a
    // request to drop it, which has no meaning — a project always has one of
    // each — so it is refused rather than ignored.
    bool take_master  = false;
    bool take_preview = false;
    if (patch.contains("master")) {
        if (!patch["master"].is_boolean() || !patch["master"].get<bool>())
            return PatchBusResult::RoleCannotBeDropped;
        take_master = true;
    }
    if (patch.contains("preview")) {
        if (!patch["preview"].is_boolean() || !patch["preview"].get<bool>())
            return PatchBusResult::RoleCannotBeDropped;
        take_preview = true;
    }
    if (take_master && take_preview) return PatchBusResult::RoleConflict;

    std::string old_master_id, old_preview_id;   // previous holders, when a role moves
    {
        std::lock_guard lock{mutex_};
        const BusDef* self = nullptr;
        for (const auto& b : buses_) if (b.id == id) { self = &b; break; }
        if (!self) return PatchBusResult::NotFound;

        if (output_legacy) {
            new_target = master_bus_id_locked();
            Logger::warn("patch_bus: output.type \"master\" is retired; routing bus '{}' to "
                         "the master bus '{}' instead", id, new_target);
        }
        // What the bus will look like after the patch, for the role checks:
        // a role may arrive in the same request as the output that qualifies
        // it.
        const BusOutputKind kind_after = output_patched ? new_kind : self->output_kind;

        if (take_master && !self->master) {
            if (self->preview)                       return PatchBusResult::RoleConflict;
            if (kind_after != BusOutputKind::Output) return PatchBusResult::RoleNeedsOutput;
            old_master_id = master_bus_id_locked();
        } else {
            take_master = false;   // already the holder: nothing to move
        }
        if (take_preview && !self->preview) {
            if (self->master)                        return PatchBusResult::RoleConflict;
            if (kind_after != BusOutputKind::Output) return PatchBusResult::RoleNeedsOutput;
            // Nothing may feed the preview bus (D25). Refused rather than
            // re-pointed: the feeders are routing the operator chose.
            for (const auto& b : buses_) {
                if (b.id != id && b.output_kind == BusOutputKind::Bus && b.output_target == id)
                    return PatchBusResult::RoleTargetFed;
            }
            old_preview_id = preview_bus_id_locked();
        } else {
            take_preview = false;
        }

        if (output_patched) {
            // Validated as the bus will be once the roles land: the holder
            // of a role is held to that role's rules even in the request
            // that grants it. Cheapest way to say so is to flip the flags
            // on a copy.
            const auto v = [&] {
                if (!take_master && !take_preview)
                    return validate_bus_output_locked(id, new_kind, new_target);
                // The role rules above already required Output-kind, and an
                // Output-kind edge has nothing else to check.
                return new_kind == BusOutputKind::Output ? PatchBusResult::Ok
                                                         : PatchBusResult::RoleNeedsOutput;
            }();
            if (v != PatchBusResult::Ok) {
                Logger::warn("patch_bus: refusing to route bus '{}' to '{}'", id, new_target);
                return v;
            }

            // Picking a device from the strip used to write that device's name
            // straight into the document as the bus's output target, leaning on
            // OutputMap's identity fallback to resolve it. That put a sound-card
            // name back inside the portable show file — the exact leak the
            // logical-output map exists to close.
            //
            // So a target that names a device present on THIS machine, and that
            // the map does not already carry, is materialised as a logical
            // output of that name. The document still says "Scarlett 2i2", but
            // it now says it as a logical output the map really carries, and the
            // operator can see and re-point it in the output map like any other.
            // At another venue the name is simply unmapped, `bound` is false and
            // the strip says so, instead of the mapping being invisible.
            //
            // Stereo on hardware 0/1, matching exactly what the identity
            // fallback produced, so nothing about where the audio goes changes
            // on the machine that made the choice.
            //
            // Built-ins are excluded: they carry their own meaning when unmapped
            // (D26) and writing an entry for them would override it.
            if (output_patched && new_kind == BusOutputKind::Output &&
                !new_target.empty() &&
                new_target != kMainOutputName &&
                new_target != kPreviewOutputName &&
                !outputs_.has(new_target) &&
                device_present_locked(new_target)) {
                to_materialise = new_target;
            }
        }
    }

    // Outside the lock: save() is file I/O, and OutputMap takes its own.
    if (!to_materialise.empty()) {
        outputs_.set(to_materialise,
                     {OutputMap::Channel{to_materialise, 0},
                      OutputMap::Channel{to_materialise, 1}});
        if (outputs_.save()) {
            Logger::info("patch_bus: '{}' named device '{}'; added it to the output map "
                         "so the project references a logical output rather than a device",
                         id, to_materialise);
            if (materialised_output) *materialised_output = to_materialise;
        } else {
            // The routing still works through the identity fallback; only the
            // explicit entry is missing, so this is a warning, not a failure.
            Logger::warn("patch_bus: could not save the output map after adding '{}'",
                         to_materialise);
        }
    }

    BusDef     updated;
    BusRouting routing;
    bool       found        = false;
    bool       needs_rewire = false;
    bool       width_moved  = false;
    bool       pan_moved    = false;
    bool       dsp_moved    = false;
    bool       order_moved  = false;
    {
        std::lock_guard lock{mutex_};
        for (auto& b : buses_) {
            if (b.id != id) continue;
            found = true;
            if (patch.contains("name"))   b.display_name = patch.value("name", b.display_name);
            if (patch.contains("color"))  b.color        = patch.value("color", b.color);
            if (patch.contains("order")) {
                b.order     = patch.value("order", b.order);
                order_moved = true;
            }
            if (patch.contains("gainDb")) b.gain_db      = patch.value("gainDb", b.gain_db);
            if (patch.contains("mute"))   b.muted        = patch.value("mute", b.muted);
            if (patch.contains("width")) {
                const int w = std::clamp(patch.value("width", b.width), 1, 2);
                if (w != b.width) { b.width = w; needs_rewire = true; width_moved = true; }
            }
            if (patch.contains("pan")) {
                const float p = std::clamp(patch.value("pan", b.pan), -1.0f, 1.0f);
                if (p != b.pan) { b.pan = p; pan_moved = true; }
            }
            if (patch.contains("dsp")) {
                merge_bus_dsp(patch["dsp"], b.dsp);
                dsp_moved = true;
            }
            if (output_patched &&
                (new_kind != b.output_kind || new_target != b.output_target)) {
                b.output_kind   = new_kind;
                b.output_target = new_target;
                needs_rewire    = true;
            }
            // The role lands here, atomically with the previous holder losing
            // it: one write, one broadcast (D24).
            if (take_master)  b.master  = true;
            if (take_preview) b.preview = true;
            updated = b;
            break;
        }
        if (!found) return PatchBusResult::NotFound;
        if (take_master) {
            for (auto& b : buses_) if (b.id != id) b.master = false;
        }
        if (take_preview) {
            for (auto& b : buses_) if (b.id != id) b.preview = false;
        }
        if (order_moved) {
            // A reorder is a drop onto the rail (D31): the moved bus takes the
            // requested order — winning a tie against the bus already there,
            // so "put me where X is" lands before X — and the rail is then
            // renumbered 1..N so orders stay dense and the next drop has an
            // unambiguous answer. The role holders are pinned and keep their
            // own sort keys.
            std::stable_sort(buses_.begin(), buses_.end(),
                             [&](const BusDef& a, const BusDef& b) {
                                 if (a.order != b.order) return a.order < b.order;
                                 return a.id == id && b.id != id;
                             });
            int next = 1;
            for (auto& b : buses_) {
                if (b.master || b.preview) continue;
                b.order = next++;
                if (b.id == id) updated.order = b.order;
            }
        }
        std::stable_sort(buses_.begin(), buses_.end(),
                         [](const BusDef& a, const BusDef& b) { return a.order < b.order; });
        write_buses_to_document_locked();
        auto it = bus_routings_.find(id);
        if (it != bus_routings_.end()) routing = it->second;
    }

    // Level and mute apply straight to the live strip — no rewire, no gap.
    if (!routing.mixer.empty()) {
        if (auto* m = engine_.find_mixer_channel(routing.mixer)) {
            m->set_gain_db(updated.gain_db);
            m->set_mute(updated.muted);
            if (patch.contains("name")) m->set_display_name(updated.display_name);
        }
        if (take_master || take_preview) {
            // Both holders change hands: the old one comes off its role pair
            // and goes back on the desk as an ordinary bus, the new one
            // leaves its pool pair for the role's. Width is re-sent too, as
            // the plain rewire path below does — a role move that also
            // changed width must not lose it.
            if (width_moved) {
                engine_.set_mixer_width(routing.mixer,
                                        static_cast<audio::ChannelCount>(updated.width));
            }
            if (take_master)  rewire_role_move(true,  old_master_id,  id);
            if (take_preview) rewire_role_move(false, old_preview_id, id);
            if (take_master) {
                // The house pair now belongs to this bus, and the resolver
                // already says every cue without a busId lands here — but a
                // cue that is PLAYING is still routed to the old holder's
                // strip, which just left the house pair for a pool one. So
                // the house went quiet while the mixer showed the cue on the
                // new master. Re-route every loaded cue that now resolves
                // here, exactly as update_item does for a busId change.
                std::vector<std::string> inherited;
                {
                    std::lock_guard lock{mutex_};
                    for (const auto& [uuid, cue] : item_uuid_to_cue_) {
                        (void)cue;
                        if (resolve_item_bus(uuid) == id) inherited.push_back(uuid);
                    }
                }
                if (!inherited.empty()) {
                    reroute_items_to_buses(inherited);
                    Logger::info("patch_bus: master role moved to '{}' — re-routed {} cue(s) live",
                                 id, inherited.size());
                }
            }
        } else if (needs_rewire) {
            // Width is part of needs_rewire, and it also decides how PFL
            // places this strip in the monitor — so the engine has to be told
            // even for a bus that is only ever listened to.
            engine_.set_mixer_width(routing.mixer,
                                    static_cast<audio::ChannelCount>(updated.width));
            if (updated.output_kind == BusOutputKind::Output) refresh_device_cache();
            unwire_bus(routing);
            wire_bus(updated, routing);
            std::lock_guard lock{mutex_};
            bus_routings_[id] = routing;
        } else if (pan_moved) {
            // Send gains only — a pan drag must not tear the routing down.
            apply_bus_pan(updated, routing);
        }
        // Tone controls never touch routing: new coefficients into the strip's
        // own slot and nothing else moves. After a role move the strip may
        // have just become the preview bus, whose mono-check rides on these.
        if (dsp_moved || take_preview) {
            engine_.set_mixer_dsp(routing.mixer, dsp_params_for(updated));
        }
    }

    // A bus's width is half of the lane law on every send that FEEDS it (D8):
    // a stereo submix arriving at a bus that has just become mono has to fold
    // at -3 dB rather than lose its right-hand lane. Done outside the block
    // above because it is about other buses' strips, not this one's.
    if (width_moved) rewire_bus_feeders(id);
    return PatchBusResult::Ok;
}

void ProjectState::rewire_role_move(bool master_role, const std::string& old_id,
                                    const std::string& new_id) {
    // Everything below runs with mutex_ released — wire_bus opens devices and
    // every engine call takes its own lock — so the definitions are copied
    // out first. buses_ already carries the flags as they now are.
    BusDef     old_def, new_def;
    BusRouting old_routing, new_routing;
    bool       have_old = false, have_new = false;
    {
        std::lock_guard lock{mutex_};
        for (const auto& b : buses_) {
            if (b.id == old_id) { old_def = b; have_old = true; }
            if (b.id == new_id) { new_def = b; have_new = true; }
        }
        if (auto it = bus_routings_.find(old_id); it != bus_routings_.end()) old_routing = it->second;
        if (auto it = bus_routings_.find(new_id); it != bus_routings_.end()) new_routing = it->second;
    }
    if (!have_new || new_routing.mixer.empty()) return;

    if (!master_role) {
        // Whatever was being auditioned was routed into the OLD preview
        // strip, which is about to become an ordinary bus — possibly one that
        // reaches the house. Stopped, not moved: an audition is momentary
        // and the operator can start it again on the new bus.
        stop_preview();
        // The new holder is the destination now, not a source. The engine's
        // tap loop skips it anyway; the flag is cleared so the surface agrees.
        engine_.set_mixer_pfl(new_routing.mixer, false);
    }
    refresh_device_cache();

    // Old holder first: the role's pair comes free before the new holder
    // claims it, so the two are never both on it. The old bus then goes back
    // on the desk exactly as an ordinary Output-kind bus would — a pool pair
    // for its target (which may resolve to nothing; it is valid and silent
    // then, as any bus is).
    if (have_old && !old_routing.mixer.empty()) {
        if (!master_role) engine_.set_monitor_mixer({});
        unwire_bus(old_routing);
        wire_bus(old_def, old_routing);
        std::lock_guard lock{mutex_};
        bus_routings_[old_id] = old_routing;
    }
    unwire_bus(new_routing);
    wire_bus(new_def, new_routing);
    {
        std::lock_guard lock{mutex_};
        bus_routings_[new_id] = new_routing;
    }
    // Nominated after wiring, as materialise_buses does, so the engine never
    // sees a monitor that is not connected to anything.
    if (master_role) engine_.set_master_mixer(new_routing.mixer);
    else             engine_.set_monitor_mixer(new_routing.mixer);

    // The old holder's DSP is rebuilt too: if it was the preview bus with
    // mono-check on, its width was being forced to 0, and that belongs to
    // the role, not the bus.
    if (have_old && !old_routing.mixer.empty()) {
        engine_.set_mixer_dsp(old_routing.mixer, dsp_params_for(old_def));
    }
    Logger::info("{} role moved: '{}' -> '{}'",
                 master_role ? "master" : "preview",
                 have_old ? old_def.display_name : std::string{"(none)"},
                 new_def.display_name);
}

void ProjectState::rewire_bus_feeders(const std::string& target_id) {
    std::vector<std::pair<BusDef, BusRouting>> feeders;
    {
        std::lock_guard lock{mutex_};
        for (const auto& b : buses_) {
            if (b.output_kind != BusOutputKind::Bus || b.output_target != target_id) continue;
            const auto rit = bus_routings_.find(b.id);
            if (rit == bus_routings_.end() || rit->second.mixer.empty()) continue;
            feeders.emplace_back(b, rit->second);
        }
    }
    // route_mixer_to_mixer replaces the send in place, so the edge never goes
    // away and nothing recorded in the routing changes — there is nothing to
    // write back.
    for (const auto& [def, routing] : feeders) apply_bus_pan(def, routing);
}

bool ProjectState::set_bus_pfl(const std::string& id, bool on) {
    audio::MixerChannelId mixer;
    {
        std::lock_guard lock{mutex_};
        const auto bit = std::find_if(buses_.begin(), buses_.end(),
                                      [&](const BusDef& b) { return b.id == id; });
        if (bit == buses_.end()) return false;
        // The preview bus is the destination, not a source. The engine
        // refuses this too; catching it here means the API can say so.
        if (bit->preview) return false;
        mixer = mixer_for_bus(id);
    }
    if (mixer.empty()) return false;
    engine_.set_mixer_pfl(mixer, on);
    return true;
}

std::size_t ProjectState::clear_all_pfl() {
    return engine_.clear_all_pfl();
}

bool ProjectState::delete_bus(const std::string& id, std::string* why) {
    if (why) why->clear();
    BusRouting routing;
    // Buses whose output fed the one going away, and what they are now.
    std::vector<std::pair<BusDef, BusRouting>> retargeted;
    {
        std::lock_guard lock{mutex_};
        auto it = std::find_if(buses_.begin(), buses_.end(),
                               [&](const BusDef& b) { return b.id == id; });
        if (it == buses_.end()) { if (why) *why = "not found"; return false; }
        if (it->master || it->preview) {
            // A role is moved, never deleted with its bus (D24).
            const char* role = it->master ? "Master" : "Preview";
            Logger::warn("delete_bus: '{}' holds the {} role and cannot be deleted", id, role);
            if (why) *why = std::string{"this bus holds the "} + role + " role; move it first";
            return false;
        }
        buses_.erase(it);

        // D9: a bus that fed the deleted one goes to the master bus rather
        // than being silently orphaned. Losing a submix is a routing change
        // the operator can see and undo; a strip that quietly stops reaching
        // an output is one they find out about from the room.
        const std::string master_id = master_bus_id_locked();
        for (auto& b : buses_) {
            if (b.output_kind != BusOutputKind::Bus || b.output_target != id) continue;
            b.output_kind   = BusOutputKind::Bus;
            b.output_target = master_id;
            const auto rit = bus_routings_.find(b.id);
            retargeted.emplace_back(b, rit == bus_routings_.end() ? BusRouting{} : rit->second);
            Logger::info("delete_bus: bus '{}' fed '{}'; re-routed to the master bus",
                         b.display_name, id);
        }

        // Items pointing at the bus that just went away fall back to the
        // master bus by losing their assignment, rather than being left
        // dangling.
        for_each_item(document_, [&](json& item, const std::string&) {
            if (item.contains("busId") && item["busId"].is_string() &&
                item["busId"].get<std::string>() == id) {
                item.erase("busId");
            }
        });
        write_buses_to_document_locked();

        auto rit = bus_routings_.find(id);
        if (rit != bus_routings_.end()) {
            routing = rit->second;
            bus_routings_.erase(rit);
        }
    }

    if (!routing.mixer.empty()) {
        unwire_bus(routing);                       // returns the pair to the pool
        // Removing the strip also drops every edge either way, so a feeder's
        // send goes with it — but the feeder's own record of that send does
        // not, which is what unwire_bus below clears before it is re-wired.
        engine_.remove_mixer_channel(routing.mixer);
    }

    // The feeders, now on the master bus, are wired to it for real. Done
    // after the deleted strip is gone so nothing is briefly routed to both.
    for (auto& [def, r] : retargeted) {
        if (r.mixer.empty()) continue;
        unwire_bus(r);
        wire_bus(def, r);
        std::lock_guard lock{mutex_};
        bus_routings_[def.id] = r;
    }
    Logger::info("delete_bus: '{}'", id);
    return true;
}

std::string ProjectState::resolve_item_bus(const std::string& item_uuid) const {
    if (item_uuid.empty()) return master_bus_id_locked();

    // Walk the tree carrying the nearest ancestor's assignment down. An item's
    // own busId overrides whatever it inherited, so "item beats group" falls
    // out of the ordering rather than needing a second pass.
    std::string found;
    bool        hit = false;
    std::function<bool(const json&, const std::string&)> walk =
        [&](const json& arr, const std::string& inherited) -> bool {
            if (!arr.is_array()) return false;
            for (const auto& it : arr) {
                if (!it.is_object()) continue;
                std::string effective = inherited;
                if (it.contains("busId") && it["busId"].is_string()) {
                    auto v = it["busId"].get<std::string>();
                    if (!v.empty()) effective = std::move(v);
                }
                if (it.value("uuid", std::string{}) == item_uuid) {
                    found = effective;
                    hit   = true;
                    return true;
                }
                if (it.value("type", std::string{}) == "group" &&
                    it.contains("children") && it["children"].is_array()) {
                    if (walk(it["children"], effective)) return true;
                }
            }
            return false;
        };

    if (document_.contains("items")) walk(document_["items"], std::string{});
    if (!hit && document_.contains("cartOnlyItems") && document_["cartOnlyItems"].is_array()) {
        for (const auto& it : document_["cartOnlyItems"]) {
            if (!it.is_object()) continue;
            if (it.value("uuid", std::string{}) != item_uuid) continue;
            if (it.contains("busId") && it["busId"].is_string())
                found = it["busId"].get<std::string>();
            break;
        }
    }

    if (found.empty()) return master_bus_id_locked();
    // An assignment naming a bus that no longer exists falls back to the
    // master bus rather than leaving the cue unrouted and silent.
    for (const auto& b : buses_) {
        if (b.id == found) return found;
    }
    // Once per unknown bus id, not once per item per resolve. This runs for
    // every item on every save and every mixer poll, so a project carrying a
    // few stale assignments produced a steady stream of identical warnings —
    // the kind of noise that hides the one line that matters mid-show. The set
    // is cleared whenever the bus list is reloaded, so a genuine change says
    // so again.
    if (warned_unknown_buses_.insert(found).second) {
        Logger::warn("resolve_item_bus: item '{}' names unknown bus '{}'; using the master bus. "
                     "The assignment is kept, so it takes effect again if that bus returns.",
                     item_uuid, found);
    }
    return master_bus_id_locked();
}

std::vector<ProjectState::BusInfo> ProjectState::list_buses() const {
    std::vector<BusInfo> out;
    {
    std::lock_guard lock{mutex_};
    out.reserve(buses_.size());
    for (const auto& b : buses_) {
        BusInfo info;
        info.def   = b;
        info.mixer = mixer_for_bus(b.id);
        // Master-kind buses need no binding — they land in the house pair,
        // which the engine always wires. Everything else is bound only if it
        // resolved to real channels when it was wired.
        //
        // Derived on every read, never cached: a Bus-kind bus is bound
        // through whatever its chain ends at (D10), so an output-map edit or
        // a re-route several buses downstream changes this answer with
        // nothing here to update.
        info.bound = bus_reaches_hardware_locked(b.id);
        // Only the preview bus can be folded to mono for auditioning, so only
        // it ever reports it.
        info.mono_check = b.preview && monitor_mono_.load(std::memory_order_relaxed);
        // The master pair its hardware output occupies (D32): the house pair,
        // the reserved pair, or a pool pair — whichever it was wired onto.
        if (const auto rit = bus_routings_.find(b.id); rit != bus_routings_.end()) {
            const auto& r = rit->second;
            if (r.has_masters || r.house_pair || r.reserved_pair)
                info.masters = std::make_pair(r.master_l, r.master_r);
        }
        out.push_back(std::move(info));
    }

    // Attribute every audio item to the bus it actually resolves to, so the
    // caller sees inherited and overridden assignments rather than only the
    // ones written on the item itself.
    const auto attribute = [&](const json& it) {
        if (!it.is_object()) return;
        if (it.value("type", std::string{}) != "audio") return;
        const auto uuid = it.value("uuid", std::string{});
        if (uuid.empty()) return;
        const auto bus_id = resolve_item_bus(uuid);
        for (auto& info : out) {
            if (info.def.id == bus_id) { info.item_uuids.push_back(uuid); return; }
        }
    };
    std::function<void(const json&)> walk = [&](const json& arr) {
        if (!arr.is_array()) return;
        for (const auto& it : arr) {
            attribute(it);
            if (it.is_object() && it.value("type", std::string{}) == "group" &&
                it.contains("children")) {
                walk(it["children"]);
            }
        }
    };
    if (document_.contains("items")) walk(document_["items"]);
    if (document_.contains("cartOnlyItems")) walk(document_["cartOnlyItems"]);
    }

    // PFL is read from the live strip, not the document — it is monitoring
    // state, not part of the show. A project that reopened with PFL latched on
    // some bus would put signal in the operator's headphones for reasons
    // nothing on screen explains. Asked outside the lock: this calls into the
    // engine, which takes its own.
    for (auto& info : out) {
        if (info.mixer.empty()) continue;
        if (auto* m = engine_.find_mixer_channel(info.mixer)) info.pfl = m->is_pfl();
    }
    return out;
}

audio::MixerChannelId ProjectState::mixer_for_bus(const std::string& bus_id) const {
    auto it = bus_routings_.find(bus_id);
    return it == bus_routings_.end() ? audio::MixerChannelId{} : it->second.mixer;
}

bool ProjectState::start_preview(const std::string& item_uuid) {
    if (item_uuid.empty()) return false;

    // 1. Resolve the source file and the strip to audition it on, under the
    //    lock. Pre-listen goes to the preview bus — the same strip PFL feeds
    //    — so there is no preview device or preview mixer to set up here any
    //    more. The preview bus is wired when the project is materialised.
    std::filesystem::path file_path;
    double in_point  = 0.0;
    double out_point = 0.0;
    float  gain_db   = 0.0f;
    audio::MixerChannelId preview_mixer;
    {
        std::lock_guard lock{mutex_};
        for_each_item(document_,
            [&](json& it, const std::string&) {
                if (it.value("uuid", std::string{}) != item_uuid) return;
                auto p = resolve_media_path(
                    it, document_.value("folderPath", std::string{}));
                if (!p.empty()) file_path = std::move(p);
                in_point  = it.value("inPoint", 0.0);
                out_point = it.value("outPoint", 0.0);
                // The item's own level, same 0..2 linear field playback uses.
                // Auditioning is meant to answer "what will this sound like
                // when I fire it", and a preview that ignored the trim you
                // just set answered a different question — audibly so now
                // that pre-listen and PFL share one meter.
                if (it.contains("volume") && it["volume"].is_number()) {
                    const float lin = it["volume"].get<float>();
                    gain_db = (lin <= 0.0001f) ? -120.0f
                                               : 20.0f * std::log10(lin);
                }
            });
        preview_mixer = mixer_for_bus(preview_bus_id_locked());
    }
    if (file_path.empty()) {
        Logger::warn("preview: item '{}' has no resolvable file path", item_uuid);
        return false;
    }
    if (preview_mixer.empty()) {
        Logger::warn("preview: the preview bus has no engine strip, so there is "
                     "nowhere to audition '{}'", item_uuid);
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

    // 3. Load the file as a fresh engine cue, route it to the preview strip
    // ONLY (no auto-routing to the house). prime + play.
    const auto cue_id = engine_.load_cue_no_route(file_path);
    if (cue_id.empty()) return false;

    auto* pi = engine_.find_cue(cue_id);
    if (pi) {
        // The item's level, and only that. Not its fades — a preview that
        // faded in would hide the start of what you are checking — and not its
        // LTC, which belongs to the show, not to an audition.
        pi->set_gain_db(gain_db);
        if (pi->source_channel_count() >= 2) {
            // Stereo: L → lane 0, R → lane 1.
            engine_.route_item_source_to_mixer(cue_id, 0, preview_mixer, 0.0f, 0);
            engine_.route_item_source_to_mixer(cue_id, 1, preview_mixer, 0.0f, 1);
        } else {
            // Mono: fan across both lanes.
            engine_.route_item_source_to_mixer(cue_id, 0, preview_mixer, 0.0f,
                                               audio::kAllMixerLanes);
        }
        // The trim, too: the preview card counts down to the out-point, and an
        // audition that ran on past it played audio the show never will.
        pi->set_out_point_seconds(out_point);
        pi->prime(audio::kPrimeSeconds, in_point);
    }
    engine_.play(cue_id);

    {
        std::lock_guard lock{mutex_};
        preview_cue_       = cue_id;
        preview_item_uuid_ = item_uuid;
    }
    Logger::info("preview: started for item '{}' on the preview bus", item_uuid);
    return true;
}

bool ProjectState::stop_preview() {
    return stop_preview_if(audio::CueId{});
}

bool ProjectState::stop_preview_if(const audio::CueId& expected,
                                   std::optional<std::chrono::milliseconds> fade) {
    const bool faded = fade && fade->count() > 0;
    audio::CueId cue;
    std::function<void()> cb;
    {
        std::lock_guard lock{mutex_};
        if (preview_cue_.empty()) return false;
        if (!expected.empty() && preview_cue_ != expected) return false;
        cue = preview_cue_;
        preview_cue_ = audio::CueId{};
        preview_item_uuid_.clear();
        // A faded audition keeps its decoder until it is silent.
        if (faded) retired_preview_cues_.push_back(cue);
        cb = preview_stopped_broadcaster_;
    }
    if (faded) {
        engine_.stop(cue, *fade);
    } else {
        engine_.stop(cue, std::chrono::milliseconds{0});
        engine_.unload_cue(cue);
    }
    Logger::info("preview: stopped");
    if (cb) cb();
    return true;
}

void ProjectState::poll_preview() {
    audio::CueId live;
    std::vector<audio::CueId> retired;
    {
        std::lock_guard lock{mutex_};
        live = preview_cue_;
        retired.swap(retired_preview_cues_);
    }
    std::vector<audio::CueId> still_fading;
    for (const auto& id : retired) {
        auto* pi = engine_.find_cue(id);
        if (pi && pi->stats().transport != audio::TransportState::Stopped) {
            still_fading.push_back(id);
            continue;
        }
        if (pi) engine_.unload_cue(id);
    }
    if (!still_fading.empty()) {
        std::lock_guard lock{mutex_};
        retired_preview_cues_.insert(retired_preview_cues_.end(),
                                     still_fading.begin(), still_fading.end());
    }
    // The audition reached its end (or anything else stopped the voice: an
    // engine-wide stop from a custom action, a failed play). Nothing else
    // notices: the preview cue is not sequenced and not in list_cues(), so
    // without this the client kept a frozen preview card forever (#60).
    if (!live.empty()) {
        auto* pi = engine_.find_cue(live);
        if (!pi || pi->stats().transport == audio::TransportState::Stopped)
            stop_preview_if(live);
    }
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
// The meter DSP gate — see set_user_meter_modes() in the header for why this
// is a union and why that is allowed to cross the R2 line.
// ---------------------------------------------------------------------------
std::pair<bool, bool> ProjectState::meter_gate_for(const json& settings) const {
    const std::string implied = project_meter_mode(settings);
    bool true_peak = implied == audio::kMeterModeTruePeak;
    bool loudness  = implied == audio::kMeterModeLoudness;

    std::lock_guard lock{user_meter_modes_mutex_};
    for (const auto& m : user_meter_modes_) {
        if (m == audio::kMeterModeTruePeak) true_peak = true;
        if (m == audio::kMeterModeLoudness) loudness  = true;
    }
    return {true_peak, loudness};
}

void ProjectState::apply_meter_gate() {
    json settings_snap;
    {
        std::lock_guard lock{mutex_};
        settings_snap = document_.value("settings", json::object());
    }
    const auto [want_true_peak, want_loudness] = meter_gate_for(settings_snap);

    // Same diffing discipline as the mirror: turning metering on or off walks
    // every item and channel, so it happens only when the answer actually
    // moved. An operator opening a second window must not re-arm the DSP.
    std::lock_guard alock{applied_engine_settings_mutex_};
    const auto& prev = applied_engine_settings_;
    if (!prev || prev->true_peak != want_true_peak)
        engine_.set_true_peak_metering(want_true_peak);
    if (!prev || prev->loudness != want_loudness)
        engine_.set_loudness_metering(want_loudness);
    if (applied_engine_settings_) {
        applied_engine_settings_->true_peak = want_true_peak;
        applied_engine_settings_->loudness  = want_loudness;
    }
}

void ProjectState::set_user_meter_modes(std::vector<std::string> modes) {
    {
        std::lock_guard lock{user_meter_modes_mutex_};
        if (modes == user_meter_modes_) return;
        user_meter_modes_ = std::move(modes);
    }
    apply_meter_gate();
}

// ---------------------------------------------------------------------------
// Settings patches
// ---------------------------------------------------------------------------
bool ProjectState::patch_settings(const json&               patch,
                                  std::vector<std::string>* dropped_out) {
    if (!patch.is_object()) return false;
    std::vector<std::string> dropped;
    bool ltc_output_changed      = false;
    bool default_device_changed  = false;
    bool output_target_changed   = false;
    bool limiter_toggle_changed  = false;
    bool limiter_disabled        = false;
    bool ballistics_changed      = false;
    bool meter_gate_changed      = false;
    float new_ceiling_db         = -0.3f;
    audio::MeterBallistics new_ballistics{};
    {
        std::lock_guard lock{mutex_};
        if (!document_.contains("settings") || !document_["settings"].is_object()) {
            document_["settings"] = json::object();
        }
        for (auto& [k, v] : patch.items()) {
            // A pre-2.5 client (or a Companion button written against the old
            // key) still patches ltcDevice. Land it on ltcOutput rather than
            // storing a second copy: a device name IS a usable logical name,
            // and two keys meaning the same thing is exactly the drift R1's
            // one-writer-per-value rule exists to prevent (D38). The load-time
            // migration handles the same field arriving inside a document.
            std::string key = k;
            if (key == "ltcDevice") {
                key = "ltcOutput";
                document_["settings"].erase("ltcDevice");
                Logger::warn("settings patch: 'ltcDevice' is now 'ltcOutput' — "
                             "applied as a logical output name");
            }

            // Validate before anything else: a key that does not survive the
            // registry must not fire its side effect either, or a rejected
            // value would still be heard while never being stored.
            std::string why;
            const auto  accepted = validate_setting(key, v, why);
            if (!accepted) {
                dropped.push_back(k);
                Logger::warn("settings patch: dropped '{}' — {}", k, why);
                continue;
            }

            if (key == "ltcOutput")           ltc_output_changed     = true;
            if (key == "defaultOutputDevice") default_device_changed = true;
            if (key == "outputTarget")        output_target_changed  = true;
            if (key == "disableLimiter") {
                limiter_toggle_changed = true;
                limiter_disabled       = accepted->get<bool>();
            }
            if (key == "meterBallistics" || key == "meterBallisticsCustom") {
                ballistics_changed = true;
            }
            // outputTarget is the only project key left that can move the
            // meter gate: it sets the unit this show implies, and since U4
            // that is all the document says about metering.
            if (key == "outputTarget") meter_gate_changed = true;
            document_["settings"][key] = *accepted;
        }
        // outputTargetLevels is derived, so it is never stored — full_document()
        // injects a fresh copy on every read and the broadcast below is built
        // from that. Erasing here also cleans the stale copy out of any project
        // saved before the registry existed, the first time its settings are
        // touched.
        document_["settings"].erase("outputTargetLevels");
        if (output_target_changed) {
            new_ceiling_db = compute_output_target_levels(document_["settings"])
                                 .value("limiterCeilingDb", -0.3f);
        }
        if (ballistics_changed) {
            new_ballistics = meter_ballistics_from_settings(document_["settings"]);
        }
    }
    // Re-apply routing when output / device selections change mid-playback.
    if (ltc_output_changed)     apply_ltc_output_routing();
    if (default_device_changed) apply_default_device_routing();
    // Apply brickwall limiter ceiling for the chosen output platform.
    if (output_target_changed)  engine_.set_master_ceiling_db(new_ceiling_db);
    // Enable/disable the limiter live so the change is heard immediately.
    if (limiter_toggle_changed) engine_.set_limiter_enabled(!limiter_disabled);
    // Retune every meter live so the operator sees the new feel immediately.
    if (ballistics_changed)     engine_.set_meter_ballistics(new_ballistics);
    // Gate the true-peak / loudness DSP. Delegated rather than computed here:
    // the project is no longer the only voice in that decision, and one owner
    // for the gate is what stops this and a connecting operator from taking
    // turns overwriting each other's answer.
    if (meter_gate_changed) apply_meter_gate();
    // Keep the mirror's applied-settings record in step with what was just
    // applied, so the save that follows this edit sees nothing to re-apply.
    // Re-applying the ceiling is audible (it rebuilds the master limiters),
    // which is why the mirror only applies differences — see the matching
    // block in start_async_mirror().
    {
        std::lock_guard alock{applied_engine_settings_mutex_};
        if (applied_engine_settings_) {
            if (output_target_changed)
                applied_engine_settings_->ceiling_db = new_ceiling_db;
            if (limiter_toggle_changed)
                applied_engine_settings_->limiter_enabled = !limiter_disabled;
            if (ballistics_changed)
                applied_engine_settings_->ballistics = new_ballistics;
            // The meter gate is not folded in here: apply_meter_gate() above
            // already updated this record itself, because it is reachable
            // from paths this function is not on.
        }
    }
    if (dropped_out) *dropped_out = std::move(dropped);
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
            // The outgoing project's device overrides are meaningless to the
            // incoming one, and their master pairs are never otherwise
            // reclaimed — see release_device_routings_locked().
            release_device_routings_locked();

            document_ = std::move(doc_repaired);
            // Read the incoming document's buses (settling the roles, synthesising if
            // it predates them) and write them back so the shape is canonical
            // from here on. Strips are created after the lock is released.
            load_buses_locked();
            write_buses_to_document_locked();
            // Ensure required top-level keys exist (migrate older client saves).
            if (!document_.contains("settings") || !document_["settings"].is_object()) {
                // Deliberately no defaultOutputDevice and no ltcDevice:
                // load_buses_locked() above has just migrated those onto the
                // master bus and onto ltcOutput respectively, and erased them.
                // Writing either back would undo the migration.
                document_["settings"] = json{
                    {"ltcOutput",           nullptr},
                };
            }
            if (!document_.contains("cartOnlyItems") ||
                !document_["cartOnlyItems"].is_array()) {
                document_["cartOnlyItems"] = json::array();
            }
            // No default theme injected here either — see load(). A document
            // either carries a legacy theme or it does not, and inventing one
            // makes "did anybody choose this?" unanswerable.
            project_name_ = document_.value("name", std::string{"Untitled"});
            update_media_root_from_folder_locked();
            pending_repair_info_ = std::move(repair);
        }

        // Create the engine strips for this project's buses. Outside the lock:
        // every engine call takes its own.
        materialise_buses();

        // Audio mirroring happens off-thread so the client can render the
        // project immediately. Items not yet loaded into the engine will
        // simply fail play() until ready (rare in practice — by the time the
        // user clicks anything, the first batch is usually done).
        start_async_mirror();
        // Arm the first playable item as "Up Next" so the operator's very first
        // GO fires without a click (#28). Reads the document (available now) and
        // is a no-op if something is already armed / playing. Runs after the
        // async mirror kickoff — cues_ is empty at this instant, so the "nothing
        // on air" guard passes on a fresh open.
        arm_first_item_on_open();
        return true;
    }

    // Otherwise: assume server's snake_case schema (current behaviour).
    json doc = doc_in;
    if (is_legacy_document(doc)) {
        Logger::info("ProjectState: detected legacy 1.x document, upgrading.");
        doc = upgrade_legacy_document(doc);
    }

    // unique_lock, not lock_guard: the bus sequence at the bottom of this
    // branch has to hand the lock back before materialise_buses() runs.
    std::unique_lock lock{mutex_};
    for (auto& [_, id] : item_uuid_to_cue_) engine_.unload_cue(id);
    item_uuid_to_cue_.clear();
    primed_cues_.clear();
    cues_.clear();
    mixers_.clear();
    item_routes_.clear();
    mixer_routes_.clear();
    master_assignments_.clear();
    // Same reason as the client branch above: the outgoing project's device
    // overrides mean nothing here, and their master pairs are never otherwise
    // reclaimed — see release_device_routings_locked().
    release_device_routings_locked();
    document_ = default_empty_document();

    project_name_ = doc.value("project_name", std::string{"Untitled"});
    if (doc.contains("media_root") && doc["media_root"].is_string()) {
        media_root_ = util::utf8_to_path(doc["media_root"].get<std::string>());
    }
    // The project folder always wins: media must live inside it so the project
    // stays portable and we never read media from outside the folder. load()
    // injects the authoritative folderPath (the directory the .liveplay sits
    // in) before calling us, so this overrides any stale stored media_root.
    if (doc.contains("folderPath") && doc["folderPath"].is_string()) {
        const std::string folder = doc["folderPath"].get<std::string>();
        if (!folder.empty()) media_root_ = util::utf8_to_path(folder) / "media";
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
            // `soloed` is the pre-PFL spelling. Reading it here and writing
            // `pfl` back is the whole of the migration §5.3 asked for — solo
            // was never reachable from the UI, so a document carrying it is
            // unlikely, and the two meant close enough to the same thing for
            // the value to survive.
            mm.pfl          = m.value("pfl", m.value("soloed", false));
            mixers_.emplace(mm.id.value, std::move(mm));
        }
    }
    if (doc.contains("item_routes") && doc["item_routes"].is_array()) {
        for (auto& r : doc["item_routes"]) {
            item_routes_.push_back(RouteSendV2{
                r.value("source_channel", (audio::ChannelIndex)0),
                audio::MixerChannelId{r.value("destination_mixer", std::string{})},
                r.value("gain_db", 0.0f),
                r.value("lane", audio::kAllMixerLanes),
            });
        }
    }
    if (doc.contains("mixer_routes") && doc["mixer_routes"].is_array()) {
        for (auto& r : doc["mixer_routes"]) {
            mixer_routes_.push_back(MixerToMasterV2{
                audio::MixerChannelId{r.value("mixer", std::string{})},
                r.value("master_channel", (audio::MasterChannelIndex)0),
                r.value("gain_db", 0.0f),
                r.value("lane", audio::kAllMixerLanes),
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

    // Same bus sequence the client branch runs, and for the same reason: a
    // document that predates buses does not name any, so this synthesises master
    // and preview buses and writes them into the document. Without it a 1.x or
    // snake_case project came up with an empty mixer — no master, no preview,
    // nothing for /api/buses to report and nowhere for a cue to resolve to.
    // It runs AFTER the document has been populated, because load_buses_locked
    // reads document_ (and migrates settings.defaultOutputDevice out of it).
    //
    // A 1.x document carries no client `items`, so there is nothing to route
    // per-item here: everything it plays goes out of the master, which is
    // exactly where the master bus lands (D1).
    load_buses_locked();
    write_buses_to_document_locked();
    const std::size_t legacy_cue_count = cues_.size();
    lock.unlock();
    // Strips for those buses. Outside the lock: every engine call takes its own.
    materialise_buses();
    // Said out loud because it is a routing decision made on the operator's
    // behalf: this format carries no bus assignments, so everything in it goes
    // to the master bus (D1). No migration wizard — the mixer shows the result.
    Logger::warn("legacy project loaded: this format carries no bus assignments, "
                 "so all {} cue(s) play through the master bus.", legacy_cue_count);
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
        // The media/ folder always lives next to the .liveplay file, so the
        // project folder is authoritatively the directory the file sits in.
        // Since 2.5 the saved file carries no folderPath at all and this is the
        // only place it comes from; older files carry one baked in wherever
        // they were last saved, which this overwrites. We rewrite it BEFORE
        // load_from_json() because that call kicks off the async engine mirror,
        // which resolves each item's media against folderPath; injecting the
        // real location first is what lets a moved project — or a legacy v1
        // project whose folderPath still points at its original home — resolve
        // its relative "media/..." paths and actually load/play.
        if (path.has_parent_path() && doc.is_object()) {
            doc["folderPath"] = util::path_to_utf8(path.parent_path());
        }
        const bool ok = load_from_json(doc);
        if (ok) {
            // Adopting the path re-anchors folderPath and media_root_ onto the
            // file's real location (see set_project_file_path).
            set_project_file_path(path);
            // Normalise media references to the portable relative form now that
            // folderPath points at the file's real location: drops stale
            // absolute mediaServerPaths so the document served to clients (and
            // written on the next save) stays portable across moves.
            std::lock_guard lock{mutex_};
            relativize_media_paths(document_);
        }
        return ok;
    } catch (const std::exception& ex) {
        Logger::error("ProjectState::load failed: {}", ex.what());
        return false;
    }
}

BusMigrationSummary ProjectState::consume_bus_migration_summary() {
    std::lock_guard lock{mutex_};
    BusMigrationSummary result;
    std::swap(result, pending_bus_migration_);
    return result;
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
            // Cache the lookup once and null-check it — find_cue can return
            // null (e.g. the load raced with an unload) and the previous code
            // dereferenced it up to six times unchecked. 
            auto* cue = engine_.find_cue(id);
            if (!cue) continue;
            cue->set_gain_db(c.gain_db);
            cue->set_fade_in (c.fade_in_ms);
            cue->set_fade_out(c.fade_out_ms);
            if (c.ltc_enabled) {
                cue->set_ltc_enabled(true);
                cue->set_ltc_frame_rate(fps_index_to_rate(c.ltc_frame_rate_index));
                cue->set_ltc_offset(c.ltc_offset_ns);
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
            m->set_pfl  (mm.pfl);
        }
    }
    // 3) Re-apply routes.
    for (auto& r : item_routes_) {
        engine_.route_item_source_to_mixer(audio::CueId{}, r.source_channel,
                                           r.destination_mixer, r.gain_db,
                                           r.lane);
    }
    for (auto& r : mixer_routes_) {
        engine_.route_mixer_to_master(r.mixer, r.master_channel, r.gain_db,
                                      r.lane);
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

    // How often this loop wakes to look at every playing item. Nothing here can
    // notice anything sooner than its next pass, so every "a hair before X"
    // below is measured against it.
    constexpr auto kPollInterval = 50ms;

    // How far before an item's out-point to start the next cue for a seamless
    // (gapless) auto-advance. Must exceed the poll interval plus a little
    // device/ring slack, so the incoming cue is already sounding by the time
    // the outgoing reaches its out-point. The resulting overlap of program
    // tails (~0.1 s) is inaudible and replaces the previous silent gap.
    //
    // Derived from the poll rather than written as 0.10, because that is what
    // it IS: one interval to be sure of seeing the marker, one for slack. As a
    // literal it was a number that silently stopped being enough the moment
    // anyone changed the sleep below — the two are one decision, not two.
    constexpr double kSeamlessLeadSec =
        2.0 * std::chrono::duration<double>(kPollInterval).count();

    while (sequencer_running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kPollInterval);
        if (!sequencer_running_.load(std::memory_order_acquire)) break;

        poll_preview();

        struct PendingAction {
            SequencedItem item;
            enum class Kind {
                NaturalEnd,    // take_natural_end() returned true
                Crossfade,     // start next + fade out current
                BeginStopFade, // begin fading out (item stays until Stopped)
                StopFadeEnded, // stop-fade complete → fire end behavior
                StartNext,     // Start Next marker crossed: start next item,
                               // current keeps playing (or begins marker fade)
                SeamlessAdvance, // auto-advance end behaviour with no crossfade:
                                 // start next item a hair before out-point so
                                 // there's no audible gap; current plays its tail
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

                // Start Next marker: fires once when the playhead crosses it,
                // independent of effective_end (it needs no known duration).
                if (!si.start_next_triggered && si.start_next_time > 0.0 &&
                    pos >= si.start_next_time) {
                    si.start_next_triggered = true;
                    pending.push_back({si, PendingAction::Kind::StartNext});
                    // The pending copy owns the duck-restore now; clear so a
                    // later Cleanup/end can't restore stale gains a second time.
                    si.ducked.clear();
                    // A marker fade behaves like a begun stop-fade: when the
                    // transport settles to Stopped, the StopFadeEnded path
                    // removes the item (its advance is suppressed below).
                    if (si.start_next_fade_sec > 0.0)
                        si.stop_fade_triggered = true;
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
                } else if (!si.advance_triggered && !si.start_next_triggered &&
                           si.crossfade_sec <= 0.0 && si.stop_fade_sec <= 0.0 &&
                           si.start_next_time <= 0.0 &&
                           (si.end_action == "next" ||
                            si.end_action == "goto-item" ||
                            si.end_action == "goto-index") &&
                           remaining <= kSeamlessLeadSec && remaining > 0.0) {
                    // Seamless auto-advance: start the next cue a hair before this
                    // one's out-point so there's no audible gap. Mark the item so
                    // its natural end doesn't advance a second time (reuse the
                    // start_next suppression path in handle_item_ended), and take
                    // ownership of the duck-restore here so Cleanup/NaturalEnd
                    // can't double-restore stale gains.
                    si.advance_triggered   = true;
                    si.start_next_triggered = true;
                    pending.push_back({si, PendingAction::Kind::SeamlessAdvance});
                    si.ducked.clear();
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

        // Execute pending actions with the sequencer lock released. Each action
        // is wrapped so a single failure (e.g. malformed end-behaviour data)
        // can never escape and tear down the sequencer thread — that would
        // silently disable all future end behaviours / auto-advance.
        for (const auto& p : pending) {
          try {
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
                        next_item_override_.clear(); next_item_override_manual_ = false;
                    } else {
                        next_uuid = resolve_next_item_locked(p.item.uuid);
                    }
                }
                // Fade the incoming cue IN over the crossfade window, and
                // exclude the outgoing cue from the incoming item's ducking so
                // its engine-owned fade-out (started just above) isn't hard-cut.
                // Skip if the operator already started the next item manually —
                // restarting it mid-play is never what a crossfade means.
                if (!next_uuid.empty()) {
                    if (item_on_air(next_uuid)) {
                        Logger::playback("CROSSFADE: next item '{}' already "
                                         "on air — not restarting", next_uuid);
                    } else {
                        trigger_item(next_uuid, p.item.crossfade_sec, p.item.cue_id);
                    }
                }
                break;
            }

            case PendingAction::Kind::StartNext: {
                // Restore gains this cue ducked so the incoming cue's own
                // ducking applies fresh (mirrors the Crossfade path).
                for (const auto& dk : p.item.ducked) {
                    if (auto* pi = engine_.find_cue(dk.cue_id))
                        pi->set_gain_db(dk.original_gain_db);
                }
                // Optional radio-style tail: begin fading this cue out at
                // the marker (over its fadeOutDuration). Without it the cue
                // simply plays on to its natural end underneath the next one.
                if (p.item.start_next_fade_sec > 0.0) {
                    if (auto* pi = engine_.find_cue(p.item.cue_id)) {
                        pi->stop_with_fade(std::chrono::milliseconds{
                            static_cast<long long>(
                                p.item.start_next_fade_sec * 1000.0)});
                    }
                }
                // Start the next cue at its own volume and fades. Honour a
                // user-set Up Next override, same as handle_item_ended.
                std::string next_uuid;
                {
                    std::lock_guard lock{mutex_};
                    if (!next_item_override_.empty()) {
                        next_uuid = std::move(next_item_override_);
                        next_item_override_.clear(); next_item_override_manual_ = false;
                    } else {
                        next_uuid = resolve_next_item_locked(p.item.uuid);
                    }
                }
                // Exclude the outgoing cue from the incoming item's ducking
                // so it keeps playing (or finishes its marker fade) underneath
                // instead of being hard-cut by a stop-all ducking mode.
                // Skip if the operator already started the next item manually.
                if (!next_uuid.empty()) {
                    if (item_on_air(next_uuid)) {
                        Logger::playback("START NEXT: next item '{}' already "
                                         "on air — not restarting", next_uuid);
                    } else {
                        trigger_item(next_uuid, -1.0, p.item.cue_id);
                    }
                }
                break;
            }

            case PendingAction::Kind::SeamlessAdvance: {
                // Restore gains this cue ducked so the incoming cue's own
                // ducking applies fresh (mirrors Crossfade / StartNext).
                for (const auto& dk : p.item.ducked) {
                    if (auto* pi = engine_.find_cue(dk.cue_id))
                        pi->set_gain_db(dk.original_gain_db);
                }
                // Resolve the advance target (consumes an Up-Next override for
                // the "next" case) and start it now. The outgoing cue keeps
                // playing its short tail to its natural end, where it stops
                // itself; excluding it from the incoming cue's ducking prevents
                // a hard cut, so the boundary has no gap.
                const std::string next_uuid = resolve_advance_target(p.item);
                if (!next_uuid.empty()) {
                    if (item_on_air(next_uuid)) {
                        Logger::playback("SEAMLESS ADVANCE: next item '{}' already "
                                         "on air — not restarting", next_uuid);
                    } else {
                        trigger_item(next_uuid, -1.0, p.item.cue_id);
                    }
                }
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
          } catch (const std::exception& e) {
            Logger::warn("sequencer action (item '{}') threw: {}", p.item.uuid, e.what());
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
        // Per-cue fades (not forced), as before. The audition obeys the same
        // stopAllStopsPreview setting as the button; when it is stopped,
        // poll_preview() notices and clears the preview state.
        audio::CueId spare;
        {
            std::lock_guard lock{mutex_};
            if (document_.contains("settings") && document_["settings"].is_object() &&
                !document_["settings"].value("stopAllStopsPreview", true))
                spare = preview_cue_;
        }
        engine_.stop_all(std::chrono::milliseconds{0}, /*force_fade=*/false, spare);
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

bool ProjectState::item_on_air(const std::string& uuid) {
    audio::CueId cue;
    {
        std::lock_guard lock{mutex_};
        auto it = item_uuid_to_cue_.find(uuid);
        if (it == item_uuid_to_cue_.end()) return false;
        cue = it->second;
    }
    if (auto* pi = engine_.find_cue(cue)) {
        const auto ts = pi->stats().transport;
        return ts == audio::TransportState::Playing  ||
               ts == audio::TransportState::FadingIn ||
               ts == audio::TransportState::Paused;
    }
    return false;
}

std::string ProjectState::resolve_advance_target(const SequencedItem& item) {
    if (item.end_action == "next") {
        std::lock_guard lock{mutex_};
        if (!next_item_override_.empty()) {
            std::string u = std::move(next_item_override_);
            next_item_override_.clear(); next_item_override_manual_ = false;
            return u;
        }
        return resolve_next_item_locked(item.uuid);
    }
    if (item.end_action == "goto-item") {
        return item.goto_target_uuid;
    }
    if (item.end_action == "goto-index" && !item.goto_target_index.empty()) {
        std::lock_guard lock{mutex_};
        return resolve_index_path_locked(item.goto_target_index);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Server-authoritative "Up Next" arming for cues with no end behaviour (#28).
// Mirrors the logic that used to live in the client's useAudioEngine so that,
// with multiple clients connected, the next-item arming is decided once by the
// authoritative server and fanned out to every client via next_item_set.
// ---------------------------------------------------------------------------
void ProjectState::arm_next_after_stop(const std::string& stopped_uuid,
                                       bool was_manual) {
    if (stopped_uuid.empty()) return;

    std::string next_to_arm;
    {
        std::lock_guard lock{mutex_};

        // Setting gate (default ON — undefined/true both enable).
        if (document_.contains("settings") && document_["settings"].is_object()) {
            const auto& s = document_["settings"];
            if (s.contains("autoCueNextWithoutEndBehavior") &&
                s["autoCueNextWithoutEndBehavior"].is_boolean() &&
                !s["autoCueNextWithoutEndBehavior"].get<bool>()) return;
        }

        // An arming the OPERATOR made wins — never clobber it. An arming the
        // server derived itself is fair game: it goes stale as soon as playback
        // moves somewhere it didn't predict (most visibly when the operator
        // jumps into a group, where the old blanket "any arming wins" check
        // meant the group's 2nd child was never armed once its 1st finished).
        if (!next_item_override_.empty() && next_item_override_manual_) return;

        // Only arm once nothing else is on air (e.g. don't fire mid-crossfade).
        // The just-stopped cue is ignored: on a manual stop it may still be
        // fading out, and arming is only a pointer (no playback), so its tail
        // must not block the advance.
        audio::CueId stopped_cue;
        {
            auto it = item_uuid_to_cue_.find(stopped_uuid);
            if (it != item_uuid_to_cue_.end()) stopped_cue = it->second;
        }
        for (auto& [_, c] : cues_) {
            if (c.id == stopped_cue) continue;
            if (auto* pi = engine_.find_cue(c.id)) {
                const auto ts = pi->stats().transport;
                if (ts == audio::TransportState::Playing  ||
                    ts == audio::TransportState::FadingIn ||
                    ts == audio::TransportState::FadingOut||
                    ts == audio::TransportState::Paused) return;
            }
        }

        // Locate the stopped item and confirm it has no end behaviour.
        json* found = nullptr;
        std::vector<int> stopped_path;
        std::function<void(json&, std::vector<int>&)> walk;
        walk = [&](json& arr, std::vector<int>& path) {
            if (found || !arr.is_array()) return;
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (found) return;
                json& it = arr[i];
                if (!it.is_object()) continue;
                path.push_back(static_cast<int>(i));
                if (it.value("uuid", std::string{}) == stopped_uuid) {
                    found = &it; stopped_path = path; path.pop_back(); return;
                }
                if (it.value("type", std::string{}) == "group" &&
                    it.contains("children")) walk(it["children"], path);
                path.pop_back();
            }
        };
        if (document_.contains("items")) {
            std::vector<int> p;
            walk(document_["items"], p);
        }
        if (!found) return;
        // Only the "nothing" end behaviour is armed here — every other action
        // is auto-advanced by the sequencer itself.
        std::string action = "nothing";
        if (found->contains("endBehavior") && (*found)["endBehavior"].is_object())
            action = json_get_or((*found)["endBehavior"], "action",
                                 std::string{"nothing"});
        if (action != "nothing") return;

        // Advance to the next sibling in document order.
        std::vector<int> next_path = stopped_path;
        if (!next_path.empty()) {
            next_path.back()++;
            const std::string nxt = resolve_index_path_locked(next_path);
            if (!nxt.empty()) {
                next_to_arm = nxt;
            } else if (!was_manual) {
                // Fell off the end on a natural end → wrap to the first playable
                // item so a single GO restarts the show. A manual stop leaves
                // the arming empty (operator is holding the show).
                next_to_arm = first_playable_item_uuid_locked();
            }
        }
    }

    if (!next_to_arm.empty()) set_next_item_override(next_to_arm, /*manual=*/false);
}

void ProjectState::arm_first_item_on_open() {
    std::string first;
    {
        std::lock_guard lock{mutex_};
        if (document_.contains("settings") && document_["settings"].is_object()) {
            const auto& s = document_["settings"];
            if (s.contains("autoCueNextWithoutEndBehavior") &&
                s["autoCueNextWithoutEndBehavior"].is_boolean() &&
                !s["autoCueNextWithoutEndBehavior"].get<bool>()) return;
        }
        if (!next_item_override_.empty()) return;  // already armed
        // Don't clobber a rejoined running session.
        for (auto& [_, c] : cues_) {
            if (auto* pi = engine_.find_cue(c.id)) {
                const auto ts = pi->stats().transport;
                if (ts != audio::TransportState::Stopped) return;
            }
        }
        first = first_playable_item_uuid_locked();
    }
    if (!first.empty()) set_next_item_override(first, /*manual=*/false);
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

    // The Start Next marker already advanced the playlist while this cue was
    // still playing — firing the end behaviour now would trigger the next
    // item a second time.
    if (item.start_next_triggered) {
        Logger::playback("END BEHAVIOUR suppressed for '{}' "
                         "(Start Next marker already fired)", item.uuid);
        return;
    }

    // Read end-behaviour from the document.
    std::string      end_action;
    std::string      target_uuid;
    std::vector<int> target_index;   // index *path* through the item tree
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
            // targetIndex is an index *path* (array of ints) — see the client's
            // findItemByIndex. Read it element-by-element: eb.value<int>(...)
            // would throw type_error.302 because the stored value is an array,
            // which previously propagated out of the sequencer thread and
            // crashed the server whenever an item carrying a targetIndex ended.
            if (eb.contains("targetIndex") && eb["targetIndex"].is_array()) {
                for (const auto& v : eb["targetIndex"]) {
                    if (v.is_number_integer()) target_index.push_back(v.get<int>());
                }
            }
        }
    }

    Logger::playback("END BEHAVIOUR: item '{}' action='{}' targetUuid='{}' targetIndexLen={}",
                     item.uuid, end_action, target_uuid, target_index.size());

    if (end_action == "loop") {
        // Normally a looping cue never reaches here (the audio thread seeks
        // back to the in-point on EOF). This is the fallback path for when the
        // engine couldn't loop (e.g. a decoder that can't seek) — re-trigger
        // the same item so "loop" still loops, just with a gap.
        play_item(item.uuid);
    } else if (end_action == "next") {
        std::string next_uuid;
        {
            std::lock_guard lock{mutex_};
            // User-set override wins; consume it.
            if (!next_item_override_.empty()) {
                next_uuid = std::move(next_item_override_);
                next_item_override_.clear(); next_item_override_manual_ = false;
            } else {
                next_uuid = resolve_next_item_locked(item.uuid);
            }
        }
        // Don't restart a next item that's already on air (the operator
        // started it manually, or a Start Next / crossfade beat us to it).
        if (!next_uuid.empty()) {
            if (item_on_air(next_uuid)) {
                Logger::playback("END BEHAVIOUR next: item '{}' already "
                                 "on air — not restarting", next_uuid);
            } else {
                trigger_item(next_uuid);
            }
        }
    } else if (end_action == "goto-item" && !target_uuid.empty()) {
        trigger_item(target_uuid);
    } else if (end_action == "goto-index" && !target_index.empty()) {
        std::string idx_uuid;
        {
            std::lock_guard lock{mutex_};
            idx_uuid = resolve_index_path_locked(target_index);
        }
        if (idx_uuid.empty()) {
            Logger::warn("END BEHAVIOUR goto-index: index path did not resolve "
                         "to any item (item '{}')", item.uuid);
        } else {
            trigger_item(idx_uuid);
        }
    } else {
        // "nothing" (or unrecognized) end behaviour: no auto-advance, but the
        // server may still arm the next item as "Up Next" so the operator can
        // step through the list with a single GO (#28). A natural end wraps to
        // the top of the playlist at the end of the list.
        arm_next_after_stop(item.uuid, /*was_manual=*/false);
    }
}

} // namespace liveplay::core
