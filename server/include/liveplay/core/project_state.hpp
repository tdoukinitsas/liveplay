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
#include "liveplay/core/output_map.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    // Was `soloed`. Solo is gone (§2.4); documents that recorded it load their
    // value into this field and are written back as `pfl`.
    bool                  pfl     = false;
};

// ---------------------------------------------------------------------------
// Buses. A bus is the user-facing name for an engine mixer strip: items and
// groups are assigned to one, and the bus alone decides where that audio goes.
// Definitions live in the project document and are materialised onto engine
// mixer channels on load.
//
// Where a bus feeds is always expressed logically — either the master bus,
// another bus, or a *named* output like "FOH". The name→hardware binding lives
// on the server, never in the document, so a show stays portable between
// venues. See BUS_ARCHITECTURE.md.
// ---------------------------------------------------------------------------
enum class BusOutputKind {
    Master,   // into the master bus (inherits the master limiter)
    Bus,      // submix feeding another bus
    Output,   // direct to a named logical output
};

// The strip's tone controls, as the project stores them.
//
// There is no separate in/out switch for the filters. A high-pass parked at
// the bottom of its range and a low-pass parked at the top are out of circuit,
// which is what the knob's origin already means on the surface and what a
// console's "park it and forget it" position does. It also removes a control
// that could disagree with the knob beside it.
struct BusFilter {
    float freq_hz = 0.0f;    // 0 = never set; falls back to the parked value
    float q       = 0.70710678f;
};

// One EQ band. The two middle bands are always bells; the outer two can be
// switched to shelves, which is the conventional four-band console layout.
//
// A band sitting at 0 dB is out of circuit, for the same reason a parked
// filter is: both a peaking section and a shelf are identities at unity gain,
// so running one would cost arithmetic to achieve nothing and four flat bands
// on every strip must not colour the desk.
//
// `q` and `slope` are separate fields because they are different quantities —
// the width of a bell versus the steepness of a shelf's transition — with
// barely overlapping useful ranges. One shared field would mean switching a
// band to shelf and back silently changed the bell, which is the same thing
// the section bypasses exist to avoid.
//
// Which END a shelf turns up is not stored: it follows from the band's
// position, so band 0 is a low shelf and band 3 a high one. Storing it would
// allow a low shelf on the HF band, which is a way of building a broken EQ
// rather than a feature.
struct BusEqBand {
    float freq_hz = 1000.0f;
    float gain_db = 0.0f;
    float q       = 1.0f;
    bool  shelf   = false;   // honoured only on the outer bands
    float slope   = 1.0f;    // shelf steepness; 1 is the steepest without peaking
};

inline constexpr std::size_t kBusEqBands = 4;

// The expander / gate, as the project stores it. Defaults match what the
// surface shows so a fresh bus does not look pre-adjusted.
struct BusGate {
    float threshold_db = -40.0f;
    float ratio        = 2.0f;
    float range_db     = -20.0f;
    float attack_ms    = 1.0f;
    float hold_ms      = 10.0f;
    float release_ms   = 100.0f;
};

// The compressor / limiter, as the project stores it. Defaults match what the
// surface shows so a fresh bus does not look pre-adjusted.
//
// `attack` clamps down and `release` recovers — the opposite sense to BusGate's
// pair of the same name, which is why they are separate structs rather than one
// shared "dynamics" record.
struct BusComp {
    float threshold_db = -18.0f;
    float ratio        = 4.0f;
    float makeup_db    = 0.0f;
    float attack_ms    = 10.0f;
    float knee_db      = 6.0f;
    float release_ms   = 200.0f;
};

// Stereo image width, as the project stores it.
//
// No in/out switch, for the same reason the filters have none: width 1 with the
// bass-mono filter parked at the bottom of its travel IS out of circuit, and a
// switch that could disagree with the knob beside it would be one control too
// many. Ignored entirely on a mono bus, which has no image to widen.
struct BusWidth {
    float width        = 1.0f;    // 0 mono .. 1 unity .. 2 wide
    float bass_mono_hz = 20.0f;   // parked at the bottom: out of circuit
    float bass_mono_q  = 0.70710678f;
};

