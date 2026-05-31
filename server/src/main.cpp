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
};

// Crash-resume state read from .crash-resume.json on startup.
struct CrashResume {
    std::string project_file;
    std::string item_uuid;
    double      position_sec = 0.0;
};

CliOptions parse_cli(int argc, char** argv) {
    CliOptions opts;
    if (const char* env_port = std::getenv("LIVEPLAY_PORT")) {
        try { opts.port = std::stoi(env_port); } catch (...) {}
    }
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        const auto next = [&](int& dst) {
            if (i + 1 < argc) { try { dst = std::stoi(argv[++i]); } catch (...) {} }
        };
        if (a == "--port" || a == "-p") {
            next(opts.port);
        } else if (a == "--bind" || a == "-b") {
            if (i + 1 < argc) opts.bind_addr = argv[++i];
        } else if (a == "--pidfile") {
            if (i + 1 < argc) opts.pidfile = argv[++i];
        } else if (a == "--start-delay-ms") {
            next(opts.start_delay_ms);
        } else if (a == "--verbose" || a == "-v") {
            opts.verbose = true;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "Usage: %s [options]\n"
                "  -p, --port <port>     Port to listen on (default %d)\n"
                "  -b, --bind <addr>     Interface to bind (default 0.0.0.0)\n"
                "      --pidfile <path>  Write JSON {pid,port,startedAt} after binding\n"
                "      --start-delay-ms <n>  Wait <n> ms before binding (used by crash-restart)\n"
                "  -v, --verbose         Enable debug-level logging\n"
                "  -h, --help            Show this help and exit\n",
                LIVEPLAY_SERVER_NAME, kDefaultPort);
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
    const CliOptions opts = parse_cli(argc, argv);
    if (opts.verbose) Logger::set_min_level(LogLevel::Debug);

    // ------------------------------------------------------------------
    // Locate our own executable; used by crash handler for auto-restart.
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
    auto engine = std::make_unique<audio::AudioEngine>();
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
    auto project = std::make_unique<core::ProjectState>(*engine);
    auto backup  = std::make_unique<core::BackupManager>(*project);
    backup->start();

    net::ControlServerConfig server_cfg;
    server_cfg.bind_address = opts.bind_addr;
    server_cfg.port         = static_cast<std::uint16_t>(opts.port);
    auto server = std::make_unique<net::ControlServer>(*engine, *project, server_cfg);
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
    auto last_heartbeat   = clock::now();
    auto last_resume_snap = clock::now();
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = clock::now();

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
