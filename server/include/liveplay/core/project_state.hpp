// ============================================================================
// liveplay/core/project_state.hpp
// ----------------------------------------------------------------------------
// The master state holder. Owns the high-level project document — cue list,
// cue metadata, mixer channel layout, routing matrix, master-bus device
// assignments — and provides JSON serialisation/deserialisation. The
// AudioEngine remains the authoritative source for *audio* state (what's
// currently playing, real-time meter values); ProjectState owns the
// *project document* (what cues exist, how they're wired up).
//
// On load(), the ProjectState walks the document and instructs the
// AudioEngine to mirror it (load cues, create mixer channels, wire routes).
//
// Legacy compatibility:
//   * `.liveplay` JSON projects from the 1.x client are accepted by load().
//     The translator maps the old "stereo file → mono speaker pair" routing
//     to the new matrix:  L→(default-device, hwCh 0), R→(default-device, hwCh 1).
//   * If the document already has a "v2" routing block (new schema), it's
//     used as-is.
// ============================================================================
#pragma once

#include "liveplay/audio/engine.hpp"
#include "liveplay/audio/types.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace liveplay::core {

using json = nlohmann::json;

struct CueMeta {
    audio::CueId             id;
    std::string              display_name;
    std::filesystem::path    file_path;
    std::string              artist;          // populated by TagLib (M4)
    std::string              title;           // populated by TagLib (M4)
    double                   duration_seconds = 0.0;
    float                    gain_db          = 0.0f;
    std::chrono::milliseconds fade_in_ms  {0};
    std::chrono::milliseconds fade_out_ms {0};
    bool                     ltc_enabled = false;
    int                      ltc_frame_rate_index = 4;     // 30 fps
    std::chrono::nanoseconds ltc_offset_ns{0};
};

struct MixerChannelMeta {
    audio::MixerChannelId id;
    std::string           display_name;
    float                 gain_db = 0.0f;
    bool                  muted   = false;
    bool                  soloed  = false;
};

struct RouteSendV2 {
    audio::ChannelIndex source_channel;
    audio::MixerChannelId destination_mixer;
    float gain_db;
};

struct MixerToMasterV2 {
    audio::MixerChannelId mixer;
    audio::MasterChannelIndex master_channel;
    float gain_db;
};

struct MasterAssignment {
    audio::MasterChannelIndex master_channel;
    audio::DeviceId device;
    audio::ChannelIndex hw_channel;
};

class ProjectState {
public:
    explicit ProjectState(audio::AudioEngine& engine);

    // Load a project file (.liveplay JSON). Returns true on success. On
    // failure, the previous state is preserved.
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    // Replace state from an in-memory JSON document. Same semantics as load.
    bool load_from_json(const json& doc);

    // Serialise the engine-facing portion (cues/mixers/routing).
    json to_json() const;

    // Return the full client-side project document — items, groups, cart,
    // theme, hotkeys, settings — as last seen / mutated. Includes engine
    // state where useful (cue_id linkage). Use this in /api/project so the
    // client can render the whole project from a single GET.
    json full_document() const;

    // Replace the full project document. The server extracts audio items
    // (uuid → file path) and re-mirrors them onto the engine. Other fields
    // are stored verbatim for the client to read back.
    bool replace_full_document(const json& doc);

    // Path to the open project file (.liveplay), if any. Empty if the project
    // is in-memory only.
    std::filesystem::path project_file_path() const;
    void set_project_file_path(std::filesystem::path p);

    // Clear everything and rewind the AudioEngine to an empty graph.
    void reset();

    // ---- High-level mutations --------------------------------------------
    // Wraps engine + state updates atomically so REST handlers can call a
    // single method.
    audio::CueId add_cue_from_file(const std::filesystem::path& file,
                                   std::string display_name = "");
    void remove_cue(const audio::CueId& id);
    void rename_cue(const audio::CueId& id, std::string new_name);
    void set_cue_gain_db(const audio::CueId& id, float db);
    void set_cue_fade_in (const audio::CueId& id, std::chrono::milliseconds d);
    void set_cue_fade_out(const audio::CueId& id, std::chrono::milliseconds d);
    void set_cue_ltc(const audio::CueId& id, bool enabled, int fps_index,
                     std::chrono::nanoseconds offset);

