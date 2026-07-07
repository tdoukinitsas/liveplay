// ============================================================================
// control_server.cpp — see control_server.hpp.
// ============================================================================

// Must come before crow.h on Windows to avoid redefinition of NOMINMAX etc.
// These must only fire on Windows — defining _WIN32_WINNT on macOS/Linux
// makes ASIO's config.hpp think it's a Windows target and try to pull in
// <winapifamily.h>, which obviously doesn't exist there.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0A00
#  endif
#endif

#include "liveplay/net/control_server.hpp"
#include "liveplay/logger.hpp"
#include "liveplay/meta/metadata.hpp"
#include "liveplay/meta/waveform.hpp"
#include "liveplay/util/unicode_path.hpp"

#if defined(_WIN32)
#  include <windows.h>      // GetLogicalDrives(), GetVolumeInformationW(), ...
#  include <winnetwk.h>     // WNetGetConnectionW() — mapped-network-drive UNC
#endif

#include <crow.h>
#include <crow/middlewares/cors.h>
#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>


namespace fs    = std::filesystem;
using json      = nlohmann::json;
namespace audio = liveplay::audio;
namespace core  = liveplay::core;

namespace liveplay::net {

// Forward declarations (definitions further down).
static nlohmann::json build_playback_snapshot(audio::AudioEngine& engine,
                                              core::ProjectState& state);

// Pimpl: all Crow + WebSocket state lives here so crow.h stays out of the
// public header.
struct ControlServer::Impl {
    crow::SimpleApp app;
    std::thread     app_thread;
    std::thread     broadcast_thread;
    std::mutex      ws_mutex;
    std::string     server_addr;
    std::unordered_set<crow::websocket::connection*> ws_clients;
    // Clients that need an initial playback_snapshot push. Populated by
    // onopen, drained by broadcast_loop under ws_mutex — keeps all send_text
    // calls on a single connection serialised through one mutex (Crow's
    // websocket::connection is not safe under concurrent writes; calling
    // send_text directly from onopen while broadcast_loop was concurrently
    // sending meters to the same conn caused the crash on connect).
    std::unordered_set<crow::websocket::connection*> ws_clients_pending_snapshot;

    // Async waveform-generation queue. REST handler enqueues a task and
    // returns immediately; waveform_worker() processes them one at a time and
    // broadcasts a waveform_ready doc_patch when each finishes.
    struct WaveformTask {
        std::filesystem::path path;
        std::string           item_uuid;
        std::filesystem::path waveforms_dir; // empty = no disk cache
        bool                  force{false};  // delete cache and recompute
    };
    std::mutex              waveform_q_mutex;
    std::condition_variable waveform_q_cv;
    std::deque<WaveformTask> waveform_q;
    std::thread             waveform_thread;
    bool                    waveform_stop{false};
};

namespace {

// Bridges Crow's internal logger into our Logger so all server output
// shares the same format and color scheme. Crow INFO logs (verbose
// request/response lines) are routed to debug and hidden by default;
// warnings and errors surface normally.
class CrowLogBridge final : public crow::ILogHandler {
public:
    void log(const std::string& message, crow::LogLevel level) override {
        switch (level) {
            case crow::LogLevel::Warning:  Logger::warn("[Crow] {}", message);  break;
            case crow::LogLevel::Error:    Logger::error("[Crow] {}", message); break;
            case crow::LogLevel::Critical: Logger::error("[Crow] {}", message); break;
            default:                       Logger::debug("[Crow] {}", message); break;
        }
    }
};

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

// Returns "Display Name (cue_id) (media/path)" for playback log lines.
// Falls back gracefully when the item or cue metadata is not yet loaded.
static std::string item_playback_info(const std::string& item_uuid, core::ProjectState& state) {
    const auto cue_id = state.item_to_cue_id(item_uuid);
    if (!cue_id) return std::format("? (uuid={})", item_uuid);
    const auto meta = state.find_cue(*cue_id);
    if (!meta) return std::format("? ({})", cue_id->value);
    return std::format("{} ({}) ({})",
                       meta->display_name,
                       cue_id->value,
                       liveplay::util::path_to_utf8(meta->file_path));
}

// ---------------------------------------------------------------------------
// Zip helpers (.lpa = zip of a project folder)
// ---------------------------------------------------------------------------
// Recursively pack every regular file under `root` into a .zip at `out_zip`.
// Entries are stored with paths relative to `root` using forward slashes,
// so the archive is portable between OSes.
static bool zip_pack_directory(const fs::path& root, const fs::path& out_zip) {
    mz_zip_archive zip{};
    std::memset(&zip, 0, sizeof(zip));
    const std::string out_utf8 = liveplay::util::path_to_utf8(out_zip);
    if (!mz_zip_writer_init_file(&zip, out_utf8.c_str(), 0)) {
        Logger::error("zip_pack_directory: init failed for '{}'", out_utf8);
        return false;
    }
    bool ok = true;
    try {
        for (auto it = fs::recursive_directory_iterator(root);
             it != fs::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            const fs::path rel = fs::relative(it->path(), root);
            // Forward-slash, UTF-8 entry name.
            std::string entry = liveplay::util::path_to_utf8(rel);
            std::replace(entry.begin(), entry.end(), '\\', '/');
            const std::string src = liveplay::util::path_to_utf8(it->path());
            if (!mz_zip_writer_add_file(&zip, entry.c_str(), src.c_str(),
                                        nullptr, 0, MZ_DEFAULT_LEVEL)) {
                Logger::error("zip_pack_directory: add '{}' failed", entry);
                ok = false;
                break;
            }
        }
    } catch (const std::exception& e) {
        Logger::error("zip_pack_directory: walk threw: {}", e.what());
        ok = false;
    }
    if (ok && !mz_zip_writer_finalize_archive(&zip)) {
        Logger::error("zip_pack_directory: finalize failed");
        ok = false;
    }
    mz_zip_writer_end(&zip);
    if (!ok) { std::error_code ec; fs::remove(out_zip, ec); }
    return ok;
}

// Extract every entry in `src_zip` into directory `out_dir`. Paths inside the
// archive are sanitised — leading slashes, "..", and absolute prefixes are
// rejected so a hostile archive can't escape `out_dir`.
static bool zip_extract_to(const fs::path& src_zip, const fs::path& out_dir) {
    mz_zip_archive zip{};
    std::memset(&zip, 0, sizeof(zip));
    const std::string in_utf8 = liveplay::util::path_to_utf8(src_zip);
    if (!mz_zip_reader_init_file(&zip, in_utf8.c_str(), 0)) {
        Logger::error("zip_extract_to: init failed for '{}'", in_utf8);
        return false;
    }
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { ok = false; break; }
        std::string name = st.m_filename;
        // Sanitise: reject absolute or parent-traversal entries.
        if (name.empty()) continue;
        if (name.front() == '/' || name.front() == '\\') {
            Logger::warn("zip_extract_to: skipping absolute entry '{}'", name);
            continue;
        }
        if (name.find("..") != std::string::npos) {
            Logger::warn("zip_extract_to: skipping suspicious entry '{}'", name);
            continue;
        }
        const fs::path dest = out_dir / liveplay::util::utf8_to_path(name);
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(dest, ec);
            continue;
        }
        if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
        const std::string dest_utf8 = liveplay::util::path_to_utf8(dest);
        if (!mz_zip_reader_extract_to_file(&zip, i, dest_utf8.c_str(), 0)) {
            Logger::error("zip_extract_to: extract '{}' failed", name);
            ok = false;
            break;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

// One-shot download tokens. The export endpoint creates a token that points
// at a server-side .lpa; the client redeems the token via GET /api/file/download
// once. Tokens are kept in memory and expire after 10 minutes. A token can
// only be redeemed once — preventing accidental link-sharing leaks.
struct DownloadToken {
    fs::path path;
    std::chrono::steady_clock::time_point expires_at;
};
static std::mutex g_download_tokens_mutex;
static std::unordered_map<std::string, DownloadToken> g_download_tokens;

static std::string make_download_token() {
    // RFC 4122-ish random hex. We don't need crypto strength — these are
    // short-lived single-use claim tickets, not auth credentials.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream os;
    for (int i = 0; i < 4; ++i) os << std::hex << std::setw(16) << std::setfill('0') << rng();
    return os.str();
}

static void register_download_token(const std::string& token, fs::path path) {
    std::lock_guard lock{g_download_tokens_mutex};
    g_download_tokens[token] = DownloadToken{
        std::move(path),
        std::chrono::steady_clock::now() + std::chrono::minutes(10),
    };
}

static std::optional<fs::path> redeem_download_token(const std::string& token) {
    std::lock_guard lock{g_download_tokens_mutex};
    // GC any expired entries while we're here.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_download_tokens.begin(); it != g_download_tokens.end();) {
        if (it->second.expires_at <= now) it = g_download_tokens.erase(it);
        else ++it;
    }
    auto it = g_download_tokens.find(token);
    if (it == g_download_tokens.end()) return std::nullopt;
    fs::path p = std::move(it->second.path);
    g_download_tokens.erase(it);
    return p;
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

    // Hand custom http-request actions off to clients. The server has no
    // HTTP client of its own; broadcasting as a doc_patch lets any
    // connected client execute the fetch. This is a best-effort fan-out;
    // if no client is connected the action is silently dropped.
    state_.set_external_action_handler([this](const json& action) {
        broadcast_doc_patch(json{
            {"type", "doc_patch"},
            {"op",   "custom_action_http"},
            {"action", action},
        });
    });

    // Fan out every "Up Next" change — whether requested by a client or armed
    // by the server itself (#28 auto-cue / first-item / end-of-list wrap) — so
    // all clients mirror the authoritative override instead of each deciding.
    state_.set_next_item_broadcaster([this](const std::string& uuid) {
        broadcast_doc_patch(json{
            {"type", "doc_patch"},
            {"op",   "next_item_set"},
            {"itemUuid", uuid},
        });
    });

    // Crow's SimpleApp::run() blocks; we shove it on a worker thread.
    impl_->app_thread = std::thread([this] {
        try {
            impl_->app.bindaddr(cfg_.bind_address).port(cfg_.port).multithreaded().run();
        } catch (const std::exception& ex) {
            Logger::error("ControlServer: crow run() threw: {}", ex.what());
        }
    });

    impl_->broadcast_thread = std::thread([this] { broadcast_loop(); });
    impl_->waveform_thread  = std::thread([this] { waveform_worker(); });
    Logger::success("Control server listening on {}:{}", cfg_.bind_address, cfg_.port);
    return true;
}

void ControlServer::stop() {
    if (!running_.exchange(false)) return;
    impl_->app.stop();
    {
        std::lock_guard lock{impl_->waveform_q_mutex};
        impl_->waveform_stop = true;
    }
    impl_->waveform_q_cv.notify_one();
    if (impl_->broadcast_thread.joinable()) impl_->broadcast_thread.join();
    if (impl_->waveform_thread.joinable())  impl_->waveform_thread.join();
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
        // Helper: append a meter frame for an arbitrary engine cue. Used for
        // both project cues and the preview cue (which is engine-only, not in
        // state_.list_cues()).
        auto append_meter_for = [&](const audio::CueId& cue_id) {
            auto* item = engine_.find_cue(cue_id);
            if (!item) return;
            const auto stats = item->stats();
            if (stats.transport == audio::TransportState::Stopped) return;
            json m;
            m["cue_id"]            = cue_id.value;
            m["transport"]         = static_cast<int>(stats.transport);
            m["playhead_seconds"]  = stats.playhead_seconds;
            json srcs = json::array();
            for (audio::ChannelIndex c = 0; c < item->source_channel_count(); ++c) {
                auto snap = item->source_meter(c);
                srcs.push_back(json{{"peak_db", snap.peak_db}, {"rms_db", snap.rms_db}});
            }
            m["sources"] = std::move(srcs);
            item_meters.push_back(std::move(m));
        };
        for (auto& cue : state_.list_cues()) append_meter_for(cue.id);
        // Preview cue lives outside list_cues() because it's loaded with
        // load_cue_no_route — emit its frame explicitly so the client's
        // preview card can drive playhead time and the seek bar.
        const auto preview_cue = state_.current_preview_cue_id();
        if (!preview_cue.empty()) append_meter_for(preview_cue);
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

        std::string serialized;
        try { serialized = payload.dump(); }
        catch (const std::exception& e) {
            Logger::error("broadcast_loop: failed to serialize meters: {}", e.what());
            const auto used = clock::now() - start;
            if (used < period) std::this_thread::sleep_for(period - used);
            continue;
        }

        // Detect transport changes and build cue_state edge events.
        std::vector<std::string> cue_state_events;
        try {
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
                    if (auto uuid = state_.cue_to_item_uuid(cue.id)) evt["item_uuid"] = *uuid;
                    cue_state_events.push_back(evt.dump());
                }
            }
        } catch (const std::exception& e) {
            Logger::error("broadcast_loop: failed to build cue_state events: {}", e.what());
        }