struct BusDsp {
    // Section bypass. Separate from a band being flat: bypass takes the whole
    // section out in one press and, crucially, PUTS IT BACK exactly as it was.
    // Zeroing four gains to compare against flat loses the settings you were
    // comparing, which is the entire reason a desk has an in/out button per
    // section rather than expecting you to undo your way back.
    bool      eq_enabled  = true;
    bool      dyn_enabled = true;
    // Whether each processor is in circuit at all. Separate from the section
    // bypass, so a strip can run the compressor with the gate out — which is
    // the common case on a bus.
    bool      gate_on     = false;
    bool      comp_on     = false;
    BusGate   gate;
    BusComp   comp;
    BusWidth  width;
    BusFilter hpf{20.0f};       // parked at the bottom: out of circuit
    BusFilter lpf{20000.0f};    // parked at the top: out of circuit
    // Conventional four-band starting layout, matching what the surface shows.
    std::array<BusEqBand, kBusEqBands> eq{{
        {100.0f,   0.0f, 0.7f},
        {500.0f,   0.0f, 1.0f},
        {2500.0f,  0.0f, 1.0f},
        {10000.0f, 0.0f, 0.7f},
    }};
};

// Where the filters sit when they are doing nothing. Shared with the client,
// which draws the same parked positions on its knobs.
inline constexpr float kHpfParkedHz = 20.0f;
inline constexpr float kLpfParkedHz = 20000.0f;

// Merge a JSON "dsp" object into a BusDsp, leaving anything it does not
// mention alone. One implementation because four callers need exactly this —
// loading a document, patching a bus, the live drag endpoint, and the REST
// list — and a partial update has to be partial in all of them.
void merge_bus_dsp(const json& src, BusDsp& out);
json bus_dsp_to_json(const BusDsp& d);

struct BusDef {
    std::string   id;
    std::string   display_name;
    std::string   color;
    int           order      = 0;
    int           width      = 2;       // 1 = mono, 2 = stereo. See §2.5.
    float         gain_db    = 0.0f;
    bool          muted      = false;
    // Position of a MONO bus between the two lanes of a stereo destination:
    // -1 hard left, 0 centre, +1 hard right. Ignored when width == 2 — see
    // §2.5.3: pan belongs to a mono->stereo send, not to a strip, and a
    // stereo bus wants balance, which is deferred.
    float         pan        = 0.0f;
    BusDsp        dsp;
    BusOutputKind output_kind = BusOutputKind::Master;
    std::string   output_target;        // bus id, or logical output name
    // System buses (Main, Monitor) are created implicitly and cannot be
    // deleted or renamed away — the document may still carry their level.
    bool          system     = false;
};

// The always-present buses. Main is where everything lands by default;
// Monitor is the PFL / pre-listen destination.
inline constexpr const char* kMainBusId    = "main";
inline constexpr const char* kMonitorBusId = "monitor";

// The logical output Monitor targets unless the operator moves it. A name,
// like every other bus output — the server's output map says which hardware
// it means here, so a show that uses PFL is still portable.
//
// Deliberately not the master: PFL summing into the house is the accident
// §2.4 exists to prevent, so Monitor is not allowed to target it at all.
//
// Monitor is also the pre-listen bus. The engine has always reserved the top
// pair of master channels for DJ-style cue preview; Monitor owns that pair
// now, and cue pre-listen routes into this strip rather than a "Preview" strip
// of its own. So PFL'ing a bus and pre-listening a cue sum in one pair of
// headphones, under one fader, on one meter — which is what a desk does, and
// what §2.4 said should eventually happen to the reserved pair.
inline constexpr const char* kMonitorOutputName = "Monitor";

