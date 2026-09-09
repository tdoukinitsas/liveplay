// ============================================================================
// main.cpp — LivePlay server entrypoint
// ----------------------------------------------------------------------------
// Milestone 1 deliverable: bring the binary up, print the startup banner, run
// the colour-coded logger through its paces, and idle on a clean shutdown
// signal. The audio engine (Milestone 2) and networking layer (Milestone 3)
// plug in at the marked extension points.
// ============================================================================
#include "liveplay/audio/engine.hpp"
#include "liveplay/core/backup_manager.hpp"
#include "liveplay/core/project_state.hpp"
#include "liveplay/core/user_prefs.hpp"
#include "liveplay/core/user_store.hpp"
#include "liveplay/crash_handler.hpp"
#include "liveplay/logger.hpp"
#include "liveplay/util/unicode_path.hpp"
#include "liveplay/net/control_server.hpp"
#include "liveplay/net/discovery.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #include <windows.h>
    #pragma comment(lib, "iphlpapi.lib")
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <ifaddrs.h>
    #include <net/if.h>        // IFF_UP, IFF_LOOPBACK (macOS/BSD don't expose
                               // these via <sys/socket.h> the way Linux does)
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>   // _NSGetExecutablePath
#  endif
#endif

#ifndef LIVEPLAY_SERVER_VERSION
#define LIVEPLAY_SERVER_VERSION "0.0.0"
#endif
#ifndef LIVEPLAY_SERVER_NAME
#define LIVEPLAY_SERVER_NAME "liveplay-server"
#endif

namespace {

namespace audio = liveplay::audio;
namespace net   = liveplay::net;
using liveplay::Logger;

// Default control-surface port. Overridable via --port or LIVEPLAY_PORT.
constexpr int kDefaultPort = 4480;

// Global "keep running" flag, flipped by the signal handler.
std::atomic<bool> g_running{true};

extern "C" void handle_signal(int sig) {
    // Stay async-signal-safe: only touch the atomic, defer the actual logging.
    (void)sig;
    g_running.store(false);
}

#if defined(_WIN32)
// Handle CTRL_CLOSE_EVENT (user closes the console window) and CTRL_BREAK_EVENT
// gracefully. Without this handler Windows hard-kills the process after 5 s with
// no crash log and no auto-restart — the same symptom the user sees as "server
// crashed with no logs". Must use WINAPI (__stdcall) calling convention.
static BOOL WINAPI console_ctrl_handler(DWORD type) {
    if (type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        g_running.store(false);
        // Sleep just under Windows' 5-second kill window so the main loop has
        // time to complete the clean-shutdown sequence before we return.
        std::this_thread::sleep_for(std::chrono::milliseconds(4500));
        return TRUE;
    }
    return FALSE; // pass CTRL_C through to the default SIGINT handler
}
#endif

void install_signal_handlers() {
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);
#if defined(SIGHUP)
    std::signal(SIGHUP,  handle_signal);
#endif
#if defined(SIGPIPE)
    // Ignore SIGPIPE so that writing to a closed socket returns EPIPE instead
    // of killing the process.  Without this, the broadcast thread can be
    // terminated silently (no crash handler, no restart) the moment a client
    // disconnects while a write is in-flight — particularly likely on loopback
    // where the TCP teardown and new-connection handshake arrive almost
    // simultaneously.
    std::signal(SIGPIPE, SIG_IGN);
#endif
#if defined(_WIN32)
    SetConsoleCtrlHandler(&console_ctrl_handler, TRUE);
#endif
}

// Best-effort discovery of a routable IPv4 address. Falls back to 127.0.0.1
// when nothing better is available (e.g. machines with only loopback).
std::string discover_local_ipv4() {
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return "127.0.0.1";

    ULONG buf_len = 15000;
    std::string buffer;
    buffer.resize(buf_len);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    DWORD rv = GetAdaptersAddresses(AF_INET,
                                    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, addrs, &buf_len);
    std::string result = "127.0.0.1";
    if (rv == NO_ERROR) {
        for (auto* a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                char buf[INET_ADDRSTRLEN] = {0};
                if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                    result = buf;
                    break;
                }
            }
            if (result != "127.0.0.1") break;
        }
    }
    WSACleanup();
    return result;
#else
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || !ifap) return "127.0.0.1";
    std::string result = "127.0.0.1";
    for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
            result = buf;
            break;
        }
    }
    freeifaddrs(ifap);
    return result;
#endif
}

const char* platform_tag() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

// Compose-and-print the startup banner. The logger lets us push raw ANSI lines
// straight through; we just build the strings here.
void print_banner(const std::string& bind_iface, int port) {
    using namespace liveplay;

    constexpr std::string_view C_RESET = "\033[0m";
    constexpr std::string_view C_BOLD  = "\033[1m";
    constexpr std::string_view C_DIM   = "\033[2m";
    constexpr std::string_view C_RED   = "\033[31m";
    constexpr std::string_view C_GREEN = "\033[32m";
    constexpr std::string_view C_YEL   = "\033[33m";

    // Compact helper: wrap a literal in an ANSI colour and reset.
    auto clr = [](std::string_view code, std::string_view text) {
        return std::string{code} + std::string{text} + "\033[0m";
    };

    // ASCII-art play-button-in-circle — same design language as the SVG icon.
    Logger::raw("");
    Logger::raw(std::string{C_RESET} + "  ▶" + std::string{C_RED} + "⬤" + std::string{C_RESET});
    Logger::raw("");

    Logger::raw(std::string{C_BOLD} + "  LivePlay Server " + std::string{C_RESET} +
                std::string{C_DIM} + "v" + LIVEPLAY_SERVER_VERSION + std::string{C_RESET} +
                std::string{C_DIM} + "  (" + platform_tag() + ")" + std::string{C_RESET});
    Logger::raw(std::string{C_DIM} +
                "  Open-source audio playback engine for live sound operators" +
                std::string{C_RESET});
    Logger::raw("");
    Logger::raw(std::string{C_DIM} + "  Repository  " + std::string{C_RESET} +
                "https://github.com/tdoukinitsas/liveplay");
    Logger::raw(std::string{C_DIM} + "  License     " + std::string{C_RESET} +
                "AGPL-3.0-only");
    Logger::raw(std::string{C_DIM} + "  Authors     " + std::string{C_RESET} +
                "Thomas Doukinitsas & LivePlay contributors");
    Logger::raw("");
    Logger::rule();
    Logger::raw("");

    // Connection instructions block.
    const std::string lan_ip = discover_local_ipv4();
    Logger::raw(std::string{C_GREEN} + std::string{C_BOLD} +
                "  Listening" + std::string{C_RESET});
    Logger::raw("    REST       " + std::string{C_BOLD} + "http://" +
                bind_iface + ":" + std::to_string(port) + std::string{C_RESET});
    Logger::raw("    WebSocket  " + std::string{C_BOLD} + "ws://" +
                bind_iface + ":" + std::to_string(port) + "/ws" + std::string{C_RESET});
    Logger::raw("    LAN reach  " + std::string{C_BOLD} + "http://" + lan_ip +
                ":" + std::to_string(port) + std::string{C_RESET});
    Logger::raw("");
    Logger::raw(std::string{C_RED} + std::string{C_BOLD} +
                "  Connect a LivePlay client" + std::string{C_RESET});
    Logger::raw(std::string{C_RESET} +
                "    1) Launch the LivePlay desktop client on this network." +
                std::string{C_RESET});
    Logger::raw(std::string{C_RESET} +
                "    2) On the welcome screen, select Remote and either use auto discovery or enter the Server Address:" +
                std::string{C_RED});
    Logger::raw("       " + std::string{C_BOLD} + lan_ip + ":" +
                std::to_string(port) + std::string{C_RESET});
    Logger::raw(std::string{C_RESET} +
                "    3) Close this window to stop the server." +
                std::string{C_RESET});
    Logger::raw("");
    Logger::rule();
    Logger::raw("");
}

