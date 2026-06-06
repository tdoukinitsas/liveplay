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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
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
    std::string              ltc_start_timecode{"00:00:00:00"};
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

// Describes issues detected and fixed when loading a corrupt project.
struct RepairInfo {
    bool repaired = false;
    std::vector<std::string> issues;
};

class ProjectState {
public:
    explicit ProjectState(audio::AudioEngine& engine);
    ~ProjectState();

    // Load a project file (.liveplay JSON). Returns true on success. On
    // failure, the previous state is preserved.
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    // Replace state from an in-memory JSON document. Same semantics as load.
    bool load_from_json(const json& doc);

    // Returns and clears any repair info recorded during the last load /
    // load_from_json call. The repair has already been applied in memory;
    // call save() if the caller wants to persist it.
    RepairInfo consume_repair_info();

    // Re-run validation + repair on the in-memory document and return the
    // result. Useful when the caller wants to trigger an explicit repair
    // after the fact. Does NOT save to disk.
    RepairInfo repair_project();

    // Serialise the engine-facing portion (cues/mixers/routing).
    json to_json() const;

    // Return the full client-side project document — items, groups, cart,
    // theme, hotkeys, settings — as last seen / mutated. Includes engine
    // state where useful (cue_id linkage). Use this in /api/project so the
    // client can render the whole project from a single GET.
    json full_document() const;

    // Lightweight header for the project: everything *except* the items
    // tree. Lets the client paint the workspace shell (theme, settings,
    // cart slots, project name) before the (potentially large) items
    // array has even started downloading. `itemCount` is the number of
    // top-level items the client can expect from /api/project/items.
    json header_document() const;

    // Return a page of top-level items in document order. `offset` and
    // `limit` are clamped to the valid range. Items are decorated with
    // their server-side `cueId` exactly as `full_document()` does, and
    // groups carry their full `children` tree (groups are usually small;
    // we don't paginate within them).
    json items_page(std::size_t offset, std::size_t limit) const;

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
    // child of that group. `cart_only` routes the item into the document's
    // separate `cartOnlyItems` array (cart slots, not the playlist) so a
    // cart-bound cue never leaks into the playlist tree. Returns the cue_id
    // of the newly engine-loaded audio item, or empty for groups / on failure.
    audio::CueId add_item(const json& item, const std::string& parent_uuid = "",
                          bool cart_only = false);
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
    // Reverse lookup: which project item (if any) owns this engine cue?
    // Used by the WS handler to route low-level play(cue_id) calls through
    // play_item() so duckingBehavior / inPoint / endBehavior take effect.
    std::optional<std::string> cue_to_item_uuid(const audio::CueId& id) const;

    // Play an item by uuid, applying its duckingBehavior to currently-playing
    // cues. Returns false if the item is not loaded into the engine.
    //   * stop-all   → stop every other playing cue (honours their fade-out)
    //   * duck-others → lower other cues' gain by duckLevel (linear)
    //   * no-ducking → leave other cues alone
    // Also honours inPoint (seeks before play) and outPoint (engine returns to
    // the same fade-out path when the playhead reaches it).
    //
    // `fade_in_override_sec` (>= 0) replaces the item's own play-fade for this
    // play only — used by the crossfade path so the incoming cue fades in over
    // the crossfade window regardless of its configured playFade. The override
    // is transient (play() captures the fade duration synchronously, so the
    // stored value is restored immediately afterward).
    // `exclude_from_ducking`, when non-empty, is a cue that this play's ducking
    // must leave untouched — used by the crossfade so the outgoing cue keeps the
    // engine-owned fade the sequencer already started instead of being hard-cut.
    bool play_item(const std::string& uuid,
                   double fade_in_override_sec = -1.0,
                   const audio::CueId& exclude_from_ducking = audio::CueId{});
    bool stop_item(const std::string& uuid);

    // Trigger an item by uuid: audio items go through play_item; group items
    // are dispatched per their startBehavior (play-first / play-all),
    // mirroring the client's triggerGroup() logic. The crossfade args are
    // forwarded to play_item (and to recursive group triggers).
    bool trigger_item(const std::string& uuid,
                      double fade_in_override_sec = -1.0,
                      const audio::CueId& exclude_from_ducking = audio::CueId{});