struct RouteSendV2 {
    audio::ChannelIndex source_channel;
    audio::MixerChannelId destination_mixer;
    float gain_db;
    // Destination strip lane (0 = L, 1 = R); kAllMixerLanes = every lane.
    // Documents written before lanes existed load as kAllMixerLanes, which
    // reproduces the old mono-bus behaviour.
    audio::ChannelIndex lane = audio::kAllMixerLanes;
};

struct MixerToMasterV2 {
    audio::MixerChannelId mixer;
    audio::MasterChannelIndex master_channel;
    float gain_db;
    // Source strip lane feeding the master; kAllMixerLanes = sum of lanes.
    audio::ChannelIndex lane = audio::kAllMixerLanes;
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
    ProjectState(audio::AudioEngine& engine, OutputMap& outputs);
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
    //
    // The returned CueId is valid immediately, but the audio decode happens on
    // a background loader thread: adding a large or network-mounted file must
    // never stall play_item / stop / state requests (#43). The cue appears in
    // list_cues()/find_cue() straight away; the engine's PlaybackItem shows up
    // when the decode finishes. play_item() waits for a still-loading cue's own
    // decode (and only that one), so an immediate play after add still works.
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

    // Stop every cue for the global "Stop All" command. When `fade_ms` is
    // provided it is used directly; when omitted the project-wide
    // settings.stopAllFadeMs (default 1000 ms) applies. The resolved fade wins
    // over every per-track fade-out (global fade always wins); a resolved fade
    // of 0 is an instant panic stop.
    void stop_all_cues(std::optional<long long> fade_ms = std::nullopt);

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
    //
    // `manual` records WHO armed it. An operator-set arming (the default —
    // every client/REST call means the operator picked it) is sticky: the
    // server's own auto-arming never clobbers it. An arming the server derived
    // itself (arm_next_after_stop / arm_first_item_on_open) passes false, so a
    // later auto-arm may replace it once it goes stale. Without this
    // distinction, an auto-arming left over from "open project" or from an
    // earlier stop blocked ALL subsequent auto-arming — e.g. jumping into a
    // group left "Up Next" stuck on the old item and never armed the group's
    // 2nd child when its 1st finished.
    void set_next_item_override(const std::string& uuid, bool manual = true);
    // Current "Up Next" override, or empty if none. Used by the control
    // server to seed newly-connected clients with the live override state.
    std::string next_item_override() const;

    // ---- Show-control surface (shared operator UI state) -----------------
    // The server owns the operator-facing UI state that every client AND
    // every control surface has to agree on: which playlist item is selected,
    // whether Show Mode is engaged, and the display locale. Clients push
    // changes here and re-render from the broadcast, so a Companion button and
    // the on-screen playlist can never disagree about what is selected.
    // Each setter returns true when the value actually changed (no-ops are
    // never broadcast) and fans the change out via the ui_state_broadcaster.

    // uuid of the selected playlist item, or empty when nothing is selected.
    std::string selected_item_uuid() const;
    // Select an item by uuid. Pass an empty string to clear the selection.
    bool set_selected_item(const std::string& uuid);
    // Move the selection `delta` places through the flattened playlist — the
    // same depth-first order (group node, then its children) that the client's
    // Select Up / Select Down keys walk. The ends clamp rather than wrap.
    // Returns the newly selected uuid, or empty when the playlist has no items.
    //
    // With nothing selected, `anchor_candidates` (the items currently playing)
    // supplies the notional current position and the step is taken from there,
    // so a control surface pressing Select Down mid-show lands on the cue
    // *after* the one that's playing rather than jumping back to the top of the
    // playlist. When several are playing we take the one furthest down the
    // playlist — a show advances downward, so that's the operator's position.
    // With nothing selected and nothing playing, the step starts from the top.
    std::string step_selection(int delta,
                               const std::vector<std::string>& anchor_candidates = {});

    // Show Mode: the simplified, touch-friendly playback view. Server-owned so
    // a Companion button, a tablet and the operator's laptop stay in step.
    bool show_mode() const;
    void set_show_mode(bool enabled);
    bool toggle_show_mode();   // returns the resulting state

