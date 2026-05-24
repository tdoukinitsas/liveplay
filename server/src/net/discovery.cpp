// ============================================================================
// discovery.cpp — see discovery.hpp.
// ============================================================================
#include "liveplay/net/discovery.hpp"
#include "liveplay/logger.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using socket_t = SOCKET;
#  define LP_INVALID_SOCKET INVALID_SOCKET
#  define LP_CLOSE_SOCKET ::closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define LP_INVALID_SOCKET (-1)
#  define LP_CLOSE_SOCKET ::close
#endif

using json = nlohmann::json;

namespace liveplay::net {

struct DiscoveryBeacon::Impl {
    socket_t sock = LP_INVALID_SOCKET;
};

namespace {

std::string hostname_or_default() {
    char buf[256] = {0};
#if defined(_WIN32)
    DWORD len = sizeof(buf);
    if (GetComputerNameA(buf, &len)) return std::string{buf, len};
#else
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return std::string{buf};
#endif
    return "liveplay";
}

std::string make_instance_id() {
    // Cheap per-run identifier so clients can de-dup beacons when the
    // server restarts (its port stays the same, the id rotates).
    std::random_device rd;
    std::mt19937_64 rng{rd()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream os;
    os << std::hex << dist(rng);
    return os.str();
}

#if defined(_WIN32)
struct WsaGuard {
    bool ok = false;
    WsaGuard() {
        WSADATA d{};
        ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    }
    ~WsaGuard() { if (ok) WSACleanup(); }
};
#endif

} // namespace

DiscoveryBeacon::DiscoveryBeacon(core::ProjectState& state, DiscoveryConfig cfg)
    : state_(state), cfg_(std::move(cfg)), impl_(std::make_unique<Impl>()) {
    if (cfg_.instance_id.empty()) cfg_.instance_id = make_instance_id();
}

DiscoveryBeacon::~DiscoveryBeacon() { stop(); }

bool DiscoveryBeacon::start() {
    if (running_.exchange(true)) return true;

#if defined(_WIN32)
    static WsaGuard g; // process-lifetime; safe to double-init guarded by WSAStartup refcount
    (void)g;
#endif

    impl_->sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->sock == LP_INVALID_SOCKET) {
        Logger::warn("DiscoveryBeacon: socket() failed; LAN discovery disabled.");
        running_.store(false);
        return false;
    }

    // Enable SO_BROADCAST so sendto() to 255.255.255.255 works.
    int yes = 1;
    if (::setsockopt(impl_->sock, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
        Logger::warn("DiscoveryBeacon: setsockopt(SO_BROADCAST) failed; LAN discovery disabled.");
        LP_CLOSE_SOCKET(impl_->sock);
        impl_->sock = LP_INVALID_SOCKET;
        running_.store(false);
        return false;
    }

    thread_ = std::thread([this] { run_loop(); });
    Logger::success("Discovery beacon broadcasting on UDP/{}", cfg_.beacon_port);
    return true;
}

void DiscoveryBeacon::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (impl_->sock != LP_INVALID_SOCKET) {
        LP_CLOSE_SOCKET(impl_->sock);
        impl_->sock = LP_INVALID_SOCKET;
    }
    Logger::info("Discovery beacon stopped.");
}

void DiscoveryBeacon::run_loop() {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(cfg_.beacon_port);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const std::string host = hostname_or_default();

    using clock = std::chrono::steady_clock;
    auto next_tick = clock::now();

    while (running_.load(std::memory_order_acquire)) {
        // Build the payload fresh each tick — project name and item count
        // change at runtime.
        json payload;
        try {
            auto header = state_.header_document();
            payload = {
                {"type",            "liveplay-beacon"},
                {"name",            host},
                {"version",         std::string{
#ifdef LIVEPLAY_SERVER_VERSION
                                       LIVEPLAY_SERVER_VERSION
#else
                                       "0.0.0"
#endif
                                   }},
                {"port",            cfg_.advertised_port},
                {"projectName",    header.value("name", "")},
                {"hasOpenProject", header.value("hasOpenProject", false)},
                {"itemCount",      header.value("itemCount", 0)},
                {"instanceId",     cfg_.instance_id},
            };
        } catch (const std::exception& e) {
            // Don't let a transient state-read failure kill the beacon.
            Logger::warn("DiscoveryBeacon: payload build failed: {}", e.what());
            payload = json{
                {"type",       "liveplay-beacon"},
                {"name",       host},
                {"port",       cfg_.advertised_port},
                {"instanceId", cfg_.instance_id},
            };
        }

        const std::string s = payload.dump();
        const int n = ::sendto(impl_->sock, s.data(),
                               static_cast<int>(s.size()), 0,
                               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (n < 0) {
            // Non-fatal — interface down, firewall, etc. Try again next tick.
            // Don't spam logs.
        }

        // Sleep in 200 ms slices so stop() doesn't have to wait a full
        // beacon interval to join.
        next_tick += cfg_.interval;
        while (running_.load(std::memory_order_acquire)) {
            auto now = clock::now();
            if (now >= next_tick) break;
            const auto remaining = next_tick - now;
            std::this_thread::sleep_for(
                std::min<std::chrono::steady_clock::duration>(
                    remaining, std::chrono::milliseconds(200)));
        }
    }
}

} // namespace liveplay::net