        // Build any pending playback_snapshot payload WITHOUT holding ws_mutex.
        // build_playback_snapshot acquires state/engine mutexes internally, and
        // HTTP handlers hold those same mutexes while calling broadcast_doc_patch
        // (which also needs ws_mutex). Acquiring ws_mutex → state mutex from the
        // broadcast thread while HTTP handlers do state mutex → ws_mutex is the
        // classic ABBA deadlock that crashes the server on client connect.
        bool has_pending = false;
        {
            std::lock_guard lock{impl_->ws_mutex};
            has_pending = !impl_->ws_clients_pending_snapshot.empty();
        }
        std::string snapshot_serialized;
        if (has_pending) {
            try {
                snapshot_serialized = build_playback_snapshot(engine_, state_).dump();
            } catch (const std::exception& e) {
                Logger::warn("build_playback_snapshot failed: {}", e.what());
            }
        }

        // Fan out meters + cue_state events to all subscribed clients,
        // plus snapshots for any client still flagged as pending.
        std::lock_guard lock{impl_->ws_mutex};
        for (auto* c : impl_->ws_clients) {
            try {
                if (!snapshot_serialized.empty() &&
                    impl_->ws_clients_pending_snapshot.erase(c)) {
                    c->send_text(snapshot_serialized);
                }
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
    std::string serialized;
    try { serialized = payload.dump(); }
    catch (const std::exception& e) {
        Logger::error("broadcast_doc_patch: serialization failed: {}", e.what());
        return;
    }
    std::lock_guard lock{impl_->ws_mutex};
    for (auto* c : impl_->ws_clients) {
        try { c->send_text(serialized); }
        catch (...) { /* onclose will clean up dead connections */ }
    }
}

// Drains the waveform generation queue. Each task runs compute_waveform()
// (which can take a few seconds for long files) then broadcasts a
// waveform_ready doc_patch so every connected client can update its UI.
void ControlServer::waveform_worker() {
    for (;;) {
        Impl::WaveformTask task;
        {
            std::unique_lock lock{impl_->waveform_q_mutex};
            impl_->waveform_q_cv.wait(lock, [this] {
                return impl_->waveform_stop || !impl_->waveform_q.empty();
            });
            if (impl_->waveform_stop && impl_->waveform_q.empty()) return;
            task = std::move(impl_->waveform_q.front());
            impl_->waveform_q.pop_front();
        }

        const fs::path json_file = task.waveforms_dir.empty()
            ? fs::path{}
            : task.waveforms_dir / (task.item_uuid + ".json");

        // Remove stale cache entry when a forced regeneration is requested.
        if (task.force && !json_file.empty() && fs::exists(json_file)) {
            std::error_code ec;
            fs::remove(json_file, ec);
        }

        // Serve from disk cache when available (skips full audio decode).
        if (!json_file.empty() && fs::exists(json_file)) {
            try {
                std::ifstream cache_f(json_file);
                const auto cached = json::parse(cache_f);
                broadcast_doc_patch(json{
                    {"type",            "doc_patch"},
                    {"op",              "waveform_ready"},
                    {"item_uuid",       task.item_uuid},
                    {"bucket_count",    cached.at("bucket_count")},
                    {"duration_ms",     cached.at("duration_ms")},
                    {"sample_rate",     cached.at("sample_rate")},
                    {"source_channels", cached.at("source_channels")},
                    {"channels",        cached.at("channels")},
                });
                Logger::info("waveform_worker: served cached waveform for '{}'", task.item_uuid);
                continue;
            } catch (const std::exception& e) {
                Logger::warn("waveform_worker: cache read failed for '{}', recomputing: {}", task.item_uuid, e.what());
            }
        }

        Logger::info("waveform_worker: computing waveform for '{}'", liveplay::util::path_to_utf8(task.path));
        const auto wf = liveplay::meta::compute_waveform(task.path);
        if (!wf.ok) {
            Logger::warn("waveform_worker: compute_waveform failed for '{}'", liveplay::util::path_to_utf8(task.path));
            broadcast_doc_patch(json{
                {"type",      "doc_patch"},
                {"op",        "waveform_failed"},
                {"item_uuid", task.item_uuid},
            });
            continue;
        }

        json channels = json::array();
        for (const auto& ch : wf.channels) {
            channels.push_back(json{{"peak", ch.peak}, {"rms", ch.rms}});
        }

        // Build the broadcast payload; also used as the on-disk cache format.
        json patch{
            {"type",            "doc_patch"},
            {"op",              "waveform_ready"},
            {"item_uuid",       task.item_uuid},
            {"bucket_count",    wf.bucket_count},
            {"duration_ms",     wf.duration.count()},
            {"sample_rate",     wf.sample_rate},
            {"source_channels", wf.source_channels},
            {"channels",        std::move(channels)},
        };

        // Persist waveform so future project opens skip recomputation.
        if (!json_file.empty()) {
            try {
                fs::create_directories(task.waveforms_dir);
                std::ofstream out(json_file);
                // Write only the waveform data fields (not WS envelope fields).
                out << json{
                    {"bucket_count",    patch["bucket_count"]},
                    {"duration_ms",     patch["duration_ms"]},
                    {"sample_rate",     patch["sample_rate"]},
                    {"source_channels", patch["source_channels"]},
                    {"channels",        patch["channels"]},
                }.dump();
            } catch (const std::exception& e) {
                Logger::warn("waveform_worker: failed to save waveform cache for '{}': {}", task.item_uuid, e.what());
            }
        }

        broadcast_doc_patch(std::move(patch));
        Logger::info("waveform_worker: done for item_uuid '{}'", task.item_uuid);
    }
}

// Build a snapshot of all currently-known playback state. Sent to each new
// WS client on connect so a freshly-reconnected client immediately mirrors
// what every other client already sees: which cues are playing, where the
// playhead is, the user-set "Up Next" override, and the active preview.
// Without this, after a reconnect (or a second client joining mid-show)
// the UI would think nothing is playing until the next transport edge fires.
static json build_playback_snapshot(audio::AudioEngine& engine,
                                    core::ProjectState& state) {
    json cues_arr = json::array();
    for (auto& cue : state.list_cues()) {
        auto* item = engine.find_cue(cue.id);
        if (!item) continue;
        const auto s = item->stats();
        if (s.transport == audio::TransportState::Stopped) continue;
        json entry{
            {"cue_id",           cue.id.value},
            {"transport",        static_cast<int>(s.transport)},
            {"playhead_seconds", s.playhead_seconds},
        };
        if (auto uuid = state.cue_to_item_uuid(cue.id)) entry["item_uuid"] = *uuid;
        cues_arr.push_back(std::move(entry));
    }
    json out_gains = json::array();
    for (audio::MasterChannelIndex i = 0; i < engine.config().master_channels; ++i) {
        const float db = engine.output_channel_gain_db(i);
        if (db != 0.0f) out_gains.push_back(json{{"channel", i}, {"db", db}});
    }
    return json{
        {"type",                "playback_snapshot"},
        {"cues",                std::move(cues_arr)},
        {"next_item_uuid",      state.next_item_override()},
        {"master_gain_db",      engine.master_gain_db()},
        {"output_channel_gains", std::move(out_gains)},
        {"preview", json{
            {"item_uuid", state.current_preview_item_uuid()},
            {"cue_id",    state.current_preview_cue_id().value},
        }},
    };
}

// ---------------------------------------------------------------------------
// Returns a non-empty string if a direct reply to this specific client is
// needed (pong, error). The caller sends it under ws_mutex so it doesn't
// race with broadcast_loop's concurrent send_text calls on the same conn.
static std::string handle_ws_message(crow::websocket::connection& conn,
                                     const std::string& msg,
                                     audio::AudioEngine& engine,
                                     core::ProjectState& state,
                                     const std::string& server_addr) {
    Logger::api_request("Client ({}) -> Server ({}) : {}", conn.get_remote_ip(), server_addr, msg);

    json j;
    try { j = json::parse(msg); }
    catch (const std::exception& e) {
        Logger::warn("WS message parse failed: {}", e.what());
        return json({{"type", "error"}, {"message", e.what()}}).dump();
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
                Logger::playback("PLAY: {}", item_playback_info(uuid, state));
                // trigger_item dispatches by item type: audio → play_item,
                // group → walks startBehavior. Without this, WS plays of
                // group items were silently ignored.
                state.trigger_item(uuid);
            } else {
                auto cue = resolve_cue(j);
                if (cue) {
                    // If this cue corresponds to a project item, route
                    // through play_item so duckingBehavior / inPoint /
                    // fades / endBehavior / sequencer auto-advance fire.
                    // Only fall back to raw engine.play() for orphan cues
                    // (e.g. ad-hoc /api/cues registrations with no item).
                    if (auto uuid = state.cue_to_item_uuid(*cue)) {
                        Logger::playback("PLAY: {}", item_playback_info(*uuid, state));
                        state.play_item(*uuid);
                    } else {
                        Logger::playback("PLAY: cue_id={} (orphan)", cue->value);
                        engine.play(*cue);
                    }
                } else {
                    Logger::warn("WS play: no valid cue target in message");
                }
            }
        }
        else if (type == "stop") {
            if (j.contains("item_uuid") && j["item_uuid"].is_string()) {
                const auto uuid = j["item_uuid"].get<std::string>();
                Logger::playback("STOP: {}", item_playback_info(uuid, state));
                state.stop_item(uuid);
            } else {
                auto cue = resolve_cue(j);
                if (cue) {
                    if (auto uuid = state.cue_to_item_uuid(*cue)) {
                        Logger::playback("STOP: {}", item_playback_info(*uuid, state));
                        state.stop_item(*uuid);
                    } else {
                        Logger::playback("STOP: cue_id={} (orphan)", cue->value);
                        engine.stop(*cue);
                    }
                } else {
                    Logger::warn("WS stop: no valid cue target in message");
                }
            }
        }
        else if (type == "pause" || type == "resume") {
            // Pause/resume hold the playhead without unloading. Routed via
            // item_uuid (preferred) or cue_id. No effect on Stopped cues.
            std::optional<audio::CueId> cue;
            if (j.contains("item_uuid") && j["item_uuid"].is_string()) {
                cue = state.item_to_cue_id(j["item_uuid"].get<std::string>());
            } else {
                cue = resolve_cue(j);
            }
            if (cue) {
                if (auto* pi = engine.find_cue(*cue)) {
                    Logger::playback("{} cue_id={}",
                                     type == "pause" ? "PAUSE" : "RESUME",
                                     cue->value);
                    if (type == "pause") pi->pause();
                    else                 pi->resume();
                }
            } else {
                Logger::warn("WS {}: no valid cue target", type);
            }
        }
        else if (type == "stop_all") {
            // Omitted fade_ms → server applies the project-wide default
            // (settings.stopAllFadeMs, default 1000 ms). An explicit fade_ms
            // (incl. 0 for an instant panic) is used verbatim. Global fade wins.
            std::optional<long long> fade;
            if (j.contains("fade_ms") && j["fade_ms"].is_number())
                fade = j["fade_ms"].get<long long>();
            Logger::playback("STOP ALL (fade {})",
                             fade ? std::to_string(*fade) + "ms" : "project default");
            state.stop_all_cues(fade);
        }
        else if (type == "gain") {
            auto cue = resolve_cue(j);
            if (cue) {
                const float db = j.value("db", 0.0f);
                Logger::api_request("Client ({}) -> Server ({}) : WS gain cue_id={} db={:.1f}",
                                    conn.get_remote_ip(), server_addr, cue->value, db);
                state.set_cue_gain_db(*cue, db);
            }
        }
        else if (type == "fade") {
            auto cue = resolve_cue(j);
            if (cue) {
                const auto in_ms  = j.value("in_ms",  (long long)0);
                const auto out_ms = j.value("out_ms", (long long)0);
                Logger::api_request("Client ({}) -> Server ({}) : WS fade cue_id={} in={}ms out={}ms",
                                    conn.get_remote_ip(), server_addr, cue->value, in_ms, out_ms);
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
            if (uuid.empty())
                Logger::playback("SET NEXT: <clear>");
            else
                Logger::playback("SET NEXT: {}", item_playback_info(uuid, state));
            state.set_next_item_override(uuid);
            // Fan-out to every client happens in the .onmessage wrapper
            // (which has access to the ControlServer for broadcast).
        }
        else if (type == "ping") {
            return json({{"type", "pong"}}).dump();
        }
        else {
            Logger::warn("WS unknown message type: {}", type);
            return json({{"type", "error"}, {"message", "unknown type"}}).dump();
        }
    } catch (const std::exception& e) {
        Logger::error("WS handler threw: {}", e.what());
        return json({{"type", "error"}, {"message", e.what()}}).dump();
    } catch (...) {
        Logger::error("WS handler caught unknown exception.");
        return json({{"type", "error"}, {"message", "internal error"}}).dump();
    }
    return {};
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------
void ControlServer::install_routes() {
    auto& app = impl_->app;
    impl_->server_addr = std::format("{}:{}", cfg_.bind_address, cfg_.port);

    // Route Crow's internal logs through our Logger and silence the noisy
    // per-request INFO lines (we log those ourselves via api_request/api_response).
    static CrowLogBridge crow_log_bridge;
    crow::logger::setHandler(&crow_log_bridge);
    crow::logger::setLogLevel(crow::LogLevel::Warning);

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
        ([] {
            try { return json_ok(json({{"ok", true}, {"name", "liveplay-server"}})); }
            catch (...) { return json_err(500, "internal error"); }
        });

    // Returns the requesting client's IP as seen by the server, plus a
    // boolean `isLocal` that's true when the client lives on the same
    // machine (loopback addresses). Used by the import/export flows to
    // decide whether to offer the dual-dialog choice — picking files from
    // "this computer" only makes sense when client and server are different
    // machines.
    CROW_ROUTE(app, "/api/whoami").methods(crow::HTTPMethod::Get)
        ([](const crow::request& req){
            const std::string ip = req.remote_ip_address;
            // Loopback test covers IPv4 127.0.0.0/8 plus the usual IPv6 forms.
            const bool is_local =
                ip == "127.0.0.1" ||
                ip == "::1" ||
                ip == "0:0:0:0:0:0:0:1" ||
                ip == "::ffff:127.0.0.1" ||
                ip.rfind("127.", 0) == 0;
            return json_ok(json{{"clientIp", ip}, {"isLocal", is_local}});
        });

    // ---- Devices ----
    CROW_ROUTE(app, "/api/devices").methods(crow::HTTPMethod::Get)
        ([this] {
            try {
                json arr = json::array();
                for (auto& d : engine_.enumerate_devices()) arr.push_back(device_info_to_json(d));
                return json_ok(arr);
            } catch (const std::exception& e) { return json_err(500, e.what()); }
            catch (...) { return json_err(500, "unknown error enumerating devices"); }
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
                // Omitted fade_ms → project-wide default; explicit value used
                // verbatim (0 = instant). Global fade wins over per-track fades.
                std::optional<long long> fade;
                if (j.contains("fade_ms") && j["fade_ms"].is_number())
                    fade = j["fade_ms"].get<long long>();
                state_.stop_all_cues(fade);
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

    CROW_ROUTE(app, "/api/master/gain").methods(crow::HTTPMethod::Get)
        ([this]{ return json_ok(json({{"db", engine_.master_gain_db()}})); });
    CROW_ROUTE(app, "/api/master/gain").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const float db = j.value("db", 0.0f);
                engine_.set_master_gain_db(db);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "master_gain_changed"},
                    {"db", engine_.master_gain_db()},
                });
                return json_ok(json({{"ok", true}, {"db", engine_.master_gain_db()}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // Per-output-channel gain. GET returns all channels; POST body { "db": float }
    // sets the gain for a specific master channel index.
    CROW_ROUTE(app, "/api/master/channels/<int>/gain").methods(crow::HTTPMethod::Get)
        ([this](int idx){
            if (idx < 0) return json_err(400, "invalid channel index");
            return json_ok(json({
                {"channel", idx},
                {"db", engine_.output_channel_gain_db(static_cast<audio::MasterChannelIndex>(idx))},
            }));
        });
    CROW_ROUTE(app, "/api/master/channels/<int>/gain").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, int idx){
            try {
                if (idx < 0) return json_err(400, "invalid channel index");
                auto j = json::parse(req.body);
                const float db = j.value("db", 0.0f);
                const auto ch = static_cast<audio::MasterChannelIndex>(idx);
                engine_.set_output_channel_gain_db(ch, db);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "output_channel_gain_changed"},
                    {"channel", idx}, {"db", engine_.output_channel_gain_db(ch)},
                });
                return json_ok(json({
                    {"ok", true}, {"channel", idx},
                    {"db", engine_.output_channel_gain_db(ch)},
                }));
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

                    auto add_entry = [&](const std::string& name,
                                         const std::string& full,
                                         const char* kind) {
                        if (full.empty()) return;
                        json e;
                        e["name"]      = name;
                        e["full_path"] = full;
                        e["kind"]      = kind;
                        out["entries"].push_back(std::move(e));
                    };

                    // Home shortcut on every platform. The dialog used to open
                    // at "/" with no way to reach $HOME or a mounted USB stick,
                    // which made opening a project off removable media painful
                    // on Linux especially. (#31)
#if defined(_WIN32)
                    if (const char* up = std::getenv("USERPROFILE"))
                        add_entry("Home", up, "home");
#else
                    if (const char* hp = std::getenv("HOME"))
                        add_entry("Home", hp, "home");
#endif

#if defined(_WIN32)
                    // Enumerate logical drives WITH volume label + drive type,
                    // e.g. "Local Disk (C:)" / "MoviesAndTV (P:)" rather than a
                    // bare "C:". Mapped network drives also resolve their UNC
                    // target for display. (#31)
                    auto wide_to_utf8 = [](const std::wstring& w) -> std::string {
                        if (w.empty()) return {};
                        const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                            static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
                        if (len <= 0) return {};
                        std::string s(static_cast<std::size_t>(len), '\0');
                        WideCharToMultiByte(CP_UTF8, 0, w.data(),
                            static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
                        return s;
                    };
                    DWORD mask = GetLogicalDrives();
                    for (char letter = 'A'; letter <= 'Z'; ++letter, mask >>= 1) {
                        if (!(mask & 1)) continue;
                        const std::wstring wroot =
                            std::wstring{} + static_cast<wchar_t>(letter) + L":\\";
                        const std::string  root = std::string{letter} + ":\\";
                        const UINT dtype = GetDriveTypeW(wroot.c_str());
                        wchar_t vol[MAX_PATH + 1] = {0};
                        std::string label;
                        if (GetVolumeInformationW(wroot.c_str(), vol, MAX_PATH,
                                nullptr, nullptr, nullptr, nullptr, 0)) {
                            label = wide_to_utf8(vol);
                        }
                        if (dtype == DRIVE_REMOTE) {
                            wchar_t remote[512];
                            DWORD rlen = 512;
                            const std::wstring dev =
                                std::wstring{} + static_cast<wchar_t>(letter) + L":";
                            if (WNetGetConnectionW(dev.c_str(), remote, &rlen) == NO_ERROR) {
                                const std::string unc = wide_to_utf8(remote);
                                if (!unc.empty())
                                    label = label.empty()
                                        ? unc : label + " (" + unc + ")";
                            }
                        }
                        if (label.empty()) {
                            switch (dtype) {
                                case DRIVE_REMOVABLE: label = "Removable Disk"; break;
                                case DRIVE_REMOTE:    label = "Network Drive";  break;
                                case DRIVE_CDROM:     label = "CD Drive";       break;
                                case DRIVE_RAMDISK:   label = "RAM Disk";       break;
                                default:              label = "Local Disk";     break;
                            }
                        }
                        add_entry(label + " (" + std::string{letter} + ":)",
                                  root, "drive");
                    }
#elif defined(__APPLE__)
                    // On macOS every mounted volume — the startup disk, external
                    // and USB drives, and network shares — lives under /Volumes.
                    add_entry("Computer", "/", "drive");
                    std::error_code vol_ec;
                    if (fs::is_directory("/Volumes", vol_ec)) {
                        for (auto& ent : fs::directory_iterator("/Volumes",
                                 fs::directory_options::skip_permission_denied, vol_ec)) {
                            std::error_code d_ec;
                            if (!ent.is_directory(d_ec)) continue;
                            add_entry(liveplay::util::path_to_utf8(ent.path().filename()),
                                      liveplay::util::path_to_utf8(ent.path()), "drive");
                        }
                    }
#else
                    // Linux/other POSIX: filesystem root plus auto-mounted media
                    // (USB sticks, network shares). udisks2/desktop environments
                    // mount removable media under /media/<user> or
                    // /run/media/<user>; fall back to a bare /media on
                    // single-user setups, and always include /mnt. (#31)
                    add_entry("File System", "/", "drive");
                    std::vector<std::string> mount_parents;
                    const char* user = std::getenv("USER");
                    bool have_user_media = false;
                    if (user) {
                        std::error_code u_ec;
                        const std::string um  = std::string("/media/") + user;
                        const std::string urm = std::string("/run/media/") + user;
                        if (fs::is_directory(um, u_ec))  { mount_parents.push_back(um);  have_user_media = true; }
                        if (fs::is_directory(urm, u_ec)) { mount_parents.push_back(urm); have_user_media = true; }
                    }
                    if (!have_user_media) mount_parents.push_back("/media");
                    mount_parents.push_back("/mnt");
                    std::set<std::string> seen;
                    for (const auto& parent : mount_parents) {
                        std::error_code m_ec;
                        if (!fs::is_directory(parent, m_ec)) continue;
                        for (auto& ent : fs::directory_iterator(parent,
                                 fs::directory_options::skip_permission_denied, m_ec)) {
                            std::error_code d_ec;
                            if (!ent.is_directory(d_ec)) continue;
                            const std::string full = liveplay::util::path_to_utf8(ent.path());
                            if (!seen.insert(full).second) continue;
                            add_entry(liveplay::util::path_to_utf8(ent.path().filename()),
                                      full, "drive");
                        }
                    }
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

    // POST /api/fs/mkdir  body: { "path": "<utf8-absolute-path>" }
    // Creates a new directory (and all parent directories). Returns { "path": "<created>" }.
    CROW_ROUTE(app, "/api/fs/mkdir").methods(crow::HTTPMethod::Post)
        ([](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                if (!j.contains("path") || !j["path"].is_string())
                    return json_err(400, "missing 'path'");
                const fs::path dir = liveplay::util::utf8_to_path(j["path"].get<std::string>());
                if (dir.empty()) return json_err(400, "empty path");
                std::error_code ec;
                fs::create_directories(dir, ec);
                if (ec) return json_err(400, ec.message());
                return json_ok(json({{"path", liveplay::util::path_to_utf8(dir)}}));
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

    // Copy an existing server-side file into the project's media root.
    // Used by the client when the user picks a file from the server file
    // browser — the file lives somewhere on disk but needs to land in the
    // project media folder before the engine can own it.
    // Body: { "source_path": "/absolute/path/to/file.ext" }
    // Response: { "dest_path": "/absolute/path/to/media/file.ext" }
    CROW_ROUTE(app, "/api/copy_to_media").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            try {
                const auto body = json::parse(req.body);
                const std::string src_str = body.value("source_path", std::string{});
                if (src_str.empty()) return json_err(400, "missing source_path");

                const fs::path src  = liveplay::util::utf8_to_path(src_str);
                if (!fs::exists(src)) return json_err(404, "source file not found");

                const fs::path media = state_.media_root();
                if (media.empty()) return json_err(500, "media root not configured");

                fs::create_directories(media);
                const fs::path dest = media / src.filename();

                // Skip the copy only when src and dest are the same file. Use
                // weakly_canonical, NOT canonical: canonical() throws when the
                // path doesn't exist, and dest normally does NOT exist yet on a
                // first import — that threw, returned 500, and the client fell
                // back to the original out-of-folder path, so the media never
                // landed in the project folder (the import bug).
                std::error_code ec;
                if (fs::weakly_canonical(src, ec) != fs::weakly_canonical(dest, ec)) {
                    fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
                }

                return json_ok(json{{"dest_path", liveplay::util::path_to_utf8(dest)}});
            } catch (const std::exception& e) { return json_err(500, e.what()); }
        });

    // Queue an async waveform computation for the given file. Returns
    // immediately; the result arrives as a waveform_ready doc_patch over
    // WebSocket once the worker thread finishes.
    // Body: { "path": "/abs/path/to/file.ext", "item_uuid": "<uuid>" }
    CROW_ROUTE(app, "/api/waveform_generate").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            try {
                const auto body = json::parse(req.body);
                const std::string path_str  = body.value("path", std::string{});
                const std::string item_uuid = body.value("item_uuid", std::string{});
                const bool        force     = body.value("force", false);
                if (path_str.empty() || item_uuid.empty())
                    return json_err(400, "missing path or item_uuid");

                const auto proj_path = state_.project_file_path();
                const auto wdir = proj_path.empty()
                    ? fs::path{}
                    : proj_path.parent_path() / "waveforms";

                {
                    std::lock_guard lock{impl_->waveform_q_mutex};
                    impl_->waveform_q.push_back({
                        liveplay::util::utf8_to_path(path_str),
                        item_uuid,
                        wdir,
                        force
                    });
                }
                impl_->waveform_q_cv.notify_one();
                return json_ok(json{{"ok", true}});
            } catch (const std::exception& e) { return json_err(500, e.what()); }
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
            try {
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
            } catch (const std::exception& e) { return json_err(500, e.what()); }
            catch (...) { return json_err(500, "unknown error computing waveform"); }
        });

    // Compute waveform for an arbitrary file path (no cue registration needed).
    // Used by the client immediately after import, before the cue is registered
    // with the engine. Query params: path=<absolute-path>&buckets=<count>.
    CROW_ROUTE(app, "/api/waveform_path").methods(crow::HTTPMethod::Get)
        ([](const crow::request& req) {
            try {
                const auto* path_param = req.url_params.get("path");
                if (!path_param) return json_err(400, "missing path parameter");

                std::uint32_t buckets = 1000;
                if (req.url_params.get("buckets")) {
                    try { buckets = static_cast<std::uint32_t>(std::stoi(req.url_params.get("buckets"))); }
                    catch (...) {}
                }

                const std::filesystem::path file_path =
                    liveplay::util::utf8_to_path(std::string{path_param});

                const auto wf = liveplay::meta::compute_waveform(file_path, buckets);
                if (!wf.ok) return json_err(500, "waveform decode failed");

                json channels = json::array();
                for (const auto& ch : wf.channels) {
                    channels.push_back(json{{"peak", ch.peak}, {"rms", ch.rms}});
                }
                return json_ok(json{
                    {"bucket_count",    wf.bucket_count},
                    {"duration_ms",     wf.duration.count()},
                    {"sample_rate",     wf.sample_rate},
                    {"source_channels", wf.source_channels},
                    {"channels",        std::move(channels)},
                });
            } catch (const std::exception& e) { return json_err(500, e.what()); }
            catch (...) { return json_err(500, "unknown error computing waveform"); }
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
        ([this](const crow::request& req) {
            Logger::api_request("Client ({}) -> Server ({}) : GET /api/project/header",
                                req.remote_ip_address, impl_->server_addr);
            auto hdr = state_.header_document();
            Logger::api_response("Client ({}) <- Server ({}) : GET /api/project/header OK — '{}' ({} items)",
                                 req.remote_ip_address, impl_->server_addr,
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
            Logger::api_request("Client ({}) -> Server ({}) : GET /api/project/items offset={} limit={}",
                                req.remote_ip_address, impl_->server_addr, offset, limit);
            auto page = state_.items_page(offset, limit);
            Logger::api_response("Client ({}) <- Server ({}) : GET /api/project/items offset={} → {}/{} items",
                                 req.remote_ip_address, impl_->server_addr, offset,
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
                    Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/load path='{}'",
                                        req.remote_ip_address, impl_->server_addr, path_str);
                    const fs::path p = liveplay::util::utf8_to_path(path_str);
                    if (!state_.load(p)) {
                        Logger::error("POST /api/project/load FAILED — load returned false for '{}'", path_str);
                        return json_err(400, "load failed");
                    }
                } else if (j.contains("document")) {
                    Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/load (from document)",
                                        req.remote_ip_address, impl_->server_addr);
                    if (!state_.load_from_json(j["document"])) {
                        Logger::error("POST /api/project/load FAILED — document rejected");
                        return json_err(400, "load failed");
                    }
                } else {
                    Logger::warn("POST /api/project/load — missing 'path' or 'document' in body");
                    return json_err(400, "expected 'path' or 'document'");
                }
                auto repair = state_.consume_repair_info();
                auto header = state_.header_document();
                const std::size_t item_count = header.value("itemCount", (std::size_t)0);
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/load OK — '{}' ({} items){}",
                                     req.remote_ip_address, impl_->server_addr,
                                     header.value("name", "?"), item_count,
                                     repair.repaired ? " [repaired]" : "");
                // Attach repair metadata so the client can prompt the user.
                header["needsRepair"] = repair.repaired;
                if (repair.repaired) {
                    auto issues = json::array();
                    for (const auto& iss : repair.issues) issues.push_back(iss);
                    header["repairIssues"] = std::move(issues);
                }
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "project_changed"},
                });
                return json_ok(header);
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/load threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    // Close the currently-loaded project on the server. After this the
    // server has no open project — the next /api/project/header will report
    // hasOpenProject=false and clients land back on the welcome screen. We
    // broadcast a project_changed doc_patch so any other connected clients
    // also drop their local mirror.
    CROW_ROUTE(app, "/api/project/close").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/close",
                                    req.remote_ip_address, impl_->server_addr);
                state_.reset();
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "project_changed"},
                });
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/close OK",
                                     req.remote_ip_address, impl_->server_addr);
                return json_ok(json{{"closed", true}});
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/close threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    // ---- Project export / import (.lpa archives) ----
    // Package a project folder into a .lpa (zip) archive on the server side.
    // Request body:
    //   {
    //     "folderPath": "/abs/path/to/project/folder",   // required
    //     "outputPath": "/abs/path/to/save/here.lpa",    // optional; when
    //                                                    // present, the file
    //                                                    // is written to this
    //                                                    // server location.
    //     "projectName": "MyShow"                        // optional, used to
    //                                                    // build a default
    //                                                    // filename when
    //                                                    // outputPath is
    //                                                    // omitted.
    //   }
    // If `outputPath` is omitted the archive is written to a temp directory
    // on the server and a one-shot download token is returned so the client
    // can fetch it back via GET /api/file/download?token=…
    // Response: { "archivePath": "...", "downloadToken": "..." (optional),
    //             "size": <bytes> }
    CROW_ROUTE(app, "/api/project/export").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/export",
                                    req.remote_ip_address, impl_->server_addr);
                auto j = json::parse(req.body);
                if (!j.contains("folderPath") || !j["folderPath"].is_string()) {
                    return json_err(400, "expected 'folderPath'");
                }
                const fs::path src = liveplay::util::utf8_to_path(
                    j["folderPath"].get<std::string>());
                if (!fs::exists(src) || !fs::is_directory(src)) {
                    return json_err(400, "folderPath does not exist or is not a directory");
                }
                const std::string default_name =
                    j.value("projectName", src.filename().string()) + ".lpa";

                fs::path out;
                bool to_temp = false;
                if (j.contains("outputPath") && j["outputPath"].is_string() &&
                    !j["outputPath"].get<std::string>().empty()) {
                    out = liveplay::util::utf8_to_path(j["outputPath"].get<std::string>());
                    if (out.has_parent_path()) fs::create_directories(out.parent_path());
                } else {
                    // Stage in a temp directory; surface via download token.
                    fs::path tmp = fs::temp_directory_path() / "liveplay-exports";
                    fs::create_directories(tmp);
                    out = tmp / default_name;
                    to_temp = true;
                }

                if (!zip_pack_directory(src, out)) {
                    return json_err(500, "failed to package archive");
                }
                std::uintmax_t size = 0;
                try { size = fs::file_size(out); } catch (...) {}

                json resp = {
                    {"archivePath", liveplay::util::path_to_utf8(out)},
                    {"size",        static_cast<std::uint64_t>(size)},
                };
                if (to_temp) {
                    const std::string token = make_download_token();
                    register_download_token(token, out);
                    resp["downloadToken"] = token;
                    resp["downloadFilename"] = default_name;
                }
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/export OK — '{}' ({} bytes)",
                                     req.remote_ip_address, impl_->server_addr,
                                     liveplay::util::path_to_utf8(out), size);
                return json_ok(resp);
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/export threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    // Stream a server-side file to the client by one-shot download token.
    // The token is consumed (single-use) on success. Used by the export flow
    // when the user picks "Save on my computer" and the .lpa was packaged in
    // a temp dir server-side.
    CROW_ROUTE(app, "/api/file/download").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req){
            const char* token = req.url_params.get("token");
            if (!token) return json_err(400, "missing ?token=");
            auto path_opt = redeem_download_token(token);
            if (!path_opt) return json_err(404, "token expired or invalid");
            const fs::path& p = *path_opt;
            std::ifstream f{p, std::ios::binary | std::ios::ate};
            if (!f) return json_err(500, "failed to open archive");
            const auto size = f.tellg();
            f.seekg(0, std::ios::beg);
            std::string body(static_cast<std::size_t>(size), '\0');
            f.read(body.data(), size);

            crow::response r{200, std::move(body)};
            r.add_header("Content-Type", "application/octet-stream");
            r.add_header("Content-Disposition",
                         "attachment; filename=\"" + p.filename().string() + "\"");
            r.add_header("Access-Control-Allow-Origin", "*");
            // The temp file has served its purpose; delete it to bound disk
            // usage on the server.
            std::error_code ec; fs::remove(p, ec);
            return r;
        });

    // Import a .lpa archive that the client uploaded via multipart, OR an
    // archive already sitting on the server's filesystem (by absolute path).
    // Request body:
    //   * multipart/form-data with one part named "file" and the uploaded
    //     .lpa, PLUS a "extractPath" form field for the destination directory
    //     on the server. The archive is extracted, then the upload is
    //     deleted. Response includes `projectFiles` (list of .liveplay files
    //     discovered) and `extractPath`.
    //   * application/json: { "archivePath": "/abs/path.lpa", "extractPath": "/abs/dest" }
    //     Same response shape; no upload step.
    CROW_ROUTE(app, "/api/project/import").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/import",
                                    req.remote_ip_address, impl_->server_addr);
                fs::path archive_path;
                fs::path extract_path;
                bool delete_archive_after = false;

                const auto ct_it = req.headers.find("Content-Type");
                const std::string ct = ct_it != req.headers.end() ? ct_it->second : "";

                if (ct.find("multipart/") != std::string::npos) {
                    crow::multipart::message mp{req};
                    std::string filename = "import.lpa";
                    const crow::multipart::part* file_part = nullptr;
                    for (const auto& part : mp.parts) {
                        auto cd = part.headers.find("Content-Disposition");
                        if (cd == part.headers.end()) continue;
                        auto name_it = cd->second.params.find("name");
                        if (name_it == cd->second.params.end()) continue;
                        if (name_it->second == "file") {
                            file_part = &part;
                            auto fn = cd->second.params.find("filename");
                            if (fn != cd->second.params.end() && !fn->second.empty())
                                filename = fn->second;
                        } else if (name_it->second == "extractPath") {
                            extract_path = liveplay::util::utf8_to_path(part.body);
                        }
                    }
                    if (!file_part) return json_err(400, "missing 'file' part");
                    if (extract_path.empty())
                        return json_err(400, "missing 'extractPath' form field");
                    fs::path tmp = fs::temp_directory_path() / "liveplay-imports";
                    fs::create_directories(tmp);
                    archive_path = tmp /
                        (liveplay::util::utf8_to_path(filename).filename().empty()
                            ? fs::path{"import.lpa"}
                            : liveplay::util::utf8_to_path(filename).filename());
                    std::ofstream of{archive_path, std::ios::binary};
                    if (!of) return json_err(500, "failed to stage uploaded archive");
                    of.write(file_part->body.data(),
                             static_cast<std::streamsize>(file_part->body.size()));
                    of.close();
                    delete_archive_after = true;
                } else {
                    auto j = json::parse(req.body);
                    if (!j.contains("archivePath") || !j["archivePath"].is_string())
                        return json_err(400, "expected 'archivePath'");
                    if (!j.contains("extractPath") || !j["extractPath"].is_string())
                        return json_err(400, "expected 'extractPath'");
                    archive_path = liveplay::util::utf8_to_path(j["archivePath"].get<std::string>());
                    extract_path = liveplay::util::utf8_to_path(j["extractPath"].get<std::string>());
                }

                if (!fs::exists(archive_path))
                    return json_err(400, "archive does not exist");
                fs::create_directories(extract_path);

                if (!zip_extract_to(archive_path, extract_path)) {
                    if (delete_archive_after) { std::error_code ec; fs::remove(archive_path, ec); }
                    return json_err(500, "extract failed");
                }
                if (delete_archive_after) { std::error_code ec; fs::remove(archive_path, ec); }

                // Find all .liveplay files in the extracted folder (top-level).
                json project_files = json::array();
                for (auto& e : fs::directory_iterator(extract_path)) {
                    if (e.is_regular_file() && e.path().extension() == ".liveplay") {
                        project_files.push_back(e.path().filename().string());
                    }
                }
                json resp = {
                    {"extractPath",  liveplay::util::path_to_utf8(extract_path)},
                    {"projectFiles", std::move(project_files)},
                };
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/import OK — extracted to '{}'",
                                     req.remote_ip_address, impl_->server_addr,
                                     liveplay::util::path_to_utf8(extract_path));
                return json_ok(resp);
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/import threw: {}", e.what());
                return json_err(400, e.what());
            }
        });

    // Repair the currently-loaded project and save it to disk. Called by the
    // client after the user confirms the repair prompt.
    CROW_ROUTE(app, "/api/project/repair").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/repair",
                                    req.remote_ip_address, impl_->server_addr);
                // The document was already repaired on load (load_from_json
                // ran detect_and_repair before storing it). Calling
                // repair_project() here just re-validates — it is a no-op if
                // the in-memory doc is already clean. We must still save
                // unconditionally, because the file on disk is the
                // unrepaired original and the user just confirmed they want
                // the repair persisted.
                const auto repair = state_.repair_project();
                const auto path = state_.project_file_path();
                bool saved = false;
                if (!path.empty()) {
                    if (!state_.save(path)) {
                        Logger::error("POST /api/project/repair — save failed for '{}'",
                                      liveplay::util::path_to_utf8(path));
                        return json_err(500, "repair succeeded but save failed");
                    }
                    saved = true;
                }
                auto issues = json::array();
                for (const auto& iss : repair.issues) issues.push_back(iss);
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/repair OK — repaired={} saved={}",
                                     req.remote_ip_address, impl_->server_addr,
                                     repair.repaired, saved);
                return json_ok(json{{"repaired", repair.repaired}, {"issues", std::move(issues)}, {"saved", saved}});
            } catch (const std::exception& e) {
                Logger::error("POST /api/project/repair threw: {}", e.what());
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
                // Authoritative-save path: if the client included the latest
                // document in the body, replace the in-memory document AND
                // re-mirror it to the audio engine before writing to disk.
                // This guarantees per-cue property edits (fade-in / stop-fade
                // / cross-fade / volume / ducking) take effect immediately even
                // when the granular item-diff watcher on the client missed a
                // change. Without this fallback the user can edit a slider and
                // see the save call land while the engine still uses stale
                // values from the previous play.
                if (j.contains("document") && j["document"].is_object()) {
                    if (!state_.replace_full_document(j["document"])) {
                        Logger::warn("POST /api/project/save — embedded document "
                                     "rejected, continuing with existing state");
                    }
                }
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/save path='{}'",
                                    req.remote_ip_address, impl_->server_addr,
                                    liveplay::util::path_to_utf8(p));
                if (!state_.save(p)) {
                    Logger::error("POST /api/project/save FAILED for '{}'",
                                  liveplay::util::path_to_utf8(p));
                    return json_err(500, "save failed");
                }
                const auto path_str = liveplay::util::path_to_utf8(p);
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/save OK → '{}'",
                                     req.remote_ip_address, impl_->server_addr, path_str);
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
                Logger::api_request("Client ({}) -> Server ({}) : PUT /api/project/document name='{}'",
                                    req.remote_ip_address, impl_->server_addr, proj_name);
                if (!state_.replace_full_document(doc)) {
                    Logger::error("PUT /api/project/document — document not accepted");
                    return json_err(400, "document not accepted");
                }
                auto header = state_.header_document();
                const std::size_t item_count = header.value("itemCount", (std::size_t)0);
                Logger::api_response("Client ({}) <- Server ({}) : PUT /api/project/document OK — '{}' ({} items)",
                                     req.remote_ip_address, impl_->server_addr,
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
                const bool cart_only = j.value("cartOnly", false);
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/items uuid='{}' name='{}'{}{}",
                                    req.remote_ip_address, impl_->server_addr, item_uuid,
                                    item_name, parent_uuid.empty() ? "" : " parent='" + parent_uuid + "'",
                                    cart_only ? " [cartOnly]" : "");
                const auto cue_id = state_.add_item(j["item"], parent_uuid, cart_only);
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/items OK — uuid='{}' cueId='{}'",
                                     req.remote_ip_address, impl_->server_addr,
                                     item_uuid, cue_id.value);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "item_added"},
                    {"uuid", item_uuid},
                    {"parentUuid", parent_uuid},
                    {"cartOnly", cart_only},
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
                Logger::api_request("Client ({}) -> Server ({}) : PATCH /api/project/items/{}",
                                    req.remote_ip_address, impl_->server_addr, uuid);
                if (!state_.update_item(uuid, patch)) {
                    Logger::warn("PATCH /api/project/items/{} — item not found", uuid);
                    return json_err(404, "item not found");
                }
                Logger::api_response("Client ({}) <- Server ({}) : PATCH /api/project/items/{} OK",
                                     req.remote_ip_address, impl_->server_addr, uuid);
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
        ([this](const crow::request& req, std::string uuid){
            Logger::api_request("Client ({}) -> Server ({}) : DELETE /api/project/items/{}",
                                req.remote_ip_address, impl_->server_addr, uuid);
            if (!state_.remove_item(uuid)) {
                Logger::warn("DELETE /api/project/items/{} — not found", uuid);
                return json_err(404, "item not found");
            }
            Logger::api_response("Client ({}) <- Server ({}) : DELETE /api/project/items/{} OK",
                                 req.remote_ip_address, impl_->server_addr, uuid);
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
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/items/reorder ({} items){}",
                                    req.remote_ip_address, impl_->server_addr, uuids.size(),
                                    parent_uuid.empty() ? "" : " parent='" + parent_uuid + "'");
                state_.reorder_items(uuids, parent_uuid);
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/items/reorder OK",
                                     req.remote_ip_address, impl_->server_addr);
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
    // GET is accepted as well as POST so the trigger URL shown in the client's
    // Properties Panel can be fired from a browser or a plain `curl`.
    CROW_ROUTE(app, "/api/project/items/<string>/play")
        .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
        ([this](const crow::request& req, std::string uuid){
            const std::string m = crow::method_name(req.method);
            Logger::api_request("Client ({}) -> Server ({}) : {} /api/project/items/{}/play",
                                req.remote_ip_address, impl_->server_addr, m, uuid);
            Logger::playback("PLAY: {}", item_playback_info(uuid, state_));
            if (!state_.play_item(uuid)) {
                Logger::warn("PLAY item_uuid={} — item not loaded into engine", uuid);
                return json_err(404, "item not loaded into engine");
            }
            Logger::api_response("Client ({}) <- Server ({}) : {} /api/project/items/{}/play OK",
                                 req.remote_ip_address, impl_->server_addr, m, uuid);
            return json_ok(json({{"ok", true}}));
        });

    // Item-by-index transport. The index is an index *path* — a list of child
    // indices that descends into groups at each level, mirroring the client's
    // findItemByIndex / endBehavior.targetIndex semantics. For example "1,11"
    // means top-level item 1 (the 2nd item, a group) then its child 11 (the
    // 12th item inside it). Both comma- and slash-separated forms are accepted
    // ("1,11" and "1/11" are equivalent), so the URL can be written either way
    // — even mixed ("1,2/0"). Routed through trigger_item so audio items play
    // and group items dispatch per their startBehavior. GET is accepted as well
    // as POST so the URL can be fired from a browser or a plain `curl`.
    CROW_ROUTE(app, "/api/project/items/by-index/<path>")
        .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
        ([this](const crow::request& req, std::string index_path){
            const std::string m = crow::method_name(req.method);
            Logger::api_request("Client ({}) -> Server ({}) : {} /api/project/items/by-index/{}",
                                req.remote_ip_address, impl_->server_addr, m, index_path);
            // Split on both ',' and '/' so "1,11", "1/11" and "1,2/0" all work.
            std::vector<int> path;
            std::string token;
            bool parse_error = false;
            auto flush = [&]{
                if (token.empty()) return;
                try {
                    std::size_t consumed = 0;
                    const int v = std::stoi(token, &consumed);
                    if (consumed != token.size() || v < 0) parse_error = true;
                    else path.push_back(v);
                } catch (...) { parse_error = true; }
                token.clear();
            };
            for (char c : index_path) {
                if (c == ',' || c == '/') flush();
                else                      token.push_back(c);
            }
            flush();
            if (parse_error || path.empty()) {
                Logger::warn("TRIGGER by-index '{}' — invalid index path", index_path);
                return json_err(400, "invalid index path");
            }
            const std::string uuid = state_.item_uuid_by_index(path);
            if (uuid.empty()) {
                Logger::warn("TRIGGER by-index '{}' — no item at that index", index_path);
                return json_err(404, "no item at that index");
            }
            Logger::playback("TRIGGER: {}", item_playback_info(uuid, state_));
            if (!state_.trigger_item(uuid)) {
                Logger::warn("TRIGGER by-index '{}' uuid={} — item not loaded into engine",
                             index_path, uuid);
                return json_err(404, "item not loaded into engine");
            }
            Logger::api_response("Client ({}) <- Server ({}) : {} /api/project/items/by-index/{} OK -> uuid={}",
                                 req.remote_ip_address, impl_->server_addr, m, index_path, uuid);
            return json_ok(json({{"ok", true}, {"uuid", uuid}, {"index", path}}));
        });

    CROW_ROUTE(app, "/api/project/items/<string>/stop").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string uuid){
            Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/items/{}/stop",
                                req.remote_ip_address, impl_->server_addr, uuid);
            Logger::playback("STOP: {}", item_playback_info(uuid, state_));
            if (!state_.stop_item(uuid)) {
                Logger::warn("STOP item_uuid={} — item not loaded into engine", uuid);
                return json_err(404, "item not loaded into engine");
            }
            Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/items/{}/stop OK",
                                 req.remote_ip_address, impl_->server_addr, uuid);
            return json_ok(json({{"ok", true}}));
        });
    CROW_ROUTE(app, "/api/project/items/<string>/seek").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string uuid){
            try {
                auto j = json::parse(req.body);
                const double secs = j.value("seconds", 0.0);
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/items/{}/seek seconds={:.2f}",
                                    req.remote_ip_address, impl_->server_addr, uuid, secs);
                Logger::playback("SEEK: {} → {:.2f}s", item_playback_info(uuid, state_), secs);
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
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/cart slot={} itemUuid='{}'",
                                    req.remote_ip_address, impl_->server_addr, slot, uuid);
                state_.set_cart_slot(slot, uuid);
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/cart OK — slot={} uuid='{}'",
                                     req.remote_ip_address, impl_->server_addr, slot, uuid);
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
        ([this](const crow::request& req, int slot){
            Logger::api_request("Client ({}) -> Server ({}) : DELETE /api/project/cart/{}",
                                req.remote_ip_address, impl_->server_addr, slot);
            state_.clear_cart_slot(slot);
            Logger::api_response("Client ({}) <- Server ({}) : DELETE /api/project/cart/{} OK",
                                 req.remote_ip_address, impl_->server_addr, slot);
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
                Logger::api_request("Client ({}) -> Server ({}) : POST /api/preview itemUuid='{}'",
                                    req.remote_ip_address, impl_->server_addr, uuid);
                Logger::playback("PREVIEW START: {}", item_playback_info(uuid, state_));
                const bool ok = state_.start_preview(uuid);
                if (!ok) {
                    Logger::warn("PREVIEW START failed for item_uuid='{}' — no device or item not found", uuid);
                    return json_err(400, "preview could not start (no device, or item not found)");
                }
                const auto cue_id = state_.current_preview_cue_id().value;
                Logger::api_response("Client ({}) <- Server ({}) : POST /api/preview OK — cueId='{}'",
                                     req.remote_ip_address, impl_->server_addr, cue_id);
                // Mirror preview state to every other connected client so
                // they can show the preview card / cue id in real time.
                broadcast_doc_patch(json{
                    {"type", "doc_patch"},
                    {"op",   "preview_started"},
                    {"itemUuid", uuid},
                    {"cueId", cue_id},
                });
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
        ([this](const crow::request& req) {
            Logger::api_request("Client ({}) -> Server ({}) : DELETE /api/preview",
                                req.remote_ip_address, impl_->server_addr);
            Logger::playback("PREVIEW STOP");
            state_.stop_preview();
            Logger::api_response("Client ({}) <- Server ({}) : DELETE /api/preview OK",
                                 req.remote_ip_address, impl_->server_addr);
            broadcast_doc_patch(json{
                {"type", "doc_patch"},
                {"op",   "preview_stopped"},
            });
            return json_ok(json({{"ok", true}}));
        });

    // ---- Theme + settings patches ----
    CROW_ROUTE(app, "/api/project/theme").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req){
            try {
                auto patch = json::parse(req.body);
                Logger::api_request("Client ({}) -> Server ({}) : PATCH /api/project/theme",
                                    req.remote_ip_address, impl_->server_addr);
                state_.patch_theme(patch);
                auto theme = state_.full_document()["theme"];
                Logger::api_response("Client ({}) <- Server ({}) : PATCH /api/project/theme OK",
                                     req.remote_ip_address, impl_->server_addr);
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
                Logger::api_request("Client ({}) -> Server ({}) : PATCH /api/project/settings",
                                    req.remote_ip_address, impl_->server_addr);
                state_.patch_settings(patch);
                auto settings = state_.full_document()["settings"];
                Logger::api_response("Client ({}) <- Server ({}) : PATCH /api/project/settings OK",
                                     req.remote_ip_address, impl_->server_addr);
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
          // Mark this client for a playback_snapshot push on the next
          // broadcast tick. The snapshot can't be sent inline here because
          // build_playback_snapshot takes both engine and project locks
          // (potentially seconds, e.g. mid project mirror) and Crow's
          // connection is not safe to write from two threads at once —
          // direct send_text here races the broadcast thread.
          impl_->ws_clients_pending_snapshot.insert(&conn);
          Logger::info("WS client connected ({} total)", impl_->ws_clients.size());
      })
      .onclose([this](crow::websocket::connection& conn, const std::string& reason, std::uint16_t /*code*/) {
          std::lock_guard lock{impl_->ws_mutex};
          impl_->ws_clients.erase(&conn);
          impl_->ws_clients_pending_snapshot.erase(&conn);
          Logger::info("WS client disconnected ({}); {} remaining",
                       reason, impl_->ws_clients.size());
      })
      .onmessage([this](crow::websocket::connection& conn,
                        const std::string& data,
                        bool is_binary) {
          if (is_binary) return;
          std::string direct_reply;
          try {
              direct_reply = handle_ws_message(conn, data, engine_, state_, impl_->server_addr);
          } catch (const std::exception& e) {
              Logger::error("WS onmessage threw past handler: {}", e.what());
          } catch (...) {
              Logger::error("WS onmessage caught unknown exception.");
          }
          // Send any direct reply (pong, error) under ws_mutex so it is
          // serialised with broadcast_loop's concurrent send_text calls.
          // Calling send_text from the ASIO thread without the mutex while
          // broadcast_loop is also writing to the same conn causes the
          // "not safe for concurrent writes" crash described in the Impl comment.
          if (!direct_reply.empty()) {
              std::lock_guard lock{impl_->ws_mutex};
              try { conn.send_text(direct_reply); } catch (...) {}
          }
          // After applying any state-mutating WS message, fan out the
          // relevant change to every other client so multi-client mirroring
          // stays consistent (the originating client gets the echo too —
          // its local state already matches so the apply is a no-op).
          // Note: set_next_item fan-out is handled centrally by the
          // next_item_broadcaster installed on ProjectState (it fires for both
          // client-requested and server-armed changes), so we don't broadcast
          // it again here.
      });
}

} // namespace liveplay::net