    // Display locale (a code from the client's locale set, e.g. "en", "el").
    // Mirrored so control surfaces can label their own buttons in the
    // operator's language.
    std::string ui_locale() const;
    void set_ui_locale(const std::string& code);

    // Install a callback invoked whenever any of the above changes. Called
    // with a complete doc_patch payload, with no ProjectState lock held. The
    // control server installs this to fan the change out to every client.
    void set_ui_state_broadcaster(std::function<void(const json&)> cb);

    // ---- External-control surface (Bitfocus Companion, custom remotes) ----
    // Compact machine-readable transport summary: project header facts, every
    // on-air item (name, colour, index path, elapsed/remaining), the effective
    // "Up Next" target, selection + Show Mode, master gain/limiter state and
    // cart-slot bindings. Built for polling-free control surfaces — fetch once
    // on connect, then keep it fresh from the /ws push messages (cue_state,
    // meters, doc_patch).
    json state_summary() const;

    // GO: trigger whatever is armed as "Up Next". Uses the user-set override
    // when present (consumed on success), otherwise derives the target from
    // the currently-playing item's endBehavior — mirroring the client's GO
    // button. Returns the uuid triggered, or empty if nothing was armed /
    // derivable / loaded.
    std::string go();

    // Item uuid bound to a cart slot, or empty if the slot is unbound.
    std::string cart_slot_item_uuid(int slot) const;

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

    // Send each of these items' loaded cues to whatever bus they now resolve
    // to. Caller must NOT hold mutex_ (this routes through the engine).
    //
    // Routing used to be established only in play_item(), so re-assigning a
    // cue's bus did nothing until the next time it was fired — the mixer said
    // one thing and the audio did another. Pass a group's uuid to move every
    // audio descendant that inherits from it.
    void reroute_items_to_buses(const std::vector<std::string>& item_uuids);

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

    // Bus definitions in display order, each with the uuids of the items that
    // resolve to it (an item's own busId, else the nearest ancestor group's,
    // else Main). The membership list is what the mixer's channel-detail view
    // shows as "what feeds this bus".
    struct BusInfo {
        BusDef                   def;
        audio::MixerChannelId    mixer;      // empty when not materialised
        std::vector<std::string> item_uuids;
        // Live, not persisted — read from the strip. See list_buses().
        bool                     pfl = false;
        // Whether this bus actually reaches hardware. The UI cannot work this
        // out from the output map alone: Monitor may be bound through
        // settings.previewDevice, which is not in the map, and flagging that
        // as unmapped would put a warning on a bus that is working.
        bool                     bound = false;
        // Monitor only, and live rather than persisted: the mono-sum audition
        // (see set_monitor_mono). False on every other bus.
        bool                     mono_check = false;
    };
    std::vector<BusInfo> list_buses() const;

    // ---- Bus mutation ----------------------------------------------------
    // All three write document_["buses"] so the change survives a save, and
    // touch only the affected strip so other buses keep playing.
    std::optional<BusDef> create_bus(const json& spec);
    // Refused is distinct from NotFound because the one thing a caller may be
    // told no about — pointing Monitor at the master — is a deliberate rule,
    // and reporting it as "no such bus" would send whoever hit it looking for
    // the wrong problem.
    enum class PatchBusResult { Ok, NotFound, Refused };
    PatchBusResult patch_bus(const std::string& id, const json& patch);
    // Refuses the system buses. Items assigned to the deleted bus fall back to
    // Main by having their busId cleared.
    bool delete_bus(const std::string& id);

    // Live pan, for the duration of a drag: moves the send gains without
    // writing the document or broadcasting, the way the strip-level gain and
    // mute endpoints do. The client persists the final value with patch_bus
    // when the gesture settles.
    bool set_bus_pan_live(const std::string& id, float pan);