    // Resolve an index path (array of child indices) to an item uuid,
    // descending into group `children` at each level. Mirrors the client's
    // findItemByIndex / endBehavior.targetIndex semantics: e.g. {1, 11} means
    // top-level item 1 (the 2nd item — a group), then its child 11 (the 12th).
    // Returns an empty string if the path is empty or out of range.
    // Thread-safe wrapper around the internal resolver.
    std::string item_uuid_by_index(const std::vector<int>& path) const;

    // User-set "Up Next" override. When the currently-playing item ends with
    // endBehavior.action == "next", this uuid is consumed (and cleared)
    // before falling back to the static sibling order. Pass empty string to
    // clear without consuming. Safe to call at any time.
    void set_next_item_override(const std::string& uuid);
    // Current "Up Next" override, or empty if none. Used by the control
    // server to seed newly-connected clients with the live override state.
    std::string next_item_override() const;

    // ---- Per-device routing ---------------------------------------------
    // Ensure a "device mixer" exists for `device_name` (the user-visible
    // name from /api/devices). Opens the audio device if needed, creates a
    // dedicated mixer + two master channels routed to it. Returns the
    // mixer id, or empty on failure (device not found).
    audio::MixerChannelId ensure_device_routing(const std::string& device_name);

    // Route a cue's first two source channels to the given mixer, removing
    // any prior item-to-mixer routes for this cue (so the cue plays through
    // exactly one device's mixer).
    void route_cue_to_mixer(const audio::CueId& cue,
                            const audio::MixerChannelId& mixer);

    // ---- Preview --------------------------------------------------------
    // Play an item through the configured preview device (project
    // settings.previewDevice). This is independent of the main project
    // playback — the user can preview one cue while another plays through
    // the main outputs (DJ-style pre-listen). At most one preview is
    // active at a time; starting a new one replaces the old. Returns true
    // on success.
    bool start_preview(const std::string& item_uuid);
    bool stop_preview();
    // uuid of the currently-previewing item, or empty if no preview active.
    std::string current_preview_item_uuid() const;
    // Engine cue ID for the active preview, or an empty CueId if none.
    audio::CueId current_preview_cue_id() const;

    // Route every LTC-enabled cue's synthetic LTC source channel to the
    // project's configured ltcDevice mixer. Safe to call at any time without
    // holding mutex_ — it acquires the lock internally as needed and delegates
    // engine operations to independently-locked engine APIs.
    void apply_ltc_device_routing();

    // Re-route all cues that have no per-item deviceOverride to the project's
    // configured defaultOutputDevice mixer. Called when defaultOutputDevice
    // changes in settings (including during playback).
    void apply_default_device_routing();

    // Stop any active preview and reset preview state so the next call to
    // start_preview() picks up the updated previewDevice setting.
    void apply_preview_device_change();

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

    // Snapshot of current playback state for crash-resume purposes.
    struct PlaybackSnapshot {
        std::string project_file;   // empty if no project is open
        std::string item_uuid;      // empty if nothing is playing
        double      position_sec = 0.0;
    };
    // Thread-safe; safe to call from the heartbeat loop every 200 ms.
    PlaybackSnapshot current_playback_snapshot() const;

    // Audio-load progress accessors — the document JSON arrives instantly,
    // but the engine load can take seconds for big projects. The client
    // shows a progress bar based on these.
    bool         audio_loading() const noexcept { return loading_audio_.load(); }
    std::size_t  audio_loaded_count() const noexcept { return load_progress_loaded_.load(); }
    std::size_t  audio_total_count()  const noexcept { return load_progress_total_.load(); }

private:
    audio::AudioEngine&        engine_;
    mutable std::mutex         mutex_;
    std::mutex                 mirror_mutex_;

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

    // Repair info from the last load. Consumed by consume_repair_info().
    RepairInfo pending_repair_info_;

    // uuid → engine cue id, for audio items currently loaded.
    std::unordered_map<std::string, audio::CueId> item_uuid_to_cue_;

    // Pending "Up Next" override set by the user mid-playback. Consumed and
    // cleared when the currently-playing item's end-behavior fires "next".
    // Guarded by mutex_.
    std::string next_item_override_;

    // Background "audio mirror is still in progress" state. Exposed to
    // clients via /api/project so the UI can show a progress bar without
    // blocking on the mirror finishing.
    std::atomic<bool>        loading_audio_{false};
    std::atomic<std::size_t> load_progress_loaded_{0};
    std::atomic<std::size_t> load_progress_total_{0};
    std::thread              load_thread_;