struct CliOptions {
    int         port      = kDefaultPort;
    std::string bind_addr = "0.0.0.0";
    std::string pidfile;                   // optional; if set, write JSON {pid,port,startedAt}
    bool        verbose   = false;
    int         start_delay_ms = 0;        // wait before binding (crash-restart uses this)

    // Engine / control-server overrides. Deliberately optional: an unset value
    // means "keep whatever EngineConfig / ControlServerConfig defaults to", so
    // those defaults live in exactly one place and improving them doesn't
    // require touching the CLI.
    std::optional<audio::SampleRate>         mix_sample_rate;
    std::optional<audio::FrameCount>         render_block;
    std::optional<audio::FrameCount>         ring_blocks;
    std::optional<audio::MasterChannelIndex> master_channels;
    std::optional<std::uint32_t>             max_buses;
    std::optional<float>                     master_ceiling_db;
    std::optional<std::size_t>               meter_broadcast_hz;
    std::optional<std::size_t>               max_upload_bytes;

    // Server policy. Both keep the pre-2.5 behaviour when unset, so the knobs
    // appear without the posture changing on upgrade.
    std::vector<std::string>                 fs_browse_roots;   // empty = unrestricted
    std::optional<std::string>               cors_allow_origin; // unset = "*"
};

// Split a platform-delimited path list (';' on Windows, ':' elsewhere, to match
// PATH), dropping empty segments so a trailing delimiter is harmless.
static std::vector<std::string> split_path_list(const std::string& raw) {
#if defined(_WIN32)
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    std::vector<std::string> out;
    std::string cur;
    for (const char c : raw) {
        if (c == kSep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Numeric option parsing. Unlike a bare stoi, a value that is unparseable or
// out of range is reported and then discarded, so a typo leaves the default in
// place rather than silently misconfiguring the engine.
template <typename T>
std::optional<T> parse_ranged(std::string_view name, const char* raw,
                              long long lo, long long hi) {
    long long v = 0;
    try {
        v = std::stoll(std::string{raw});
    } catch (...) {
        Logger::warn("{}: '{}' is not a number - ignoring, using default", name, raw);
        return std::nullopt;
    }
    if (v < lo || v > hi) {
        Logger::warn("{}: {} is outside the supported range [{}, {}] - ignoring, "
                     "using default", name, v, lo, hi);
        return std::nullopt;
    }
    return static_cast<T>(v);
}

std::optional<float> parse_ranged_db(std::string_view name, const char* raw,
                                     double lo, double hi) {
    double v = 0.0;
    try {
        v = std::stod(std::string{raw});
    } catch (...) {
        Logger::warn("{}: '{}' is not a number - ignoring, using default", name, raw);
        return std::nullopt;
    }
    if (v < lo || v > hi) {
        Logger::warn("{}: {} dB is outside the supported range [{}, {}] - ignoring, "
                     "using default", name, v, lo, hi);
        return std::nullopt;
    }
    return static_cast<float>(v);
}

// Crash-resume state read from .crash-resume.json on startup.
struct CrashResume {
    std::string project_file;
    std::string item_uuid;
    double      position_sec = 0.0;
};

// ---------------------------------------------------------------------------
// liveplay.json — boot configuration
// ---------------------------------------------------------------------------
// A file beside the executable, next to outputs.json, holding what an
// installation wants every launch to have. The alternative is a shortcut with
// nine flags on it, and policy that has to survive a reinstall, a
// crash-restart, and whoever launches the server next does not belong in a
// shortcut.
//
// SPARSE on purpose: an absent key means "not set", NOT "the default". A file
// written today therefore keeps taking improved defaults rather than freezing
// this version's. It is also why the server never writes this file — nothing it
// wrote could tell an operator's deliberate choice apart from whatever default
// happened to be current the day the file was created.
//
// Precedence, lowest first:
//     built-in default  <  liveplay.json  <  environment  <  command line
// The file is where an installation states its posture, the environment is
// where a launcher varies it, and a flag is a person overriding both on
// purpose — so the flag wins.
constexpr int kConfigSchemaVersion = 1;

// Every key the file may carry, and the flag it stands in for. One list: the
// unknown-key check and `--help` both read it, so a key cannot be added to one
// and forgotten in the other.
struct ConfigKey { const char* name; const char* flag; };
constexpr ConfigKey kConfigKeys[] = {
    {"port",            "--port"},
    {"bind",            "--bind"},
    {"meterHz",         "--meter-hz"},
    {"maxUploadMb",     "--max-upload-mb"},
    {"mixSampleRate",   "--mix-sample-rate"},
    {"renderBlock",     "--render-block"},
    {"ringBlocks",      "--ring-blocks"},
    {"masterChannels",  "--master-channels"},
    {"maxBuses",        "--max-buses"},
    {"masterCeilingDb", "--master-ceiling-db"},
    {"fsRoots",         "--fs-root (an array; one entry per root)"},
    {"corsOrigin",      "--cors-origin"},
    {"verbose",         "--verbose"},
};

// Fold whatever `path` validly contains into `opts`.
//
// Every problem is reported and then survived: a malformed file, a wrong type,
// an out-of-range number and an unknown key each cost that one key and nothing
// else. A server that refuses to boot over a typo is worse than one that boots
// on a default and says so — this one is routinely started by a shortcut with
// nobody watching the console.
void apply_config_file(CliOptions& opts, const std::filesystem::path& path) {
    std::ifstream f{path};
    if (!f) {
        Logger::warn("liveplay.json: cannot open '{}' - continuing without it",
                     liveplay::util::path_to_utf8(path));
        return;
    }
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        Logger::error("liveplay.json: not a valid JSON object - the WHOLE file is "
                      "ignored, and every setting in it falls back to its default");
        return;
    }

    const int ver = j.value("schema_version", kConfigSchemaVersion);
    if (ver > kConfigSchemaVersion) {
        Logger::warn("liveplay.json declares schema_version {} and this server knows {}. "
                     "Reading what it recognises; anything newer is reported as unknown.",
                     ver, kConfigSchemaVersion);
    }

    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "schema_version") continue;
        bool known = false;
        for (const auto& k : kConfigKeys) {
            if (it.key() == k.name) { known = true; break; }
        }
        if (!known) {
            Logger::warn("liveplay.json: unknown key '{}' - ignored. Run with --help "
                         "for the keys this server reads.", it.key());
        }
    }

    // Same range checks the flags and the environment apply, so a value cannot
    // arrive out of range just because it came in by a different door.
    const auto num = [&](const char* key, long long lo, long long hi)
            -> std::optional<long long> {
        const auto it = j.find(key);
        if (it == j.end()) return std::nullopt;
        if (!it->is_number_integer() && !it->is_number_unsigned()) {
            Logger::warn("liveplay.json: '{}' must be a whole number - ignoring, "
                         "using default", key);
            return std::nullopt;
        }
        const long long v = it->get<long long>();
        if (v < lo || v > hi) {
            Logger::warn("liveplay.json: {} = {} is outside the supported range "
                         "[{}, {}] - ignoring, using default", key, v, lo, hi);
            return std::nullopt;
        }
        return v;
    };
    const auto real = [&](const char* key, double lo, double hi) -> std::optional<double> {
        const auto it = j.find(key);
        if (it == j.end()) return std::nullopt;
        if (!it->is_number()) {
            Logger::warn("liveplay.json: '{}' must be a number - ignoring, using default", key);
            return std::nullopt;
        }
        const double v = it->get<double>();
        if (v < lo || v > hi) {
            Logger::warn("liveplay.json: {} = {} is outside the supported range "
                         "[{}, {}] - ignoring, using default", key, v, lo, hi);
            return std::nullopt;
        }
        return v;
    };
    const auto text = [&](const char* key) -> std::optional<std::string> {
        const auto it = j.find(key);
        if (it == j.end()) return std::nullopt;
        if (!it->is_string() || it->get<std::string>().empty()) {
            Logger::warn("liveplay.json: '{}' must be a non-empty string - ignoring, "
                         "using default", key);
            return std::nullopt;
        }
        return it->get<std::string>();
    };

    if (auto v = num("port", 1, 65535))              opts.port = static_cast<int>(*v);
    if (auto v = text("bind"))                       opts.bind_addr = *v;
    if (auto v = num("meterHz", 1, 120))
        opts.meter_broadcast_hz = static_cast<std::size_t>(*v);
    if (auto v = num("maxUploadMb", 1, 8'192))
        opts.max_upload_bytes = static_cast<std::size_t>(*v) * 1024ull * 1024ull;
    if (auto v = num("mixSampleRate", 8'000, 192'000))
        opts.mix_sample_rate = static_cast<audio::SampleRate>(*v);
    if (auto v = num("renderBlock", 32, 8'192))
        opts.render_block = static_cast<audio::FrameCount>(*v);
    if (auto v = num("ringBlocks", 2, 512))
        opts.ring_blocks = static_cast<audio::FrameCount>(*v);
    if (auto v = num("masterChannels", audio::kMinMasterChannels, 1'024))
        opts.master_channels = static_cast<audio::MasterChannelIndex>(*v);
    if (auto v = num("maxBuses", 2, 512))
        opts.max_buses = static_cast<std::uint32_t>(*v);
    if (auto v = real("masterCeilingDb", -24.0, 0.0))
        opts.master_ceiling_db = static_cast<float>(*v);
    if (auto v = text("corsOrigin"))                 opts.cors_allow_origin = *v;

    // An array rather than a delimited string: this is a file, so there is no
    // shell to quote around and no reason to inherit PATH's separator problem.
    if (const auto it = j.find("fsRoots"); it != j.end()) {
        if (!it->is_array()) {
            Logger::warn("liveplay.json: 'fsRoots' must be an array of paths - "
                         "ignoring, the filesystem API stays unrestricted");
        } else {
            std::vector<std::string> roots;
            for (const auto& entry : *it) {
                if (entry.is_string() && !entry.get<std::string>().empty()) {
                    roots.push_back(entry.get<std::string>());
                } else {
                    Logger::warn("liveplay.json: 'fsRoots' entry is not a path string - "
                                 "ignoring that entry");
                }
            }
            // Only adopt a list that survived: an fsRoots of entirely bad
            // entries must not read as "confined to nothing", which would
            // refuse every path, nor silently as "unrestricted".
            if (!roots.empty()) opts.fs_browse_roots = std::move(roots);
            else Logger::warn("liveplay.json: 'fsRoots' named no usable path - "
                              "the filesystem API stays unrestricted");
        }
    }

    if (const auto it = j.find("verbose"); it != j.end()) {
        if (it->is_boolean()) opts.verbose = it->get<bool>();
        else Logger::warn("liveplay.json: 'verbose' must be true or false - ignoring");
    }
}

// Where to read boot configuration from, and whether a person named it.
//
// An explicitly named file that is missing is a mistake worth saying out loud;
// the default file being absent is the ordinary case and says nothing.
struct ConfigChoice {
    std::filesystem::path path;
    bool                  explicitly_named = false;
};

ConfigChoice choose_config_path(int argc, char** argv,
                                const std::filesystem::path& exe_dir) {
    // Scanned ahead of the main loop: the config file has to be read before the
    // flags that override it, so it cannot wait its turn in argv order.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--config" && *argv[i + 1]) {
            return {liveplay::util::utf8_to_path(argv[i + 1]), true};
        }
    }
    if (const char* v = std::getenv("LIVEPLAY_CONFIG")) {
        if (*v) return {liveplay::util::utf8_to_path(v), true};
    }
    return {exe_dir / "liveplay.json", false};
}

CliOptions parse_cli(int argc, char** argv, const std::filesystem::path& exe_dir) {
    CliOptions opts;

    // The file first: it is the lowest of the three, so everything below can
    // simply overwrite what it set.
    {
        const ConfigChoice cfg = choose_config_path(argc, argv, exe_dir);
        std::error_code ec;
        if (std::filesystem::exists(cfg.path, ec)) {
            Logger::info("Boot configuration: {}", liveplay::util::path_to_utf8(cfg.path));
            apply_config_file(opts, cfg.path);
        } else if (cfg.explicitly_named) {
            Logger::warn("Boot configuration '{}' does not exist - continuing with "
                         "defaults. A named config that isn't there is almost always "
                         "a typo, not an empty one.",
                         liveplay::util::path_to_utf8(cfg.path));
        }
    }

    // Environment next so that a CLI flag always wins, regardless of order.
    if (const char* v = std::getenv("LIVEPLAY_PORT")) {
        if (auto p = parse_ranged<int>("LIVEPLAY_PORT", v, 1, 65535)) opts.port = *p;
    }
    if (const char* v = std::getenv("LIVEPLAY_MIX_SAMPLE_RATE")) {
        opts.mix_sample_rate = parse_ranged<audio::SampleRate>(
            "LIVEPLAY_MIX_SAMPLE_RATE", v, 8'000, 192'000);
    }
    if (const char* v = std::getenv("LIVEPLAY_RENDER_BLOCK")) {
        opts.render_block = parse_ranged<audio::FrameCount>(
            "LIVEPLAY_RENDER_BLOCK", v, 32, 8'192);
    }
    if (const char* v = std::getenv("LIVEPLAY_RING_BLOCKS")) {
        opts.ring_blocks = parse_ranged<audio::FrameCount>(
            "LIVEPLAY_RING_BLOCKS", v, 2, 512);
    }
    if (const char* v = std::getenv("LIVEPLAY_MASTER_CHANNELS")) {
        opts.master_channels = parse_ranged<audio::MasterChannelIndex>(
            "LIVEPLAY_MASTER_CHANNELS", v, audio::kMinMasterChannels, 1'024);
    }
    if (const char* v = std::getenv("LIVEPLAY_MAX_BUSES")) {
        opts.max_buses = parse_ranged<std::uint32_t>("LIVEPLAY_MAX_BUSES", v, 2, 512);
    }
    if (const char* v = std::getenv("LIVEPLAY_MASTER_CEILING_DB")) {
        opts.master_ceiling_db = parse_ranged_db(
            "LIVEPLAY_MASTER_CEILING_DB", v, -24.0, 0.0);
    }
    if (const char* v = std::getenv("LIVEPLAY_METER_HZ")) {
        opts.meter_broadcast_hz = parse_ranged<std::size_t>(
            "LIVEPLAY_METER_HZ", v, 1, 120);
    }
    if (const char* v = std::getenv("LIVEPLAY_FS_ROOTS")) {
        opts.fs_browse_roots = split_path_list(v);
    }
    if (const char* v = std::getenv("LIVEPLAY_CORS_ORIGIN")) {
        if (*v) opts.cors_allow_origin = std::string{v};
    }
    if (const char* v = std::getenv("LIVEPLAY_MAX_UPLOAD_MB")) {
        if (auto mb = parse_ranged<std::size_t>("LIVEPLAY_MAX_UPLOAD_MB", v, 1, 8'192)) {
            opts.max_upload_bytes = *mb * 1024ull * 1024ull;
        }
    }

    // Whether the command line has supplied any --fs-root yet; see below.
    bool cli_roots_seen = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        // Returns the next argv entry, or nullptr if the flag was given without
        // a value (in which case the default is left untouched).
        const auto next_value = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            Logger::warn("{}: missing value - ignoring", a);
            return nullptr;
        };
        const auto next = [&](int& dst, long long lo, long long hi) {
            if (const char* v = next_value()) {
                if (auto p = parse_ranged<int>(a, v, lo, hi)) dst = *p;
            }
        };
        if (a == "--port" || a == "-p") {
            next(opts.port, 1, 65535);
        } else if (a == "--bind" || a == "-b") {
            if (const char* v = next_value()) opts.bind_addr = v;
        } else if (a == "--pidfile") {
            if (const char* v = next_value()) opts.pidfile = v;
        } else if (a == "--start-delay-ms") {
            next(opts.start_delay_ms, 0, 600'000);
        } else if (a == "--mix-sample-rate") {
            if (const char* v = next_value()) {
                opts.mix_sample_rate =
                    parse_ranged<audio::SampleRate>(a, v, 8'000, 192'000);
            }
        } else if (a == "--render-block") {
            if (const char* v = next_value()) {
                opts.render_block = parse_ranged<audio::FrameCount>(a, v, 32, 8'192);
            }
        } else if (a == "--ring-blocks") {
            // How far ahead of the device the engine runs, in render blocks.
            // This is the output latency; raise it only on a machine that
            // genuinely stutters.
            if (const char* v = next_value()) {
                opts.ring_blocks = parse_ranged<audio::FrameCount>(a, v, 2, 512);
            }
        } else if (a == "--master-channels") {
            if (const char* v = next_value()) {
                opts.master_channels = parse_ranged<audio::MasterChannelIndex>(
                    a, v, audio::kMinMasterChannels, 1'024);
            }
        } else if (a == "--max-buses") {
            if (const char* v = next_value()) {
                opts.max_buses = parse_ranged<std::uint32_t>(a, v, 2, 512);
            }
        } else if (a == "--master-ceiling-db") {
            if (const char* v = next_value()) {
                opts.master_ceiling_db = parse_ranged_db(a, v, -24.0, 0.0);
            }
        } else if (a == "--meter-hz") {
            if (const char* v = next_value()) {
                opts.meter_broadcast_hz = parse_ranged<std::size_t>(a, v, 1, 120);
            }
        } else if (a == "--max-upload-mb") {
            if (const char* v = next_value()) {
                if (auto mb = parse_ranged<std::size_t>(a, v, 1, 8'192)) {
                    opts.max_upload_bytes = *mb * 1024ull * 1024ull;
                }
            }
        } else if (a == "--config") {
            // Already read, ahead of everything else — see choose_config_path.
            // Consumed here only so its value isn't mistaken for a flag.
            next_value();
        } else if (a == "--fs-root") {
            // Repeatable, so a shell can pass paths containing the list
            // separator without quoting games. The FIRST one clears whatever
            // the file or the environment set: repeating a flag builds one
            // list, but a flag still overrides a lower tier rather than
            // extending it, or `--fs-root` could only ever widen an
            // installation's jail and never narrow it.
            if (const char* v = next_value()) {
                if (*v) {
                    if (!cli_roots_seen) { opts.fs_browse_roots.clear(); cli_roots_seen = true; }
                    opts.fs_browse_roots.emplace_back(v);
                }
            }
        } else if (a == "--cors-origin") {
            if (const char* v = next_value()) {
                if (*v) opts.cors_allow_origin = std::string{v};
            }
        } else if (a == "--verbose" || a == "-v") {
            opts.verbose = true;
        } else if (a == "--help" || a == "-h") {
            const audio::EngineConfig      eng_defaults;
            const net::ControlServerConfig srv_defaults;
            std::printf(
                "Usage: %s [options]\n"
                "\n"
                "Server:\n"
                "  -p, --port <port>     Port to listen on (default %d)\n"
                "  -b, --bind <addr>     Interface to bind (default 0.0.0.0)\n"
                "      --pidfile <path>  Write JSON {pid,port,startedAt} after binding\n"
                "      --start-delay-ms <n>  Wait <n> ms before binding (used by crash-restart)\n"
                "      --meter-hz <n>    WebSocket meter push rate, 1-120 (default %zu)\n"
                "      --max-upload-mb <n>  Max upload size in MiB, 1-8192 (default %zu)\n"
                "\n"
                "Engine (applied at boot; the engine cannot be re-initialised later):\n"
                "      --mix-sample-rate <hz>  Mix sample rate, 8000-192000 (default %u)\n"
                "      --render-block <frames> Render block size, 32-8192 (default %llu)\n"
                "      --ring-blocks <n>       Output latency in render blocks, 2-512.\n"
                "                        Raise it only on a machine that genuinely stutters.\n"
                "      --master-channels <n>   Master bus width, %u-1024 (default %u)\n"
                "      --max-buses <n>         Max simultaneous mixer strips, 2-512 (default %u)\n"
                "      --master-ceiling-db <db>  Limiter ceiling, -24.0-0.0 (default %.1f)\n"
                "\n"
                "Security (both default to the pre-2.5 behaviour, so an upgrade changes\n"
                "nothing until you set them):\n"
                "      --fs-root <path>  Confine the filesystem API to <path>. Repeatable.\n"
                "                        Unset, the API can reach the whole filesystem.\n"
                "      --cors-origin <origin>  Access-Control-Allow-Origin value\n"
                "                        (default \"*\", i.e. any origin may call this server)\n"
                "\n"
                "Diagnostics:\n"
                "  -v, --verbose         Enable debug-level logging\n"
                "  -h, --help            Show this help and exit\n"
                "      --config <path>   Read boot configuration from <path> instead of\n"
                "                        liveplay.json beside this executable\n"
                "\n"
                "Every option above can also be set via environment variable using the\n"
                "LIVEPLAY_ prefix (LIVEPLAY_PORT, LIVEPLAY_MIX_SAMPLE_RATE, ...), or in a\n"
                "liveplay.json beside this executable. Precedence, lowest first:\n"
                "\n"
                "    built-in default  <  liveplay.json  <  environment  <  command line\n"
                "\n"
                "Out-of-range values are reported and ignored rather than silently applied,\n"
                "whichever door they came in by. The config file is sparse: a key it does\n"
                "not mention is not set, so an installation keeps taking improved defaults\n"
                "instead of freezing this version's. The server never writes it.\n"
                "\n"
                "Config keys (schema_version %d), each the same value as its flag:\n",
                LIVEPLAY_SERVER_NAME, kDefaultPort,
                srv_defaults.meter_broadcast_hz,
                srv_defaults.max_upload_bytes / (1024ull * 1024ull),
                eng_defaults.mix_sample_rate,
                static_cast<unsigned long long>(eng_defaults.render_block),
                audio::kMinMasterChannels, eng_defaults.master_channels,
                eng_defaults.max_mixer_channels,
                static_cast<double>(eng_defaults.master_ceiling_db),
                kConfigSchemaVersion);
            // Printed from the same list the file is validated against, so a
            // key can never be readable and undocumented, or documented and
            // rejected.
            for (const auto& k : kConfigKeys)
                std::printf("      %-16s %s\n", k.name, k.flag);
            std::exit(0);
        }
    }
    return opts;
}

// ---------------------------------------------------------------------------
// Port conflict helpers
// ---------------------------------------------------------------------------

// Returns true if the given address:port is already bound by another process.
static bool is_port_in_use(const std::string& addr, uint16_t port) {
#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL opt = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    inet_pton(AF_INET, addr.c_str(), &sa.sin_addr);
    bool in_use = (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR);
    ::closesocket(s);
    return in_use;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    inet_pton(AF_INET, addr.c_str(), &sa.sin_addr);
    bool in_use = (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0);
    ::close(s);
    return in_use;
#endif
}

#if !defined(_WIN32)
// Returns the PID string of the process holding tcp:PORT, or "" if unknown.
static std::string find_pid_on_port(uint16_t port) {
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "lsof -ti tcp:%u 2>/dev/null", static_cast<unsigned>(port));
    FILE* fp = popen(cmd, "r");
    if (!fp) return {};
    char buf[64] = {};
    const bool got = (fgets(buf, sizeof(buf) - 1, fp) != nullptr);
    pclose(fp);
    if (!got) return {};
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return {};
    return s;
}

#  if defined(__APPLE__)
// Shows a GUI dialog via osascript when no terminal is available (e.g. standalone
// .app bundle with LSUIElement=true). Returns true if the user chose to kill the
// existing process.
static bool show_macos_port_conflict_dialog(int port, const std::string& pid) {
    char cmd[1024];
    // Use multiple -e flags to avoid embedded-newline quoting headaches.
    // PID is validated as all-digits before this call, so no injection risk.
    std::snprintf(cmd, sizeof(cmd),
        "osascript"
        " -e 'set msg to \"LivePlay Server: port %d is already in use (PID %s).\""
              " & return & return & \"Kill the existing server and start a new one?\"'"
        " -e 'tell application \"System Events\" to set r to button returned of"
              " (display dialog msg buttons {\"Quit\", \"Kill & Restart\"}"
              " default button \"Kill & Restart\")'"
        " -e 'r'"
        " 2>/dev/null",
        port, pid.c_str());
    FILE* fp = popen(cmd, "r");
    if (!fp) return false;
    char buf[64] = {};
    const bool got = (fgets(buf, sizeof(buf) - 1, fp) != nullptr);
    pclose(fp);
    if (!got) return false;
    std::string result(buf);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result == "Kill & Restart";
}
#  endif // __APPLE__
#endif // !_WIN32

} // namespace

int main(int argc, char** argv) {
    using namespace liveplay;
    namespace audio = liveplay::audio;
    namespace core  = liveplay::core;
    namespace net   = liveplay::net;

#if defined(_WIN32)
    SetConsoleTitle(L"LivePlay Server");
    // Set the taskbar icon to the embedded IDI_APPICON resource. Console
    // windows use conhost.exe's icon by default; we must push ours explicitly.
    {
        // Resource ordinal 1 matches IDI_APPICON from winresrc.h / server.rc
        HICON hIcon = static_cast<HICON>(
            LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(1),
                      IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
        if (hIcon) {
            HWND hwnd = GetConsoleWindow();
            if (hwnd) {
                SendMessage(hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hIcon));
                SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
            }
        }
    }
#endif

    Logger::init();

    // ------------------------------------------------------------------
    // Locate our own executable; used by the crash handler for auto-restart,
    // and by parse_cli to find liveplay.json — which is why this runs first.
    // ------------------------------------------------------------------
    std::filesystem::path exe_dir;
    std::string exe_path_str;
    {
        std::error_code ec;
        exe_dir = std::filesystem::current_path(ec);
#if defined(_WIN32)
        wchar_t exe_path_w[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe_path_w, MAX_PATH) > 0) {
            const std::filesystem::path p{exe_path_w};
            exe_dir      = p.parent_path();
            exe_path_str = p.string();
        }
#elif defined(__linux__)
        char buf[4096] = {};
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            exe_path_str = std::string{buf, static_cast<std::size_t>(n)};
            exe_dir      = std::filesystem::path{exe_path_str}.parent_path();
        }
#elif defined(__APPLE__)
        {
            uint32_t sz = 4096;
            std::string p(sz, '\0');
            if (_NSGetExecutablePath(p.data(), &sz) == 0) {
                p.resize(std::strlen(p.c_str()));
                exe_path_str = p;
                exe_dir      = std::filesystem::path{p}.parent_path();
            }
        }
#endif
        if (exe_path_str.empty() && argv && argv[0])
            exe_path_str = argv[0];
    }

    const CliOptions opts = parse_cli(argc, argv, exe_dir);
    if (opts.verbose) Logger::set_min_level(LogLevel::Debug);

    // Rebuild the original arg string (argv[1..]) so a restarted instance
    // inherits the same port / bind / verbose flags. Skip --pidfile (the new
    // instance writes its own) but keep everything else.
    std::string restart_args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if ((a == "--pidfile") && i + 1 < argc) { ++i; continue; }
        if (!restart_args.empty()) restart_args += ' ';
        restart_args += argv[i];
    }

    // Crash logs go to exe_dir/crash-logs by default; set_crash_project_dir
    // overrides this to <project_dir>/logs/ once a project is open.
    install_crash_handlers((exe_dir / "crash-logs").string());
    set_crash_exe_info(exe_path_str, restart_args);

    // Mirror the session to a persistent, size-rotated log file so operators
    // have history even when stdout isn't captured (server launched by the
    // Electron client). Best-effort; console logging is unaffected on failure.
    Logger::set_log_file((exe_dir / "logs" / "liveplay-server.log").string());

    // Crash-loop protection. Read the persisted consecutive-crash count left by
    // any crashing predecessor; after kMaxConsecutiveCrashes back-to-back
    // crashes the handler stops auto-restarting so a deterministic fault (e.g. a
    // bad crash-resume project) can't relaunch forever. The count is reset once
    // this instance has run healthily (see the heartbeat loop) or shuts cleanly.
    constexpr int kMaxConsecutiveCrashes = 5;
    constexpr int kCrashGuardHealthySec  = 30;
    int prior_crash_count = 0;
    {
        std::ifstream cf{(exe_dir / ".crash-count").string()};
        int v = 0;
        if (cf && (cf >> v) && v > 0) prior_crash_count = v;
    }
    set_crash_restart_guard((exe_dir / ".crash-count").string(),
                            prior_crash_count, kMaxConsecutiveCrashes);
    if (prior_crash_count > 0) {
        Logger::warn("Recovered from a crash ({} consecutive). Auto-restart disables after {}.",
                     prior_crash_count, kMaxConsecutiveCrashes);
    }
    // Keep only the most recent crash logs in the fallback dir (project /logs/
    // crash logs are pruned by the project itself).
    prune_crash_logs((exe_dir / "crash-logs").string(), 20);

    // ------------------------------------------------------------------
    // Check for a crash-resume file left by a previous crashed instance.
    // ------------------------------------------------------------------
    std::optional<CrashResume> crash_resume;
    {
        const std::filesystem::path resume_path = exe_dir / ".crash-resume.json";
        std::error_code ec;
        if (std::filesystem::exists(resume_path, ec)) {
            try {
                std::ifstream f{resume_path};
                if (f) {
                    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
                    if (!j.is_discarded()) {
                        CrashResume cr;
                        cr.project_file  = j.value("projectFile",  std::string{});
                        cr.item_uuid     = j.value("itemUuid",      std::string{});
                        cr.position_sec  = j.value("positionSec",   0.0);
                        if (!cr.project_file.empty()) {
                            crash_resume = std::move(cr);
                            Logger::warn("Crash-resume: reloading '{}' and resuming playback.",
                                         crash_resume->project_file);
                        }
                    }
                }
            } catch (...) {}
            std::filesystem::remove(resume_path, ec);  // consume it
        }
    }

    install_signal_handlers();

    // ------------------------------------------------------------------
    // Crash-restart hand-off delay. A crashing instance spawns us immediately
    // and then exits to release the listening port; we wait here so the port
    // is free by the time we try to bind it (avoids a restart-vs-dying-parent
    // race that would otherwise make us exit on "port in use").
    // ------------------------------------------------------------------
    if (opts.start_delay_ms > 0) {
        Logger::info("Start delay: waiting {} ms before binding (crash-restart).",
                     opts.start_delay_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.start_delay_ms));
    }

    // ------------------------------------------------------------------
    // Port conflict check — give a clear diagnostic before Crow tries to
    // bind and emits a misleading "0.0.0.0:0" error.
    // ------------------------------------------------------------------
    if (is_port_in_use(opts.bind_addr, static_cast<uint16_t>(opts.port))) {
        Logger::error("Port {} is already in use.", opts.port);
#if !defined(_WIN32)
        const std::string existing_pid = find_pid_on_port(static_cast<uint16_t>(opts.port));
        if (!existing_pid.empty()) {
            Logger::warn("Another process (PID {}) is already bound to port {}.",
                         existing_pid, opts.port);
            bool kill_existing = false;
            if (isatty(STDIN_FILENO)) {
                // Interactive terminal — prompt the user directly.
                std::fprintf(stderr,
                    "\n  Options:\n"
                    "    [k] Kill the existing process and start this one\n"
                    "    [q] Quit  (keep the existing server running)\n"
                    "\n  Choice [k/q]: ");
                std::fflush(stderr);
                char choice = '\0';
                kill_existing = (std::scanf(" %c", &choice) == 1 &&
                                 (choice == 'k' || choice == 'K'));
            }
#  if defined(__APPLE__)
            else {
                // No terminal (e.g. standalone .app bundle) — use a GUI dialog.
                kill_existing = show_macos_port_conflict_dialog(opts.port, existing_pid);
            }
#  endif
            if (kill_existing) {
                const int target = std::stoi(existing_pid);
                ::kill(target, SIGTERM);
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
                if (::kill(target, 0) == 0) ::kill(target, SIGKILL);
                Logger::info("Terminated PID {}. Starting new instance...", target);
            } else {
                Logger::info("Keeping existing server. Exiting.");
                return 0;
            }
        } else {
            Logger::error("Use --port <port> to choose a different port.");
            return 1;
        }
#else
        Logger::error("Use --port <port> to choose a different port, "
                      "or stop the existing server first.");
        return 1;
#endif
    }

    print_banner(opts.bind_addr, opts.port);

    Logger::info("Booting LivePlay server (build {} on {})",
                 LIVEPLAY_SERVER_VERSION, platform_tag());
    Logger::debug("CLI -> port={} bind={} verbose={}",
                  opts.port, opts.bind_addr, opts.verbose);

    // ------------------------------------------------------------------
    // Audio engine (Milestone 2)
    // ------------------------------------------------------------------
    audio::EngineConfig engine_cfg;
    if (opts.mix_sample_rate)   engine_cfg.mix_sample_rate   = *opts.mix_sample_rate;
    if (opts.render_block)      engine_cfg.render_block      = *opts.render_block;
    if (opts.ring_blocks)       engine_cfg.ring_blocks       = *opts.ring_blocks;
    if (opts.master_channels)   engine_cfg.master_channels   = *opts.master_channels;
    if (opts.max_buses)         engine_cfg.max_mixer_channels = *opts.max_buses;
    if (opts.master_ceiling_db) engine_cfg.master_ceiling_db = *opts.master_ceiling_db;

    auto engine = std::make_unique<audio::AudioEngine>(engine_cfg);
    if (!engine->start()) {
        Logger::error("Audio engine failed to start.");
        return 1;
    }
    Logger::success("Audio engine running ({} Hz mix, {} frame blocks, {} master ch).",
                    engine->config().mix_sample_rate,
                    engine->config().render_block,
                    engine->config().master_channels);

    // Enumerate devices for the operator's visibility (M3 will expose this
    // over REST so the client can pick).
    auto devices = engine->enumerate_devices();
    if (devices.empty()) {
        Logger::warn("No audio output devices detected.");
    } else {
        Logger::info("Detected {} playback device(s):", devices.size());
        for (auto& d : devices) {
            Logger::raw(std::string{"    "} +
                        (d.is_default ? "[default] " : "          ") +
                        d.display_name +
                        "  (" + std::to_string(d.channel_count) + " ch)");
        }
    }
    // ------------------------------------------------------------------
    // Project state + control plane (Milestone 3)
    // ------------------------------------------------------------------
    // Logical output name → hardware for this machine. Server-owned and kept
    // out of project files, so a show stays portable between venues.
    auto outputs = std::make_unique<core::OutputMap>();
    outputs->set_path(exe_dir / "outputs.json");
    outputs->load();   // absent file is fine — names resolve by identity

    // Who may talk to this server. Machine-owned, beside outputs.json for the
    // same reason: the accounts belong to the rig, not to the show on it.
    auto users = std::make_unique<core::UserStore>();
    users->set_path(exe_dir / "users.json");
    users->load();   // absent file is the open posture, stated below
    if (users->corrupt()) {
        // Refusing to start is the right answer and the uncomfortable one. The
        // alternative is booting with an unreadable store, which the rest of
        // the server can only interpret as "no accounts" — turning a corrupt
        // file into an unlocked door at exactly the moment nobody is watching.
        // An operator who wants the server open can delete the file and mean it.
        Logger::error("users.json exists but could not be read. Refusing to start: "
                      "continuing would silently serve WITHOUT authentication. "
                      "Repair or delete '{}' to proceed.",
                      util::path_to_utf8(users->path()));
        engine->stop();
        return 1;
    }

    // Where a signed-in operator's own preferences live: theme, meter unit,
    // scroll-to-playing, transport keymap. One file per user, created on first
    // sign-in and never before — an installation that leaves authentication off
    // has nobody to keep preferences for and grows no directory here. There is
    // deliberately no load() to match: profiles fault in per user, so one bad
    // file costs one person's colours rather than the server's ability to boot.
    auto prefs = std::make_unique<core::UserPrefs>();
    prefs->set_dir(exe_dir / "prefs");

    auto project = std::make_unique<core::ProjectState>(*engine, *outputs);
    auto backup  = std::make_unique<core::BackupManager>(*project);
    backup->start();

    net::ControlServerConfig server_cfg;
    server_cfg.bind_address = opts.bind_addr;
    server_cfg.port         = static_cast<std::uint16_t>(opts.port);
    if (opts.meter_broadcast_hz) server_cfg.meter_broadcast_hz = *opts.meter_broadcast_hz;
    if (opts.max_upload_bytes)   server_cfg.max_upload_bytes   = *opts.max_upload_bytes;
    server_cfg.fs_browse_roots = opts.fs_browse_roots;
    if (opts.cors_allow_origin)  server_cfg.cors_allow_origin  = *opts.cors_allow_origin;

    // State the posture at boot rather than leaving it implicit. Both defaults
    // are the permissive pre-2.5 behaviour, which is right for compatibility
    // and wrong for a server reachable off the box — so say so once, here,
    // where an integrator reading the log will find it.
    if (server_cfg.fs_browse_roots.empty()) {
        Logger::warn("Filesystem API is UNRESTRICTED — any client that can reach this "
                     "server can browse and read the whole filesystem. Set --fs-root "
                     "(repeatable) or LIVEPLAY_FS_ROOTS to confine it.");
    } else {
        for (const auto& r : server_cfg.fs_browse_roots)
            Logger::info("Filesystem API confined to '{}'", r);
    }
    if (server_cfg.cors_allow_origin == "*" && opts.bind_addr != "127.0.0.1") {
        Logger::warn("CORS allows any origin (*) while bound to {} — a page on any "
                     "site the operator visits can call this server. Set --cors-origin "
                     "to pin it.", opts.bind_addr);
    }
    if (!users->auth_required()) {
        Logger::warn("NO ACCOUNTS configured — this server is UNAUTHENTICATED. Anyone "
                     "who can reach {}:{} can run the show. Create the first account "
                     "(POST /api/users, or the client's setup screen) to require a "
                     "login; it is stored in '{}'.",
                     opts.bind_addr, opts.port, util::path_to_utf8(users->path()));
    } else {
        Logger::info("Authentication is ON — {} account(s) in '{}'",
                     users->user_count(), util::path_to_utf8(users->path()));
        if (!users->has_admin()) {
            // The store forces the first account to admin, so reaching this
            // means the file was hand-edited. Say so rather than letting the
            // Server-tier routes quietly become unreachable to everyone.
            Logger::warn("No administrator in the user store — the output map, the "
                         "accounts and the client list cannot be changed by anyone. "
                         "Promote a user by editing '{}'.",
                         util::path_to_utf8(users->path()));
        }
    }

    auto server = std::make_unique<net::ControlServer>(*engine, *project, *outputs,
                                                       *users, *prefs, server_cfg);
    if (!server->start()) {
        Logger::error("Control server failed to start.");
        engine->stop();
        return 1;
    }

    // Write the pidfile so the Electron client can find the real server PID
    // when launched indirectly (e.g. via `cmd /c start`, which gives back the
    // PID of `cmd.exe`, not us). Failures here are non-fatal — the server
    // runs fine without a pidfile, the client just can't adopt it on restart.
    if (!opts.pidfile.empty()) {
        try {
            using clock = std::chrono::system_clock;
            const auto now = clock::now();
            const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                                now.time_since_epoch()).count();
            nlohmann::json j{
                {"pid",       static_cast<long long>(
#if defined(_WIN32)
                    GetCurrentProcessId()
#else
                    ::getpid()
#endif
                )},
                {"port",      opts.port},
                {"startedAt", static_cast<long long>(ts)},
            };
            std::ofstream f{opts.pidfile, std::ios::binary | std::ios::trunc};
            if (f) f << j.dump(2);
        } catch (const std::exception& ex) {
            Logger::warn("Failed to write pidfile '{}': {}", opts.pidfile, ex.what());
        }
    }

    // ------------------------------------------------------------------
    // LAN auto-discovery beacon — best-effort, non-fatal if it can't bind.
    // ------------------------------------------------------------------
    net::DiscoveryConfig disc_cfg;
    disc_cfg.advertised_port = static_cast<std::uint16_t>(opts.port);
    auto beacon = std::make_unique<net::DiscoveryBeacon>(*project, disc_cfg);
    if (!beacon->start()) {
        Logger::warn("LAN discovery beacon disabled (clients must connect by IP).");
    }

    // ------------------------------------------------------------------
    // Crash-resume: if a previous instance crashed with an open project,
    // reload it now. We wait for audio loading to finish before playing.
    // ------------------------------------------------------------------
    struct PendingResume {
        std::string item_uuid;
        double      position_sec = 0.0;
        std::chrono::steady_clock::time_point retry_after;
        int         attempts = 0;
    };
    std::optional<PendingResume> pending_resume;

    if (crash_resume) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto resume_fspath = util::utf8_to_path(crash_resume->project_file);
        if (fs::exists(resume_fspath, ec)) {
            Logger::info("Crash-resume: loading project '{}'", crash_resume->project_file);
            if (project->load(resume_fspath)) {
                // Update project dir for crash handler and logs.
                set_crash_project_dir(
                    util::path_to_utf8(resume_fspath.parent_path()));
                if (!crash_resume->item_uuid.empty()) {
                    pending_resume = PendingResume{
                        crash_resume->item_uuid,
                        crash_resume->position_sec,
                        std::chrono::steady_clock::now() + std::chrono::seconds{3},
                        0
                    };
                    Logger::info("Crash-resume: will play item '{}' at {:.1f}s once audio loads.",
                                 crash_resume->item_uuid, crash_resume->position_sec);
                }
            } else {
                Logger::warn("Crash-resume: failed to load '{}'", crash_resume->project_file);
            }
        } else {
            Logger::warn("Crash-resume: project file no longer exists: '{}'",
                         crash_resume->project_file);
        }
    }

    // Heartbeat loop. Every 30 s we tick a debug line so operators can confirm
    // the process is alive over long sessions. SIGINT/SIGTERM flips g_running.
    using clock = std::chrono::steady_clock;
    const auto program_start = clock::now();
    bool crash_guard_reset   = false;
    auto last_heartbeat   = clock::now();
    auto last_resume_snap = clock::now();
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = clock::now();

        // Once we've been up long enough to be considered "healthy", clear the
        // consecutive-crash counter so isolated crashes over a long session
        // don't accumulate toward the auto-restart give-up threshold.
        if (!crash_guard_reset &&
            std::chrono::duration_cast<std::chrono::seconds>(now - program_start).count()
                >= kCrashGuardHealthySec) {
            reset_crash_restart_guard();
            crash_guard_reset = true;
        }

        // ---- crash-resume: play item once audio has fully loaded ------------
        if (pending_resume) {
            const bool audio_ready = !project->audio_loading();
            if (audio_ready && now >= pending_resume->retry_after) {
                Logger::info("Crash-resume: playing item '{}'", pending_resume->item_uuid);
                if (project->play_item(pending_resume->item_uuid)) {
                    // Seek to the saved position (best-effort — item may be
                    // shorter than the saved position if the file changed).
                    if (auto cue_id = project->item_to_cue_id(pending_resume->item_uuid)) {
                        if (auto* pi = engine->find_cue(*cue_id)) {
                            if (pending_resume->position_sec > 1.0)
                                pi->seek_seconds(pending_resume->position_sec);
                        }
                    }
                }
                pending_resume.reset();
            } else if (!audio_ready) {
                ++pending_resume->attempts;
                if (pending_resume->attempts == 1)
                    Logger::debug("Crash-resume: waiting for audio load...");
                // Give up after 2 minutes (600 × 200 ms ticks) to avoid
                // hanging forever if audio loading stalls.
                if (pending_resume->attempts > 600) {
                    Logger::warn("Crash-resume: audio load timed out, giving up.");
                    pending_resume.reset();
                }
            }
        }

        // ---- update crash-handler resume state every ~2 s ------------------
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_resume_snap).count() >= 2000) {
            const auto snap = project->current_playback_snapshot();
            if (!snap.project_file.empty()) {
                namespace fs = std::filesystem;
                set_crash_project_dir(
                    fs::path{snap.project_file}.parent_path().string());
                update_crash_resume_state(snap.project_file,
                                          snap.item_uuid,
                                          snap.position_sec);
            }
            last_resume_snap = now;
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_heartbeat).count() >= 30) {
            Logger::debug("heartbeat — server running");
            last_heartbeat = now;
        }
    }

    Logger::raw("");
    Logger::info("Shutdown signal received — stopping cleanly.");
    // A clean exit is not a crash — clear the consecutive-crash counter so the
    // next launch starts fresh.
    reset_crash_restart_guard();
    if (beacon) beacon->stop();
    beacon.reset();
    server->stop();
    server.reset();
    backup->stop();
    backup.reset();
    project.reset();
    engine->stop();
    engine.reset();
    if (!opts.pidfile.empty()) {
        std::error_code ec;
        std::filesystem::remove(opts.pidfile, ec);
    }
    Logger::success("Bye.");
    return 0;
}