    // Live tone controls, for the duration of a knob drag: pushes coefficients
    // straight at the strip without writing the document or broadcasting, the
    // same shape as set_bus_pan_live. The client PATCHes the settled value.
    bool set_bus_dsp_live(const std::string& id, const json& dsp);

    // Turn a bus's stored tone controls into engine parameters. Public because
    // the shape of StripDspParams is the engine's, not the document's, and
    // both the materialise path and the live path need the same translation.
    //
    // Not static: the Monitor bus's parameters depend on the live mono-check
    // flag as well as on what the document says.
    audio::StripDspParams dsp_params_for(const BusDef& bus) const;

    // ---- Mono-sum audition -----------------------------------------------
    // Fold the Monitor bus to mono, so what is in the phones can be checked
    // for mono compatibility without touching the house. PFL a bus, press
    // this, and anything that cancels when summed will audibly drop out.
    //
    // Implemented as the Monitor strip's own width control forced to 0, which
    // is exactly a mono fold, so it costs no new DSP and glides over ~20 ms
    // instead of clicking.
    //
    // Deliberately amplitude-preserving rather than kDefaultDownmixDb: at
    // width 0 both lanes become (L+R)/2, so mono-compatible material — the
    // common case, and the case you are checking against — does not change
    // level at all. A power-preserving fold would make the check 3 dB louder
    // on exactly that material, and a louder A/B always sounds better. The
    // -3 dB constant still governs actually ROUTING a stereo bus to a mono
    // output (§2.5.2); this is an audition, which is a different question.
    //
    // Not persisted, for the same reason PFL is not: it is what the operator
    // is listening to right now, not part of the show.
    bool set_monitor_mono(bool on);
    bool monitor_mono() const noexcept {
        return monitor_mono_.load(std::memory_order_relaxed);
    }

    // ---- PFL -------------------------------------------------------------
    // Raise or lower pre-fade listen on a bus: a pre-fader, pre-mute tap into
    // the Monitor bus. Nothing else changes — the house mix is untouched and
    // several buses can be PFL'd at once, which is the point of PFL over solo.
    //
    // Not written to the document. PFL is what the operator is listening to
    // right now, not part of the show, so it does not survive a reload.
    // Refused for the Monitor bus itself, which is the destination.
    bool set_bus_pfl(const std::string& id, bool on);
    // Drop PFL everywhere. Returns how many buses were cleared.
    std::size_t clear_all_pfl();

    // Re-wire the buses an output-map edit actually moved, and only those.
    // Call after OutputMap changes: a bus already pointing at "FOH" keeps the
    // routing it was wired with otherwise, so remapping FOH would appear to do
    // nothing until the project was reloaded.
    //
    // Deliberately narrow. Rewiring every bus would drop audio on buses the
    // edit did not touch, which is not acceptable mid-show, so each bus is
    // re-resolved and left alone unless its channels differ from what it was
    // wired to. Returns the number of buses moved.
    std::size_t rewire_buses_for_output_map();

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
    // Logical output name → hardware, owned by the server (see output_map.hpp).
    OutputMap&                 outputs_;
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
    // True when next_item_override_ was set by the operator rather than derived
    // by the server. See set_next_item_override(). Guarded by mutex_.
    bool next_item_override_manual_{false};

    // Shared operator UI state (see the Show-control surface above). All
    // guarded by mutex_.
    std::string selected_item_uuid_;
    bool        show_mode_ = false;
    std::string ui_locale_ = "en";

    // Trigger ordering: every play_item() stamps the item with the next value
    // of trigger_seq_counter_, so control surfaces can tell which of several
    // on-air items was fired LAST (what an operator means by "currently
    // playing") rather than which sits highest in the playlist. Guarded by
    // mutex_; entries are dropped when the project resets.
    std::unordered_map<std::string, long long> item_trigger_seq_;
    long long                                  trigger_seq_counter_ = 0;

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

    // ---- Buses -----------------------------------------------------------
    // Definitions as loaded from the document, in display order, and the
    // engine mixer strip each one was materialised onto. Both guarded by
    // mutex_ and rebuilt whenever the document is loaded or replaced.
    std::vector<BusDef> buses_;

