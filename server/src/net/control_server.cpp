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
#include "liveplay/audio/spectrum.hpp"
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
#include <functional>
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

// ---------------------------------------------------------------------------
// AuthGuard — the single place a REST request is admitted or refused (U3)
// ---------------------------------------------------------------------------
// Crow middleware rather than a guard line at the top of each handler, and the
// reason is the whole design: there are around ninety routes, and a check that
// every future route has to REMEMBER to call is a check that will eventually
// be forgotten — silently, on the one route where it matters. Here the default
// is deny. access_for() lists what is public and what needs an administrator;
// everything it does not name, including a path that matches no route at all,
// requires a valid token. Adding a route without thinking about auth yields a
// route that is protected, which is the failure mode worth having.
//
// It cannot be the whole story: Crow runs middleware for a WebSocket upgrade
// but then hands the connection to the rule REGARDLESS of what the middleware
// did to the response (http_connection.h calls the middlewares and proceeds to
// handle_upgrade without consulting res). So res.end() here does not refuse a
// socket, and /ws is deliberately Public below — the socket's credential is
// checked in its .onaccept handler, which is the one hook that can actually
// stop the handshake. Two enforcement points because there are two doors, not
// because one was forgotten.
struct AuthGuard {
    // What a route needs. Named for the ownership model's tiers rather than
    // invented alongside them: User is "an authenticated person running the
    // show", Admin is "the machine's own state".
    enum class Access { Public, User, Admin };

    struct context {
        bool                       authenticated = false;  // false when auth is off
        core::UserStore::Principal principal;
    };

    // Set once, before the socket opens. Null only in the window before
    // ControlServer::start(), during which no request can arrive.
    core::UserStore* users = nullptr;

    void before_handle(crow::request& req, crow::response& res, context& ctx);
    // Stamps the CORS headers on EVERY response. It has to live here rather
    // than in a route: Crow answers every OPTIONS request itself inside
    // handle_initial() (a 204 with only an Allow header) before any user rule
    // could run, and skips before_handle for it — after_handle is the one hook
    // that still sees a preflight (issue #62). It also covers Crow's own
    // 404/405 bodies and AuthGuard's early 401/403.
    void after_handle(crow::request& req, crow::response& res, context&);
};

// Pimpl: all Crow + WebSocket state lives here so crow.h stays out of the
// public header.
struct ControlServer::Impl {
    // Not SimpleApp (= Crow<>) any more: the auth check has to run ahead of
    // every route, and a middleware is the only hook Crow offers for that.
    crow::App<AuthGuard> app;
    std::thread     app_thread;
    std::thread     broadcast_thread;
    std::mutex      ws_mutex;
    std::string     server_addr;
    // Spectrum analyser (channel view). Which bus each engine tap is armed
    // for, and the strip it resolved to — re-resolved every tick so a bus
    // rebuilt by a project load re-arms on its new strip. Under analyser_mutex,
    // which is never held together with ws_mutex.
    std::mutex analyser_mutex;
    std::array<std::string, audio::kMaxAnalyserTaps>           analyser_bus{};
    std::array<audio::MixerChannelId, audio::kMaxAnalyserTaps> analyser_strip{};
    // What the server knows about one connected client (U1).
    //
    // A connection used to be an element of a set — a bare pointer, with no
    // room to say who is on the other end. Everything the ownership model
    // calls the User tier needs somewhere to hang: a locale that is this
    // operator's rather than the server's, a meter rate this surface asked
    // for, and eventually a principal that says what this client may do.
    // This is that place; the tier is built on it rather than beside it.
    //
    // Deliberately holds no `crow::request` and no pointer into one. The
    // request is gone by the time onopen runs, and a session that outlived a
    // dangling reference to it would be a use-after-free waiting for the
    // first client that disconnects mid-handshake.
    struct ClientSession {
        std::uint64_t id = 0;               // stable for the life of the connection
        std::string   remote_ip;
        std::chrono::steady_clock::time_point connected_at{};
        // Needs an initial playback_snapshot push. Set by onopen, cleared by
        // broadcast_loop under ws_mutex — which keeps every send_text on a
        // single connection serialised through one mutex (Crow's
        // websocket::connection is not safe under concurrent writes; calling
        // send_text directly from onopen while broadcast_loop was concurrently
        // sending meters to the same conn caused the crash on connect).
        //
        // This was a second parallel set until U1. Two containers keyed by the
        // same pointer had to be kept in step by hand, and onclose had to
        // remember to erase from both; per-connection state belongs in the
        // per-connection record.
        bool          wants_snapshot = false;

        // ---- User tier (U2) ----------------------------------------------
        // This operator's display language. Empty means "no preference" —
        // the installation default stands, and a later change to that default
        // reaches this connection. A session that HAS chosen is left alone by
        // such a change, which is the whole override chain in one field.
        std::string   locale;
        // How often this connection wants meter frames, in Hz. 0 means "no
        // preference": every tick of the server's own broadcast rate.
        //
        // Never faster than the server ticks — the loop is the only consumer
        // of the consuming meter reads, so a per-connection rate can thin the
        // stream but cannot conjure samples that were never taken. A tablet
        // over Wi-Fi asking for 10 Hz is the case this exists for.
        std::size_t   meter_hz = 0;
        // Bresenham accumulator for the thinning. Carrying the remainder
        // spreads the kept frames evenly instead of bunching them, which a
        // simple "every Nth" counter does not do for rates that do not divide.
        std::size_t   meter_accum = 0;

        // ---- Principal (U3) ----------------------------------------------
        // Who this connection turned out to be, decided once during the
        // handshake. Authorization is rechecked below. Empty when the installation has no
        // accounts, which is the default posture — an anonymous session is a
        // real session here, not a rejected one.
        //
        // Stored by value: this records who the socket was opened as, which is
        // what /api/clients reports and what a log line needs to stay true
        // about a connection that has since ended.
        //
        // It is deliberately NOT the authority on what anyone may do. A token
        // carries only a user id and an epoch, so every REST request looks the
        // role up in the store as it stands at that moment — a demotion takes
        // effect on the caller's very next request, not at their next login.
        // No WebSocket message is admin-gated (the socket carries show control,
        // which is the operator tier in full), so nothing reads is_admin to
        // decide anything; if a Server-tier command ever arrives over the
        // socket it must re-ask the store rather than trust this field.
        std::string   user_id;
        std::string   user_name;
        bool          is_admin = false;
        // An API token rather than a person. Kept because the two read very
        // differently in a list of who is connected: "Companion — FOH rack" is
        // a machine that will still be there tomorrow, and an operator looking
        // at the list must not take it for a colleague who could be asked to
        // close a window.
        bool          is_api   = false;
        // Kept only in memory, never included in /api/clients or logs. Recheck
        // it against the store before commands and broadcasts, so revocation,
        // expiry, password changes and enabling login affect open sockets too.
        std::string   credential;
        bool          auth_rejected = false;
        bool          close_requested = false;

        // ---- User tier, persistent half (U4) ------------------------------
        // The meter display unit this operator's stored profile asks for, or
        // empty for "no preference". Cached on the session rather than read
        // from UserPrefs on demand because the thing that consumes it is a
        // union across every connection (ProjectState::set_user_meter_modes),
        // and computing that must not do a disk read per connection while
        // holding ws_mutex.
        //
        // Only the meter unit is mirrored here. The other preferences are the
        // client's business entirely — the server has no opinion about a
        // colour or a keymap, and caching them would be inventing a second
        // copy of a value that already has an owner.
        std::string   meter_mode;

        // The bus whose spectrum this connection's channel view is showing,
        // or empty. View state (D17 allows it, like the meter rate): each
        // operator can look at a different bus.
        std::string   analyser_bus;
    };
    struct WebSocketAuth {
        core::UserStore::Principal principal;
        std::string credential;
    };
    std::unordered_map<crow::websocket::connection*, ClientSession> ws_clients;

    // Caller holds ws_mutex. UserStore never calls back into the server while
    // holding its lock. Rejection sticks until onclose removes the connection.
    bool authorize_ws_locked(crow::websocket::connection* conn, core::UserStore& users) {
        const auto it = ws_clients.find(conn);
        if (it == ws_clients.end()) return false;
        auto& s = it->second;
        if (s.auth_rejected) return false;
        if (!users.auth_required()) return true;
        if (!s.credential.empty() && users.verify_token(s.credential)) return true;
        s.auth_rejected = true;
        return false;
    }
    // Monotonic, never reused within a process run, so a session id in a log
    // line always means one connection. Guarded by ws_mutex.
    std::uint64_t next_client_id = 1;

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

// ---------------------------------------------------------------------------
// Server policy: CORS origin and the filesystem allow-list
// ---------------------------------------------------------------------------
// json_ok / json_err are free functions called from ~90 route lambdas, so the
// configured origin reaches them through file scope rather than by threading a
// parameter through every one. Written once in the ControlServer constructor,
// before start() opens the socket, and only read afterwards — there is one
// ControlServer per process.
std::string  g_cors_allow_origin = "*";
// Same lifetime, same reason: the path guard is called from a dozen handlers.
std::vector<std::string> g_fs_browse_roots{};

// Comparable form of a path: case-folded and separator-normalised on Windows,
// where the filesystem is case-insensitive and a case-sensitive prefix test
// would let "c:\shows" escape a root configured as "C:\Shows".
static std::string fs_compare_key(const fs::path& p) {
    std::string s = liveplay::util::path_to_utf8(p);
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::replace(s.begin(), s.end(), '\\', '/');
#endif
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// Is this path inside one of the configured roots? No roots means unrestricted
// (see ControlServerConfig::fs_browse_roots).
//
// weakly_canonical, not canonical: the path may not exist yet — mkdir and
// export name their destination before creating it — and canonical() throws on
// a missing path. It still resolves "..", symlinks and short names on the parts
// that do exist, which is what stops "<root>/../../etc/passwd" escaping.
//
// The boundary test is deliberately "equal, or prefixed by root + separator",
// so a root of /media does not also admit /mediaX.
bool path_within_fs_roots(const fs::path& p) {
    if (g_fs_browse_roots.empty()) return true;
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec) canon = p.lexically_normal();
    const std::string key = fs_compare_key(canon);

    for (const auto& root : g_fs_browse_roots) {
        std::error_code rec;
        fs::path rp = liveplay::util::utf8_to_path(root);
        fs::path rcanon = fs::weakly_canonical(rp, rec);
        if (rec) rcanon = rp.lexically_normal();
        const std::string rkey = fs_compare_key(rcanon);
        if (rkey.empty()) continue;
        if (key == rkey) return true;
        if (key.starts_with(rkey + "/")) return true;
    }
    return false;
}

// May a WebSocket upgrade carrying this Origin proceed?
//
// CORS does not cover WebSockets. `Access-Control-Allow-Origin` is a rule the
// browser applies to XHR and fetch; the WebSocket handshake is exempt from it,
// and a page on any origin may open a socket to any host the user can reach.
// So before U1 an installation started with --cors-origin https://console.here
// had its REST surface restricted and its WebSocket wide open — and the socket
// carries play, stop, bus gain, mute and selection. The lock was on the front
// door while the side door stood open.
//
// The rules, in order:
//   • `*` (the default) admits everything, because that is what S2's default
//     posture means and no upgrade behaviour should change on upgrade.
//   • No Origin header at all is admitted. Native clients, Companion, curl and
//     the Electron app send none, and refusing them would break every control
//     surface that is not a browser tab. This is not the hole it looks like: a
//     browser cannot suppress its own Origin, which is the whole reason the
//     check works, and an attacker who is not in a browser does not need the
//     trick — they can reach the socket directly either way. The check exists
//     to stop a page the operator merely VISITED from driving the rig.
//   • Otherwise it must match the configured origin exactly.
bool ws_origin_allowed(const std::string& origin) {
    if (g_cors_allow_origin == "*") return true;
    if (origin.empty())             return true;
    return origin == g_cors_allow_origin;
}

// ---------------------------------------------------------------------------
// Login throttling
// ---------------------------------------------------------------------------
// Argon2id at the interactive limits already costs ~100 ms per attempt, which
// is a real brake, but it is not one on its own: a handful of parallel
// connections still gets hundreds of guesses a minute against an 8-character
// password. This adds a per-address backoff on top.
//
// Deliberately small and in-memory. It is a brake on guessing, not a security
// boundary — an attacker with a /16 to spray from is not stopped by it, and
// nothing here pretends otherwise. What it does stop is the realistic case:
// one machine on the LAN grinding through a word list.
struct LoginAttempts {
    int                                   failures = 0;
    std::chrono::steady_clock::time_point blocked_until{};
};
std::mutex                                  g_login_mutex;
std::unordered_map<std::string, LoginAttempts> g_login_attempts;

// After this many consecutive failures from one address, that address waits.
constexpr int  kLoginFailuresBeforeDelay = 5;
constexpr int  kLoginBlockSeconds        = 30;
// Bound on the table, so a spray from many forged addresses cannot grow it
// without limit. Reaching it clears the lot: the alternative is evicting an
// entry an attacker chose, which hands them the eviction as a tool.
constexpr std::size_t kLoginAttemptsMax  = 4096;

// Seconds this address must wait, or 0 if it may try now.
int login_block_remaining(const std::string& ip) {
    std::lock_guard lock{g_login_mutex};
    const auto it = g_login_attempts.find(ip);
    if (it == g_login_attempts.end()) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (now >= it->second.blocked_until) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
        it->second.blocked_until - now).count()) + 1;
}

void login_note_failure(const std::string& ip) {
    std::lock_guard lock{g_login_mutex};
    if (g_login_attempts.size() >= kLoginAttemptsMax) g_login_attempts.clear();
    auto& a = g_login_attempts[ip];
    if (++a.failures >= kLoginFailuresBeforeDelay) {
        a.blocked_until = std::chrono::steady_clock::now() +
                          std::chrono::seconds{kLoginBlockSeconds};
        a.failures = 0;
    }
}

void login_note_success(const std::string& ip) {
    std::lock_guard lock{g_login_mutex};
    g_login_attempts.erase(ip);
}

// A stop's optional "fade_ms" (a non-negative number), or nullopt when the
// caller left it out and the cue's own manual-stop fade should apply.
std::optional<long long> optional_fade_ms(const json& j) {
    if (!j.is_object()) return std::nullopt;
    auto it = j.find("fade_ms");
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return std::max<long long>(0, static_cast<long long>(it->get<double>()));
}

crow::response json_ok(const json& body) {
    crow::response r{200, body.dump()};
    r.add_header("Content-Type", "application/json");
    r.add_header("Access-Control-Allow-Origin", g_cors_allow_origin);
    return r;
}

crow::response json_err(int status, std::string_view message) {
    crow::response r{status, json({{"error", message}}).dump()};
    r.add_header("Content-Type", "application/json");
    r.add_header("Access-Control-Allow-Origin", g_cors_allow_origin);
    return r;
}

// Refusal for a path outside the allow-list. Deliberately does not echo the
// path back: the caller already knows what it asked for, and reflecting it
// turns the error into a probe that confirms what exists.
crow::response json_fs_denied() {
    return json_err(403, "path is outside the server's permitted directories");
}

// Why a bus output was refused, in words the operator can act on. Every one of
// these is a 409: the request was understood and is not allowed (D6). The
// cycle wording is fixed by the plan and the client matches on it.
static std::string_view bus_output_refusal_text(core::ProjectState::PatchBusResult r) {
    using PR = core::ProjectState::PatchBusResult;
    switch (r) {
        case PR::RefusedPreviewToBus:
            return "the Preview bus cannot be routed to another bus";
        case PR::RefusedMasterToBus:
            return "the Master bus must send to an output";
        case PR::UnknownTarget:
            return "no such bus to route to";
        case PR::IllegalTarget:
            return "a bus cannot feed the Preview bus";
        case PR::Cycle:
            return "routing this bus would create a cycle";
        case PR::RoleCannotBeDropped:
            return "move the role to another bus instead";
        case PR::RoleNeedsOutput:
            return "a bus must send to an output to hold the Master or Preview role";
        case PR::RoleConflict:
            return "one bus cannot hold both the Master and Preview roles";
        case PR::RoleTargetFed:
            return "buses feed this bus; re-route them before making it the Preview bus";
        // Aux sends (M1). Worded for a send rather than reusing the routing
        // texts above: what the operator tried was to add a send, and being
        // told the bus "cannot be routed" would send them to the wrong control.
        case PR::RefusedPreviewSend:
            return "the Preview bus cannot send to another bus";
        case PR::SendUnknownTarget:
            return "no such bus to send to";
        case PR::SendIllegalTarget:
            return "a bus cannot send to the Preview bus";
        case PR::SendCycle:
            return "this send would create a cycle";
        default:
            return "the bus output was refused";
    }
}

