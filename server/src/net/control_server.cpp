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

// Windows defines DELETE as a macro (access-rights constant) which breaks
// crow::HTTPMethod::DELETE. Undefine it after all system/crow includes.
#ifdef DELETE
#  undef DELETE
#endif

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
    j["file_path"]     = c.file_path.string();
    j["artist"]        = c.artist;
    j["title"]         = c.title;
    j["duration_sec"]  = c.duration_seconds;
    j["gain_db"]       = c.gain_db;
    j["fade_in_ms"]    = c.fade_in_ms.count();
    j["fade_out_ms"]   = c.fade_out_ms.count();
    j["ltc"] = json{
        {"enabled",  c.ltc_enabled},
        {"fps",      c.ltc_frame_rate_index},
        {"offset_ns", static_cast<long long>(c.ltc_offset_ns.count())},
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
// Meter broadcaster (~60 fps)
// ---------------------------------------------------------------------------
void ControlServer::broadcast_loop() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds{
        1'000'000'000LL / static_cast<long long>(std::max<std::size_t>(1, cfg_.meter_broadcast_hz))};

    while (running_.load(std::memory_order_acquire)) {
        const auto start = clock::now();

        // Build the meters payload.
        json payload;
        payload["type"] = "meters";

        json item_meters = json::array();
        for (auto& cue : state_.list_cues()) {
            if (auto* item = engine_.find_cue(cue.id)) {
                json m;
                m["cue_id"]   = cue.id.value;
                m["transport"] = static_cast<int>(item->stats().transport);
                m["playhead_seconds"] = item->stats().playhead_seconds;
                json srcs = json::array();
                for (audio::ChannelIndex c = 0; c < item->source_channel_count(); ++c) {
                    auto snap = item->source_meter(c);
                    srcs.push_back(json{{"peak_db", snap.peak_db}, {"rms_db", snap.rms_db}});
                }
                m["sources"] = std::move(srcs);
                item_meters.push_back(std::move(m));
            }
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

        // Fan out to all subscribed clients.
        std::lock_guard lock{impl_->ws_mutex};
        for (auto* c : impl_->ws_clients) {
            try { c->send_text(serialized); }
            catch (...) { /* connection will be cleaned up by onclose */ }
        }

        // Sleep to maintain the broadcast cadence.
        const auto used = clock::now() - start;
        if (used < period) std::this_thread::sleep_for(period - used);
    }
}

// ---------------------------------------------------------------------------
static void handle_ws_message(crow::websocket::connection& conn,
                               const std::string& msg,
                               audio::AudioEngine& engine,
                               core::ProjectState& state) {
    json j;
    try { j = json::parse(msg); }
    catch (const std::exception& e) {
        conn.send_text(json({{"type", "error"}, {"message", e.what()}}).dump());
        return;
    }
    const std::string type = j.value("type", "");
    try {
        if (type == "play")       engine.play(audio::CueId{j.at("cue_id").get<std::string>()});
        else if (type == "stop")  engine.stop(audio::CueId{j.at("cue_id").get<std::string>()});
        else if (type == "stop_all")
            engine.stop_all(std::chrono::milliseconds{j.value("fade_ms", (long long)0)});
        else if (type == "gain")
            state.set_cue_gain_db(audio::CueId{j.at("cue_id").get<std::string>()},
                                   j.value("db", 0.0f));
        else if (type == "fade")
            state.set_cue_fade_in(audio::CueId{j.at("cue_id").get<std::string>()},
                std::chrono::milliseconds{j.value("in_ms", (long long)0)}),
            state.set_cue_fade_out(audio::CueId{j.at("cue_id").get<std::string>()},
                std::chrono::milliseconds{j.value("out_ms", (long long)0)});
        else if (type == "ping")
            conn.send_text(json({{"type", "pong"}}).dump());
        else
            conn.send_text(json({{"type", "error"}, {"message", "unknown type"}}).dump());
    } catch (const std::exception& e) {
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
    CROW_ROUTE(app, "/<path>").methods(crow::HTTPMethod::OPTIONS)
        ([](const crow::request&, std::string){
            crow::response r{204};
            r.add_header("Access-Control-Allow-Origin",  "*");
            r.add_header("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
            r.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            return r;
        });

    // ---- Health ----
    CROW_ROUTE(app, "/api/health").methods(crow::HTTPMethod::GET)
        ([] { return json_ok(json({{"ok", true}, {"name", "liveplay-server"}})); });

    // ---- Devices ----
    CROW_ROUTE(app, "/api/devices").methods(crow::HTTPMethod::GET)
        ([this] {
            json arr = json::array();
            for (auto& d : engine_.enumerate_devices()) arr.push_back(device_info_to_json(d));
            return json_ok(arr);
        });

    CROW_ROUTE(app, "/api/devices/open").methods(crow::HTTPMethod::POST)
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

    CROW_ROUTE(app, "/api/devices/close").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                engine_.close_device(audio::DeviceId{j.at("id").get<std::string>()});
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Cues ----
    CROW_ROUTE(app, "/api/cues").methods(crow::HTTPMethod::GET)
        ([this] {
            json arr = json::array();
            for (auto& c : state_.list_cues()) arr.push_back(cue_to_json(c, engine_));
            return json_ok(arr);
        });

    CROW_ROUTE(app, "/api/cues").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const fs::path file = j.at("file_path").get<std::string>();
                std::string name = j.value("display_name", "");
                const auto id = state_.add_cue_from_file(file, std::move(name));
                if (id.empty()) return json_err(400, "failed to load file");
                auto meta = state_.find_cue(id);
                return json_ok(meta ? cue_to_json(*meta, engine_) : json{{"id", id.value}});
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/cues/<string>").methods(crow::HTTPMethod::GET)
        ([this](std::string id) {
            auto m = state_.find_cue(audio::CueId{id});
            if (!m) return json_err(404, "not found");
            return json_ok(cue_to_json(*m, engine_));
        });

    CROW_ROUTE(app, "/api/cues/<string>").methods(crow::HTTPMethod::DELETE)
        ([this](std::string id) {
            state_.remove_cue(audio::CueId{id});
            return json_ok(json({{"ok", true}}));
        });

    CROW_ROUTE(app, "/api/cues/<string>/play").methods(crow::HTTPMethod::POST)
        ([this](std::string id) {
            engine_.play(audio::CueId{id});
            return json_ok(json({{"ok", true}}));
        });
    CROW_ROUTE(app, "/api/cues/<string>/stop").methods(crow::HTTPMethod::POST)
        ([this](std::string id) {
            engine_.stop(audio::CueId{id});
            return json_ok(json({{"ok", true}}));
        });

    CROW_ROUTE(app, "/api/cues/<string>/gain").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, std::string id){
            try {
                auto j = json::parse(req.body);
                state_.set_cue_gain_db(audio::CueId{id}, j.value("db", 0.0f));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/cues/<string>/fade").methods(crow::HTTPMethod::POST)
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

    CROW_ROUTE(app, "/api/cues/<string>/ltc").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, std::string id){
            try {
                auto j = json::parse(req.body);
                state_.set_cue_ltc(audio::CueId{id},
                                   j.value("enabled", false),
                                   j.value("fps", 4),
                                   std::chrono::nanoseconds{j.value("offset_ns", (long long)0)});
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Transport / master ----
    CROW_ROUTE(app, "/api/transport/stop_all").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body.empty() ? std::string{"{}"} : req.body);
                engine_.stop_all(std::chrono::milliseconds{j.value("fade_ms", (long long)0)});
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/master/ceiling").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                engine_.set_master_ceiling_db(j.value("db", -0.3f));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Mixer channels ----
    CROW_ROUTE(app, "/api/mixers").methods(crow::HTTPMethod::GET)
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

    CROW_ROUTE(app, "/api/mixers").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const auto id = engine_.create_mixer_channel(j.value("name", "Channel"));
                return json_ok(json({{"id", id.value}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/mixers/<string>").methods(crow::HTTPMethod::DELETE)
        ([this](std::string id){
            engine_.remove_mixer_channel(audio::MixerChannelId{id});
            return json_ok(json({{"ok", true}}));
        });

    // ---- Routing ----
    CROW_ROUTE(app, "/api/routing/item_to_mixer").methods(crow::HTTPMethod::POST)
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

    CROW_ROUTE(app, "/api/routing/mixer_to_master").methods(crow::HTTPMethod::POST)
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

    CROW_ROUTE(app, "/api/routing/master_to_device").methods(crow::HTTPMethod::POST)
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
    CROW_ROUTE(app, "/api/fs/list").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req){
            try {
                std::string path = req.url_params.get("path") ? req.url_params.get("path") : "";
                fs::path p = path.empty() ? state_.media_root() : fs::path{path};
                p = fs::weakly_canonical(p);
                if (!fs::exists(p)) return json_err(404, "no such path");

                json out;
                out["path"]    = p.string();
                out["parent"]  = p.has_parent_path() ? p.parent_path().string() : "";
                out["entries"] = json::array();

                if (fs::is_directory(p)) {
                    for (auto& entry : fs::directory_iterator(p, fs::directory_options::skip_permission_denied)) {
                        json e;
                        e["name"]      = entry.path().filename().string();
                        e["full_path"] = entry.path().string();
                        if (entry.is_directory()) {
                            e["kind"] = "dir";
                            out["entries"].push_back(std::move(e));
                        } else if (entry.is_regular_file() && is_audio_file(entry.path())) {
                            e["kind"] = "file";
                            std::error_code ec;
                            e["size"] = static_cast<long long>(fs::file_size(entry.path(), ec));
                            out["entries"].push_back(std::move(e));
                        }
                    }
                }
                return json_ok(out);
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Multipart upload ----
    CROW_ROUTE(app, "/api/upload").methods(crow::HTTPMethod::POST)
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
                    // Strip path traversal.
                    fs::path safe_name = fs::path{filename}.filename();
                    if (safe_name.empty()) safe_name = "upload.bin";
                    fs::path dest = media / safe_name;

                    std::ofstream f{dest, std::ios::binary};
                    if (!f) return json_err(500, "failed to write file");
                    f.write(part.body.data(), static_cast<std::streamsize>(part.body.size()));
                    saved.push_back(dest.string());
                }
                return json_ok(json({{"saved", saved}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Metadata + Waveform ----
    CROW_ROUTE(app, "/api/metadata").methods(crow::HTTPMethod::GET)
        ([](const crow::request& req) {
            const char* path = req.url_params.get("path");
            if (!path) return json_err(400, "missing ?path=");
            const auto md = liveplay::meta::read_metadata(fs::path{path});
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

    CROW_ROUTE(app, "/api/waveform/<string>").methods(crow::HTTPMethod::GET)
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
    CROW_ROUTE(app, "/api/project").methods(crow::HTTPMethod::GET)
        ([this] { return json_ok(state_.to_json()); });

    CROW_ROUTE(app, "/api/project/load").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                if (j.contains("path")) {
                    const fs::path p = j["path"].get<std::string>();
                    if (!state_.load(p)) return json_err(400, "load failed");
                } else if (j.contains("document")) {
                    if (!state_.load_from_json(j["document"])) return json_err(400, "load failed");
                } else {
                    return json_err(400, "expected 'path' or 'document'");
                }
                return json_ok(state_.to_json());
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/project/save").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const fs::path p = j.at("path").get<std::string>();
                return state_.save(p) ? json_ok(json({{"ok", true}}))
                                      : json_err(500, "save failed");
            } catch (const std::exception& e) { return json_err(400, e.what()); }
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
