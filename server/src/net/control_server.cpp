// ============================================================================
// control_server.cpp — see control_server.hpp.
// ============================================================================

// Must come before crow.h on Windows to avoid redefinition of NOMINMAX etc.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00
#endif

#include "liveplay/net/control_server.hpp"
#include "liveplay/logger.hpp"
#include "liveplay/meta/metadata.hpp"
#include "liveplay/meta/waveform.hpp"
#include "liveplay/util/unicode_path.hpp"

#if defined(_WIN32)
#  include <windows.h>      // GetLogicalDrives()
#endif

#include <crow.h>
#include <crow/middlewares/cors.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>


namespace fs    = std::filesystem;
using json      = nlohmann::json;
namespace audio = liveplay::audio;
namespace core  = liveplay::core;

namespace liveplay::net {

// Pimpl: all Crow + WebSocket state lives here so crow.h stays out of the
// public header.
struct ControlServer::Impl {
    crow::SimpleApp app;
    std::thread     app_thread;
    std::thread     broadcast_thread;
    std::mutex      ws_mutex;
    std::unordered_set<crow::websocket::connection*> ws_clients;
};

namespace {

// File extensions we accept as cue audio files.
const std::set<std::string>& audio_extensions() {
    static const std::set<std::string> exts {
        ".wav", ".aiff", ".aif", ".flac", ".mp3", ".ogg", ".m4a", ".aac",
        ".opus", ".wma", ".caf"
    };
    return exts;
}

bool is_audio_file(const fs::path& p) {
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return audio_extensions().count(e) > 0;
}

crow::response json_ok(const json& body) {
    crow::response r{200, body.dump()};
    r.add_header("Content-Type", "application/json");
    r.add_header("Access-Control-Allow-Origin", "*");
    return r;
}

crow::response json_err(int status, std::string_view message) {
    crow::response r{status, json({{"error", message}}).dump()};
    r.add_header("Content-Type", "application/json");
    r.add_header("Access-Control-Allow-Origin", "*");
    return r;
}

json device_info_to_json(const audio::DeviceInfo& d) {
    return json{
        {"id",            d.id.value},
        {"display_name",  d.display_name},
        {"channel_count", d.channel_count},
        {"sample_rate",   d.sample_rate},
        {"is_default",    d.is_default},
    };
}

json cue_to_json(const core::CueMeta& c, audio::AudioEngine& engine) {
    json j;
    j["id"]            = c.id.value;
    j["display_name"]  = c.display_name;
    j["file_path"]     = liveplay::util::path_to_utf8(c.file_path);
    j["artist"]        = c.artist;
    j["title"]         = c.title;
    j["duration_sec"]  = c.duration_seconds;
    j["gain_db"]       = c.gain_db;
    j["fade_in_ms"]    = c.fade_in_ms.count();
    j["fade_out_ms"]   = c.fade_out_ms.count();
    j["ltc"] = json{
        {"enabled",        c.ltc_enabled},
        {"fps",            c.ltc_frame_rate_index},
        {"offset_ns",      static_cast<long long>(c.ltc_offset_ns.count())},
        {"start_timecode", c.ltc_start_timecode},
    };
    if (auto* item = engine.find_cue(c.id)) {
        const auto s = item->stats();
        j["transport"] = static_cast<int>(s.transport);
        j["playhead_seconds"] = s.playhead_seconds;
        j["source_channels"]  = s.source_channels;
        j["file_loaded"]      = s.file_loaded;
    }
    return j;
}

} // namespace

// ---------------------------------------------------------------------------

ControlServer::ControlServer(audio::AudioEngine& engine,
                             core::ProjectState& state,
                             ControlServerConfig cfg)
    : engine_(engine), state_(state), cfg_(std::move(cfg)),
      impl_(std::make_unique<Impl>()) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start() {
    if (running_.exchange(true)) return true;
    install_routes();

    // Crow's SimpleApp::run() blocks; we shove it on a worker thread.
    impl_->app_thread = std::thread([this] {
        try {
            impl_->app.bindaddr(cfg_.bind_address).port(cfg_.port).multithreaded().run();
        } catch (const std::exception& ex) {
            Logger::error("ControlServer: crow run() threw: {}", ex.what());
        }
    });

    impl_->broadcast_thread = std::thread([this] { broadcast_loop(); });
    Logger::success("Control server listening on {}:{}", cfg_.bind_address, cfg_.port);
    return true;
}

void ControlServer::stop() {
    if (!running_.exchange(false)) return;
    impl_->app.stop();
    if (impl_->broadcast_thread.joinable()) impl_->broadcast_thread.join();
    if (impl_->app_thread.joinable())       impl_->app_thread.join();
    Logger::info("Control server stopped.");
}

// ---------------------------------------------------------------------------
// Meter broadcaster (~60 fps) + cue_state edge events
// ---------------------------------------------------------------------------
void ControlServer::broadcast_loop() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds{
        1'000'000'000LL / static_cast<long long>(std::max<std::size_t>(1, cfg_.meter_broadcast_hz))};

    // Track previous transport state per cue so we can emit cue_state events
    // exactly once on each transition (rather than every tick).
    std::unordered_map<std::string, audio::TransportState> prev_transports;

    while (running_.load(std::memory_order_acquire)) {
        const auto start = clock::now();

        // Build the meters payload.
        json payload;
        payload["type"] = "meters";

        json item_meters = json::array();
        for (auto& cue : state_.list_cues()) {
            auto* item = engine_.find_cue(cue.id);
            if (!item) continue;
            const auto stats = item->stats();
            // Skip stopped/silent items entirely. With 100+ cues most are
            // idle and shipping their meters every 40 ms wastes the WS — the
            // client only needs UI updates for active ones.
            const bool is_active =
                stats.transport != audio::TransportState::Stopped;
            if (!is_active) continue;

            json m;
            m["cue_id"]            = cue.id.value;
            m["transport"]         = static_cast<int>(stats.transport);
            m["playhead_seconds"]  = stats.playhead_seconds;
            json srcs = json::array();
            for (audio::ChannelIndex c = 0; c < item->source_channel_count(); ++c) {
                auto snap = item->source_meter(c);
                srcs.push_back(json{{"peak_db", snap.peak_db}, {"rms_db", snap.rms_db}});
            }
            m["sources"] = std::move(srcs);
            item_meters.push_back(std::move(m));
        }
        payload["items"] = std::move(item_meters);

        json mixer_meters = json::array();
        for (auto& mch : state_.list_mixer_channels()) {
            if (auto* m = engine_.find_mixer_channel(mch.id)) {
                auto s = m->meter_snapshot();
                mixer_meters.push_back(json{
                    {"mixer_id", mch.id.value},
                    {"peak_db",  s.peak_db},
                    {"rms_db",   s.rms_db},
                });
            }
        }
        payload["mixer_channels"] = std::move(mixer_meters);

        json master_meters = json::array();
        for (audio::MasterChannelIndex i = 0; i < engine_.config().master_channels; ++i) {
            auto s = engine_.read_master_meter(i);
            const float gr = engine_.read_master_gain_reduction_db(i);
            // Only include non-silent channels to keep the payload light.
            if (s.peak_db > -119.0f || gr < -0.05f) {
                master_meters.push_back(json{
                    {"index",   i},
                    {"peak_db", s.peak_db},
                    {"rms_db",  s.rms_db},
                    {"gain_reduction_db", gr},
                });
            }
        }
        payload["master_channels"] = std::move(master_meters);

        const std::string serialized = payload.dump();

        // Detect transport changes and build cue_state edge events.
        std::vector<std::string> cue_state_events;
        for (auto& cue : state_.list_cues()) {
            auto* item = engine_.find_cue(cue.id);
            const auto current = item
                ? item->stats().transport
                : audio::TransportState::Stopped;
            auto& prev = prev_transports[cue.id.value]; // default → Stopped (0)
            if (current != prev) {
                prev = current;
                json evt;
                evt["type"]             = "cue_state";
                evt["cue_id"]           = cue.id.value;
                evt["transport"]        = static_cast<int>(current);
                evt["playhead_seconds"] = item ? item->stats().playhead_seconds : 0.0;
                cue_state_events.push_back(evt.dump());
            }
        }

        // Fan out meters + cue_state events to all subscribed clients.
        std::lock_guard lock{impl_->ws_mutex};
        for (auto* c : impl_->ws_clients) {
            try {
                c->send_text(serialized);
                for (const auto& e : cue_state_events) c->send_text(e);
            }
            catch (...) { /* connection will be cleaned up by onclose */ }
        }

        // Sleep to maintain the broadcast cadence.
        const auto used = clock::now() - start;
        if (used < period) std::this_thread::sleep_for(period - used);
    }
}

// ---------------------------------------------------------------------------
// Multi-client mutation fan-out. Mutating REST routes call this with a
// doc_patch event so every connected client mirrors the change in real
// time. The originating client also receives the echo — applying it is
// idempotent on its side (uuid lookup) and keeps the local diff-watcher
// quiet because the client wraps the apply in its `isHydrating` flag.
void ControlServer::broadcast_doc_patch(const json& payload) {
    const std::string serialized = payload.dump();
    std::lock_guard lock{impl_->ws_mutex};
    for (auto* c : impl_->ws_clients) {
        try { c->send_text(serialized); }
        catch (...) { /* onclose will clean up dead connections */ }
    }
}

// ---------------------------------------------------------------------------
static void handle_ws_message(crow::websocket::connection& conn,
                               const std::string& msg,
                               audio::AudioEngine& engine,
                               core::ProjectState& state) {
    Logger::api_request("WS ← {}", msg);

    json j;
    try { j = json::parse(msg); }
    catch (const std::exception& e) {
        Logger::warn("WS message parse failed: {}", e.what());
        conn.send_text(json({{"type", "error"}, {"message", e.what()}}).dump());
        return;
    }
    const std::string type = j.value("type", "");
    // Resolve a transport target: prefer "item_uuid" (preserves duckingBehavior
    // / inPoint semantics defined in the project document); fall back to
    // "cue_id" (raw engine id) for low-level callers.
    auto resolve_cue = [&](const json& jj) -> std::optional<audio::CueId> {
        if (jj.contains("item_uuid") && jj["item_uuid"].is_string()) {
            return state.item_to_cue_id(jj["item_uuid"].get<std::string>());
        }
        if (jj.contains("cue_id") && jj["cue_id"].is_string()) {
            return audio::CueId{jj["cue_id"].get<std::string>()};
        }
        return std::nullopt;
    };

    try {
        if (type == "play") {
            if (j.contains("item_uuid") && j["item_uuid"].is_string()) {
                const auto uuid = j["item_uuid"].get<std::string>();
                Logger::playback("PLAY item_uuid={}", uuid);
                state.play_item(uuid);
            } else {
                auto cue = resolve_cue(j);
                if (cue) {
                    Logger::playback("PLAY cue_id={}", cue->value);
                    engine.play(*cue);
                } else {
                    Logger::warn("WS play: no valid cue target in message");
                }
            }
        }
        else if (type == "stop") {
            if (j.contains("item_uuid") && j["item_uuid"].is_string()) {
                const auto uuid = j["item_uuid"].get<std::string>();
                Logger::playback("STOP item_uuid={}", uuid);
                state.stop_item(uuid);
            } else {
                auto cue = resolve_cue(j);
                if (cue) {
                    Logger::playback("STOP cue_id={}", cue->value);
                    engine.stop(*cue);
                } else {
                    Logger::warn("WS stop: no valid cue target in message");
                }
            }
        }
        else if (type == "stop_all") {
            const auto fade_ms = j.value("fade_ms", (long long)0);
            Logger::playback("STOP ALL (fade {}ms)", fade_ms);
            engine.stop_all(std::chrono::milliseconds{fade_ms});
        }
        else if (type == "gain") {
            auto cue = resolve_cue(j);
            if (cue) {
                const float db = j.value("db", 0.0f);
                Logger::api_request("WS gain cue_id={} db={:.1f}", cue->value, db);
                state.set_cue_gain_db(*cue, db);
            }
        }
        else if (type == "fade") {
            auto cue = resolve_cue(j);
            if (cue) {
                const auto in_ms  = j.value("in_ms",  (long long)0);
                const auto out_ms = j.value("out_ms", (long long)0);
                Logger::api_request("WS fade cue_id={} in={}ms out={}ms",
                                    cue->value, in_ms, out_ms);
                state.set_cue_fade_in (*cue, std::chrono::milliseconds{in_ms});
                state.set_cue_fade_out(*cue, std::chrono::milliseconds{out_ms});
            }
        }
        else if (type == "seek") {
            auto cue = resolve_cue(j);
            if (cue) {
                const double secs = j.value("seconds", 0.0);
                Logger::playback("SEEK {:.2f}s → cue_id={}", secs, cue->value);
                if (auto* pi = engine.find_cue(*cue)) {
                    pi->seek_seconds(secs);
                }
            }
        }
        else if (type == "set_next_item") {
            // User-set "Up Next" override. Empty/null item_uuid clears it.
            std::string uuid;
            if (j.contains("item_uuid") && j["item_uuid"].is_string()) {
                uuid = j["item_uuid"].get<std::string>();
            }
            Logger::playback("SET NEXT item_uuid={}", uuid.empty() ? "<clear>" : uuid);
            state.set_next_item_override(uuid);
        }
        else if (type == "ping") {
            conn.send_text(json({{"type", "pong"}}).dump());
        }
        else {
            Logger::warn("WS unknown message type: {}", type);
            conn.send_text(json({{"type", "error"}, {"message", "unknown type"}}).dump());
        }
    } catch (const std::exception& e) {
        Logger::error("WS handler threw: {}", e.what());
        conn.send_text(json({{"type", "error"}, {"message", e.what()}}).dump());
    }
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------
void ControlServer::install_routes() {
    auto& app = impl_->app;

    // Permissive CORS preflight for everything (the Electron client is on a
    // different origin).
    CROW_ROUTE(app, "/<path>").methods(crow::HTTPMethod::Options)
        ([](const crow::request&, std::string){
            crow::response r{204};
            r.add_header("Access-Control-Allow-Origin",  "*");
            r.add_header("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
            r.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            return r;
        });

    // ---- Health ----
    CROW_ROUTE(app, "/api/health").methods(crow::HTTPMethod::Get)
        ([] { return json_ok(json({{"ok", true}, {"name", "liveplay-server"}})); });

    // ---- Devices ----
    CROW_ROUTE(app, "/api/devices").methods(crow::HTTPMethod::Get)
        ([this] {
            json arr = json::array();
            for (auto& d : engine_.enumerate_devices()) arr.push_back(device_info_to_json(d));
            return json_ok(arr);
        });

    CROW_ROUTE(app, "/api/devices/open").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const std::string name = j.value("name", "");
                const audio::ChannelCount ch = j.value("channels", (audio::ChannelCount)2);
                const auto id = name.empty()
                                  ? engine_.open_default_device(ch)
                                  : engine_.open_device_by_name(name, ch);
                if (id.empty()) return json_err(400, "device open failed");
                return json_ok(json({{"device_id", id.value}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/devices/close").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                engine_.close_device(audio::DeviceId{j.at("id").get<std::string>()});
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Cues ----
    CROW_ROUTE(app, "/api/cues").methods(crow::HTTPMethod::Get)
        ([this] {
            json arr = json::array();
            for (auto& c : state_.list_cues()) arr.push_back(cue_to_json(c, engine_));
            return json_ok(arr);
        });

    CROW_ROUTE(app, "/api/cues").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const fs::path file = liveplay::util::utf8_to_path(j.at("file_path").get<std::string>());
                std::string name = j.value("display_name", "");
                const auto id = state_.add_cue_from_file(file, std::move(name));
                if (id.empty()) return json_err(400, "failed to load file");
                auto meta = state_.find_cue(id);
                return json_ok(meta ? cue_to_json(*meta, engine_) : json{{"id", id.value}});
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/cues/<string>").methods(crow::HTTPMethod::Get)
        ([this](std::string id) {
            auto m = state_.find_cue(audio::CueId{id});
            if (!m) return json_err(404, "not found");
            return json_ok(cue_to_json(*m, engine_));
        });

    CROW_ROUTE(app, "/api/cues/<string>").methods(crow::HTTPMethod::Delete)
        ([this](std::string id) {
            state_.remove_cue(audio::CueId{id});
            return json_ok(json({{"ok", true}}));
        });

    CROW_ROUTE(app, "/api/cues/<string>/play").methods(crow::HTTPMethod::Post)
        ([this](std::string id) {
            engine_.play(audio::CueId{id});
            return json_ok(json({{"ok", true}}));
        });
    CROW_ROUTE(app, "/api/cues/<string>/stop").methods(crow::HTTPMethod::Post)
        ([this](std::string id) {
            engine_.stop(audio::CueId{id});
            return json_ok(json({{"ok", true}}));
        });

    CROW_ROUTE(app, "/api/cues/<string>/gain").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id){
            try {
                auto j = json::parse(req.body);
                state_.set_cue_gain_db(audio::CueId{id}, j.value("db", 0.0f));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/cues/<string>/fade").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id){
            try {
                auto j = json::parse(req.body);
                state_.set_cue_fade_in (audio::CueId{id},
                    std::chrono::milliseconds{j.value("in_ms",  (long long)0)});
                state_.set_cue_fade_out(audio::CueId{id},
                    std::chrono::milliseconds{j.value("out_ms", (long long)0)});
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/cues/<string>/ltc").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id){
            try {
                auto j        = json::parse(req.body);
                const bool enabled = j.value("enabled", false);
                const int  fps     = j.value("fps", 4);
                // Accept either a human-readable "HH:MM:SS:FF" start_timecode
                // or a raw offset_ns (legacy callers). The timecode string takes
                // priority when present.
                std::chrono::nanoseconds offset{j.value("offset_ns", (long long)0)};
                std::string tc_str = j.value("start_timecode", std::string{"00:00:00:00"});
                if (j.contains("start_timecode") && j["start_timecode"].is_string()) {
                    // Convert "HH:MM:SS:FF" to nanoseconds using the fps index.
                    int hh = 0, mm = 0, ss = 0, ff = 0;
                    static const int kFpsInt[]  = {24, 25, 30, 30, 30};
                    static const double kFps[]  = {24.0, 25.0, 30000.0/1001.0,
                                                   30000.0/1001.0, 30.0};
                    if (std::sscanf(tc_str.c_str(), "%d:%d:%d:%d", &hh, &mm, &ss, &ff) < 4)
                        std::sscanf(tc_str.c_str(), "%d:%d:%d;%d", &hh, &mm, &ss, &ff);
                    const int idx = std::clamp(fps, 0, 4);
                    ff = std::clamp(ff, 0, kFpsInt[idx] - 1);
                    const long long frames = static_cast<long long>(hh) * 3600LL * kFpsInt[idx]
                                           + static_cast<long long>(mm) *   60LL * kFpsInt[idx]
                                           + static_cast<long long>(ss)           * kFpsInt[idx]
                                           + ff;
                    const double secs = static_cast<double>(frames) / kFps[idx];
                    offset = std::chrono::nanoseconds{static_cast<long long>(secs * 1e9)};
                }
                state_.set_cue_ltc(audio::CueId{id}, enabled, fps, offset);
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Transport / master ----
    CROW_ROUTE(app, "/api/transport/stop_all").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body.empty() ? std::string{"{}"} : req.body);
                engine_.stop_all(std::chrono::milliseconds{j.value("fade_ms", (long long)0)});
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/master/ceiling").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                engine_.set_master_ceiling_db(j.value("db", -0.3f));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Mixer channels ----
    CROW_ROUTE(app, "/api/mixers").methods(crow::HTTPMethod::Get)
        ([this] {
            json arr = json::array();
            for (auto& m : state_.list_mixer_channels()) {
                arr.push_back(json{
                    {"id",           m.id.value},
                    {"display_name", m.display_name},
                    {"gain_db",      m.gain_db},
                    {"muted",        m.muted},
                    {"soloed",       m.soloed},
                });
            }
            return json_ok(arr);
        });

    CROW_ROUTE(app, "/api/mixers").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const auto id = engine_.create_mixer_channel(j.value("name", "Channel"));
                return json_ok(json({{"id", id.value}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/mixers/<string>").methods(crow::HTTPMethod::Delete)
        ([this](std::string id){
            engine_.remove_mixer_channel(audio::MixerChannelId{id});
            return json_ok(json({{"ok", true}}));
        });

    // ---- Routing ----
    CROW_ROUTE(app, "/api/routing/item_to_mixer").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                engine_.route_item_source_to_mixer(
                    audio::CueId{j.at("cue").get<std::string>()},
                    j.value("source_channel", (audio::ChannelIndex)0),
                    audio::MixerChannelId{j.at("mixer").get<std::string>()},
                    j.value("gain_db", 0.0f));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/routing/mixer_to_master").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                engine_.route_mixer_to_master(
                    audio::MixerChannelId{j.at("mixer").get<std::string>()},
                    j.value("master_channel", (audio::MasterChannelIndex)0),
                    j.value("gain_db", 0.0f));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/routing/master_to_device").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                engine_.assign_master_to_device(
                    j.value("master_channel", (audio::MasterChannelIndex)0),
                    audio::DeviceId{j.at("device").get<std::string>()},
                    j.value("hw_channel", (audio::ChannelIndex)0));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Filesystem browsing ----
    // GET /api/fs/list?path=<utf8>&filter=<comma-separated-exts|all|audio>
    //   path  ""  → "computer" root: enumerate logical drives on Windows,
    //                 "/" on POSIX. Each drive is reported with kind=="drive".
    //   filter:
    //     "audio" (default) — only show known audio file extensions
    //     "all"             — list every regular file
    //     ".liveplay,.lpa"  — comma-separated extension allow-list
    CROW_ROUTE(app, "/api/fs/list").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req){
            try {
                const char* path_param   = req.url_params.get("path");
                const char* filter_param = req.url_params.get("filter");
                std::string path        = path_param ? path_param : "";
                std::string filter      = filter_param ? filter_param : "audio";

                // Empty path == "computer" root. On Windows we enumerate
                // logical drives (and mapped network drives). On POSIX we
                // start at '/'. This lets the picker behave like a native
                // file dialog.
                if (path.empty()) {
                    json out;
                    out["path"]    = "";        // sentinel: computer root
                    out["parent"]  = "";
                    out["is_root"] = true;
                    out["entries"] = json::array();
#if defined(_WIN32)
                    DWORD mask = GetLogicalDrives();
                    for (char letter = 'A'; letter <= 'Z'; ++letter, mask >>= 1) {
                        if (!(mask & 1)) continue;
                        char drive[4] = { letter, ':', '\\', 0 };
                        json e;
                        e["name"]      = std::string{letter} + ":";
                        e["full_path"] = drive;
                        e["kind"]      = "drive";
                        out["entries"].push_back(std::move(e));
                    }
#else
                    // On POSIX, root is just '/'; surface it as a single drive.
                    json e;
                    e["name"]      = "/";
                    e["full_path"] = "/";
                    e["kind"]      = "drive";
                    out["entries"].push_back(std::move(e));
#endif
                    return json_ok(out);
                }

                fs::path p = liveplay::util::utf8_to_path(path);
                std::error_code canon_ec;
                fs::path canon = fs::weakly_canonical(p, canon_ec);
                if (!canon_ec) p = canon;
                if (!fs::exists(p)) return json_err(404, "no such path");

                json out;
                out["path"]    = liveplay::util::path_to_utf8(p);
                out["parent"]  = p.has_parent_path() && p.parent_path() != p
                                   ? liveplay::util::path_to_utf8(p.parent_path()) : "";
                out["is_root"] = false;
                out["entries"] = json::array();

                // Build the extension allow-list.
                std::set<std::string> allow;
                bool allow_all = false;
                if (filter == "all") {
                    allow_all = true;
                } else if (filter == "audio") {
                    allow = audio_extensions();
                } else {
                    // Custom comma-separated list, e.g. ".liveplay,.lpa".
                    std::string token;
                    for (char c : filter) {
                        if (c == ',') {
                            if (!token.empty()) {
                                if (token[0] != '.') token.insert(token.begin(), '.');
                                std::transform(token.begin(), token.end(), token.begin(),
                                    [](unsigned char ch){ return (char)std::tolower(ch); });
                                allow.insert(token);
                                token.clear();
                            }
                        } else {
                            token.push_back(c);
                        }
                    }
                    if (!token.empty()) {
                        if (token[0] != '.') token.insert(token.begin(), '.');
                        std::transform(token.begin(), token.end(), token.begin(),
                            [](unsigned char ch){ return (char)std::tolower(ch); });
                        allow.insert(token);
                    }
                }

                auto ext_passes = [&](const fs::path& pp) -> bool {
                    if (allow_all) return true;
                    auto e = pp.extension().string();
                    std::transform(e.begin(), e.end(), e.begin(),
                                   [](unsigned char c){ return (char)std::tolower(c); });
                    return allow.count(e) > 0;
                };

                if (fs::is_directory(p)) {
                    for (auto& entry : fs::directory_iterator(p, fs::directory_options::skip_permission_denied)) {
                        const auto& ep = entry.path();
                        // Hide hidden entries on POSIX (leading dot). Windows
                        // hidden flag is honoured by fs::directory_iterator
                        // implicitly only for system files.
                        const std::string name = liveplay::util::path_to_utf8(ep.filename());
                        if (!name.empty() && name[0] == '.') continue;

                        json e;
                        e["name"]      = name;
                        e["full_path"] = liveplay::util::path_to_utf8(ep);
                        std::error_code dir_ec;
                        if (entry.is_directory(dir_ec)) {
                            e["kind"] = "dir";
                            out["entries"].push_back(std::move(e));
                        } else if (entry.is_regular_file(dir_ec) && ext_passes(ep)) {
                            e["kind"] = "file";
                            std::error_code size_ec;
                            e["size"] = static_cast<long long>(fs::file_size(ep, size_ec));
                            out["entries"].push_back(std::move(e));
                        }
                    }
                }
                return json_ok(out);
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Multipart upload ----
    CROW_ROUTE(app, "/api/upload").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                if (req.body.size() > cfg_.max_upload_bytes) {
                    return json_err(413, "payload too large");
                }
                crow::multipart::message multipart{req};
                const auto& parts = multipart.parts;
                if (parts.empty()) return json_err(400, "no multipart parts");

                fs::path media = state_.media_root();
                fs::create_directories(media);

                json saved = json::array();
                for (const auto& part : parts) {
                    // Pick filename from Content-Disposition header.
                    std::string filename = "upload.bin";
                    auto it = part.headers.find("Content-Disposition");
                    if (it != part.headers.end()) {
                        const auto& params = it->second.params;
                        auto fn = params.find("filename");
                        if (fn != params.end() && !fn->second.empty()) {
                            filename = fn->second;
                        }
                    }
                    // Strip path traversal — treat the filename bytes as UTF-8.
                    fs::path safe_name = liveplay::util::utf8_to_path(filename).filename();
                    if (safe_name.empty()) safe_name = "upload.bin";
                    fs::path dest = media / safe_name;

                    std::ofstream f{dest, std::ios::binary};
                    if (!f) return json_err(500, "failed to write file");
                    f.write(part.body.data(), static_cast<std::streamsize>(part.body.size()));
                    saved.push_back(liveplay::util::path_to_utf8(dest));
                }
                return json_ok(json({{"saved", saved}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Metadata + Waveform ----
    CROW_ROUTE(app, "/api/metadata").methods(crow::HTTPMethod::Get)
        ([](const crow::request& req) {
            const char* path = req.url_params.get("path");
            if (!path) return json_err(400, "missing ?path=");
            const auto md = liveplay::meta::read_metadata(liveplay::util::utf8_to_path(path));
            return json_ok(json{
                {"valid",        md.valid},
                {"artist",       md.artist},
                {"title",        md.title},
                {"album",        md.album},
                {"genre",        md.genre},
                {"year",         md.year},
                {"track_number", md.track_number},
                {"duration_ms",  md.duration.count()},
                {"sample_rate",  md.sample_rate},
                {"channels",     md.channels},
                {"bitrate_kbps", md.bitrate_kbps},
            });
        });

    CROW_ROUTE(app, "/api/waveform/<string>").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req, std::string cue_id) {
            const auto meta = state_.find_cue(audio::CueId{cue_id});
            if (!meta) return json_err(404, "no such cue");

            std::uint32_t buckets = 1000;
            if (req.url_params.get("buckets")) {
                try { buckets = static_cast<std::uint32_t>(std::stoi(req.url_params.get("buckets"))); }
                catch (...) {}
            }

            const auto wf = liveplay::meta::compute_waveform(meta->file_path, buckets);
            if (!wf.ok) return json_err(500, "waveform decode failed");

            json channels = json::array();
            for (const auto& ch : wf.channels) {
                channels.push_back(json{{"peak", ch.peak}, {"rms", ch.rms}});
            }
            return json_ok(json{
                {"cue_id",          cue_id},
                {"bucket_count",    wf.bucket_count},
                {"duration_ms",     wf.duration.count()},
                {"sample_rate",     wf.sample_rate},
                {"source_channels", wf.source_channels},
                {"channels",        std::move(channels)},
            });
        });

    // ---- Project I/O ----
    // Returns the *full* client-shaped project document (items, groups, cart,
    // theme, settings) plus a server-side decoration of engine cue ids. This
    // is the single GET a remote client needs to render the whole project.
    CROW_ROUTE(app, "/api/project").methods(crow::HTTPMethod::Get)
        ([this] { return json_ok(state_.full_document()); });

    // Lightweight header — theme, settings, cart, project name, item count.
    // Clients hit this first so they can paint the workspace shell before
    // the (potentially large) items array has even started downloading.
    // Pair with /api/project/items?offset=&limit= to stream the playlist.
    CROW_ROUTE(app, "/api/project/header").methods(crow::HTTPMethod::Get)
        ([this] {
            Logger::api_request("GET /api/project/header");
            auto hdr = state_.header_document();
            Logger::api_response("GET /api/project/header OK — '{}' ({} items)",
                                 hdr.value("name", "?"),
                                 hdr.value("itemCount", (std::size_t)0));
            return json_ok(hdr);
        });

    // Paged top-level items. `offset` defaults to 0, `limit` to 100
    // (sane upper bound: even on slow LANs a 100-item page comes back
    // in well under a frame). Returns { offset, limit, total, items: [...] }.
    CROW_ROUTE(app, "/api/project/items").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req){
            std::size_t offset = 0, limit = 100;
            if (const char* p = req.url_params.get("offset")) {
                try { offset = static_cast<std::size_t>(std::max(0, std::stoi(p))); }
                catch (...) {}
            }
            if (const char* p = req.url_params.get("limit")) {
                try { limit = static_cast<std::size_t>(std::clamp(std::stoi(p), 1, 1000)); }
                catch (...) {}
            }
            Logger::api_request("GET /api/project/items offset={} limit={}", offset, limit);
            auto page = state_.items_page(offset, limit);
            Logger::api_response("GET /api/project/items offset={} → {}/{} items",
                                 offset,
                                 page.value("items", json::array()).size(),
                                 page.value("total", (std::size_t)0));
            return json_ok(page);
        });

    // Cheap progress poll endpoint — the client hits this during project
    // open so it can show "loaded X / Y audio cues" without re-fetching the
    // whole document on a timer.
    CROW_ROUTE(app, "/api/project/progress").methods(crow::HTTPMethod::Get)
        ([this] {
            return json_ok(json({
                {"loading", state_.audio_loading()},
                {"loaded",  state_.audio_loaded_count()},
                {"total",   state_.audio_total_count()},
            }));
        });

    CROW_ROUTE(app, "/api/project/load").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                if (j.contains("path")) {
                    const std::string path_str = j["path"].get<std::string>();
                    Logger::api_request("POST /api/project/load path='{}'", path_str);
                    const fs::path p = liveplay::util::utf8_to_path(path_str);
                    if (!state_.load(p)) {
                        Logger::error("POST /api/project/load FAILED — load returned false for '{}'", path_str);
                        return json_err(400, "load failed");
                    }
                } else if (j.contains("document")) {
                    Logger::api_request("POST /api/project/load (from document)");
                    if (!state_.load_from_json(j["document"])) {
                        Logger::error("POST /api/project/load FAILED — document rejected");
                        return json_err(400, "load failed");
                    }
                } else {
                    Logger::warn("POST /api/project/load — missing 'path' or 'document' in body");
                    return json_err(400, "expected 'path' or 'document'");
                }
                auto header = state_.header_document();
                const std::size_t item_count = header.value("itemCount", (std::size_t)0);
                Logger::api_response("POST /api/project/load OK — '{}' ({} items)",
                                     header.value("name", "?"), item_count);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "project_changed"},
                });
                return json_ok(header);
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/load threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    CROW_ROUTE(app, "/api/project/save").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                fs::path p;
                if (j.contains("path") && j["path"].is_string()) {
                    p = liveplay::util::utf8_to_path(j["path"].get<std::string>());
                    state_.set_project_file_path(p);
                } else {
                    p = state_.project_file_path();
                    if (p.empty()) {
                        Logger::warn("POST /api/project/save — no path set");
                        return json_err(400, "no project file path set");
                    }
                }
                Logger::api_request("POST /api/project/save path='{}'",
                                    liveplay::util::path_to_utf8(p));
                if (!state_.save(p)) {
                    Logger::error("POST /api/project/save FAILED for '{}'",
                                  liveplay::util::path_to_utf8(p));
                    return json_err(500, "save failed");
                }
                const auto path_str = liveplay::util::path_to_utf8(p);
                Logger::api_response("POST /api/project/save OK → '{}'", path_str);
                return json_ok(json({{"ok", true}, {"path", path_str}}));
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/save threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    // Replace the entire project document. Client uses this on app startup if
    // it has an existing in-memory project it wants to push to the server.
    // Like /api/project/load, this returns the header rather than the full
    // document so the round-trip stays cheap for large projects.
    CROW_ROUTE(app, "/api/project/document").methods(crow::HTTPMethod::Put)
        ([this](const crow::request& req){
            try {
                auto doc = json::parse(req.body);
                const std::string proj_name = doc.value("name", "?");
                Logger::api_request("PUT /api/project/document name='{}'", proj_name);
                if (!state_.replace_full_document(doc)) {
                    Logger::error("PUT /api/project/document — document not accepted");
                    return json_err(400, "document not accepted");
                }
                auto header = state_.header_document();
                const std::size_t item_count = header.value("itemCount", (std::size_t)0);
                Logger::api_response("PUT /api/project/document OK — '{}' ({} items)",
                                     proj_name, item_count);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "project_changed"},
                });
                return json_ok(header);
            } catch (const std::exception& e) {
                Logger::error("PUT /api/project/document threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    // ---- Items (mirror of client's hierarchical playlist) ----
    // Mutating endpoints return only {ok:true} (plus the affected uuid on
    // add) instead of the full project document. Sending the full doc on
    // every property tweak was saturating the network for large projects
    // and causing WebSocket buffer write errors on slow clients.
    CROW_ROUTE(app, "/api/project/items").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                if (!j.contains("item") || !j["item"].is_object()) {
                    Logger::warn("POST /api/project/items — missing 'item' object");
                    return json_err(400, "missing 'item' object");
                }
                const std::string item_uuid  = j["item"].value("uuid", std::string{});
                const std::string item_name  = j["item"].value("displayName", std::string{});
                const std::string parent_uuid = j.value("parentUuid", std::string{});
                Logger::api_request("POST /api/project/items uuid='{}' name='{}'{}", item_uuid,
                                    item_name, parent_uuid.empty() ? "" : " parent='" + parent_uuid + "'");
                const auto cue_id = state_.add_item(j["item"], parent_uuid);
                Logger::api_response("POST /api/project/items OK — uuid='{}' cueId='{}'",
                                     item_uuid, cue_id.value);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "item_added"},
                    {"uuid", item_uuid},
                    {"parentUuid", parent_uuid},
                    {"item", j["item"]},
                    {"cueId", cue_id.value},
                });
                return json_ok(json({
                    {"ok",    true},
                    {"uuid",  item_uuid},
                    {"cueId", cue_id.value},
                }));
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/items threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    CROW_ROUTE(app, "/api/project/items/<string>").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req, std::string uuid){
            try {
                auto patch = json::parse(req.body);
                Logger::api_request("PATCH /api/project/items/{}", uuid);
                if (!state_.update_item(uuid, patch)) {
                    Logger::warn("PATCH /api/project/items/{} — item not found", uuid);
                    return json_err(404, "item not found");
                }
                Logger::api_response("PATCH /api/project/items/{} OK", uuid);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "item_updated"},
                    {"uuid", uuid}, {"patch", patch},
                });
                return json_ok(json({{"ok", true}, {"uuid", uuid}}));
            } catch (const std::exception& e) {
                Logger::error("PATCH /api/project/items/{} threw: {}", uuid, e.what());
                return json_err(400, e.what());
            }
        });

    CROW_ROUTE(app, "/api/project/items/<string>").methods(crow::HTTPMethod::Delete)
        ([this](std::string uuid){
            Logger::api_request("DELETE /api/project/items/{}", uuid);
            if (!state_.remove_item(uuid)) {
                Logger::warn("DELETE /api/project/items/{} — not found", uuid);
                return json_err(404, "item not found");
            }
            Logger::api_response("DELETE /api/project/items/{} OK", uuid);
            broadcast_doc_patch(json{
                {"type", "doc_patch"}, {"op", "item_removed"}, {"uuid", uuid},
            });
            return json_ok(json({{"ok", true}, {"uuid", uuid}}));
        });

    CROW_ROUTE(app, "/api/project/items/reorder").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const std::string parent_uuid = j.value("parentUuid", std::string{});
                std::vector<std::string> uuids;
                if (j.contains("uuids") && j["uuids"].is_array()) {
                    for (const auto& u : j["uuids"]) {
                        if (u.is_string()) uuids.push_back(u.get<std::string>());
                    }
                }
                Logger::api_request("POST /api/project/items/reorder ({} items){}", uuids.size(),
                                    parent_uuid.empty() ? "" : " parent='" + parent_uuid + "'");
                state_.reorder_items(uuids, parent_uuid);
                Logger::api_response("POST /api/project/items/reorder OK");
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "items_reordered"},
                    {"parentUuid", parent_uuid}, {"uuids", uuids},
                });
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/items/reorder threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    // Item-by-uuid transport. Routed through ProjectState so duckingBehavior,
    // inPoint, and fade settings from the project document are honoured.
    CROW_ROUTE(app, "/api/project/items/<string>/play").methods(crow::HTTPMethod::Post)
        ([this](std::string uuid){
            Logger::playback("PLAY item_uuid={} (REST)", uuid);
            if (!state_.play_item(uuid)) {
                Logger::warn("PLAY item_uuid={} — item not loaded into engine", uuid);
                return json_err(404, "item not loaded into engine");
            }
            Logger::api_response("PLAY item_uuid={} OK", uuid);
            return json_ok(json({{"ok", true}}));
        });
    CROW_ROUTE(app, "/api/project/items/<string>/stop").methods(crow::HTTPMethod::Post)
        ([this](std::string uuid){
            Logger::playback("STOP item_uuid={} (REST)", uuid);
            if (!state_.stop_item(uuid)) {
                Logger::warn("STOP item_uuid={} — item not loaded into engine", uuid);
                return json_err(404, "item not loaded into engine");
            }
            Logger::api_response("STOP item_uuid={} OK", uuid);
            return json_ok(json({{"ok", true}}));
        });
    CROW_ROUTE(app, "/api/project/items/<string>/seek").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string uuid){
            try {
                auto j = json::parse(req.body);
                const double secs = j.value("seconds", 0.0);
                Logger::playback("SEEK {:.2f}s → item_uuid={} (REST)", secs, uuid);
                const auto cue = state_.item_to_cue_id(uuid);
                if (!cue) {
                    Logger::warn("SEEK item_uuid={} — not loaded into engine", uuid);
                    return json_err(404, "item not loaded into engine");
                }
                if (auto* pi = engine_.find_cue(*cue)) {
                    pi->seek_seconds(secs);
                }
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/items/{}/seek threw: {}", uuid, e.what());
                return json_err(400, e.what());
            }
        });

    // ---- Cart slot bindings ----
    CROW_ROUTE(app, "/api/project/cart").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const int slot = j.value("slot", -1);
                const std::string uuid = j.value("itemUuid", std::string{});
                if (slot < 0 || uuid.empty()) {
                    Logger::warn("POST /api/project/cart — slot and itemUuid required");
                    return json_err(400, "slot and itemUuid required");
                }
                Logger::api_request("POST /api/project/cart slot={} itemUuid='{}'", slot, uuid);
                state_.set_cart_slot(slot, uuid);
                Logger::api_response("POST /api/project/cart OK — slot={} uuid='{}'", slot, uuid);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "cart_slot_set"},
                    {"slot", slot}, {"itemUuid", uuid},
                });
                return json_ok(json({{"ok", true}, {"slot", slot}, {"itemUuid", uuid}}));
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/cart threw: {}", e.what());
                return json_err(400, e.what());
            }
        });
    CROW_ROUTE(app, "/api/project/cart/<int>").methods(crow::HTTPMethod::Delete)
        ([this](int slot){
            Logger::api_request("DELETE /api/project/cart/{}", slot);
            state_.clear_cart_slot(slot);
            Logger::api_response("DELETE /api/project/cart/{} OK", slot);
            broadcast_doc_patch(json{
                {"type", "doc_patch"}, {"op", "cart_slot_cleared"}, {"slot", slot},
            });
            return json_ok(json({{"ok", true}, {"slot", slot}}));
        });

    // ---- Preview (DJ-style pre-listening on settings.previewDevice) ----
    CROW_ROUTE(app, "/api/preview").methods(crow::HTTPMethod::Get)
        ([this] {
            const auto item_uuid = state_.current_preview_item_uuid();
            const auto cue_id    = state_.current_preview_cue_id();
            return json_ok(json({
                {"active",   !item_uuid.empty()},
                {"itemUuid", item_uuid},
                {"cueId",    cue_id.value},
            }));
        });
    CROW_ROUTE(app, "/api/preview").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const std::string uuid = j.value("itemUuid", std::string{});
                if (uuid.empty()) {
                    Logger::warn("POST /api/preview — itemUuid required");
                    return json_err(400, "itemUuid required");
                }
                Logger::playback("PREVIEW START item_uuid='{}'", uuid);
                const bool ok = state_.start_preview(uuid);
                if (!ok) {
                    Logger::warn("PREVIEW START failed for item_uuid='{}' — no device or item not found", uuid);
                    return json_err(400, "preview could not start (no device, or item not found)");
                }
                const auto cue_id = state_.current_preview_cue_id().value;
                Logger::api_response("POST /api/preview OK — cueId='{}'", cue_id);
                return json_ok(json({
                    {"ok",      true},
                    {"itemUuid", uuid},
                    {"cueId",   cue_id},
                }));
            } catch (const std::exception& e) {
                Logger::error("POST /api/preview threw: {}", e.what());
                return json_err(400, e.what());
            }
        });
    CROW_ROUTE(app, "/api/preview").methods(crow::HTTPMethod::Delete)
        ([this] {
            Logger::playback("PREVIEW STOP");
            state_.stop_preview();
            Logger::api_response("DELETE /api/preview OK");
            return json_ok(json({{"ok", true}}));
        });

    // ---- Theme + settings patches ----
    CROW_ROUTE(app, "/api/project/theme").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req){
            try {
                auto patch = json::parse(req.body);
                Logger::api_request("PATCH /api/project/theme");
                state_.patch_theme(patch);
                auto theme = state_.full_document()["theme"];
                Logger::api_response("PATCH /api/project/theme OK");
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "theme_patched"}, {"theme", theme},
                });
                return json_ok(theme);
            } catch (const std::exception& e) {
                Logger::error("PATCH /api/project/theme threw: {}", e.what());
                return json_err(400, e.what());
            }
        });
    CROW_ROUTE(app, "/api/project/settings").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req){
            try {
                auto patch = json::parse(req.body);
                Logger::api_request("PATCH /api/project/settings");
                state_.patch_settings(patch);
                auto settings = state_.full_document()["settings"];
                Logger::api_response("PATCH /api/project/settings OK");
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "settings_patched"}, {"settings", settings},
                });
                return json_ok(settings);
            } catch (const std::exception& e) {
                Logger::error("PATCH /api/project/settings threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    // ------------------------------------------------------------------
    // WebSocket
    // ------------------------------------------------------------------
    CROW_WEBSOCKET_ROUTE(app, "/ws")
      .onopen([this](crow::websocket::connection& conn) {
          std::lock_guard lock{impl_->ws_mutex};
          impl_->ws_clients.insert(&conn);
          Logger::info("WS client connected ({} total)", impl_->ws_clients.size());
      })
      .onclose([this](crow::websocket::connection& conn, const std::string& reason, std::uint16_t /*code*/) {
          std::lock_guard lock{impl_->ws_mutex};
          impl_->ws_clients.erase(&conn);
          Logger::info("WS client disconnected ({}); {} remaining",
                       reason, impl_->ws_clients.size());
      })
      .onmessage([this](crow::websocket::connection& conn,
                        const std::string& data,
                        bool is_binary) {
          if (is_binary) return;
          handle_ws_message(conn, data, engine_, state_);
      });
}

} // namespace liveplay::net