// The full mixer view of one bus, shared by GET /api/buses and
// GET /api/buses/<id> so the two can never drift apart.
static json bus_info_to_json(const core::ProjectState::BusInfo& b) {
    const char* kind = core::output_kind_name(b.def.output_kind);
    return json{
        {"id",       b.def.id},
        {"name",     b.def.display_name},
        {"color",    b.def.color},
        {"order",    b.def.order},
        {"width",    b.def.width},
        {"gainDb",   b.def.gain_db},
        {"mute",     b.def.muted},
        {"pan",      b.def.pan},
        {"dsp",      core::bus_dsp_to_json(b.def.dsp)},
        // Live monitoring state, not part of the document —
        // it comes from the strip, and a reload clears it.
        {"pfl",      b.pfl},
        // Preview bus only: the mono-sum audition. Live too.
        {"monoCheck", b.mono_check},
        // Whether it actually reaches hardware — see BusInfo.
        {"bound",    b.bound},
        // The real device names that target resolves to on this machine
        // ("" = the default device), so a client never string-matches.
        {"outputDevices", b.output_devices},
        // Roles (D24): the house, and where PFL / pre-listen land.
        {"master",   b.def.master},
        {"preview",  b.def.preview},
        // The master pair its hardware output occupies, or null (D32).
        {"masters",  b.masters ? json{b.masters->first, b.masters->second} : json{}},
        {"output",   json{{"type", kind}, {"target", b.def.output_target}}},
        // Aux sends (M1). Where this bus ALSO sends a copy of itself, at what
        // level, and whether that copy is taken before or after its fader —
        // distinct from `output` above, which is the one place its whole
        // signal goes.
        {"sends",    core::bus_sends_to_json(b.def)},
        {"mixerId",  b.mixer.value},
        {"itemUuids", b.item_uuids},
    };
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
    // GC any expired entries while we're here. An expired-and-unclaimed token
    // still has its .lpa sitting on disk (redeem is the only path that deletes
    // it), so remove the file before dropping the entry — otherwise abandoned
    // exports leak temp files indefinitely.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_download_tokens.begin(); it != g_download_tokens.end();) {
        if (it->second.expires_at <= now) {
            std::error_code ec;
            fs::remove(it->second.path, ec);
            it = g_download_tokens.erase(it);
        } else {
            ++it;
        }
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
// What each route needs (U3)
// ---------------------------------------------------------------------------
// The whole access-control policy, in one readable list, because a policy
// spread across ninety handlers is not a policy anyone can check.
//
// Admin is the ownership model's SERVER tier and nothing else: state that
// belongs to this machine rather than to the show or to the person at it. That
// is why the list is short and why it can be justified rather than merely
// asserted — "does an operator own this?" has an answer the docs already give.
//
//   /api/outputs   the logical-output map: what "FOH" is wired to here.
//   /api/users     the accounts in this file.
//   /api/clients   who else is connected, and from what address.
//
// Everything else is the Project and User tiers — running the show — and any
// authenticated person may do it. An operator who cannot start a cue is not an
// operator.
//
// Two calls in that "everything else" are worth stating rather than leaving to
// be discovered as inconsistencies:
//
//   POST /api/ui/locale sets the INSTALLATION default language, which by the
//   letter of the rule is machine state. It is left to operators because it
//   behaves like Show Mode and selection — shared presentation state of the
//   running installation, changeable by anyone running it, reversible in one
//   click, and incapable of silencing anything. The output map can take the
//   show off the air; a default language cannot.
//
//   PATCH /api/buses can WRITE to outputs.json, because O2 materialises a
//   logical output when a bus is pointed at a device. That is not a way around
//   the admin gate: choosing where your own bus goes is the show's routing, and
//   the gate on /api/outputs is about editing the venue's named outputs —
//   renaming them, repointing "FOH" at different hardware, deleting them —
//   which is what an operator must not be able to do to everyone else.
static AuthGuard::Access access_for(std::string_view path) {
    // Trim a query string defensively. Crow hands us a path-only url, but this
    // function decides who gets in and must not depend on that staying true.
    if (const auto q = path.find('?'); q != std::string_view::npos)
        path = path.substr(0, q);

    // Public: exactly the three things a client must be able to ask BEFORE it
    // holds a credential, and no more.
    //   health  — a liveness probe, and what the client polls to find a server
    //   status  — "do I need to log in at all?", which cannot itself need a login
    //   login   — the door
    if (path == "/api/health")      return AuthGuard::Access::Public;
    if (path == "/api/auth/status") return AuthGuard::Access::Public;
    if (path == "/api/auth/login")  return AuthGuard::Access::Public;

    // Not really public — checked in .onaccept instead, because a middleware
    // cannot refuse an upgrade (see the AuthGuard comment). Listed here so the
    // exemption is a stated decision rather than a gap someone finds later.
    if (path == "/ws")              return AuthGuard::Access::Public;

    // The Server tier.
    if (path.rfind("/api/outputs", 0) == 0) return AuthGuard::Access::Admin;
    // Moving the whole account list in or out. Already covered by the prefix
    // below, and listed anyway: the export carries every password hash on the
    // machine, and these two must stay admin-only even if somebody later
    // narrows the prefix to let a person read their own row.
    if (path == "/api/users/export")        return AuthGuard::Access::Admin;
    if (path == "/api/users/import")        return AuthGuard::Access::Admin;
    if (path.rfind("/api/users", 0)   == 0) return AuthGuard::Access::Admin;
    // NOT here: /api/auth/me/avatar. A person's own picture is the User tier,
    // like their own preferences — the default below — and the route acts only
    // on the caller, so there is no request shape that reaches somebody else's.
    if (path == "/api/clients")             return AuthGuard::Access::Admin;
    // API tokens, which are Server-tier for the same reason accounts are: they
    // decide who may talk to this machine. A token may not reach this path even
    // so — see api_token_forbidden() — because a credential that can mint
    // another credential is a credential nobody can revoke.
    if (path.rfind("/api/tokens", 0)  == 0) return AuthGuard::Access::Admin;
    // Turning authentication off, and back on. Admin, obviously — but this gate
    // is NOT what protects it, and that is worth being explicit about: when
    // authentication is off the whole guard short-circuits (see before_handle),
    // so every Admin path including this one is wide open. The route therefore
    // verifies an administrator's NAME AND PASSWORD in the body itself, in both
    // directions, and that check is the real boundary. Listed here anyway so the
    // path is covered while authentication IS on, and so nobody later reads the
    // absence of an entry as an oversight.
    if (path == "/api/auth/required")       return AuthGuard::Access::Admin;
    // The machine's own configuration (P3): the port it binds, how wide the
    // master bus is, where the filesystem API may reach, which origins may call
    // in. Squarely the Server tier, and the last two are security policy — an
    // operator who could widen fsRoots could read any file on the machine
    // through /api/fs/list. --lock-server-config refuses writes even to an
    // admin, which is R3.
    if (path.rfind("/api/server", 0)  == 0) return AuthGuard::Access::Admin;

    // Default deny. This is the load-bearing line: a route added next year is
    // covered by it without anyone having to remember, and a path matching no
    // route at all needs a token too — so an anonymous caller cannot map the
    // route table by reading which 404s come back.
    return AuthGuard::Access::User;
}

// What an API TOKEN may not reach, on top of everything access_for() already
// says. The second axis, and it is deliberately a separate list rather than a
// third Access value: a token is not a lesser operator, it is a different KIND
// of principal, and what it may not do does not sit anywhere on the tier ladder.
//
// Two groups, and the reasons are different (plus a third, belt-and-braces,
// for the account file — see the list itself):
//
//  1. THE FILESYSTEM. A token is a string in somebody else's configuration
//     file — in a Companion instance, a cue list, a shell script, a repository.
//     It leaks in ways a password does not, and §6.2 of the ownership model
//     promises in writing that this principal never reaches the filesystem.
//     Browsing directories, reading a file out, writing bytes in, and copying
//     media around are all refused. Opening and SAVING the show is not: that is
//     the automation everybody actually wants (a Companion button that loads
//     tomorrow's set, a script that saves before the house opens), and both
//     routes are already bounded by --fs-root where an administrator set one.
//
//  2. THINGS THAT ARE ONLY TRUE OF A PERSON. Preferences belong to somebody —
//     a theme, a keymap, what one pair of hands does — and a token has no
//     somebody. "Sign me out everywhere" has nothing to sign out. Changing
//     whether this server requires a login is a posture decision that asks for
//     a password at the moment of the act, which a token cannot answer. And
//     /api/tokens itself: a credential that can issue more of itself is one
//     nobody can fully revoke.
//
// Everything else is the show, and a token exists to run the show.
static bool api_token_forbidden(std::string_view path) {
    if (const auto q = path.find('?'); q != std::string_view::npos)
        path = path.substr(0, q);

    // 1 — the filesystem
    if (path.rfind("/api/fs", 0) == 0)   return true;   // list, mkdir
    if (path == "/api/upload")           return true;
    if (path == "/api/file/download")    return true;
    if (path == "/api/copy_to_media")    return true;
    if (path == "/api/project/import")   return true;   // an archive in…
    if (path == "/api/project/export")   return true;   // …and a copy of the show out

    // 2 — things that are only true of a person
    if (path == "/api/prefs")            return true;
    if (path == "/api/auth/logout_all")  return true;
    if (path == "/api/auth/required")    return true;
    if (path.rfind("/api/tokens", 0) == 0) return true;
    // A face. A token has none, and changing the picture of whoever issued it
    // is not something a Companion button should be able to do.
    if (path == "/api/auth/me/avatar")   return true;

    // 3 — the account file itself. A token is never an administrator, so the
    // admin gate refuses these already; they are named here as well because
    // the export is every password hash on the machine, and "a token can never
    // fetch it" should not rest on one check.
    if (path == "/api/users/export")     return true;
    if (path == "/api/users/import")     return true;

    return false;
}

void AuthGuard::after_handle(crow::request& req, crow::response& res, context&) {
    // Never overrides: json_ok/json_err and the download route already set the
    // origin, and a duplicate header is rejected by browsers.
    const auto set_once = [&res](const char* key, const std::string& value) {
        if (res.get_header_value(key).empty()) res.add_header(key, value);
    };
    set_once("Access-Control-Allow-Origin", g_cors_allow_origin);
    if (req.method == crow::HTTPMethod::Options) {
        // Explicit lists, not "*": a wildcard Allow-Headers does not cover
        // Authorization, which every call carries once accounts exist.
        set_once("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
        set_once("Access-Control-Allow-Headers", "Content-Type, Authorization");
        set_once("Access-Control-Max-Age", "600");
    }
}

void AuthGuard::before_handle(crow::request& req, crow::response& res, context& ctx) {
    if (!users) return;

    // A CORS preflight carries no credentials by design — the browser strips
    // them — so refusing it would only turn every cross-origin call into an
    // opaque failure with the wrong cause on the console.
    if (req.method == crow::HTTPMethod::Options) return;

    const Access need = access_for(req.url);
    if (need == Access::Public) return;

    // The default posture, and the reason an upgrade changes nothing: with no
    // accounts configured there is no one to be, so every request is admitted
    // and ctx stays anonymous. main.cpp says so at boot.
    if (!users->auth_required()) return;

    // Header only, never a query parameter. A token in a URL ends up in
    // proxy logs, in browser history and in the Referer of anything the page
    // subsequently loads; the one place we cannot avoid it is the WebSocket
    // upgrade, where a browser cannot set headers, and that is handled in
    // .onaccept rather than by widening the rule here.
    const std::string auth = req.get_header_value("Authorization");
    std::optional<core::UserStore::Principal> principal;
    if (auth.rfind("Bearer ", 0) == 0)
        principal = users->verify_token(auth.substr(7));

    if (!principal) {
        // 401 with a challenge, so a client can tell "log in" apart from the
        // 403 that means "you are logged in and this is still not yours".
        res = json_err(401, "authentication required");
        res.add_header("WWW-Authenticate", "Bearer");
        res.end();
        return;
    }

    ctx.authenticated = true;
    ctx.principal     = *principal;

    if (principal->is_api()) {
        // Recorded here rather than in verify_token so that verification stays
        // const and touches no disk. This is the one place every REST request
        // is admitted, which makes it the one place that can say "last used".
        users->note_api_token_use(principal->id);

        // Checked BEFORE the admin gate so the refusal says what is actually
        // true. A token reaching /api/tokens is not an operator who needs
        // promoting, and telling it so would send somebody looking for a role
        // to change that would not help.
        if (api_token_forbidden(req.url)) {
            Logger::warn("{} {} refused: API token '{}' may not reach this",
                         crow::method_name(req.method), req.url, principal->name);
            res = json_err(403, "an API token may not use this — it runs the show, "
                                "it does not administer the machine or reach the disk");
            res.end();
            return;
        }
    }

    if (need == Access::Admin && !principal->is_admin()) {
        Logger::warn("{} {} refused: '{}' is not an administrator",
                     crow::method_name(req.method), req.url, principal->name);
        res = json_err(403, "this requires an administrator");
        res.end();
    }
}

// ---------------------------------------------------------------------------

ControlServer::ControlServer(audio::AudioEngine& engine,
                             core::ProjectState& state,
                             core::OutputMap&    outputs,
                             core::UserStore&    users,
                             core::UserPrefs&    prefs,
                             core::ServerConfig& server_config,
                             ControlServerConfig cfg)
    : engine_(engine), state_(state), outputs_(outputs), users_(users),
      prefs_(prefs), server_config_(server_config), cfg_(std::move(cfg)),
      impl_(std::make_unique<Impl>()) {
    // Publish policy to the file-scope copies the free helpers read. Done here,
    // before start() opens the socket, so nothing can observe a half-set value.
    g_cors_allow_origin = cfg_.cors_allow_origin.empty() ? "*" : cfg_.cors_allow_origin;
    g_fs_browse_roots   = cfg_.fs_browse_roots;
    // Same reasoning, same moment: the guard has to know where to look up a
    // token before any request can arrive.
    impl_->app.get_middleware<AuthGuard>().users = &users_;
}

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
    // Every way a preview can stop (DELETE, Stop All, the audition reaching its
    // end, a moved preview role, a closed project) lands here, so no client is
    // left showing a preview that is no longer playing (#60).
    state_.set_preview_stopped_broadcaster([this] {
        broadcast_doc_patch(json{
            {"type", "doc_patch"},
            {"op",   "preview_stopped"},
        });
    });

    state_.set_next_item_broadcaster([this](const std::string& uuid) {
        broadcast_doc_patch(json{
            {"type", "doc_patch"},
            {"op",   "next_item_set"},
            {"itemUuid", uuid},
        });
    });

    // Shared operator UI state (selection / Show Mode / locale). ProjectState
    // hands us a ready-made doc_patch payload; we only have to fan it out, so
    // a Companion button, a touch tablet and the operator's laptop all end up
    // showing the same selected cue and the same view mode.
    state_.set_ui_state_broadcaster([this](const json& patch) {
        broadcast_doc_patch(patch);
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
// Meter broadcaster (cfg_.meter_broadcast_hz) + cue_state edge events.
// Uses CONSUMING meter reads (snapshot_consume_max) — this loop must remain
// the only consumer or readers would steal each other's peaks.
// ---------------------------------------------------------------------------
void ControlServer::broadcast_loop() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds{
        1'000'000'000LL / static_cast<long long>(std::max<std::size_t>(1, cfg_.meter_broadcast_hz))};

    // Track previous transport state per cue so we can emit cue_state events
    // exactly once on each transition (rather than every tick).
    std::unordered_map<std::string, audio::TransportState> prev_transports;

    // Absolute-deadline schedule. sleep_for(period - work) systematically
    // undershoots the target rate on Windows (~15.6 ms sleep granularity
    // rounds every sleep up); sleep_until against an advancing deadline
    // self-corrects, so the average rate converges on meter_broadcast_hz.
    auto next_tick = clock::now() + period;

    while (running_.load(std::memory_order_acquire)) {
        // Guard the entire tick: an exception escaping this thread would call
        // std::terminate() and take the whole audio process down mid-show.
        // Log-and-continue instead so a transient fault (e.g. a flaky media
        // share throwing out of a filesystem call) just drops one meter frame.
        try {

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
                auto snap = item->source_meter_consume(c);
                srcs.push_back(json{{"peak_db", snap.peak_db},
                                    {"rms_db", snap.rms_db},
                                    {"peak_max_db", snap.peak_max_db},
                                    {"true_peak_db", snap.true_peak_db},
                                    {"true_peak_max_db", snap.true_peak_max_db},
                                    {"kw_ms", snap.kw_ms},
                                    {"kw_ms_s", snap.kw_ms_s}});
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
        // Engine strips, not ProjectState's legacy mixers_ table — that table
        // is cleared and never repopulated on the client document path, so
        // this section has always serialised empty.
        for (auto& mch : engine_.list_mixer_channels()) {
            if (auto* m = engine_.find_mixer_channel(mch.id)) {
                // One consuming read, split per lane, so a stereo strip can
                // show separate L/R meters. The combined values are derived
                // here rather than read again — a second consuming call would
                // find the maxima already reset.
                const auto lanes = m->meter_snapshot_consume_lanes();

                audio::MeterSnapshot c{};
                json lane_arr = json::array();
                for (const auto& s : lanes) {
                    c.peak_db          = std::max(c.peak_db,          s.peak_db);
                    c.rms_db           = std::max(c.rms_db,           s.rms_db);
                    c.peak_max_db      = std::max(c.peak_max_db,      s.peak_max_db);
                    c.true_peak_db     = std::max(c.true_peak_db,     s.true_peak_db);
                    c.true_peak_max_db = std::max(c.true_peak_max_db, s.true_peak_max_db);
                    // Loudness sums across the channel group (BS.1770).
                    c.kw_ms   += s.kw_ms;
                    c.kw_ms_s += s.kw_ms_s;
                    lane_arr.push_back(json{
                        {"peak_db",          s.peak_db},
                        {"rms_db",           s.rms_db},
                        {"peak_max_db",      s.peak_max_db},
                        {"true_peak_db",     s.true_peak_db},
                        {"true_peak_max_db", s.true_peak_max_db},
                        {"kw_ms",            s.kw_ms},
                        {"kw_ms_s",          s.kw_ms_s},
                    });
                }

                mixer_meters.push_back(json{
                    {"mixer_id",         mch.id.value},
                    // How far each dynamics processor is pulling down, for the
                    // panel's GR meters. Zero when idle or switched out. One
                    // figure each rather than per lane: both detectors are
                    // linked across the strip's lanes.
                    // Deepest since the last tick, not the last block: a 30 Hz
                    // reader of a last-block value misses most of what a fast
                    // compressor does between ticks.
                    {"gate_gr_db",       m->dsp().take_gate_gr_db()},
                    {"comp_gr_db",       m->dsp().take_comp_gr_db()},
                    // Peak level into and out of the dynamics since the last
                    // tick (dBFS), for the transfer-curve meter. Pre-fader.
                    {"dyn_in_db",        m->dsp().take_dyn_in_db()},
                    {"dyn_out_db",       m->dsp().take_dyn_out_db()},
                    // Inter-channel correlation: +1 mono-compatible, 0 wide,
                    // negative means the lanes are cancelling and material will
                    // disappear the moment anything sums the strip to mono.
                    {"correlation",      m->correlation()},
                    {"peak_db",          c.peak_db},
                    {"rms_db",           c.rms_db},
                    {"peak_max_db",      c.peak_max_db},
                    {"true_peak_db",     c.true_peak_db},
                    {"true_peak_max_db", c.true_peak_max_db},
                    {"kw_ms",            c.kw_ms},
                    {"kw_ms_s",          c.kw_ms_s},
                    {"lanes",            std::move(lane_arr)},
                });
            }
        }
        payload["mixer_channels"] = std::move(mixer_meters);

        json master_meters = json::array();
        for (audio::MasterChannelIndex i = 0; i < engine_.config().master_channels; ++i) {
            auto s = engine_.read_master_meter_consume(i);
            const float gr = engine_.read_master_gain_reduction_db(i);
            // Only include non-silent channels to keep the payload light.
            // (peak_max_db is checked too so an isolated transient inside an
            // otherwise-silent frame still gets reported.)
            if (s.peak_db > -119.0f || s.peak_max_db > -119.0f || gr < -0.05f) {
                master_meters.push_back(json{
                    {"index",            i},
                    {"peak_db",          s.peak_db},
                    {"rms_db",           s.rms_db},
                    {"peak_max_db",      s.peak_max_db},
                    {"true_peak_db",     s.true_peak_db},
                    {"true_peak_max_db", s.true_peak_max_db},
                    {"kw_ms",            s.kw_ms},
                    {"kw_ms_s",          s.kw_ms_s},
                    {"gain_reduction_db", gr},
                });
            }
        }
        payload["master_channels"] = std::move(master_meters);

        std::string serialized;
        try { serialized = payload.dump(); }
        catch (const std::exception& e) {
            Logger::error("broadcast_loop: failed to serialize meters: {}", e.what());
            std::this_thread::sleep_until(next_tick);
            next_tick += period;
            if (next_tick < clock::now()) next_tick = clock::now() + period;
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
            for (const auto& [_, s] : impl_->ws_clients)
                if (s.wants_snapshot) { has_pending = true; break; }
        }
        std::string snapshot_serialized;
        if (has_pending) {
            try {
                snapshot_serialized = build_playback_snapshot(engine_, state_).dump();
            } catch (const std::exception& e) {
                Logger::warn("build_playback_snapshot failed: {}", e.what());
            }
        }

        // Spectrum analyser frames, one per armed tap, built outside ws_mutex
        // (the FFT is the most expensive thing on this tick). Each goes only
        // to the connections watching that bus.
        std::vector<std::pair<std::string, std::string>> analyser_frames;
        try {
            static audio::SpectrumAnalyser fft(4096);
            static std::vector<float> pre(4096), post(4096);
            constexpr std::size_t kBins = 96;
            std::array<float, kBins> pre_db{}, post_db{};
            std::lock_guard alock{impl_->analyser_mutex};
            for (std::size_t slot = 0; slot < audio::kMaxAnalyserTaps; ++slot) {
                const auto& bus = impl_->analyser_bus[slot];
                if (bus.empty()) continue;
                const auto strip = state_.bus_strip(bus);
                if (strip != impl_->analyser_strip[slot]) {
                    engine_.set_analyser_tap(slot, strip);
                    impl_->analyser_strip[slot] = strip;
                    continue;                      // let the new tap fill first
                }
                if (!engine_.read_analyser_tap(slot, pre.data(), post.data(), fft.size())) continue;
                const double fs = static_cast<double>(engine_.config().mix_sample_rate);
                fft.analyse(pre.data(),  fs, kBins, 20.0, 20000.0, pre_db.data());
                fft.analyse(post.data(), fs, kBins, 20.0, 20000.0, post_db.data());
                json a = json::array(), b = json::array();
                // One decimal is finer than any screen draws it.
                for (std::size_t k = 0; k < kBins; ++k) {
                    a.push_back(std::round(pre_db[k] * 10.0f) / 10.0f);
                    b.push_back(std::round(post_db[k] * 10.0f) / 10.0f);
                }
                analyser_frames.emplace_back(bus, json{
                    {"type", "analyser"}, {"busId", bus},
                    {"fLo", 20}, {"fHi", 20000},
                    {"pre", std::move(a)}, {"post", std::move(b)},
                }.dump());
            }
        } catch (const std::exception& e) {
            Logger::warn("broadcast_loop: analyser frame failed: {}", e.what());
        }

        // Fan out meters + cue_state events to all subscribed clients,
        // plus snapshots for any client still flagged as pending.
        std::lock_guard lock{impl_->ws_mutex};
        const std::size_t tick_hz = std::max<std::size_t>(1, cfg_.meter_broadcast_hz);
        for (auto& [c, session] : impl_->ws_clients) {
            try {
                if (!impl_->authorize_ws_locked(c, users_)) {
                    // This thread is outside Crow's I/O context: close posts
                    // asynchronously, and onclose owns removal from the map.
                    if (!session.close_requested) {
                        session.close_requested = true;
                        c->close("authentication required", 1008);
                    }
                    continue;
                }
                if (!snapshot_serialized.empty() && session.wants_snapshot) {
                    session.wants_snapshot = false;
                    c->send_text(snapshot_serialized);
                }
                // Meters are SAMPLES, so a connection that asked for fewer of
                // them just gets fewer (U2). Everything else on this tick is an
                // EDGE — the snapshot above, the cue_state events below — and
                // edges are never thinned: dropping one does not cost
                // resolution, it costs the client a transition it will never
                // hear about again, and its transport display stays wrong until
                // something else moves.
                bool send_meters = true;
                if (session.meter_hz > 0 && session.meter_hz < tick_hz) {
                    session.meter_accum += session.meter_hz;
                    if (session.meter_accum < tick_hz) send_meters = false;
                    else                               session.meter_accum -= tick_hz;
                }
                if (send_meters) c->send_text(serialized);
                if (send_meters && !session.analyser_bus.empty()) {
                    for (const auto& [bus, frame] : analyser_frames)
                        if (bus == session.analyser_bus) c->send_text(frame);
                }
                for (const auto& e : cue_state_events) c->send_text(e);
            }
            catch (...) { /* connection will be cleaned up by onclose */ }
        }

        // Sleep to maintain the broadcast cadence (absolute deadline; see
        // next_tick comment above). After a long stall, re-anchor instead of
        // burst-firing to catch up.
        std::this_thread::sleep_until(next_tick);
        next_tick += period;
        if (next_tick < clock::now()) next_tick = clock::now() + period;

        } catch (const std::exception& e) {
            Logger::error("broadcast_loop: unhandled exception (continuing): {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next_tick = clock::now() + period;
        } catch (...) {
            Logger::error("broadcast_loop: unknown exception (continuing).");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            next_tick = clock::now() + period;
        }
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
    for (auto& [c, _] : impl_->ws_clients) {
        if (!impl_->authorize_ws_locked(c, users_)) continue;
        try { c->send_text(serialized); }
        catch (...) { /* onclose will clean up dead connections */ }
    }
}

void ControlServer::broadcast_to_user(const std::string& user_id, const json& payload) {
    // No user id means nobody is signed in, and "every anonymous session" is
    // not the same set as "this person's windows" — it is everyone. Refuse
    // rather than fan out to the building.
    if (user_id.empty()) return;

    std::string serialized;
    try { serialized = payload.dump(); }
    catch (const std::exception& e) {
        Logger::error("broadcast_to_user: serialization failed: {}", e.what());
        return;
    }
    std::lock_guard lock{impl_->ws_mutex};
    for (auto& [c, session] : impl_->ws_clients) {
        if (session.user_id != user_id) continue;
        if (!impl_->authorize_ws_locked(c, users_)) continue;
        try { c->send_text(serialized); }
        catch (...) { /* onclose will clean up dead connections */ }
    }
}

void ControlServer::refresh_analyser_taps() {
    // The union of what connections are watching, capped at the tap pool.
    std::vector<std::string> wanted;
    {
        std::lock_guard lock{impl_->ws_mutex};
        for (const auto& [_, s] : impl_->ws_clients) {
            if (s.analyser_bus.empty()) continue;
            if (std::find(wanted.begin(), wanted.end(), s.analyser_bus) != wanted.end()) continue;
            if (wanted.size() < audio::kMaxAnalyserTaps) wanted.push_back(s.analyser_bus);
        }
    }
    std::lock_guard alock{impl_->analyser_mutex};
    // Keep a bus on the slot it already has, so a second viewer arriving does
    // not reset the first one's window; fill free slots with the newcomers.
    std::array<std::string, audio::kMaxAnalyserTaps> next{};
    for (std::size_t i = 0; i < next.size(); ++i) {
        const auto& cur = impl_->analyser_bus[i];
        if (!cur.empty() && std::find(wanted.begin(), wanted.end(), cur) != wanted.end()) next[i] = cur;
    }
    for (const auto& w : wanted) {
        if (std::find(next.begin(), next.end(), w) != next.end()) continue;
        for (auto& slot : next) if (slot.empty()) { slot = w; break; }
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (next[i] == impl_->analyser_bus[i]) continue;
        impl_->analyser_bus[i] = next[i];
        const auto strip = next[i].empty() ? audio::MixerChannelId{} : state_.bus_strip(next[i]);
        engine_.set_analyser_tap(i, strip);
        impl_->analyser_strip[i] = strip;
    }
}

void ControlServer::refresh_user_meter_modes() {
    std::vector<std::string> modes;
    {
        std::lock_guard lock{impl_->ws_mutex};
        modes.reserve(impl_->ws_clients.size());
        for (const auto& [_, session] : impl_->ws_clients) {
            if (!session.meter_mode.empty()) modes.push_back(session.meter_mode);
        }
    }
    // Sorted so an unchanged set compares equal regardless of the map's
    // iteration order — otherwise a reconnect would look like a change and
    // re-arm the metering DSP for no reason.
    std::sort(modes.begin(), modes.end());
    modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
    // Outside ws_mutex: this reaches into the engine, and the broadcast loop
    // already establishes that ws_mutex is the inner lock.
    state_.set_user_meter_modes(std::move(modes));
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

        // Guard the whole task: an exception here (e.g. the throwing fs::exists
        // overload faulting on a disconnected network share, or compute_waveform
        // throwing) would escape this thread and std::terminate() the process.
        try {

        std::error_code fs_ec;
        const fs::path json_file = task.waveforms_dir.empty()
            ? fs::path{}
            : task.waveforms_dir / (task.item_uuid + ".json");

        // Remove stale cache entry when a forced regeneration is requested.
        if (task.force && !json_file.empty() && fs::exists(json_file, fs_ec)) {
            fs::remove(json_file, fs_ec);
        }

        // Serve from disk cache when available (skips full audio decode).
        if (!json_file.empty() && fs::exists(json_file, fs_ec)) {
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

        } catch (const std::exception& e) {
            Logger::error("waveform_worker: unhandled exception (skipping task): {}", e.what());
        } catch (...) {
            Logger::error("waveform_worker: unknown exception (skipping task).");
        }
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
        // Shared operator UI state, so a client (or control surface) that joins
        // mid-show adopts the running selection / view mode instead of
        // imposing its own stale local one.
        {"selected_item_uuid",  state.selected_item_uuid()},
        {"show_mode",           state.show_mode()},
        // The installation default. A connection that has chosen its own
        // language does not learn it from here — it already knows, having
        // asked for it — so the snapshot carries the value a client with no
        // preference should adopt (U2).
        {"locale",              state.default_ui_locale()},
        {"preview", json{
            {"item_uuid", state.current_preview_item_uuid()},
            {"cue_id",    state.current_preview_cue_id().value},
        }},
        // Master-bus geometry, so the UI can label and place output meters
        // without hardcoding a bus width. Preview always occupies the top pair,
        // which is only 30/31 at the default 32-wide bus.
        {"master_bus", json{
            {"channels",   engine.config().master_channels},
            {"preview_l",  audio::preview_master_base(engine.config().master_channels)},
            {"preview_r",  audio::preview_master_base(engine.config().master_channels) + 1},
        }},
    };
}

// uuids of every item that is currently sounding, in cue-registration order.
// Used as the fallback anchor for selection stepping: with nothing selected,
// "select down" should continue from what the operator is hearing rather than
// snapping back to the top of the playlist. Returns empty when something IS
// already selected — an explicit selection always wins, and skipping the walk
// keeps the common case free.
static std::vector<std::string> selection_anchors(audio::AudioEngine& engine,
                                                  core::ProjectState& state) {
    std::vector<std::string> playing;
    if (!state.selected_item_uuid().empty()) return playing;
    for (auto& cue : state.list_cues()) {
        auto* item = engine.find_cue(cue.id);
        if (!item) continue;
        if (item->stats().transport == audio::TransportState::Stopped) continue;
        if (auto uuid = state.cue_to_item_uuid(cue.id)) playing.push_back(*uuid);
    }
    return playing;
}

// ---------------------------------------------------------------------------
// Returns a non-empty string if a direct reply to this specific client is
// needed (pong, error). The caller sends it under ws_mutex so it doesn't
// race with broadcast_loop's concurrent send_text calls on the same conn.
// `broadcast` fans a doc_patch out to every connected client (see
// ControlServer::broadcast_doc_patch); bus commands need it to converge a
// second client the same way the REST endpoints they mirror do (D17).
// Writes to the ClientSession this message arrived on (U2). Bound to the
// connection by the onmessage lambda, because the sessions live in Impl and
// this handler is a free function. Each returns the EFFECTIVE value after the
// change, which is what the caller replies with — a request for 500 Hz meters
// is not refused, it is honoured as far as the server actually ticks, and the
// client is told what it really got rather than being left to assume.
struct SessionOps {
    std::function<std::string(const std::string&)> set_locale;
    std::function<std::size_t(std::size_t)>        set_meter_hz;
    std::function<void(const std::string&)>        set_analyser;
};

static std::string handle_ws_message(crow::websocket::connection& conn,
                                     const std::string& msg,
                                     audio::AudioEngine& engine,
                                     core::ProjectState& state,
                                     const std::string& server_addr,
                                     const std::function<void(const json&)>& broadcast,
                                     const SessionOps& session) {
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
            // Optional fade_ms overrides the cue's own manual-stop fade for
            // this one stop (0 = cut) (#56).
            const auto fade_ms = optional_fade_ms(j);
            if (j.contains("item_uuid") && j["item_uuid"].is_string()) {
                const auto uuid = j["item_uuid"].get<std::string>();
                Logger::playback("STOP: {}", item_playback_info(uuid, state));
                state.stop_item(uuid, fade_ms);
            } else {
                auto cue = resolve_cue(j);
                if (cue) {
                    if (auto uuid = state.cue_to_item_uuid(*cue)) {
                        Logger::playback("STOP: {}", item_playback_info(*uuid, state));
                        state.stop_item(*uuid, fade_ms);
                    } else {
                        Logger::playback("STOP: cue_id={} (orphan)", cue->value);
                        if (fade_ms) engine.stop(*cue, std::chrono::milliseconds{*fade_ms});
                        else         engine.stop(*cue);
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
                } else {
                    Logger::warn("WS {}: cue_id={} not live in engine", type, cue->value);
                    return json({{"type", "error"},
                                 {"message", type + ": cue not loaded into engine"}}).dump();
                }
            } else {
                Logger::warn("WS {}: no valid cue target", type);
                return json({{"type", "error"},
                             {"message", type + ": no valid cue target"}}).dump();
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
        else if (type == "go") {
            // Play whatever is armed as "Up Next" (override first, else the
            // playing item's endBehavior target). Same semantics as
            // POST /api/transport/go.
            const auto uuid = state.go();
            if (uuid.empty()) {
                Logger::warn("WS go: nothing armed or derivable to play");
                return json({{"type", "error"}, {"message", "nothing armed to GO to"}}).dump();
            }
            Logger::playback("GO: {}", item_playback_info(uuid, state));
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
                } else {
                    Logger::warn("WS seek: cue_id={} not live in engine", cue->value);
                    return json({{"type", "error"},
                                 {"message", "seek: cue not loaded into engine"}}).dump();
                }
            } else {
                Logger::warn("WS seek: no valid cue target");
                return json({{"type", "error"},
                             {"message", "seek: no valid cue target"}}).dump();
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
        else if (type == "set_selection") {
            // Shared playlist selection. Empty/absent item_uuid clears it.
            // ProjectState broadcasts the change (including back to the sender,
            // which is what keeps two clients from diverging).
            std::string uuid;
            if (j.contains("item_uuid") && j["item_uuid"].is_string())
                uuid = j["item_uuid"].get<std::string>();
            state.set_selected_item(uuid);
        }
        else if (type == "select_step") {
            // Move the shared selection through the flattened playlist.
            //
            // With nothing selected we hand ProjectState the items that are
            // currently sounding, so the step continues from what the operator
            // is hearing instead of snapping back to the top of the show. This
            // only applies when there is no selection — an explicit selection
            // always wins.
            const int delta = j.value("delta", 0);
            if (delta != 0) state.step_selection(delta, selection_anchors(engine, state));
        }
        else if (type == "set_show_mode") {
            // Omit "enabled" to toggle.
            if (j.contains("enabled") && j["enabled"].is_boolean())
                state.set_show_mode(j["enabled"].get<bool>());
            else
                state.toggle_show_mode();
        }
        else if (type == "set_locale") {
            // This connection's language, and nobody else's (U2). It used to
            // set a server-global and broadcast it, so one operator switching
            // to Greek switched every other client and every control surface
            // with them — a presentation preference imposed across users,
            // which is the leak rule R2 exists to catch.
            //
            // The reply goes back down this socket alone, through the same
            // direct_reply path a pong takes. The sender's own UI already
            // holds the value, so applying it is a no-op there; what matters
            // is that nothing arrives at anyone else.
            if (j.contains("locale") && j["locale"].is_string() && session.set_locale) {
                const auto effective = session.set_locale(j["locale"].get<std::string>());
                return json{{"type", "doc_patch"}, {"op", "locale_changed"},
                            {"locale", effective}}.dump();
            }
        }
        else if (type == "set_analyser") {
            // Which bus's spectrum this connection wants ("busId": null or ""
            // for none). Spectra then arrive as {"type":"analyser",...}
            // frames for that bus only, at the meter rate.
            if (session.set_analyser) {
                const std::string bus = (j.contains("busId") && j["busId"].is_string())
                                            ? j["busId"].get<std::string>() : std::string{};
                session.set_analyser(bus);
                return json{{"type", "analyser_subscribed"}, {"busId", bus}}.dump();
            }
        }
        else if (type == "set_meter_hz") {
            // How often THIS client wants meters. The spec's nominated first
            // test of the User tier, and the one a tablet over Wi-Fi actually
            // needs: the desk can run its meters at 60 Hz without every remote
            // paying for it.
            if (j.contains("hz") && j["hz"].is_number() && session.set_meter_hz) {
                const auto asked = j["hz"].get<double>();
                const auto effective = session.set_meter_hz(
                    asked <= 0 ? 0 : static_cast<std::size_t>(asked));
                return json{{"type", "doc_patch"}, {"op", "meter_hz_changed"},
                            {"hz", effective}}.dump();
            }
        }
        else if (type == "bus_gain") {
            // Same code path as PATCH /api/buses/<id>: persists to the
            // document and applies straight to the live strip, then
            // broadcasts buses_patched so a second client converges (D17).
            using PR = core::ProjectState::PatchBusResult;
            const std::string busId = j.value("busId", "");
            if (busId.empty()) {
                return json({{"type", "error"}, {"message", "bus_gain: missing busId"}}).dump();
            }
            const float db = j.value("gainDb", 0.0f);
            const auto r = state.patch_bus(busId, json{{"gainDb", db}});
            if (r == PR::NotFound) {
                return json({{"type", "error"}, {"message", "bus_gain: not found"}}).dump();
            }
            if (r != PR::Ok) {
                return json({{"type", "error"},
                             {"message", std::string(bus_output_refusal_text(r))}}).dump();
            }
            if (broadcast) {
                broadcast(json{
                    {"type", "doc_patch"}, {"op", "buses_patched"},
                    {"buses", state.full_document().value("buses", json::array())},
                });
            }
        }
        else if (type == "bus_mute") {
            // Omit "mute" to toggle the bus's current state. Same code path
            // as PATCH /api/buses/<id> for the actual mutation.
            using PR = core::ProjectState::PatchBusResult;
            const std::string busId = j.value("busId", "");
            if (busId.empty()) {
                return json({{"type", "error"}, {"message", "bus_mute: missing busId"}}).dump();
            }
            bool mute = false;
            if (j.contains("mute") && j["mute"].is_boolean()) {
                mute = j["mute"].get<bool>();
            } else {
                bool found = false;
                for (const auto& b : state.list_buses()) {
                    if (b.def.id == busId) { mute = !b.def.muted; found = true; break; }
                }
                if (!found) {
                    return json({{"type", "error"}, {"message", "bus_mute: not found"}}).dump();
                }
            }
            const auto r = state.patch_bus(busId, json{{"mute", mute}});
            if (r == PR::NotFound) {
                return json({{"type", "error"}, {"message", "bus_mute: not found"}}).dump();
            }
            if (r != PR::Ok) {
                return json({{"type", "error"},
                             {"message", std::string(bus_output_refusal_text(r))}}).dump();
            }
            if (broadcast) {
                broadcast(json{
                    {"type", "doc_patch"}, {"op", "buses_patched"},
                    {"buses", state.full_document().value("buses", json::array())},
                });
            }
        }
        else if (type == "bus_pfl") {
            // Omit "pfl" to toggle. Same code path as
            // POST /api/buses/<id>/pfl, including its broadcast shape.
            const std::string busId = j.value("busId", "");
            if (busId.empty()) {
                return json({{"type", "error"}, {"message", "bus_pfl: missing busId"}}).dump();
            }
            bool pfl = false;
            if (j.contains("pfl") && j["pfl"].is_boolean()) {
                pfl = j["pfl"].get<bool>();
            } else {
                bool found = false;
                for (const auto& b : state.list_buses()) {
                    if (b.def.id == busId) { pfl = !b.pfl; found = true; break; }
                }
                if (!found) {
                    return json({{"type", "error"}, {"message", "bus_pfl: not found"}}).dump();
                }
            }
            if (!state.set_bus_pfl(busId, pfl)) {
                return json({{"type", "error"},
                             {"message", "bus_pfl: not found, or not a bus that can be PFL'd"}}).dump();
            }
            if (broadcast) {
                broadcast(json{
                    {"type", "doc_patch"}, {"op", "bus_pfl_changed"},
                    {"id", busId}, {"pfl", pfl},
                });
            }
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

    // Central exception handler: any route that throws past its own try/catch
    // (or has none) lands here instead of Crow's bare 500. Crow invokes this
    // from inside a catch(...) block, so a `throw;` re-raises the active
    // exception, letting us recover its message. We reply with the same JSON +
    // CORS shape as json_err() so browser clients never see an opaque,
    // CORS-less 500.
    app.exception_handler([](crow::response& res){
        std::string message = "internal server error";
        try {
            throw;
        } catch (const std::exception& e) {
            message = e.what();
            Logger::error("Uncaught exception in route handler: {}", e.what());
        } catch (...) {
            Logger::error("Uncaught non-std exception in route handler.");
        }
        res = json_err(500, message);
    });

    // No OPTIONS route: Crow answers preflights itself and never dispatches
    // them to a rule, so the CORS headers are added in AuthGuard::after_handle.

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

    // Who is on this server right now (U1). One row per live WebSocket, which
    // is what "a client" means here: REST is stateless and a curl against it
    // is not a session, but anything driving the rig holds a socket open.
    //
    // The read surface for the per-connection record — without one, identity
    // would be a structure nothing could observe, and the Server pane has
    // nothing to show an operator asking who else is holding the desk.
    // U2 added each session's locale and meter rate; U3 adds the principal and
    // gates the route to administrators, so the addresses are no longer
    // readable by anyone who can reach the port. On an installation with no
    // accounts it stays open, because there is nobody to be an administrator —
    // the same posture as every other route, and the reason the boot warning
    // says so out loud.
    CROW_ROUTE(app, "/api/clients").methods(crow::HTTPMethod::Get)
        ([this]{
            try {
                const auto now = std::chrono::steady_clock::now();
                // Read before taking ws_mutex: default_ui_locale() takes the
                // project lock, and the broadcast loop's ABBA note applies to
                // every ws_mutex → project-lock ordering, not just its own.
                const std::string fallback_locale = state_.default_ui_locale();
                const std::size_t tick_hz = std::max<std::size_t>(1, cfg_.meter_broadcast_hz);
                json arr = json::array();
                {
                    std::lock_guard lock{impl_->ws_mutex};
                    for (const auto& [_, s] : impl_->ws_clients) {
                        arr.push_back(json{
                            {"id",               s.id},
                            {"remoteIp",         s.remote_ip},
                            // Empty on an installation with no accounts. A
                            // null user is reported as null rather than as a
                            // name like "anonymous", so a caller cannot
                            // confuse the open posture with someone who
                            // happens to be called that.
                            {"user",             s.user_name.empty()
                                                     ? json(nullptr)
                                                     : json(s.user_name)},
                            {"userId",           s.user_id.empty()
                                                     ? json(nullptr)
                                                     : json(s.user_id)},
                            {"isAdmin",          s.is_admin},
                            // "user", "token", or "anonymous" while the server
                            // has no accounts. A name alone cannot carry this:
                            // a token's label and a login name are both just
                            // text, and they mean entirely different things in
                            // a list of who is connected right now.
                            {"kind",             s.is_api    ? "token"
                                                 : s.user_id.empty() ? "anonymous"
                                                                     : "user"},
                            // Effective values, not raw ones: an empty locale
                            // and a zero rate both mean "no preference", and
                            // reporting them as blanks would make the caller
                            // re-derive the override chain to say anything
                            // useful. `localeIsOwn` keeps the distinction that
                            // actually matters — whether a change to the
                            // default will reach this session.
                            {"locale",           s.locale.empty() ? fallback_locale : s.locale},
                            {"localeIsOwn",      !s.locale.empty()},
                            {"meterHz",          s.meter_hz == 0 ? tick_hz : s.meter_hz},
                            {"meterHzIsOwn",     s.meter_hz != 0},
                            // Elapsed, not a wall-clock stamp: it is what an
                            // operator actually wants ("that one has been on
                            // for ten minutes"), it survives a clock step, and
                            // it does not duplicate a timestamp formatter that
                            // already has one owner elsewhere.
                            {"connectedSeconds",
                             std::chrono::duration_cast<std::chrono::seconds>(
                                 now - s.connected_at).count()},
                        });
                    }
                }
                // Stable order, so a UI listing them does not reshuffle on
                // every poll — the map's own order is a hash of pointers.
                std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
                    return a.value("id", 0ULL) < b.value("id", 0ULL);
                });
                return json_ok(arr);
            } catch (const std::exception& e) { return json_err(500, e.what()); }
        });

    // ------------------------------------------------------------------
    // Authentication (U3)
    // ------------------------------------------------------------------
    // What posture this installation is in. Public, and it has to be: a client
    // cannot know whether to ask for a password until it has asked this, and
    // requiring a token to find out whether a token is required is a loop.
    //
    // It reveals only that — whether there are accounts, and how many. Not who
    // they are. A prospective attacker learns nothing they could not learn by
    // watching whether /api/health and /api/cues disagree.
    CROW_ROUTE(app, "/api/auth/status").methods(crow::HTTPMethod::Get)
        ([this]{
            return json_ok(json{
                {"authRequired", users_.auth_required()},
                {"userCount",    users_.user_count()},
                // The bootstrap window, named rather than inferred. While this
                // is true anyone who can reach the server may create the first
                // account — which is exactly as open as the server already is
                // with no accounts at all, and it closes the moment one exists.
                {"setupRequired", users_.user_count() == 0},
                {"tokenTtlSeconds", core::kTokenTtlSeconds},
            });
        });

    // ------------------------------------------------------------------
    // Turn authentication off, or back on, without throwing the accounts away.
    //
    // WHY THIS EXISTS: `auth_required()` used to be "the store is not empty",
    // and `remove_user` refuses to delete the last administrator — so the guard
    // that stops an operator locking themselves out of account management also
    // made authentication PERMANENT. The documented recovery was deleting
    // users.json by hand on the machine, which discards the whole team to undo a
    // posture change. The accounts survive this.
    //
    // WHAT PROTECTS IT — read this before changing anything here:
    //
    //  1. An ADMINISTRATOR'S NAME AND PASSWORD, in the body, every time, in BOTH
    //     directions. Not the session, and not only when turning it off. The
    //     access_for entry cannot be what protects this route, because while
    //     authentication is off the guard short-circuits and every admin path is
    //     open — so anyone on the LAN could otherwise flip it. Turning it ON
    //     needs the password too, or an anonymous caller could lock a desk mid
    //     show; that is a denial of service rather than a breach, but it is free
    //     to close and so it is closed.
    //  2. RE-ENTRY RATHER THAN THE TOKEN, deliberately, because tokens here are
    //     stateless, signed, long-lived and cross the LAN with no TLS anywhere in
    //     this server. A token can be read off the wire; a password is asked for
    //     at the moment of the act.
    //  3. THE LOGIN THROTTLE, shared with /api/auth/login. Without it this is an
    //     unthrottled password oracle that walks straight past the brake on the
    //     front door — the same guess, the same rate limit.
    //  4. An operator role is refused even with the right password, and it is
    //     reported as a role refusal, not as a bad password: telling somebody
    //     their password was wrong when it was right teaches them to distrust
    //     the message that matters.
    CROW_ROUTE(app, "/api/auth/required").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req){
            try {
                const auto body = json::parse(req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object())
                    return json_err(400, "expected a JSON object");
                if (!body.contains("required") || !body["required"].is_boolean())
                    return json_err(400, "\"required\" must be true or false");
                const bool required = body["required"].get<bool>();

                // Nothing to authenticate against and nothing to protect: say so
                // plainly rather than failing the password check, which would
                // read as "you typed it wrong" on a server that has no accounts.
                if (users_.user_count() == 0)
                    return json_err(409, "this server has no accounts — "
                                         "authentication is already off");

                const auto& ctx = impl_->app.get_context<AuthGuard>(req);
                // Defaulted from the session when there is one, so a signed-in
                // administrator confirms with their password alone. When
                // authentication is off there is no session and the name is
                // required, which is also the case where this check IS the gate.
                const std::string name = body.value(
                    "name", ctx.authenticated ? ctx.principal.name : std::string{});
                const std::string pass = body.value("password", std::string{});
                if (name.empty() || pass.empty())
                    return json_err(400, "an administrator's name and password "
                                         "are required to change this");

                if (const int wait = login_block_remaining(req.remote_ip_address); wait > 0) {
                    auto r = json_err(429, "too many failed attempts — wait and try again");
                    r.add_header("Retry-After", std::to_string(wait));
                    return r;
                }

                const auto principal = users_.authenticate(name, pass);
                if (!principal) {
                    login_note_failure(req.remote_ip_address);
                    Logger::warn("Auth posture change refused for '{}' from {} "
                                 "(bad credentials)", name, req.remote_ip_address);
                    return json_err(401, "incorrect user name or password");
                }
                if (!principal->is_admin()) {
                    // Counted as a failure as well: the throttle exists to slow
                    // guessing, and an operator account is still a valid guess.
                    login_note_failure(req.remote_ip_address);
                    Logger::warn("Auth posture change refused for '{}' from {} "
                                 "(not an administrator)", name, req.remote_ip_address);
                    return json_err(403, "only an administrator can change "
                                         "whether this server requires a login");
                }
                login_note_success(req.remote_ip_address);

                using R = core::UserStore::Result;
                const auto r = users_.set_auth_required(required);
                if (r == R::NoAccounts) return json_err(409, core::UserStore::describe(r));
                if (r != R::Ok)        return json_err(500, core::UserStore::describe(r));

                Logger::warn("Authentication turned {} by '{}' from {} — "
                             "{} account(s) kept",
                             required ? "ON" : "OFF", principal->name,
                             req.remote_ip_address, users_.user_count());
                return json_ok(json{
                    {"authRequired", users_.auth_required()},
                    {"userCount",    users_.user_count()},
                });
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/auth/login").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                const auto body = json::parse(req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object())
                    return json_err(400, "expected a JSON object");
                const std::string name = body.value("name",     std::string{});
                const std::string pass = body.value("password", std::string{});
                if (name.empty() || pass.empty())
                    return json_err(400, "name and password are required");

                if (!users_.auth_required())
                    return json_err(409, "this server has no accounts — "
                                         "authentication is not in use");

                if (const int wait = login_block_remaining(req.remote_ip_address); wait > 0) {
                    auto r = json_err(429, "too many failed attempts — wait and try again");
                    r.add_header("Retry-After", std::to_string(wait));
                    return r;
                }

                const auto principal = users_.authenticate(name, pass);
                if (!principal) {
                    login_note_failure(req.remote_ip_address);
                    // One message for a wrong name and a wrong password.
                    // Distinguishing them would hand out the account list one
                    // guess at a time, and authenticate() already spends the
                    // same time on both so the reply cannot be told apart by
                    // how long it took either.
                    Logger::warn("Login failed for '{}' from {}", name, req.remote_ip_address);
                    return json_err(401, "incorrect user name or password");
                }
                login_note_success(req.remote_ip_address);

                const std::string token = users_.mint_token(*principal);
                if (token.empty()) return json_err(500, "could not issue a token");
                Logger::info("Login: '{}' ({}) from {}", principal->name,
                             core::to_string(principal->role), req.remote_ip_address);
                const auto account = users_.find_by_id(principal->id);
                return json_ok(json{
                    {"token",     token},
                    {"expiresIn", core::kTokenTtlSeconds},
                    {"user", json{
                        {"id",   principal->id},
                        {"name", principal->name},
                        {"role", core::to_string(principal->role)},
                        {"avatar", account && !account->avatar.empty()
                                       ? json(account->avatar) : json(nullptr)},
                    }},
                });
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // Who the caller is according to their token. The client uses it to
    // confirm a stored token is still good before it shows the desk, which is
    // why it answers 401 rather than a body saying "not logged in" — the guard
    // has already refused by the time this handler would run.
    CROW_ROUTE(app, "/api/auth/me").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req){
            const auto& ctx = impl_->app.get_context<AuthGuard>(req);
            if (!ctx.authenticated)
                return json_ok(json{{"authRequired", false}, {"user", nullptr}});
            // Looked up rather than carried in the principal: the picture is not
            // part of who somebody IS, and keeping it out of Principal keeps a
            // copy of it out of every request that authenticates. A token has
            // no account and so no picture — null, not absent, so the shape is
            // the same for both kinds.
            std::optional<core::UserStore::User> account;
            if (!ctx.principal.is_api()) account = users_.find_by_id(ctx.principal.id);
            return json_ok(json{
                {"authRequired", true},
                {"user", json{
                    {"id",   ctx.principal.id},
                    {"name", ctx.principal.name},
                    {"role", core::to_string(ctx.principal.role)},
                    // Which kind of credential asked. A Companion instance
                    // pointed at this route should be told it is a token rather
                    // than left to infer it from a role it never had.
                    {"kind", ctx.principal.is_api() ? "token" : "user"},
                    {"avatar", account && !account->avatar.empty()
                                   ? json(account->avatar) : json(nullptr)},
                }},
            });
        });

    // ---- The caller's own picture (User tier) ----
    //
    // Like /api/prefs, neither route names a user: the account changed is
    // always the caller's, so an operator can change their own face and has no
    // way to ask for anybody else's. An administrator changes other people's
    // through PATCH /api/users/<id>, which is the Server tier.
    //
    // An API token is refused by the deny list before this runs — a machine has
    // no face — and with authentication off there is nobody to be, so the
    // answer is the same 409 /api/prefs gives rather than a guess about whose
    // picture was meant. (Every /api/users route is open in that posture, which
    // is how the pane still sets pictures there.)
    const auto own_avatar = [this](const crow::request& req, const std::string& avatar) {
        const auto& ctx = impl_->app.get_context<AuthGuard>(req);
        if (!ctx.authenticated || ctx.principal.is_api())
            return json_err(409, "no signed-in user — sign in to change your own picture");
        using R = core::UserStore::Result;
        const auto r = users_.set_avatar(ctx.principal.id, avatar);
        if (r == R::NoSuchUser) return json_err(404, "no such user");
        if (r == R::AvatarTooLarge) return json_err(413, core::UserStore::describe(r));
        if (r == R::BadAvatar)  return json_err(400, core::UserStore::describe(r));
        if (r != R::Ok)         return json_err(500, core::UserStore::describe(r));
        return json_ok(json{
            {"id",     ctx.principal.id},
            {"avatar", avatar.empty() ? json(nullptr) : json(avatar)},
        });
    };

    CROW_ROUTE(app, "/api/auth/me/avatar").methods(crow::HTTPMethod::Put)
        ([own_avatar](const crow::request& req){
            try {
                const auto body = json::parse(req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object())
                    return json_err(400, "expected a JSON object");
                if (!body.contains("avatar") || !body["avatar"].is_string())
                    return json_err(400, "\"avatar\" must be an image data URL — "
                                         "DELETE removes the picture");
                return own_avatar(req, body["avatar"].get<std::string>());
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/auth/me/avatar").methods(crow::HTTPMethod::Delete)
        ([own_avatar](const crow::request& req){
            return own_avatar(req, std::string{});
        });

    // "Sign me out everywhere." Bumps the caller's token epoch, which
    // invalidates every token ever issued to them — including the one making
    // this request, and including the one on the tablet they left at the venue,
    // which is the entire point.
    CROW_ROUTE(app, "/api/auth/logout_all").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            const auto& ctx = impl_->app.get_context<AuthGuard>(req);
            if (!ctx.authenticated) return json_err(400, "not authenticated");
            const auto r = users_.bump_epoch(ctx.principal.id);
            if (r != core::UserStore::Result::Ok)
                return json_err(500, core::UserStore::describe(r));
            Logger::info("'{}' invalidated all of their tokens", ctx.principal.name);
            return json_ok(json{{"ok", true}});
        });

    // ---- The caller's own preferences (User tier — U4) ----
    //
    // Neither route names a user, and that is the access decision: the profile
    // acted on is always the caller's, so there is no shape of request that
    // reads or writes somebody else's. An administrator owns the machine, not
    // the people using it — and playbackKeys in particular is a description of
    // what one person's hands do, which nobody else has a reason to fetch.
    //
    // Note also what is NOT here: no entry in access_for(). These are covered
    // by its default-deny, which is the middleware doing the job it was built
    // for — a route added later is protected without anyone remembering.
    CROW_ROUTE(app, "/api/prefs").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req){
            const auto& ctx = impl_->app.get_context<AuthGuard>(req);
            if (!ctx.authenticated) {
                // Not an error the client should retry, and not a 404 either:
                // the route exists, there is simply nobody for it to be about.
                // With no accounts configured the client keeps these values in
                // its own machine store, which is the correct answer for a
                // preference with no person attached.
                return json_err(409, "no signed-in user — preferences are local "
                                     "to this surface while authentication is off");
            }
            // THE MIGRATION, in one line. A profile that does not exist yet is
            // created from whatever the open project is still carrying, at the
            // first moment there is somewhere to put it — so the operator's
            // theme and keymap survive the values leaving the document, and
            // this can never run twice for the same person.
            const json profile = prefs_.get_or_seed(ctx.principal.id,
                                                    state_.legacy_user_prefs());
            return json_ok(profile);
        });

    CROW_ROUTE(app, "/api/prefs").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req){
            const auto& ctx = impl_->app.get_context<AuthGuard>(req);
            if (!ctx.authenticated)
                return json_err(409, "no signed-in user — preferences are local "
                                     "to this surface while authentication is off");
            try {
                const auto patch = json::parse(req.body);
                if (!patch.is_object()) return json_err(400, "expected an object");

                json profile;
                const auto r = prefs_.patch(ctx.principal.id, patch, &profile);
                if (r != core::UserPrefs::Result::Ok)
                    return json_err(500, std::string{core::UserPrefs::describe(r)});

                // The meter unit is the one preference the server acts on, so
                // it is the one that has to reach the engine. Every session
                // this person has open is updated, not just the one that sent
                // the patch: the desk and the detached mixer window are two
                // sockets belonging to one operator.
                const std::string mode = profile.value("meterMode", std::string{});
                {
                    std::lock_guard lock{impl_->ws_mutex};
                    for (auto& [_, session] : impl_->ws_clients) {
                        if (session.user_id == ctx.principal.id) session.meter_mode = mode;
                    }
                }
                refresh_user_meter_modes();

                // ...and the rest reaches this person's other windows so they
                // do not sit on a stale theme until the next reconnect. Sent
                // to them ONLY — the other operators in the building are not
                // interested in somebody else's colour scheme.
                broadcast_to_user(ctx.principal.id, json{
                    {"type", "doc_patch"}, {"op", "prefs_changed"}, {"prefs", profile},
                });
                return json_ok(profile);
            } catch (const std::exception& e) {
                return json_err(400, e.what());
            }
        });

    // ---- The machine's own configuration (Server tier — admins only) ----
    //
    // One response renders the whole settings form: every key in the schema,
    // its range, what it is for, what is stored in the file, what is actually
    // in force, and WHO SET IT. That last field is the one that keeps the page
    // honest — the desktop app always launches with --port, so a page that
    // offered to edit the port without saying so would write the file, report
    // success and change nothing.
    CROW_ROUTE(app, "/api/server/config").methods(crow::HTTPMethod::Get)
        ([this]{
            const json stored = server_config_.read();
            json fields = json::array();
            for (const auto& f : core::ServerConfig::schema()) {
                const std::string key{f.key};
                // Absent from the sources map means nobody set it: the built-in
                // default stands, and the file is free to claim it.
                const std::string source =
                    cfg_.boot_sources.value(key, std::string{"default"});
                json entry{
                    {"key",     key},
                    {"flag",    std::string{f.flag}},
                    {"help",    std::string{f.help}},
                    {"policy",  f.policy},
                    {"source",  source},
                    {"stored",  stored.contains(key) ? stored[key] : json(nullptr)},
                    {"value",   cfg_.boot_effective.contains(key)
                                    ? cfg_.boot_effective[key] : json(nullptr)},
                    // Every one of these is read once, at boot. Saying so per
                    // field rather than in prose somewhere is what stops an
                    // operator believing a sample-rate change took hold in the
                    // middle of a show.
                    {"appliesAt", f.applies == core::ServerConfig::Applies::Live
                                    ? "live" : "restart"},
                    // A value the environment or a flag is supplying cannot be
                    // changed by writing the file. The page greys the field and
                    // says which tier is winning, instead of accepting an edit
                    // that goes nowhere.
                    {"overridden", source == "env" || source == "cli"},
                };
                switch (f.kind) {
                    case core::ServerConfig::Kind::Int:      entry["type"] = "int";      break;
                    case core::ServerConfig::Kind::Real:     entry["type"] = "real";     break;
                    case core::ServerConfig::Kind::Text:     entry["type"] = "text";     break;
                    case core::ServerConfig::Kind::Bool:     entry["type"] = "bool";     break;
                    case core::ServerConfig::Kind::PathList: entry["type"] = "pathList"; break;
                }
                if (f.kind == core::ServerConfig::Kind::Int ||
                    f.kind == core::ServerConfig::Kind::Real) {
                    entry["min"] = f.min;
                    entry["max"] = f.max;
                }
                fields.push_back(std::move(entry));
            }
            return json_ok(json{
                {"path",          util::path_to_utf8(server_config_.path())},
                {"schemaVersion", core::kConfigSchemaVersion},
                {"locked",        server_config_.locked()},
                {"fields",        std::move(fields)},
            });
        });

    CROW_ROUTE(app, "/api/server/config").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req){
            try {
                const auto patch = json::parse(req.body);
                if (!patch.is_object()) return json_err(400, "expected an object");

                json file;
                std::vector<std::string> dropped;
                const auto r = server_config_.patch(patch, &file, &dropped);
                using R = core::ServerConfig::Result;
                if (r == R::Locked) {
                    // 403 rather than 409: this is not a conflict to retry, it
                    // is a refusal, and the client should render the page
                    // read-only rather than offer the edit again.
                    return json_err(403, std::string{core::ServerConfig::describe(r)});
                }
                if (r != R::Ok) return json_err(500, std::string{core::ServerConfig::describe(r)});

                // What was written but is NOT in force, and why. Two separate
                // reasons, and conflating them would be a lie either way: a
                // value can be waiting on a restart, or it can be shadowed by a
                // flag that will still be there after one.
                json pending = json::array(), shadowed = json::array();
                for (const auto& [k, v] : patch.items()) {
                    const auto* f = core::ServerConfig::find(k);
                    if (!f) continue;
                    const std::string source = cfg_.boot_sources.value(k, std::string{"default"});
                    if (source == "env" || source == "cli") shadowed.push_back(k);
                    else if (f->applies == core::ServerConfig::Applies::Restart)
                        pending.push_back(k);
                }

                Logger::info("Server configuration patched ({} key(s) stored, {} dropped)",
                             file.size(), dropped.size());
                return json_ok(json{
                    {"stored",           file},
                    {"dropped",          dropped},
                    {"restartRequired",  pending},
                    {"overriddenAtLaunch", shadowed},
                });
            } catch (const std::exception& e) {
                return json_err(400, e.what());
            }
        });

    // ---- Accounts (Server tier — administrators only) ----
    CROW_ROUTE(app, "/api/users").methods(crow::HTTPMethod::Get)
        ([this]{
            json arr = json::array();
            for (const auto& u : users_.users()) {
                arr.push_back(json{
                    {"id",        u.id},
                    {"name",      u.name},
                    {"role",      core::to_string(u.role)},
                    {"createdAt", u.created_at},
                    // Inline, as a data URL, because it is small by
                    // construction (kMaxAvatarBytes) and because the list is
                    // the one place a pane needs every face at once — a second
                    // request per row would be worse than the bytes.
                    {"avatar",    u.avatar.empty() ? json(nullptr) : json(u.avatar)},
                });
            }
            return json_ok(arr);
        });

    // Admin-gated by access_for — EXCEPT while the store is empty, when the
    // guard admits everything because there is nobody to be an administrator.
    // That is the bootstrap: the first account creates itself, and it is forced
    // to admin by the store so the installation cannot end up locked with no
    // one able to manage it.
    //
    // The window is real and it is worth being plain about: until the first
    // account exists, whoever reaches the port first can claim the rig. That is
    // not a new exposure — it is the state every release so far has shipped in,
    // permanently — and unlike that state it closes, the moment someone sets up.
    CROW_ROUTE(app, "/api/users").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                const auto body = json::parse(req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object())
                    return json_err(400, "expected a JSON object");
                const std::string name = body.value("name",     std::string{});
                const std::string pass = body.value("password", std::string{});
                const auto role = core::role_from_string(
                    body.value("role", std::string{"operator"}));
                if (!role) return json_err(400, "role must be 'admin' or 'operator'");

                const bool bootstrapping = users_.user_count() == 0;
                std::string id;
                const auto r = users_.add_user(name, pass, *role, &id);
                if (r != core::UserStore::Result::Ok) {
                    // 409 for a name clash (understood, refused), 400 for input
                    // that was never going to work, 500 for a disk that would
                    // not take it — the same split D6 uses everywhere else.
                    const int code =
                        r == core::UserStore::Result::NameTaken ? 409 :
                        r == core::UserStore::Result::IoError   ? 500 :
                        r == core::UserStore::Result::HashFailed? 500 : 400;
                    return json_err(code, core::UserStore::describe(r));
                }
                if (bootstrapping) {
                    Logger::warn("First account created — this server now requires "
                                 "authentication. Anonymous clients will be refused.");
                }
                const auto created = users_.find_by_id(id);
                return json_ok(json{
                    {"id",   id},
                    {"name", created ? created->name : name},
                    {"role", core::to_string(created ? created->role : *role)},
                });
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // Change a name, a role, or a password. Split from POST because "make this
    // person an admin" and "create this person" are different mistakes to make
    // by accident.
    CROW_ROUTE(app, "/api/users/<string>").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req, const std::string& id){
            try {
                const auto body = json::parse(req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object())
                    return json_err(400, "expected a JSON object");
                if (!users_.find_by_id(id)) return json_err(404, "no such user");

                using R = core::UserStore::Result;
                const auto refuse = [](R r) {
                    const int code = r == R::NameTaken ? 409
                                   : r == R::LastAdmin ? 409
                                   : r == R::IoError   ? 500 : 400;
                    return json_err(code, core::UserStore::describe(r));
                };

                // The picture is checked BEFORE anything is applied. The other
                // fields are applied one at a time, so a bad picture found last
                // would otherwise leave a renamed account and an error saying
                // nothing happened.
                if (body.contains("avatar") && body["avatar"].is_string()) {
                    const auto v = core::UserStore::validate_avatar(body["avatar"].get<std::string>());
                    if (v == R::AvatarTooLarge) return json_err(413, core::UserStore::describe(v));
                    if (v != R::Ok) return json_err(400, core::UserStore::describe(v));
                }

                if (body.contains("name")) {
                    if (!body["name"].is_string()) return json_err(400, "name must be a string");
                    if (const auto r = users_.rename_user(id, body["name"].get<std::string>());
                        r != R::Ok) return refuse(r);
                }
                if (body.contains("role")) {
                    const auto role = core::role_from_string(body.value("role", std::string{}));
                    if (!role) return json_err(400, "role must be 'admin' or 'operator'");
                    if (const auto r = users_.set_role(id, *role); r != R::Ok) return refuse(r);
                }
                if (body.contains("password")) {
                    if (!body["password"].is_string())
                        return json_err(400, "password must be a string");
                    if (const auto r = users_.set_password(id, body["password"].get<std::string>());
                        r != R::Ok) return refuse(r);
                    Logger::info("Password changed for user {} — their existing "
                                 "tokens are now invalid", id);
                }
                // A picture: a data URL to set, null to clear. An administrator
                // may set anyone's; a person setting their own goes through
                // /api/auth/me/avatar instead, which needs no admin.
                if (body.contains("avatar")) {
                    const auto& a = body["avatar"];
                    if (!a.is_null() && !a.is_string())
                        return json_err(400, "avatar must be an image data URL or null");
                    const auto r = users_.set_avatar(id, a.is_null() ? std::string{}
                                                                      : a.get<std::string>());
                    if (r == R::AvatarTooLarge) return json_err(413, core::UserStore::describe(r));
                    if (r != R::Ok) return refuse(r);
                }
                const auto after = users_.find_by_id(id);
                if (!after) return json_err(404, "no such user");
                return json_ok(json{
                    {"id",     after->id},
                    {"name",   after->name},
                    {"role",   core::to_string(after->role)},
                    {"avatar", after->avatar.empty() ? json(nullptr) : json(after->avatar)},
                });
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/users/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const std::string& id){
            using R = core::UserStore::Result;
            const auto r = users_.remove_user(id);
            if (r == R::NoSuchUser) return json_err(404, "no such user");
            // Refusing to remove the last administrator is not paternalism: the
            // store would still require authentication and nobody left could
            // manage it, so the only way back would be editing users.json by
            // hand on the machine. Turning authentication OFF is a separate,
            // explicit act — PATCH /api/auth/required — which keeps the accounts
            // rather than asking anyone to empty the store to change a posture.
            if (r == R::LastAdmin)  return json_err(409, core::UserStore::describe(r));
            if (r != R::Ok)         return json_err(500, core::UserStore::describe(r));
            // The account is gone, so its preferences go with it. Ids are
            // never reused, so this is not strictly required to stop a new
            // account inheriting a stranger's keymap — it is required because
            // leaving the file behind means deleting a user does not actually
            // delete what the server knows about them.
            prefs_.forget(id);
            return json_ok(json{{"ok", true}});
        });

    // ---- Moving the account list to another machine ----
    //
    // Both routes are admin-only three times over: access_for names them, the
    // /api/users prefix covers them, and api_token_forbidden refuses a token
    // before the role is even asked about.
    //
    // THE EXPORT needs a SIGNED-IN administrator, even in the open posture
    // where every other /api/users route is admitted without one. It is the
    // only route that hands out password hashes, and with the login off anyone
    // on the network would otherwise be able to take them away and guess at
    // them offline. The other open routes can change accounts but cannot
    // reveal a password; this one could. So: turn the login on to export.
    CROW_ROUTE(app, "/api/users/export").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req){
            const auto& ctx = impl_->app.get_context<AuthGuard>(req);
            if (!ctx.authenticated || !ctx.principal.is_admin())
                return json_err(409, "turn the login on and sign in as an administrator "
                                     "to export accounts — the file contains password hashes");
            Logger::warn("Account list exported by '{}' from {} ({} account(s))",
                         ctx.principal.name, req.remote_ip_address, users_.user_count());
            auto res = json_ok(users_.export_json());
            // Hashes do not belong in a browser or proxy cache.
            res.add_header("Cache-Control", "no-store");
            return res;
        });

    // THE IMPORT follows the rest of /api/users: admin while the login is on,
    // open while it is off — which is what lets a FRESH machine, with no
    // accounts and so no administrator, take the team from another one. That
    // is the bootstrap window POST /api/users already opens, and it closes the
    // same way: the moment the import lands, there are accounts and the login
    // is on.
    //
    //   { "mode": "merge" | "replace", "data": <the exported file> }
    //
    // After a replace the caller's own session may no longer be anybody's (the
    // id is gone, or the password changed and the epoch moved). The reply says
    // so in `sessionValid`, the client then shows the login, and every open
    // socket that no longer verifies is closed on the next broadcast tick.
    CROW_ROUTE(app, "/api/users/import").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                const auto body = json::parse(req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object())
                    return json_err(400, "expected a JSON object");
                const std::string mode_s = body.contains("mode") && body["mode"].is_string()
                                               ? body["mode"].get<std::string>() : std::string{};
                using M = core::UserStore::ImportMode;
                if (mode_s != "merge" && mode_s != "replace")
                    return json_err(400, "mode must be 'merge' or 'replace'");
                if (!body.contains("data")) return json_err(400, "\"data\" is required");
                const M mode = mode_s == "replace" ? M::Replace : M::Merge;

                const bool was_open = !users_.auth_required();
                core::UserStore::ImportReport rep;
                std::string detail;
                using R = core::UserStore::Result;
                const auto r = users_.import_json(body["data"], mode, &rep, &detail);
                if (r == R::BadImport) return json_err(400, detail);
                if (r == R::NoAdmin || r == R::AuthOff)
                    return json_err(409, core::UserStore::describe(r));
                if (r != R::Ok) return json_err(500, core::UserStore::describe(r));

                // Removed accounts take their preferences with them, as DELETE does.
                for (const auto& id : rep.users_removed) prefs_.forget(id);

                const auto& ctx = impl_->app.get_context<AuthGuard>(req);
                if (was_open && users_.auth_required()) {
                    Logger::warn("Accounts imported into an open server — it now requires "
                                 "authentication. Anonymous clients will be refused.");
                }
                // Is the credential that made this request still one? Asked of
                // the store directly rather than inferred, because the answer
                // depends on ids, epochs and hashes the client cannot see.
                bool session_valid = true;
                if (ctx.authenticated) {
                    const std::string auth = req.get_header_value("Authorization");
                    session_valid = auth.rfind("Bearer ", 0) == 0 &&
                                    users_.verify_token(auth.substr(7)).has_value();
                }
                Logger::warn("Accounts {} by '{}' from {}",
                             mode == M::Replace ? "REPLACED" : "merged",
                             ctx.authenticated ? ctx.principal.name : std::string{"(open server)"},
                             req.remote_ip_address);
                return json_ok(json{
                    {"mode",           mode_s},
                    {"usersAdded",     rep.users_added},
                    {"usersSkipped",   rep.users_skipped},
                    {"usersRemoved",   rep.users_removed.size()},
                    {"tokensAdded",    rep.tokens_added},
                    {"tokensSkipped",  rep.tokens_skipped},
                    {"idsRegenerated", rep.ids_regenerated},
                    {"authRequired",   users_.auth_required()},
                    {"userCount",      users_.user_count()},
                    {"sessionValid",   session_valid},
                });
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- API tokens (Server tier — administrators only) ----
    //
    // The credential a Companion instance, a show-control cue or a script
    // carries. §6.2 of the ownership model called this "a principal that is not
    // a person" and listed it as not built; this is it.
    //
    // WHAT PROTECTS ISSUING ONE — three things, none of them redundant:
    //
    //  1. THE ADMIN GATE, via access_for. Ordinary here, unlike
    //     /api/auth/required: issuing is refused outright while authentication
    //     is off (the store enforces that, not this route), so there is no
    //     state in which the middleware has short-circuited and an anonymous
    //     caller reaches this handler.
    //  2. THE CALLER'S PASSWORD, re-entered. The same reasoning U3 recorded for
    //     the posture switch, and it applies harder here: tokens cross the LAN
    //     with no TLS anywhere in this server, and what is being minted is a
    //     credential that does not expire. A sniffed admin session should not be
    //     convertible into permanent access.
    //  3. THE SHARED LOGIN THROTTLE, because (2) makes this another place a
    //     password can be guessed, and a brake on one door is not a brake.
    //
    // Revoking needs none of that beyond the admin gate: friction on the way out
    // costs security, and a revocation only ever removes access.
    CROW_ROUTE(app, "/api/tokens").methods(crow::HTTPMethod::Get)
        ([this]{
            json arr = json::array();
            for (const auto& t : users_.api_tokens()) {
                arr.push_back(json{
                    {"id",        t.id},
                    {"name",      t.name},
                    {"createdBy", t.created_by.empty() ? json(nullptr) : json(t.created_by)},
                    {"createdAt", t.created_at},
                    // 0 means never used — reported as null so a listing cannot
                    // render the epoch as a date and call it 1970.
                    {"lastUsedAt", t.last_used_at == 0 ? json(nullptr)
                                                       : json(t.last_used_at)},
                });
            }
            // No hash, and no secret: there is no secret here to leak. It exists
            // for exactly one response, below, and was never stored.
            return json_ok(arr);
        });

    CROW_ROUTE(app, "/api/tokens").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                const auto body = json::parse(req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object())
                    return json_err(400, "expected a JSON object");
                const std::string name = body.value("name",     std::string{});
                const std::string pass = body.value("password", std::string{});
                if (name.empty()) return json_err(400, "a token needs a name");

                const auto& ctx = impl_->app.get_context<AuthGuard>(req);
                if (!ctx.authenticated)
                    return json_err(409, core::UserStore::describe(
                                             core::UserStore::Result::AuthOff));
                if (pass.empty())
                    return json_err(400, "your password is required to issue a token");

                if (const int wait = login_block_remaining(req.remote_ip_address); wait > 0) {
                    auto r = json_err(429, "too many failed attempts — wait and try again");
                    r.add_header("Retry-After", std::to_string(wait));
                    return r;
                }
                // The caller's OWN password, taken from the session's name — an
                // administrator confirming who they are, not naming somebody.
                if (!users_.authenticate(ctx.principal.name, pass)) {
                    login_note_failure(req.remote_ip_address);
                    Logger::warn("API token refused for '{}' from {} (bad password)",
                                 ctx.principal.name, req.remote_ip_address);
                    return json_err(401, "incorrect password");
                }
                login_note_success(req.remote_ip_address);

                core::UserStore::ApiToken created;
                std::string secret;
                using R = core::UserStore::Result;
                const auto r = users_.create_api_token(name, ctx.principal.id,
                                                       &created, &secret);
                if (r != R::Ok) {
                    const int code = r == R::NameTaken ? 409
                                   : r == R::AuthOff   ? 409
                                   : r == R::IoError   ? 500 : 400;
                    return json_err(code, core::UserStore::describe(r));
                }
                Logger::warn("API token '{}' issued by '{}' from {}",
                             created.name, ctx.principal.name, req.remote_ip_address);
                return json_ok(json{
                    {"id",        created.id},
                    {"name",      created.name},
                    {"createdAt", created.created_at},
                    // The one and only time this string exists anywhere. The
                    // store kept a hash; if the client drops this, the token is
                    // unrecoverable and the answer is to revoke and issue again.
                    {"token",     secret},
                });
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/tokens/<string>").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req, const std::string& id){
            try {
                const auto body = json::parse(req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object())
                    return json_err(400, "expected a JSON object");
                if (!body.contains("name") || !body["name"].is_string())
                    return json_err(400, "name must be a string");
                using R = core::UserStore::Result;
                const auto r = users_.rename_api_token(id, body["name"].get<std::string>());
                if (r == R::NoSuchToken) return json_err(404, core::UserStore::describe(r));
                if (r == R::NameTaken)   return json_err(409, core::UserStore::describe(r));
                if (r != R::Ok)          return json_err(400, core::UserStore::describe(r));
                return json_ok(json{{"id", id}, {"name", body["name"].get<std::string>()}});
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/tokens/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const std::string& id){
            using R = core::UserStore::Result;
            const auto r = users_.revoke_api_token(id);
            if (r == R::NoSuchToken) return json_err(404, core::UserStore::describe(r));
            if (r != R::Ok)          return json_err(500, core::UserStore::describe(r));
            // Immediate and total: the record is gone, and verification is a
            // lookup, so the next request carrying that token is a 401. Nothing
            // has to time out and nothing is left to revoke later.
            return json_ok(json{{"ok", true}});
        });

    // ---- Devices ----
    CROW_ROUTE(app, "/api/devices").methods(crow::HTTPMethod::Get)
        ([this] {
            try {
                json arr = json::array();
                for (auto& d : engine_.enumerate_devices()) arr.push_back(device_info_to_json(d));
                // `bound` for a bus that names a device is decided against
                // the cached list (D26); whoever asks for the devices is
                // the right moment to bring it up to date.
                state_.refresh_device_cache();
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
            // remove_cue() returns void, so probe existence first and 404 if
            // the target cue is unknown (mirrors the item play/stop routes).
            if (!state_.find_cue(audio::CueId{id})) return json_err(404, "not found");
            state_.remove_cue(audio::CueId{id});
            return json_ok(json({{"ok", true}}));
        });

    // A cue that belongs to a project item plays and stops through the item,
    // exactly as the UI and the WS "play"/"stop" frames do: in-point, trim,
    // fades, ducking, routing, end behaviour. These two called the engine
    // directly, so a second play resumed wherever the last one stopped (past
    // the trim, or at EOF = silence) and nothing auto-advanced (#65). Only an
    // ad-hoc cue with no item is left to the engine.
    CROW_ROUTE(app, "/api/cues/<string>/play").methods(crow::HTTPMethod::Post)
        ([this](std::string id) {
            const audio::CueId cid{id};
            if (!engine_.find_cue(cid)) return json_err(404, "not found");
            if (auto uuid = state_.cue_to_item_uuid(cid)) {
                Logger::playback("PLAY: {}", item_playback_info(*uuid, state_));
                state_.play_item(*uuid);
            } else {
                engine_.play(cid);
            }
            return json_ok(json({{"ok", true}}));
        });
    CROW_ROUTE(app, "/api/cues/<string>/stop").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id) {
            const audio::CueId cid{id};
            if (!engine_.find_cue(cid)) return json_err(404, "not found");
            std::optional<long long> fade_ms;
            if (!req.body.empty()) {
                const auto j = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
                if (j.is_discarded()) return json_err(400, "body must be JSON");
                fade_ms = optional_fade_ms(j);
            }
            if (auto uuid = state_.cue_to_item_uuid(cid)) {
                Logger::playback("STOP: {}", item_playback_info(*uuid, state_));
                state_.stop_item(*uuid, fade_ms);
            } else if (fade_ms) {
                engine_.stop(cid, std::chrono::milliseconds{*fade_ms});
            } else {
                engine_.stop(cid);
            }
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

    // ---- External-control surface (Bitfocus Companion, custom remotes) ----
    // Compact machine-readable transport summary. Control surfaces fetch this
    // once on connect (and after a project_changed doc_patch), then keep it
    // fresh from the /ws push stream — no polling.
    CROW_ROUTE(app, "/api/state/summary").methods(crow::HTTPMethod::Get)
        ([this] {
            try {
                json s = state_.state_summary();
                s["server"] = json{
                    {"version", std::string{
#ifdef LIVEPLAY_SERVER_VERSION
                        LIVEPLAY_SERVER_VERSION
#else
                        "0.0.0"
#endif
                    }},
                    {"meterBroadcastHz", cfg_.meter_broadcast_hz},
                    // Master-bus geometry. The bus width is configurable at
                    // boot, so clients must read the preview pair from here
                    // rather than assuming the historical 30/31.
                    {"masterChannels", engine_.config().master_channels},
                    {"previewMasterL",
                     audio::preview_master_base(engine_.config().master_channels)},
                    {"previewMasterR",
                     audio::preview_master_base(engine_.config().master_channels) + 1},
                    {"maxUploadBytes", cfg_.max_upload_bytes},
                };
                return json_ok(s);
            } catch (const std::exception& e) { return json_err(500, e.what()); }
            catch (...) { return json_err(500, "internal error"); }
        });

    // GO — play whatever is armed as "Up Next" (user override first, else the
    // playing item's endBehavior target). GET is accepted as well as POST so
    // the URL can be fired from a browser or a plain `curl`.
    CROW_ROUTE(app, "/api/transport/go")
        .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
        ([this](const crow::request& req){
            Logger::api_request("Client ({}) -> Server ({}) : {} /api/transport/go",
                                req.remote_ip_address, impl_->server_addr,
                                crow::method_name(req.method));
            const std::string uuid = state_.go();
            if (uuid.empty()) {
                Logger::warn("GO — nothing armed or derivable to play");
                return json_err(404, "nothing armed or playing to GO to");
            }
            Logger::playback("GO: {}", item_playback_info(uuid, state_));
            return json_ok(json({{"ok", true}, {"uuid", uuid}}));
        });

    // ---- Shared operator UI state (selection / Show Mode / locale) --------
    // These back the control-surface equivalents of the client's arrow keys,
    // Show Mode switch and language picker. Every mutation is broadcast as a
    // doc_patch, so the on-screen playlist and a Companion button can never
    // disagree about what is selected.

    CROW_ROUTE(app, "/api/selection").methods(crow::HTTPMethod::Get)
        ([this]{
            const auto uuid = state_.selected_item_uuid();
            return json_ok(json({{"itemUuid", uuid}}));
        });

    // Body: { "itemUuid": "..." } to select (empty string clears), or
    //       { "delta": -1 | 1 }  to step through the flattened playlist.
    CROW_ROUTE(app, "/api/selection").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body.empty() ? std::string{"{}"} : req.body);
                std::string uuid;
                if (j.contains("delta") && j["delta"].is_number_integer()) {
                    const int delta = j["delta"].get<int>();
                    if (delta == 0) return json_err(400, "delta must be non-zero");
                    uuid = state_.step_selection(delta, selection_anchors(engine_, state_));
                    if (uuid.empty()) return json_err(404, "playlist is empty");
                } else if (j.contains("itemUuid") && j["itemUuid"].is_string()) {
                    uuid = j["itemUuid"].get<std::string>();
                    state_.set_selected_item(uuid);
                } else {
                    return json_err(400, "expected \"itemUuid\" or \"delta\"");
                }
                return json_ok(json({{"ok", true}, {"itemUuid", uuid}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // Arm the selected item as "Up Next" — the control-surface equivalent of
    // the client's "Set As Next" context action.
    CROW_ROUTE(app, "/api/transport/arm_selected")
        .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
        ([this]{
            const auto uuid = state_.selected_item_uuid();
            if (uuid.empty()) return json_err(404, "nothing is selected");
            state_.set_next_item_override(uuid);
            Logger::playback("ARM SELECTED: {}", item_playback_info(uuid, state_));
            return json_ok(json({{"ok", true}, {"itemUuid", uuid}}));
        });

    // Trigger the selected item (the client's Enter / "Play Selected" key).
    CROW_ROUTE(app, "/api/transport/play_selected")
        .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
        ([this]{
            const auto uuid = state_.selected_item_uuid();
            if (uuid.empty()) return json_err(404, "nothing is selected");
            Logger::playback("PLAY SELECTED: {}", item_playback_info(uuid, state_));
            if (!state_.trigger_item(uuid))
                return json_err(404, "item not loaded into engine");
            return json_ok(json({{"ok", true}, {"itemUuid", uuid}}));
        });

    // Pause / resume everything on air in one press — the control-surface
    // equivalent of the client's Pause/Resume key. Resumes if anything is
    // paused, otherwise pauses everything sounding; that way a single button
    // is never ambiguous about which way it will go.
    CROW_ROUTE(app, "/api/transport/pause_toggle")
        .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
        ([this]{
            const json summary = state_.state_summary();
            std::vector<std::string> paused, sounding;
            for (const auto& p : summary.value("playing", json::array())) {
                const auto uuid = p.value("itemUuid", std::string{});
                if (uuid.empty()) continue;
                if (p.value("paused", false)) paused.push_back(uuid);
                else                          sounding.push_back(uuid);
            }
            if (paused.empty() && sounding.empty())
                return json_err(404, "nothing is on air");
            const bool resuming = !paused.empty();
            for (const auto& uuid : resuming ? paused : sounding) {
                if (auto cue = state_.item_to_cue_id(uuid)) {
                    if (auto* pi = engine_.find_cue(*cue)) {
                        if (resuming) pi->resume(); else pi->pause();
                    }
                }
            }
            Logger::playback("PAUSE TOGGLE: {} {} item(s)",
                             resuming ? "resumed" : "paused",
                             resuming ? paused.size() : sounding.size());
            return json_ok(json({{"ok", true}, {"resumed", resuming}}));
        });

    CROW_ROUTE(app, "/api/ui/showmode").methods(crow::HTTPMethod::Get)
        ([this]{ return json_ok(json({{"enabled", state_.show_mode()}})); });

    // Body: { "enabled": bool }; omit the field (or send an empty body) to toggle.
    CROW_ROUTE(app, "/api/ui/showmode").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body.empty() ? std::string{"{}"} : req.body);
                bool enabled;
                if (j.contains("enabled") && j["enabled"].is_boolean()) {
                    enabled = j["enabled"].get<bool>();
                    state_.set_show_mode(enabled);
                } else {
                    enabled = state_.toggle_show_mode();
                }
                return json_ok(json({{"ok", true}, {"enabled", enabled}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // The installation's DEFAULT locale, not "the" locale (U2). REST is
    // stateless, so there is no connection here to attach a preference to;
    // what this addresses is the value a connection starts from.
    CROW_ROUTE(app, "/api/ui/locale").methods(crow::HTTPMethod::Get)
        ([this]{ return json_ok(json({{"locale", state_.default_ui_locale()}})); });

    CROW_ROUTE(app, "/api/ui/locale").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body.empty() ? std::string{"{}"} : req.body);
                if (!j.contains("locale") || !j["locale"].is_string())
                    return json_err(400, "expected \"locale\"");
                state_.set_default_ui_locale(j["locale"].get<std::string>());
                const auto now = state_.default_ui_locale();
                // Fanned out HERE rather than from ProjectState, and only to
                // the sessions that have expressed no preference — a client
                // that chose Greek for itself must not be dragged back by the
                // house changing its default. That selectivity is why
                // set_default_ui_locale() does not broadcast: it cannot see
                // the sessions, so it could only have gone all-or-nothing.
                const std::string frame =
                    json{{"type", "doc_patch"}, {"op", "locale_changed"},
                         {"locale", now}}.dump();
                {
                    std::lock_guard lock{impl_->ws_mutex};
                    for (auto& [c, s] : impl_->ws_clients) {
                        if (!impl_->authorize_ws_locked(c, users_)) continue;
                        if (!s.locale.empty()) continue;
                        try { c->send_text(frame); } catch (...) { /* onclose cleans up */ }
                    }
                }
                return json_ok(json({{"ok", true}, {"locale", now}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // First-class body-addressed variant of /api/project/items/by-index/…
    // Body: { "index": [1, 11] } — an index path descending into groups.
    CROW_ROUTE(app, "/api/transport/play_index").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                std::vector<int> path;
                if (j.contains("index") && j["index"].is_array()) {
                    for (const auto& v : j["index"]) {
                        if (!v.is_number_integer() || v.get<int>() < 0)
                            return json_err(400, "index must contain non-negative integers");
                        path.push_back(v.get<int>());
                    }
                }
                if (path.empty())
                    return json_err(400, "index must be a non-empty array of child indices");
                const std::string uuid = state_.item_uuid_by_index(path);
                if (uuid.empty()) return json_err(404, "no item at that index");
                Logger::playback("TRIGGER: {}", item_playback_info(uuid, state_));
                if (!state_.trigger_item(uuid))
                    return json_err(404, "item not loaded into engine");
                return json_ok(json({{"ok", true}, {"uuid", uuid}, {"index", path}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // Trigger the item bound to a cart slot. GET accepted for curl/browser.
    CROW_ROUTE(app, "/api/transport/cart/<int>/play")
        .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get)
        ([this](const crow::request& req, int slot){
            Logger::api_request("Client ({}) -> Server ({}) : {} /api/transport/cart/{}/play",
                                req.remote_ip_address, impl_->server_addr,
                                crow::method_name(req.method), slot);
            const std::string uuid = state_.cart_slot_item_uuid(slot);
            if (uuid.empty()) return json_err(404, "cart slot is empty");
            Logger::playback("CART {}: {}", slot, item_playback_info(uuid, state_));
            if (!state_.trigger_item(uuid))
                return json_err(404, "item not loaded into engine");
            return json_ok(json({{"ok", true}, {"slot", slot}, {"uuid", uuid}}));
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
                // "db" sets an absolute gain; "delta" nudges the current gain
                // (control-surface increment/decrement without a read-modify-
                // write race on the caller's side). "db" wins if both present.
                float db;
                if (j.contains("db") && j["db"].is_number())
                    db = j["db"].get<float>();
                else if (j.contains("delta") && j["delta"].is_number())
                    db = engine_.master_gain_db() + j["delta"].get<float>();
                else
                    // Neither: refuse, rather than read a typo'd body as
                    // "set the whole show to 0 dB".
                    return json_err(400, "body needs a numeric \"db\" or \"delta\"");
                engine_.set_master_gain_db(db);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "master_gain_changed"},
                    {"db", engine_.master_gain_db()},
                });
                return json_ok(json({{"ok", true}, {"db", engine_.master_gain_db()}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // Master brickwall limiter enable/bypass. POST body { "enabled": bool };
    // omitting "enabled" toggles the current state (single-button surfaces).
    CROW_ROUTE(app, "/api/master/limiter").methods(crow::HTTPMethod::Get)
        ([this]{ return json_ok(json({{"enabled", engine_.limiter_enabled()}})); });
    CROW_ROUTE(app, "/api/master/limiter").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body.empty() ? std::string{"{}"} : req.body);
                const bool enabled =
                    (j.contains("enabled") && j["enabled"].is_boolean())
                        ? j["enabled"].get<bool>()
                        : !engine_.limiter_enabled();
                engine_.set_limiter_enabled(enabled);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "limiter_changed"},
                    {"enabled", enabled},
                });
                return json_ok(json({{"ok", true}, {"enabled", enabled}}));
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

    // ---- Buses ----
    // The user-facing view of the mixer: every bus in display order, with the
    // items that resolve to it (own assignment, inherited from a group, or the
    // Main fallback).
    CROW_ROUTE(app, "/api/buses").methods(crow::HTTPMethod::Get)
        ([this] {
            try {
                json arr = json::array();
                for (const auto& b : state_.list_buses()) arr.push_back(bus_info_to_json(b));
                return json_ok(arr);
            } catch (const std::exception& e) { return json_err(500, e.what()); }
        });

    // Single-resource read: the same shape as one element of the list above.
    // Unknown id is a 404, same as every other /api/buses/<id> route.
    CROW_ROUTE(app, "/api/buses/<string>").methods(crow::HTTPMethod::Get)
        ([this](std::string id){
            try {
                for (const auto& b : state_.list_buses()) {
                    if (b.def.id == id) return json_ok(bus_info_to_json(b));
                }
                return json_err(404, "not found");
            } catch (const std::exception& e) { return json_err(500, e.what()); }
        });

    CROW_ROUTE(app, "/api/buses").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                using PR = core::ProjectState::PatchBusResult;
                PR   why     = PR::Ok;
                auto created = state_.create_bus(json::parse(req.body), &why);
                if (!created) {
                    // A refused output is the caller's mistake (409); anything
                    // else means the desk is full, which is not (507).
                    if (why != PR::Ok) return json_err(409, bus_output_refusal_text(why));
                    return json_err(507, "no mixer strip available");
                }
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "buses_patched"},
                    {"buses", state_.full_document().value("buses", json::array())},
                });
                return json_ok(json({{"id", created->id}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/buses/<string>").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req, std::string id){
            try {
                using PR = core::ProjectState::PatchBusResult;
                std::string materialised;
                const auto r = state_.patch_bus(id, json::parse(req.body), &materialised);
                if (r == PR::NotFound) return json_err(404, "not found");
                if (r != PR::Ok) return json_err(409, bus_output_refusal_text(r));
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "buses_patched"},
                    {"buses", state_.full_document().value("buses", json::array())},
                });
                // Targeting a present device adds a logical output of that name
                // to the map, so every other client's output-map view is now a
                // row short. Same broadcast PUT /api/outputs sends.
                if (!materialised.empty()) {
                    auto out = outputs_.to_json();
                    // Same shape PUT /api/outputs broadcasts, rewiredBuses
                    // included: nothing was re-wired, because the entry
                    // resolves to exactly what the identity fallback already
                    // gave this bus, and a client that keys off the field
                    // should see 0 rather than nothing.
                    broadcast_doc_patch(json{
                        {"type", "doc_patch"}, {"op", "outputs_changed"},
                        {"version",      out.value("version", 1)},
                        {"outputs",      out.value("outputs", json::array())},
                        {"rewiredBuses", 0},
                    });
                }
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // Live pan while the knob is being dragged: moves the send gains only.
    // No document write, no broadcast — the client PATCHes the settled value.
    // Same shape as the strip gain/mute endpoints, and for the same reason:
    // a PATCH per drag event would rewrite the document and bounce the knob
    // back to the stale value until the round-trip landed.
    // Client-internal (D16): this endpoint is a drag surface, not part of the
    // external-control API. External controllers use PATCH /api/buses/<id>.
    CROW_ROUTE(app, "/api/buses/<string>/pan").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id){
            try {
                auto j = json::parse(req.body);
                if (!state_.set_bus_pan_live(id, j.value("pan", 0.0f)))
                    return json_err(404, "not found");
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // Live tone controls while a filter knob is being dragged. Coefficients
    // straight at the strip: no document write, no broadcast, no re-wire.
    // Same shape and the same reason as the pan endpoint — a PATCH per drag
    // event would rewrite the document and bounce the knob back to the stale
    // value until the round trip landed.
    // Client-internal (D16): this endpoint is a drag surface, not part of the
    // external-control API. External controllers use PATCH /api/buses/<id>.
    CROW_ROUTE(app, "/api/buses/<string>/dsp").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id){
            try {
                if (!state_.set_bus_dsp_live(id, json::parse(req.body)))
                    return json_err(404, "not found");
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // PFL — pre-fade listen. A tap into the Monitor bus, taken before the
    // fader and before the mute, so a channel can be checked without the house
    // hearing anything change. No document write: PFL is what the operator is
    // listening to now, not part of the show. The broadcast is what keeps a
    // second mixer window's buttons in step.
    CROW_ROUTE(app, "/api/buses/pfl/clear").methods(crow::HTTPMethod::Post)
        ([this]{
            try {
                const auto cleared = state_.clear_all_pfl();
                if (cleared > 0) {
                    broadcast_doc_patch(json{
                        {"type", "doc_patch"}, {"op", "bus_pfl_cleared"},
                    });
                }
                return json_ok(json({{"cleared", cleared}}));
            } catch (const std::exception& e) { return json_err(500, e.what()); }
        });

    CROW_ROUTE(app, "/api/buses/<string>/pfl").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id){
            try {
                auto j = json::parse(req.body);
                const bool on = j.value("pfl", false);
                if (!state_.set_bus_pfl(id, on))
                    return json_err(404, "not found, or not a bus that can be PFL'd");
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "bus_pfl_changed"},
                    {"id", id}, {"pfl", on},
                });
                return json_ok(json({{"ok", true}, {"pfl", on}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // What the engine is measurably doing, for diagnosing latency and dropouts.
    // Measured rather than configured: the device gets a say in the period it
    // actually runs, and how long a block takes to render is the only thing
    // that says whether the queue depth is buying anything.
    //
    // `?reset=1` clears the render-time peak, so a probe can bound a
    // measurement to a window it controls.
    CROW_ROUTE(app, "/api/engine/stats")
        ([this](const crow::request& req){
            const bool reset = req.url_params.get("reset") != nullptr;
            const auto s = state_.engine().stats(reset);
            // Per-device drift telemetry (D15). `ppm` is how hard the drift
            // loop is bending a non-clock device to follow the house clock,
            // and `fillPercent` is the queue depth it is holding it at — the
            // loop's own smoothed measurement, which is the one the 50% target
            // refers to, so a figure parked anywhere else is a real symptom.
            // `ringFillPercent` is the unfiltered occupancy beside it: it
            // jitters by a whole device period, and its job is to show a queue
            // genuinely pinned at 0 or 100 before the smoothed one admits it.
            auto devices = json::array();
            for (const auto& d : s.device_stats) {
                devices.push_back(json{
                    {"name",            d.name},
                    {"isClock",         d.is_clock},
                    {"ppm",             d.ppm},
                    {"fillPercent",     d.fill_percent},
                    {"ringFillPercent", d.ring_fill_percent},
                });
            }
            return json_ok(json{
                {"queuedFrames",       s.queued_frames},
                {"queuedMs",           s.queued_ms},
                {"ringCapacityFrames", s.ring_capacity_frames},
                {"devicePeriodFrames", s.device_period_frames},
                {"devicePeriods",      s.device_periods},
                {"deviceMs",           s.device_ms},
                {"renderBlockUsMax",   s.render_block_us_max},
                {"renderBlockUsAvg",   s.render_block_us_avg},
                {"blockBudgetUs",      s.block_budget_us},
                {"blocksRendered",     s.blocks_rendered},
                {"underruns",          s.underruns},
                {"topologyRebuilds",   s.topology_rebuilds},
                {"mutexWaitUsMax",     s.mutex_wait_us_max},
                {"discontinuities",    s.discontinuities},
                {"worstSeam",          s.worst_seam},
                {"deviceCount",        s.devices},
                {"devices",            devices},
            });
        });

    // The mono-sum audition. Not per bus: it folds the MONITOR to mono, which
    // is one control for the whole monitoring path rather than one per strip —
    // PFL whichever buses you want to check, then press this.
    // Mono-check on the preview bus. /api/preview/mono is the name that
    // matches the role; /api/monitor/mono is kept for controllers written
    // against round 1 (D33). One handler, two routes.
    const auto preview_mono = [this](const crow::request& req){
        try {
            auto j = json::parse(req.body);
            const bool on = j.value("mono", false);
            if (!state_.set_monitor_mono(on))
                return json_err(409, "the Preview bus has no strip");
            broadcast_doc_patch(json{
                {"type", "doc_patch"}, {"op", "monitor_mono_changed"},
                {"mono", on},
            });
            return json_ok(json({{"ok", true}, {"mono", on}}));
        } catch (const std::exception& e) { return json_err(400, e.what()); }
    };
    CROW_ROUTE(app, "/api/monitor/mono").methods(crow::HTTPMethod::Post)(preview_mono);
    CROW_ROUTE(app, "/api/preview/mono").methods(crow::HTTPMethod::Post)(preview_mono);

    CROW_ROUTE(app, "/api/buses/<string>").methods(crow::HTTPMethod::Delete)
        ([this](std::string id){
            try {
                // Refused for a role holder (D24); assigned items fall back
                // to the master bus.
                std::string why;
                if (!state_.delete_bus(id, &why)) {
                    if (why == "not found") return json_err(404, "not found");
                    return json_err(409, why.empty() ? "not deletable" : why);
                }
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "buses_patched"},
                    {"buses", state_.full_document().value("buses", json::array())},
                });
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Logical outputs ----
    // Server-owned: what a project's output names mean on THIS machine. Never
    // part of a project document — that is what keeps a show portable.
    CROW_ROUTE(app, "/api/outputs").methods(crow::HTTPMethod::Get)
        ([this] {
            try {
                auto out = outputs_.to_json();
                // The names that mean something unmapped (D26), so a client
                // can list them ahead of the machine's own.
                out["builtin"] = core::OutputMap::builtin_names();
                return json_ok(out);
            }
            catch (const std::exception& e) { return json_err(500, e.what()); }
        });

    CROW_ROUTE(app, "/api/outputs").methods(crow::HTTPMethod::Put)
        ([this](const crow::request& req){
            try {
                if (!outputs_.from_json(json::parse(req.body)))
                    return json_err(400, "malformed output map");
                outputs_.save();
                // Buses are wired from the map at load time, so without this a
                // remapped output would appear to do nothing until the project
                // was reloaded. Only the buses this edit actually moved are
                // re-wired — the rest keep playing.
                const auto moved = state_.rewire_buses_for_output_map();
                auto out = outputs_.to_json();
                out["rewiredBuses"] = moved;
                // Same shape as GET /api/outputs plus rewiredBuses, so a second
                // connected client (detached mixer, Companion, curl) converges
                // on the new map without polling.
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "outputs_changed"},
                    {"version", out.value("version", 1)},
                    {"outputs", out.value("outputs", json::array())},
                    {"rewiredBuses", moved},
                });
                return json_ok(out);
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Mixer channels ----
    CROW_ROUTE(app, "/api/mixers").methods(crow::HTTPMethod::Get)
        ([this] {
            // Enumerate the engine's live strips, not ProjectState's legacy
            // mixers_ table. POST/DELETE here have always operated on the
            // engine, while this listed the table — which the client-format
            // load path clears and never repopulates, so it always read empty
            // and a strip created through this API could never be seen again.
            json arr = json::array();
            for (const auto& m : engine_.list_mixer_channels()) {
                arr.push_back(json{
                    {"id",           m.id.value},
                    {"display_name", m.display_name},
                    {"gain_db",      m.gain_db},
                    {"muted",        m.muted},
                    {"pfl",          m.pfl},
                });
            }
            return json_ok(arr);
        });

    CROW_ROUTE(app, "/api/mixers").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                const auto id = engine_.create_mixer_channel(j.value("name", "Channel"));
                if (id.empty())
                    return json_err(507, "mixer channel limit reached");
                return json_ok(json({{"id", id.value}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/mixers/<string>").methods(crow::HTTPMethod::Delete)
        ([this](std::string id){
            // remove_mixer_channel() returns void; probe first and 404 if absent.
            if (!engine_.find_mixer_channel(audio::MixerChannelId{id}))
                return json_err(404, "not found");
            engine_.remove_mixer_channel(audio::MixerChannelId{id});
            return json_ok(json({{"ok", true}}));
        });

    // Strip level / mute. GET /api/mixers has always reported these, but until
    // now nothing could set them over the API — only server-internal code could.
    CROW_ROUTE(app, "/api/mixers/<string>/gain").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id){
            try {
                auto* m = engine_.find_mixer_channel(audio::MixerChannelId{id});
                if (!m) return json_err(404, "not found");
                auto j = json::parse(req.body);
                m->set_gain_db(j.value("db", 0.0f));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/mixers/<string>/mute").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string id){
            try {
                auto* m = engine_.find_mixer_channel(audio::MixerChannelId{id});
                if (!m) return json_err(404, "not found");
                auto j = json::parse(req.body);
                m->set_mute(j.value("muted", false));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    // ---- Routing ----
    CROW_ROUTE(app, "/api/routing/item_to_mixer").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                // "lane": destination strip lane (0 = L, 1 = R). Omitted →
                // every lane (legacy mono-bus behaviour / mono sources).
                engine_.route_item_source_to_mixer(
                    audio::CueId{j.at("cue").get<std::string>()},
                    j.value("source_channel", (audio::ChannelIndex)0),
                    audio::MixerChannelId{j.at("mixer").get<std::string>()},
                    j.value("gain_db", 0.0f),
                    j.value("lane", audio::kAllMixerLanes));
                return json_ok(json({{"ok", true}}));
            } catch (const std::exception& e) { return json_err(400, e.what()); }
        });

    CROW_ROUTE(app, "/api/routing/mixer_to_master").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req){
            try {
                auto j = json::parse(req.body);
                // "lane": source strip lane feeding this master (0 = L,
                // 1 = R). Omitted → sum of every lane (mono downmix; legacy).
                engine_.route_mixer_to_master(
                    audio::MixerChannelId{j.at("mixer").get<std::string>()},
                    j.value("master_channel", (audio::MasterChannelIndex)0),
                    j.value("gain_db", 0.0f),
                    j.value("lane", audio::kAllMixerLanes));
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

                    // With an allow-list configured, the computer root IS the
                    // allow-list: offering Home and every drive letter would
                    // just be a list of places the next request gets a 403
                    // from. Unrestricted, this falls through to the original
                    // native-file-dialog behaviour below.
                    if (!g_fs_browse_roots.empty()) {
                        for (const auto& root : g_fs_browse_roots) {
                            const fs::path rp = liveplay::util::utf8_to_path(root);
                            std::error_code rec;
                            fs::path rc = fs::weakly_canonical(rp, rec);
                            if (rec) rc = rp.lexically_normal();
                            auto name = rc.filename().empty()
                                          ? liveplay::util::path_to_utf8(rc)
                                          : liveplay::util::path_to_utf8(rc.filename());
                            add_entry(name, liveplay::util::path_to_utf8(rc), "drive");
                        }
                        return json_ok(out);
                    }

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
                // Before the existence check, so a refusal cannot be used to
                // probe for what exists outside the roots.
                if (!path_within_fs_roots(p)) return json_fs_denied();
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
                if (!path_within_fs_roots(dir)) return json_fs_denied();
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
                // Checked before the existence probe, so a denied path cannot
                // be used to test whether a file exists outside the roots.
                if (!path_within_fs_roots(src)) return json_fs_denied();
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
                if (!path_within_fs_roots(liveplay::util::utf8_to_path(path_str)))
                    return json_fs_denied();

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
            const fs::path mp = liveplay::util::utf8_to_path(path);
            if (!path_within_fs_roots(mp)) return json_fs_denied();
            const auto md = liveplay::meta::read_metadata(mp);
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
                if (!path_within_fs_roots(file_path)) return json_fs_denied();

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
                    if (!path_within_fs_roots(p)) return json_fs_denied();
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
                auto repair    = state_.consume_repair_info();
                auto migration = state_.consume_bus_migration_summary();
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
                // The server re-routed this project on the operator's behalf.
                // Told to the client that asked, and to every other client so
                // they converge on the same story (D12, D17).
                if (migration.any()) {
                    header["migration"] = migration.to_json();
                    json patch = migration.to_json();
                    patch["type"] = "doc_patch";
                    patch["op"]   = "project_migrated";
                    broadcast_doc_patch(patch);
                }
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
                if (!path_within_fs_roots(src)) return json_fs_denied();
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
                    // Guard before create_directories, or a denied export would
                    // still leave a directory tree behind wherever it pointed.
                    if (!path_within_fs_roots(out)) return json_fs_denied();
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
            try {
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
                // Encode the filename via path_to_utf8 rather than the native
                // .string() (which decodes through the active code page and can
                // throw on non-representable Unicode names).
                r.add_header("Content-Disposition",
                             "attachment; filename=\""
                                 + liveplay::util::path_to_utf8(p.filename()) + "\"");
                r.add_header("Access-Control-Allow-Origin", g_cors_allow_origin);
                // The temp file has served its purpose; delete it to bound disk
                // usage on the server.
                std::error_code ec; fs::remove(p, ec);
                return r;
            } catch (const std::exception& e) {
                Logger::error("GET /api/file/download threw: {}", e.what());
                return json_err(400, e.what());
            }
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

                // Both ends of an import: the archive being read and the
                // directory it explodes into. The multipart form supplies the
                // archive from a temp dir we chose, but extractPath is always
                // caller-supplied.
                if (!path_within_fs_roots(extract_path)) return json_fs_denied();
                if (!archive_path.empty() && !path_within_fs_roots(archive_path)
                    && !delete_archive_after) {
                    return json_fs_denied();
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
                    // Guard before set_project_file_path: adopting the path
                    // would re-anchor media_root() outside the roots even if
                    // the write itself were refused.
                    if (!path_within_fs_roots(p)) return json_fs_denied();
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
                // A save carrying an embedded document goes through
                // replace_full_document(), so it can migrate too. Always
                // drained, so a migration reported here can't leak into the
                // response of some later, unrelated load.
                auto migration = state_.consume_bus_migration_summary();
                json body{{"ok", true}, {"path", path_str}};
                if (migration.any()) {
                    body["migration"] = migration.to_json();
                    json patch = migration.to_json();
                    patch["type"] = "doc_patch";
                    patch["op"]   = "project_migrated";
                    broadcast_doc_patch(patch);
                }
                return json_ok(body);
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
                auto migration = state_.consume_bus_migration_summary();
                auto header = state_.header_document();
                const std::size_t item_count = header.value("itemCount", (std::size_t)0);
                Logger::api_response("Client ({}) <- Server ({}) : PUT /api/project/document OK — '{}' ({} items)",
                                     req.remote_ip_address, impl_->server_addr,
                                     proj_name, item_count);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "project_changed"},
                });
                if (migration.any()) {
                    header["migration"] = migration.to_json();
                    json patch = migration.to_json();
                    patch["type"] = "doc_patch";
                    patch["op"]   = "project_migrated";
                    broadcast_doc_patch(patch);
                }
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
            // trigger_item, not play_item: a group uuid dispatches its start
            // behaviour, as the WS "play" frame and by-index already did —
            // play_item only knows audio items and answered 404 for a group.
            if (!state_.trigger_item(uuid)) {
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
            // Optional body {"fade_ms": n}: this stop only, 0 = cut (#56).
            std::optional<long long> fade_ms;
            if (!req.body.empty()) {
                const auto j = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
                if (j.is_discarded()) return json_err(400, "body must be JSON");
                fade_ms = optional_fade_ms(j);
            }
            Logger::playback("STOP: {}", item_playback_info(uuid, state_));
            if (!state_.stop_item(uuid, fade_ms)) {
                Logger::warn("STOP item_uuid={} — item not loaded into engine", uuid);
                return json_err(404, "item not loaded into engine");
            }
            Logger::api_response("Client ({}) <- Server ({}) : POST /api/project/items/{}/stop OK",
                                 req.remote_ip_address, impl_->server_addr, uuid);
            return json_ok(json({{"ok", true}}));
        });
    // Pause / resume hold the playhead without unloading. REST mirror of the
    // WS "pause"/"resume" messages so stateless control surfaces can use them.
    CROW_ROUTE(app, "/api/project/items/<string>/pause").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string uuid){
            Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/items/{}/pause",
                                req.remote_ip_address, impl_->server_addr, uuid);
            const auto cue = state_.item_to_cue_id(uuid);
            if (!cue) return json_err(404, "item not loaded into engine");
            if (auto* pi = engine_.find_cue(*cue)) {
                Logger::playback("PAUSE: {}", item_playback_info(uuid, state_));
                pi->pause();
                return json_ok(json({{"ok", true}}));
            }
            return json_err(404, "item not loaded into engine");
        });
    CROW_ROUTE(app, "/api/project/items/<string>/resume").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req, std::string uuid){
            Logger::api_request("Client ({}) -> Server ({}) : POST /api/project/items/{}/resume",
                                req.remote_ip_address, impl_->server_addr, uuid);
            const auto cue = state_.item_to_cue_id(uuid);
            if (!cue) return json_err(404, "item not loaded into engine");
            if (auto* pi = engine_.find_cue(*cue)) {
                Logger::playback("RESUME: {}", item_playback_info(uuid, state_));
                pi->resume();
                return json_ok(json({{"ok", true}}));
            }
            return json_err(404, "item not loaded into engine");
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
            // clear_cart_slot() returns false when the slot held no binding.
            if (!state_.clear_cart_slot(slot)) {
                Logger::warn("DELETE /api/project/cart/{} — no binding at slot", slot);
                return json_err(404, "no binding at slot");
            }
            Logger::api_response("Client ({}) <- Server ({}) : DELETE /api/project/cart/{} OK",
                                 req.remote_ip_address, impl_->server_addr, slot);
            broadcast_doc_patch(json{
                {"type", "doc_patch"}, {"op", "cart_slot_cleared"}, {"slot", slot},
            });
            return json_ok(json({{"ok", true}, {"slot", slot}}));
        });

    // ---- Preview (DJ-style pre-listening) ----
    // Auditions the item on the Monitor bus — the same strip PFL feeds, on the
    // master pair reserved at the top of the bus. Where that lands is the
    // Monitor bus's own output: the "Monitor" logical output if this machine
    // maps one, else settings.previewDevice.
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
                    return json_err(400, "preview could not start (item not found, or the Preview bus has no strip)");
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
            // stop_preview() announces preview_stopped itself (see the
            // broadcaster installed in start()), and only when there was one.
            state_.stop_preview();
            Logger::api_response("Client ({}) <- Server ({}) : DELETE /api/preview OK",
                                 req.remote_ip_address, impl_->server_addr);
            return json_ok(json({{"ok", true}}));
        });

    // ---- Settings patches ----
    // PATCH /api/project/theme is GONE (U4). It wrote document_["theme"],
    // which save() now drops on the way to disk — so keeping it would leave a
    // route that answers 200, broadcasts a change, and quietly loses the value
    // at the next save. A removed endpoint is a clear failure; a working-looking
    // one that discards its input is the kind nobody finds until a show.
    // Colours are a person's, and they are set through PATCH /api/prefs.
    CROW_ROUTE(app, "/api/project/settings").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req){
            try {
                auto patch = json::parse(req.body);
                Logger::api_request("Client ({}) -> Server ({}) : PATCH /api/project/settings",
                                    req.remote_ip_address, impl_->server_addr);
                std::vector<std::string> dropped;
                state_.patch_settings(patch, &dropped);
                auto settings = state_.full_document()["settings"];
                Logger::api_response("Client ({}) <- Server ({}) : PATCH /api/project/settings OK",
                                     req.remote_ip_address, impl_->server_addr);
                broadcast_doc_patch(json{
                    {"type", "doc_patch"}, {"op", "settings_patched"}, {"settings", settings},
                });
                // The response stays exactly the settings object it always was.
                // Keys the registry refused are reported in the log rather than
                // the body: the body's shape is the settings map itself, so
                // there is no free field to hang a report on without making
                // "droppedKeys" indistinguishable from a setting of that name.
                if (!dropped.empty()) {
                    Logger::warn("PATCH /api/project/settings from {}: {} key(s) not stored",
                                 req.remote_ip_address, dropped.size());
                }
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
      // The one place an upgrade can be refused, and the seam the user tier
      // is built on: it runs during the HTTP handshake, so it is the last
      // point at which the request — headers, origin, and the credential — is
      // still in hand. It enforces the origin policy and, since U3, the
      // authentication policy.
      //
      // It has to. Crow does run the AuthGuard middleware for an upgrade
      // request, but then calls handle_upgrade REGARDLESS of what the
      // middleware left in the response — so a socket cannot be refused there,
      // and everything the socket carries (play, stop, bus gain, mute,
      // selection) would have been reachable without a token while REST was
      // locked. That is the same shape of hole U1 found in the origin check,
      // one layer down.
      //
      // The three-argument form, not the bool one, purely so a refusal can say
      // 403 or 401 instead of Crow's bare 400: an operator reading a browser
      // console should be able to tell a rejected origin from a missing login
      // from a malformed request.
      .onaccept([this](const crow::request& req,
                       std::optional<crow::response>& res, void** userdata) {
          const std::string origin = req.get_header_value("Origin");
          if (!ws_origin_allowed(origin)) {
              // Named in the log, not in the response — the caller already knows
              // what it sent, and echoing it back only helps someone probing for
              // what this server will accept. Same rule as json_fs_denied().
              Logger::warn("WS upgrade refused from {} — origin '{}' is not '{}'",
                           req.remote_ip_address, origin, g_cors_allow_origin);
              res = crow::response{403};
              return;
          }

          if (!users_.auth_required()) return;   // the open posture

          // A browser cannot set headers on a WebSocket handshake — the API
          // simply has no room for them — so the token comes in the query
          // string here and nowhere else. Named access_token rather than
          // token so it cannot be confused with the one-shot capability the
          // export flow puts on /api/file/download.
          //
          // A token in a URL is normally a mistake because URLs get logged.
          // Crow's per-request log line is not reached on this path (the
          // upgrade branch returns before it), and this is a session token
          // rather than a password, but the trade is real and it is why REST
          // refuses to accept the same parameter.
          const char* qp = req.url_params.get("access_token");
          auto principal = qp ? users_.verify_token(qp)
                              : std::optional<core::UserStore::Principal>{};
          if (!principal) {
              Logger::warn("WS upgrade refused from {} — no valid credential",
                           req.remote_ip_address);
              res = crow::response{401};
              return;
          }
          // An API token may open a socket: play, stop, bus gain and selection
          // are the operator tier in full, which is exactly what this principal
          // is for. Nothing the socket carries touches the filesystem, so the
          // deny list that guards REST has nothing to say here — stated rather
          // than left to be noticed, because a new socket op that DID touch the
          // disk would need this re-examined.
          if (principal->is_api()) users_.note_api_token_use(principal->id);
          // Hand the principal to onopen, which has the connection but not the
          // request that authenticated it.
          //
          // Allocating here is safe ONLY because it happens after every
          // refusal above: Crow assigns userdata to the connection only when
          // this handler leaves the response empty (websocket.h sets it after
          // the `if (res) { ...; return; }`), so an allocation on a path that
          // then refuses would leak on every rejected handshake. Nothing is
          // allocated when authentication is off either — that path returns
          // early and leaves userdata null, which onopen reads as anonymous.
          *userdata = new Impl::WebSocketAuth{*principal, qp};
      })
      .onopen([this](crow::websocket::connection& conn) {
          // Taken and freed immediately: the session owns a copy, so the
          // allocation's lifetime is this function rather than the socket's,
          // and no cleanup depends on onclose firing.
          std::unique_ptr<Impl::WebSocketAuth> auth{
              static_cast<Impl::WebSocketAuth*>(conn.userdata())};
          conn.userdata(nullptr);
          const auto* principal = auth ? &auth->principal : nullptr;

          // Read before ws_mutex is taken: this can touch the disk, and the
          // broadcast loop wants that mutex sixty times a second.
          std::string meter_mode;
          if (principal) {
              if (const auto profile = prefs_.find(principal->id)) {
                  meter_mode = profile->value("meterMode", std::string{});
              }
          }

          {
              std::lock_guard lock{impl_->ws_mutex};
              auto& session = impl_->ws_clients[&conn];
              session.id           = impl_->next_client_id++;
              session.remote_ip    = conn.get_remote_ip();
              session.connected_at = std::chrono::steady_clock::now();
              session.meter_mode   = std::move(meter_mode);
              if (principal) {
                  session.credential = std::move(auth->credential);
                  session.user_id   = principal->id;
                  session.user_name = principal->name;
                  session.is_admin  = principal->is_admin();
                  session.is_api    = principal->is_api();
              }
          // Mark this client for a playback_snapshot push on the next
          // broadcast tick. The snapshot can't be sent inline here because
          // build_playback_snapshot takes both engine and project locks
          // (potentially seconds, e.g. mid project mirror) and Crow's
          // connection is not safe to write from two threads at once —
          // direct send_text here races the broadcast thread.
              session.wants_snapshot = true;
              Logger::info("WS client #{} connected from {} as {} ({} total)",
                           session.id, session.remote_ip,
                           session.user_name.empty() ? "anonymous" : session.user_name,
                           impl_->ws_clients.size());
          }
          // Outside the lock, and after the session exists: an operator whose
          // profile asks for dBTP needs the true-peak DSP running before their
          // first meter frame, not after they next touch a setting.
          refresh_user_meter_modes();
      })
      .onclose([this](crow::websocket::connection& conn, const std::string& reason, std::uint16_t /*code*/) {
          {
              std::lock_guard lock{impl_->ws_mutex};
              std::uint64_t id = 0;
              if (auto it = impl_->ws_clients.find(&conn); it != impl_->ws_clients.end()) {
                  id = it->second.id;
                  impl_->ws_clients.erase(it);
              }
              Logger::info("WS client #{} disconnected ({}); {} remaining",
                           id, reason, impl_->ws_clients.size());
          }
          // The other half of the union: the last operator wanting loudness
          // leaving is what lets the DSP stop again.
          refresh_user_meter_modes();
          // Likewise the last viewer of an analyser disarms its tap.
          refresh_analyser_taps();
      })
      .onmessage([this](crow::websocket::connection& conn,
                        const std::string& data,
                        bool is_binary) {
          if (is_binary) return;
          {
              std::lock_guard lock{impl_->ws_mutex};
              if (!impl_->authorize_ws_locked(&conn, users_)) return;
          }
          std::string direct_reply;
          try {
              SessionOps session;
              // Both take ws_mutex to touch the session, and neither touches
              // the project or engine locks — so they cannot re-enter the
              // ABBA ordering the broadcast loop's comment describes.
              session.set_locale = [this, &conn](const std::string& code) {
                  std::lock_guard lock{impl_->ws_mutex};
                  auto it = impl_->ws_clients.find(&conn);
                  if (it == impl_->ws_clients.end()) return code;
                  if (!code.empty()) it->second.locale = code;
                  return it->second.locale;
              };
              session.set_analyser = [this, &conn](const std::string& bus) {
                  {
                      std::lock_guard lock{impl_->ws_mutex};
                      auto it = impl_->ws_clients.find(&conn);
                      if (it == impl_->ws_clients.end()) return;
                      it->second.analyser_bus = bus;
                  }
                  refresh_analyser_taps();
              };
              session.set_meter_hz = [this, &conn](std::size_t hz) -> std::size_t {
                  // Clamped to what the server actually ticks at: the loop is
                  // the only consumer of the consuming meter reads, so a
                  // faster request cannot be honoured by anyone.
                  const std::size_t ceiling =
                      std::max<std::size_t>(1, cfg_.meter_broadcast_hz);
                  std::lock_guard lock{impl_->ws_mutex};
                  auto it = impl_->ws_clients.find(&conn);
                  if (it == impl_->ws_clients.end()) return hz;
                  it->second.meter_hz    = (hz == 0) ? 0 : std::min(hz, ceiling);
                  it->second.meter_accum = 0;
                  return it->second.meter_hz == 0 ? ceiling : it->second.meter_hz;
              };
              direct_reply = handle_ws_message(conn, data, engine_, state_, impl_->server_addr,
                                               [this](const json& p) { broadcast_doc_patch(p); },
                                               session);
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