    // Per-device routing infrastructure: each unique output device name
    // referenced by any item's deviceOverride gets its own mixer + a pair
    // of master channels wired to that device. Indexed by device display
    // name. The default device's entry is the "Main" mixer + masters 0/1
    // that the engine sets up automatically.
    struct DeviceRouting {
        audio::DeviceId            device;
        audio::MixerChannelId      mixer;
        audio::MasterChannelIndex  master_l;
        audio::MasterChannelIndex  master_r;
    };
    std::unordered_map<std::string, DeviceRouting> device_routings_;
    // Next free master channel pair when allocating new device routings.
    // Default device occupies 0/1; preview occupies 30/31; overrides start
    // at 2 and increment by 2.
    audio::MasterChannelIndex next_override_master_ = 2;

    // Preview state. The preview infrastructure is opened lazily on first
    // preview request, then re-used (cheaper than reopening the audio
    // device every time the user pre-listens to a new cue).
    audio::CueId           preview_cue_;
    std::string            preview_item_uuid_;
    audio::MixerChannelId  preview_mixer_;
    audio::DeviceId        preview_device_;
    std::string            preview_device_name_;   // last opened, for cleanup on change

    // ---- Sequencer: server-side auto-advance, crossfade, ducking restore ----
    struct DuckedEntry {
        audio::CueId cue_id;
        float        original_gain_db;
    };
    // A scheduled custom action: at `time_point` seconds (absolute time
    // within the audio file, before in_point trimming), do `action`. Driven
    // by the sequencer alongside crossfade / stop-fade.
    struct ScheduledCustomAction {
        double  time_point  = 0.0;
        json    action;       // copy of the project document's action node
        bool    triggered    = false;
    };
    struct SequencedItem {
        std::string              uuid;
        audio::CueId             cue_id;
        double                   crossfade_sec       = 0.0;
        double                   stop_fade_sec       = 0.0;
        double                   effective_end       = 0.0;
        bool                     crossfade_triggered = false;
        bool                     stop_fade_triggered = false;
        std::vector<DuckedEntry> ducked;
        std::vector<ScheduledCustomAction> custom_actions;
    };
    std::vector<SequencedItem> sequenced_items_;
    std::mutex                 sequencer_mutex_;
    std::thread                sequencer_thread_;
    std::atomic<bool>          sequencer_running_{false};

    void start_sequencer();
    void stop_sequencer();
    void sequencer_loop();
    void handle_item_ended(const SequencedItem& item);
    void execute_custom_action(const json& action);

public:
    // Subscribe to "external" custom actions the server can't perform on its
    // own — currently just http-request. The control server installs a
    // handler that broadcasts the action as a doc_patch so a connected
    // client makes the actual fetch. Keeps a libcurl/winhttp dependency
    // out of the server.
    void set_external_action_handler(std::function<void(const json&)> handler);

private:
    std::function<void(const json&)> external_action_handler_;

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

    // Run mirror_items_to_engine_locked() asynchronously on a worker thread.
    // The caller's mutex is released first. Sets loading_audio_=true for the
    // duration. Used by load() so the document is returned to the client
    // immediately and audio loading proceeds in the background.
    void start_async_mirror();

    // Walk the items tree calling `visit(item_json, parent_uuid)` for each.
    static void for_each_item(json& doc,
                              const std::function<void(json& /*item*/,
                                                       const std::string& /*parent_uuid*/)>& visit);

    // Return the uuid of the item that should play *after* `current_uuid`,
    // based on its endBehavior. Empty string if there isn't a deterministic
    // next item (e.g. "nothing"). Used for cache pre-warming. Caller must
    // hold mutex_.
    std::string resolve_next_item_locked(const std::string& current_uuid) const;

    // Resolve an index *path* (array of child indices, as produced by the
    // client's endBehavior.targetIndex / findItemByIndex) to the uuid of the
    // item it points at. Descends into group `children` at each level. Empty
    // string if the path is out of range. Caller must hold mutex_.
    std::string resolve_index_path_locked(const std::vector<int>& path) const;

    // Re-apply the in-memory state to the AudioEngine (post-load or reset).
    void apply_to_engine_locked();

    // Point media_root_ at the current project's "media" subfolder, derived
    // from document_["folderPath"]. This keeps every uploaded / copied media
    // file inside the project folder so the project stays fully portable, and
    // guarantees the server never reads or writes media outside that folder.
    // No-op when folderPath is empty (unsaved project). Caller must hold mutex_.
    void update_media_root_from_folder_locked();
};

} // namespace liveplay::core