    // What a bus was materialised onto. A bus keeps its master pair for its
    // lifetime, so changing where it outputs re-points those masters rather
    // than reserving a fresh pair each time.
    struct BusRouting {
        audio::MixerChannelId     mixer;
        audio::MasterChannelIndex master_l    = 0;
        audio::MasterChannelIndex master_r    = 0;
        bool                      has_masters = false;
        // What the bus's logical output name resolved to when it was wired.
        // Kept so an output-map edit can rewire only the buses the edit
        // actually moved, instead of interrupting every bus on the desk.
        // Empty for Master-kind buses, which do not consult the map.
        std::vector<OutputMap::Channel> wired_channels;
        // The Monitor bus sits on the master pair the engine reserves at the
        // top of the bus, not on one drawn from the pool. Flagged so unwiring
        // releases the routing without handing that pair out to a bus that
        // would then be sharing the operator's headphones.
        bool                      reserved_pair = false;
    };
    std::unordered_map<std::string, BusRouting> bus_routings_;

    // Bus ids already reported as unknown, so an item pointing at a bus that
    // no longer exists is logged once rather than on every save and every
    // mixer poll. Cleared by load_buses_locked(). Guarded by mutex_.
    mutable std::unordered_set<std::string> warned_unknown_buses_;

    // Master pairs handed back by deleted or rewired buses. Reused before the
    // monotonic counter grows — without this, repeatedly changing a bus's
    // output would walk the counter into the preview reserve and exhaust it.
    std::vector<audio::MasterChannelIndex> free_master_pairs_;

    // The mono-sum audition. Atomic rather than mutex-guarded because
    // dsp_params_for reads it while building parameters and takes no lock.
    // Never written to the document — see set_monitor_mono.
    std::atomic<bool> monitor_mono_{false};

    // Cue ids whose decoder has already been primed, so priming happens once
    // per cue rather than once per mirror. The mirror runs on every save, and
    // priming is a seek plus a two-second decode per cart binding — a real
    // load spike mid-show, for cues that were already hot. Guarded by mutex_;
    // entries are dropped by forget_primed_cue_locked() when a cue is
    // unloaded, so a replaced file is primed again.
    std::unordered_set<std::string> primed_cues_;
    void forget_primed_cue_locked(const audio::CueId& id) {
        primed_cues_.erase(id.value);
    }

    // Read document_["buses"] into buses_, synthesising the system buses and
    // migrating legacy per-item deviceOverride values into real buses. Caller
    // holds mutex_.
    void load_buses_locked();
    // One-way conversion of the pre-bus per-item `deviceOverride` field into
    // buses. Caller holds mutex_; runs as part of load_buses_locked().
    void migrate_device_overrides_locked();
    // Create an engine mixer strip per bus and wire its output. Caller must
    // NOT hold mutex_ (engine calls take their own locks).
    void materialise_buses();
    // Serialise buses_ back into document_["buses"]. Caller holds mutex_.
    void write_buses_to_document_locked();
    // Resolve an item's effective bus by walking up its group ancestry.
    // Returns the Main bus when nothing along the chain assigns one.
    std::string resolve_item_bus(const std::string& item_uuid) const;
    // Engine strip for a bus id, or empty if unknown / not materialised.
    audio::MixerChannelId mixer_for_bus(const std::string& bus_id) const;
    // Reserve the next free pair of master channels below the preview reserve.
    // False when the bus is exhausted. Caller holds mutex_.
    bool allocate_master_pair_locked(audio::MasterChannelIndex& l,
                                     audio::MasterChannelIndex& r);
    // Return a pair to the pool for reuse. Caller holds mutex_.
    void release_master_pair_locked(audio::MasterChannelIndex l);
    // Connect a materialised strip to wherever its bus says it goes, reserving
    // a master pair if the destination needs one. Caller must NOT hold mutex_.
    void wire_bus(const BusDef& bus, BusRouting& routing);
    // Monitor's own wiring: the reserved master pair, bound to whatever this
    // machine calls the headphone output. Split out because it is the one bus
    // whose master channels are fixed rather than allocated.
    void wire_monitor_bus(const BusDef& bus, BusRouting& routing);
    // Where the headphones are on this machine. The logical output map answers
    // first — that is what keeps a show portable — and settings.previewDevice
    // is the legacy answer every existing project already carries. Empty when
    // neither is configured, in which case Monitor is valid and silent.
    std::vector<OutputMap::Channel> resolve_monitor_channels(
            const std::string& logical_name) const;
    // Re-issues just the two mixer->master sends that carry a mono bus's pan.
    // Separate from wire_bus because panning must not tear the routing down:
    // route_mixer_to_master replaces an existing send in place, so a pan drag
    // never drops audio or churns the device assignment.
    void apply_bus_pan(const BusDef& bus, const BusRouting& routing);
    // Drop every master route and assignment the bus holds and return its
    // pair to the pool. Caller must NOT hold mutex_.
    void unwire_bus(BusRouting& routing);
    // Next free master channel pair when allocating new device routings.
    // Default device occupies 0/1; preview occupies the top pair of the bus
    // (see audio::preview_master_base); overrides start here and step by 2.
    static constexpr audio::MasterChannelIndex kFirstOverrideMaster = 2;
    audio::MasterChannelIndex next_override_master_ = kFirstOverrideMaster;