    // ---- Item-level operations (mirror the client's Project model) -------
    // Insert/update/remove an item in the document, and keep the engine in
    // sync for audio items. `parent_uuid` empty = top level; non-empty =
    // child of that group. Returns the cue_id of the newly engine-loaded
    // audio item, or empty for groups / on failure.
    audio::CueId add_item(const json& item, const std::string& parent_uuid = "");
    bool         update_item(const std::string& uuid, const json& patch);
    bool         remove_item(const std::string& uuid);

    // Reorder a top-level / inside-group sequence. `uuids` is the desired
    // ordering of items at the target level. `parent_uuid` empty = top-level.
    bool         reorder_items(const std::vector<std::string>& uuids,
                               const std::string& parent_uuid = "");

    // Find an audio item's file path by uuid (for engine play hookups etc.).
    std::optional<std::filesystem::path> resolve_item_path(const std::string& uuid) const;
    // Find the engine cue id associated with an item uuid.
    std::optional<audio::CueId> item_to_cue_id(const std::string& uuid) const;

    // Cart slot binding (slot → item uuid). Slot < 0 clears.
    bool set_cart_slot(int slot, const std::string& item_uuid);
    bool clear_cart_slot(int slot);

    // Theme + project settings patches. Each accepts a JSON object that's
    // shallow-merged into the corresponding section.
    bool patch_theme(const json& patch);
    bool patch_settings(const json& patch);

    // ---- Introspection ---------------------------------------------------
    std::vector<CueMeta> list_cues() const;
    std::optional<CueMeta> find_cue(const audio::CueId& id) const;

    std::vector<MixerChannelMeta> list_mixer_channels() const;

    std::filesystem::path media_root() const;
    void set_media_root(std::filesystem::path p);

    audio::AudioEngine& engine() noexcept { return engine_; }

private:
    audio::AudioEngine&        engine_;
    mutable std::mutex         mutex_;

    std::unordered_map<std::string, CueMeta>          cues_;
    std::unordered_map<std::string, MixerChannelMeta> mixers_;
    std::vector<RouteSendV2>       item_routes_;
    std::vector<MixerToMasterV2>   mixer_routes_;
    std::vector<MasterAssignment>  master_assignments_;

    std::filesystem::path media_root_ = std::filesystem::current_path() / "media";
    std::string           project_name_ = "Untitled";
    std::filesystem::path project_file_path_;

    // The full client-side project document (items, cart, theme, etc.). The
    // server treats most of this as opaque user data — only audio items have
    // engine-facing meaning, and they are mirrored into `cues_`. We keep the
    // whole document here so a remote client can fetch it back via GET.
    json document_;

    // uuid → engine cue id, for audio items currently loaded.
    std::unordered_map<std::string, audio::CueId> item_uuid_to_cue_;

    // Backward-compat translator: takes a legacy 1.x project document and
    // produces a v2-flavoured one with the equivalent routing matrix.
    json upgrade_legacy_document(const json& legacy) const;
    bool is_legacy_document(const json& doc) const;

    // Recognise the *client*-flavoured project document (camelCase, has
    // "items" with uuid/displayName, etc.) as written by the Electron client.
    bool is_client_document(const json& doc) const;

    // Build an initial empty document with default theme + empty sections.
    static json default_empty_document();

    // Extract every audio item from the client document and mirror it onto
    // the engine + cues_/item_uuid_to_cue_ tables. Used after load and after
    // replace_full_document.
    void mirror_items_to_engine_locked();

    // Walk the items tree calling `visit(item_json, parent_uuid)` for each.
    static void for_each_item(json& doc,
                              const std::function<void(json& /*item*/,
                                                       const std::string& /*parent_uuid*/)>& visit);

    // Re-apply the in-memory state to the AudioEngine (post-load or reset).
    void apply_to_engine_locked();
};

} // namespace liveplay::core
