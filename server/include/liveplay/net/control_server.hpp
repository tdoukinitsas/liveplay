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
//   POST   /api/routing/item_to_mixer        — { cue, source_channel, mixer, gain_db }
//   POST   /api/routing/mixer_to_master      — { mixer, master_channel, gain_db }
//   POST   /api/routing/master_to_device     — { master_channel, device, hw_channel }
//   POST   /api/mixers                       — { "name": "..." }
//   DELETE /api/mixers/{id}
//   GET    /api/fs/list?path=...             — list directory (audio + dirs)
//   POST   /api/upload                       — multipart upload to media root
//   GET    /api/project                      — current project JSON
//   POST   /api/project/load                 — { "path": "..." }
//   POST   /api/project/save                 — { "path": "..." }
//
// WebSocket: /ws — bidirectional JSON message stream.
//   Server → Client: { "type": "meters", ... } @ ~60Hz, plus
//                    { "type": "cue_state", ... } on transport transitions.
//   Client → Server: { "type": "play"|"stop"|"stop_all"|"gain"|... }
// ============================================================================
#pragma once

#include "liveplay/audio/engine.hpp"
#include "liveplay/core/project_state.hpp"

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace liveplay::net {

struct ControlServerConfig {
    std::string   bind_address       = "0.0.0.0";
    std::uint16_t port               = 4480;
    // Meter frame rate. Peaks between frames are never lost — the broadcaster
    // uses consuming max-since-read meter reads — so this only sets how
    // fluid the meters look, not what they catch.
    std::size_t   meter_broadcast_hz = 30;
    std::size_t   max_upload_bytes   = 256ull * 1024 * 1024;   // 256 MiB
};

class ControlServer {
public:
    ControlServer(audio::AudioEngine& engine,
                  core::ProjectState& state,
                  ControlServerConfig cfg = {});
    ~ControlServer();   // defined in .cpp where Impl is complete

    bool start();
    void stop();

private:
    audio::AudioEngine& engine_;
    core::ProjectState& state_;
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