    // Unwire and forget every per-device override routing. Caller holds mutex_.
    void release_device_routings_locked();

    // Preview state. There is no longer a preview mixer or a separately-opened
    // preview device: pre-listen routes into the Monitor bus, which owns the
    // reserved master pair and is wired when the project is materialised. What
    // is left is just which cue is being auditioned.
    audio::CueId           preview_cue_;
    std::string            preview_item_uuid_;

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
        // "Start Next" segue marker: when the playhead crosses
        // start_next_time (absolute seconds within the file, same convention
        // as ScheduledCustomAction::time_point), the next item starts at its
        // own volume/fades while this cue keeps playing. 0 = disabled.
        // start_next_fade_sec > 0 additionally begins this cue's fade-out at
        // the marker. Once triggered, the item's own end-behaviour advance is
        // suppressed (the playlist already moved on).
        double                   start_next_time      = 0.0;
        double                   start_next_fade_sec  = 0.0;
        bool                     start_next_triggered = false;
        // End-behaviour snapshot, used by the seamless-advance pre-roll so the
        // next cue can be started a hair before this one's out-point (no
        // audible gap) for auto-advancing behaviours with no crossfade / stop-
        // fade / start-next marker. Populated at play_item() time.
        std::string              end_action;            // next / goto-item / goto-index / loop / nothing
        std::string              goto_target_uuid;      // goto-item target
        std::vector<int>         goto_target_index;     // goto-index path
        bool                     advance_triggered    = false;
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

    // Resolve the uuid the given item should auto-advance to, for its
    // endBehavior (next / goto-item / goto-index only; empty for loop /
    // nothing / no target). For "next" this consumes the user-set "Up Next"
    // override exactly once, mirroring handle_item_ended. Takes its own locks —
    // call with no lock held. Used by the sequencer's seamless-advance pre-roll.
    std::string resolve_advance_target(const SequencedItem& item);

    // Server-authoritative "Up Next" arming for cues with no end behaviour
    // (endBehavior.action == "nothing") when settings.autoCueNextWithoutEnd-
    // Behavior is enabled. Advances the arming to the next document item; at
    // the end of the list a natural end wraps to the first item while a manual
    // stop leaves the arming untouched. Broadcasts via next_item_set doc_patch.
    // Owns all locking — call with no lock held.
    void arm_next_after_stop(const std::string& stopped_uuid, bool was_manual);

