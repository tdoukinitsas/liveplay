// ============================================================================
// liveplay/net/control_server.hpp
// ----------------------------------------------------------------------------
// Crow-backed REST + WebSocket front-end for the AudioEngine and
// ProjectState. The desktop client talks to this server exclusively — there
// is no direct in-process API call once a client is connected.
//
// REST surface (all responses are JSON):
//   GET    /api/health                       — liveness probe
//   GET    /api/devices                      — list playback devices
//   POST   /api/devices/open                 — { "name": "..." } open by-name
//   POST   /api/devices/close                — { "id": "..." }
//   GET    /api/cues                         — list cues
//   POST   /api/cues                         — { "file_path": "..." } register
//   GET    /api/cues/{id}                    — cue detail
//   DELETE /api/cues/{id}                    — unload
//   POST   /api/cues/{id}/play
//   POST   /api/cues/{id}/stop
//   POST   /api/cues/{id}/gain               — { "db": -3.0 }
//   POST   /api/cues/{id}/fade               — { "in_ms": N, "out_ms": M }
//   POST   /api/cues/{id}/ltc                — { "enabled":..., "fps":..., "offset_ns":... }
//   POST   /api/transport/stop_all           — { "fade_ms": 250 }
//   GET    /api/state/summary                — compact transport + bus state (external control)
//   POST   /api/transport/go                 — play the armed "Up Next" item
//   POST   /api/transport/play_index         — { "index": [1, 11] } trigger by index path
//   POST   /api/transport/cart/{slot}/play   — trigger a cart slot's bound item
//   POST   /api/transport/pause_toggle       — resume all paused, else pause all sounding
//   GET    /api/selection                    — { "itemUuid": "..." } shared selection
//   POST   /api/selection                    — { "itemUuid": "..." } or { "delta": -1|1 }
//   POST   /api/transport/arm_selected       — arm the selected item as "Up Next"
//   POST   /api/transport/play_selected      — trigger the selected item
//   GET    /api/ui/showmode                  — { "enabled": bool }
//   POST   /api/ui/showmode                  — { "enabled": bool } (omit = toggle)
//   GET    /api/ui/locale                    — { "locale": "en" }
//   POST   /api/ui/locale                    — { "locale": "el" }
//   GET    /api/master/limiter               — { "enabled": bool }
//   POST   /api/master/limiter               — { "enabled": bool } (omit = toggle)
//   POST   /api/routing/item_to_mixer        — { cue, source_channel, mixer, gain_db }
//   POST   /api/routing/mixer_to_master      — { mixer, master_channel, gain_db }
//   POST   /api/routing/master_to_device     — { master_channel, device, hw_channel }
//   POST   /api/mixers                       — { "name": "..." }
//   DELETE /api/mixers/{id}
//   GET    /api/buses                        — list buses, mixer-view shape
//   GET    /api/buses/{id}                   — single bus, same shape as one list element
//   POST   /api/buses                        — { name, color, order, ... } create
//   PATCH  /api/buses/{id}                   — persist-and-broadcast edit (name/color/order/
//                                               width/gainDb/mute/pan/dsp/output)
//   DELETE /api/buses/{id}                   — refused for system buses
//   POST   /api/buses/{id}/pan               — live-drag only; client-internal (D16) —
//                                               external controllers use PATCH
//   POST   /api/buses/{id}/dsp               — live-drag only; client-internal (D16) —
//                                               external controllers use PATCH
//   POST   /api/buses/{id}/pfl               — { "pfl": bool } persist-and-broadcast
//   POST   /api/buses/pfl/clear              — clear PFL on every bus
//   POST   /api/monitor/mono                 — { "mono": bool } Monitor bus mono-sum audition
//   GET    /api/fs/list?path=...             — list directory (audio + dirs)
//   POST   /api/upload                       — multipart upload to media root
//   GET    /api/project                      — current project JSON
//   POST   /api/project/load                 — { "path": "..." }
//   POST   /api/project/save                 — { "path": "..." }
//
// WebSocket: /ws — bidirectional JSON message stream.
//   Server → Client: { "type": "meters", ... } @ ~60Hz, plus
//                    { "type": "cue_state", ... } on transport transitions.
//   Client → Server: { "type": "play"|"stop"|"stop_all"|"gain"|... }, plus bus commands that
//                    mirror the REST persist-and-broadcast endpoints above:
//                    { "type": "bus_gain", "busId": "...", "gainDb": -3.0 }
//                    { "type": "bus_mute", "busId": "...", "mute": bool }        (omit = toggle)
//                    { "type": "bus_pfl",  "busId": "...", "pfl":  bool }        (omit = toggle)
// ============================================================================
#pragma once

#include "liveplay/audio/engine.hpp"
#include "liveplay/core/project_state.hpp"

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace liveplay::net {

struct ControlServerConfig {
    std::string   bind_address       = "0.0.0.0";
    std::uint16_t port               = 4480;
    // Meter frame rate. Peaks between frames are never lost — the broadcaster
    // uses consuming max-since-read meter reads — so this only sets how
    // fluid the meters look, not what they catch.
    std::size_t   meter_broadcast_hz = 30;
    std::size_t   max_upload_bytes   = 256ull * 1024 * 1024;   // 256 MiB

    // Directories the filesystem API may reach: /api/fs/list, /api/fs/mkdir,
    // /api/metadata, /api/copy_to_media, the waveform readers and the project
    // load/export/import paths. A request naming a path outside every root is
    // refused with 403.
    //
    // EMPTY MEANS UNRESTRICTED, and empty is the default. That is not an
    // oversight: every release so far has served the whole filesystem, shows
    // legitimately live on other volumes, and silently jailing them on upgrade
    // would break opening a project rather than protect it. The server logs a
    // warning at boot while this is empty, so the posture is stated rather
    // than assumed. Set it to lock an install down.
    std::vector<std::string> fs_browse_roots{};

    // Value sent as Access-Control-Allow-Origin on every response. "*" is what
    // every release so far has hardcoded, and it stays the default so no
    // existing deployment changes behaviour on upgrade; an integrator can pin
    // it to one origin.
    std::string   cors_allow_origin  = "*";
};

class ControlServer {
public:
    ControlServer(audio::AudioEngine& engine,
                  core::ProjectState& state,
                  core::OutputMap&    outputs,
                  ControlServerConfig cfg = {});
    ~ControlServer();   // defined in .cpp where Impl is complete

    bool start();
    void stop();

private:
    audio::AudioEngine& engine_;
    core::ProjectState& state_;
    core::OutputMap&    outputs_;
    ControlServerConfig cfg_;
    std::atomic<bool>   running_{false};

    // Crow + WebSocket state hidden behind pimpl so crow.h stays out of this
    // header and out of every file that only needs to start/stop the server.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void install_routes();
    void broadcast_loop();
    void waveform_worker();   // drains the async waveform-generation queue

    // Fan-out helper for multi-client mutation sync. Mutating REST routes
    // call this with a doc_patch payload so every connected client mirrors
    // the change. Defined in control_server.cpp where Impl is complete.
    void broadcast_doc_patch(const nlohmann::json& payload);
};

} // namespace liveplay::net