    // Arm the very first playable item when a project opens and nothing is
    // armed or playing, so the operator's first GO fires without a click.
    // Gated on settings.autoCueNextWithoutEndBehavior. Call with no lock held.
    void arm_first_item_on_open();

    // True when the item's cue is currently on air (Playing, FadingIn or
    // Paused). The automatic advance paths (Start Next, crossfade,
    // end-behaviour "next") use this to avoid restarting a next item the
    // operator already started manually. Groups have no cue → false.
    bool item_on_air(const std::string& uuid);

public:
    // Subscribe to "external" custom actions the server can't perform on its
    // own — currently just http-request. The control server installs a
    // handler that broadcasts the action as a doc_patch so a connected
    // client makes the actual fetch. Keeps a libcurl/winhttp dependency
    // out of the server.
    void set_external_action_handler(std::function<void(const json&)> handler);

    // Install a callback invoked whenever the "Up Next" override changes for
    // ANY reason (client request or server-side arming). The control server
    // installs this to fan the change out to every client as a next_item_set
    // doc_patch, so server-initiated arming (#28 auto-cue, first-item, wrap) is
    // mirrored by all clients instead of being decided per-client. Called with
    // no ProjectState lock held.
    void set_next_item_broadcaster(std::function<void(const std::string&)> cb);

private:
    std::function<void(const json&)> external_action_handler_;
    std::function<void(const std::string&)> next_item_broadcaster_;
    std::function<void(const json&)> ui_state_broadcaster_;

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

    // ---- Single-item async audio load (#43) --------------------------------
    // add_item() / update_item() used to run mirror_items_to_engine_locked()
    // while holding mutex_, so the synchronous audio decode of ONE new file
    // blocked every other request (play_item, stop, state, WS/HTTP handlers)
    // for its full duration. Instead they now reserve the CueId under the lock,
    // register a placeholder CueMeta, and hand the decode to a background
    // loader thread which publishes the finished cue afterwards.

    // Reserve a cue id for `uuid`, register the placeholder CueMeta, and queue
    // the decode. Caller MUST hold mutex_. Returns the reserved id (never empty
    // unless `path` is empty). `item` is the document node, used for the
    // placeholder's display name / duration and re-read when the load lands.
    audio::CueId begin_item_load_locked(const std::string& uuid,
                                        const std::filesystem::path& path,
                                        const json& item);

    // Loader thread body: decode queued items and publish them.
    void loader_loop();
    void stop_loaders();

    // Apply one document item's engine-visible properties (gain, fades,
    // out point, LTC) to its PlaybackItem. Caller must hold mutex_. Shared by
    // mirror_items_to_engine_locked() and the loader's publish step so the two
    // paths can't drift apart.
    void apply_item_properties_locked(const json& item, const audio::CueId& cue);

    // Block until `uuid`'s queued decode has finished (or `timeout` elapses).
    // Call with NO lock held. Returns true if the item is no longer pending.
    // Only ever waits on that one item — unrelated requests are untouched.
    bool wait_for_item_load(const std::string& uuid,
                            std::chrono::milliseconds timeout);

    struct LoadRequest {
        std::string           uuid;
        audio::CueId          cue_id;
        std::filesystem::path path;
    };
    std::deque<LoadRequest>         load_queue_;
    std::unordered_set<std::string> pending_load_uuids_;
    std::mutex                      loader_mutex_;      // guards both of the above
    std::condition_variable         loader_cv_;         // work available
    std::condition_variable         loader_done_cv_;    // a pending load finished
    std::thread                     loader_thread_;
    bool                            loaders_stop_ = false;

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

    // Uuid of the first audio item in document order (depth-first through
    // groups), or empty if the playlist has no audio items. Caller must hold
    // mutex_. Used by the server-side "Up Next" arming.
    std::string first_playable_item_uuid_locked() const;

    // Every playlist item uuid in the client's flat selection order: each node
    // followed by its children, depth-first. Groups are included (they are
    // selectable and triggerable). Caller must hold mutex_.
    std::vector<std::string> flat_item_uuids_locked() const;

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
