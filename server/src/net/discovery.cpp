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
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
   using socket_t = SOCKET;
#  define LP_INVALID_SOCKET INVALID_SOCKET
#  define LP_CLOSE_SOCKET ::closesocket
#else
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
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

// One sendable target: an IPv4 address (network byte order) plus, for
// multicast, the local interface to egress from (also network byte order,
// 0 = default route).
struct SendTarget {
    std::uint32_t addr        = 0;  // destination (network order)
    std::uint32_t mcast_iface = 0;  // IP_MULTICAST_IF source (network order), 0 if N/A
    bool          is_multicast = false;
};

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

// Enumerate every up, non-loopback IPv4 interface and return, for each:
//   * its directed subnet broadcast address (ip | ~mask), and
//   * the interface's own address (for IP_MULTICAST_IF).
// `out_ifaces` receives the interface unicast addresses (network order).
std::vector<std::uint32_t> enumerate_interfaces(
    std::vector<std::uint32_t>& out_ifaces) {
    std::vector<std::uint32_t> broadcasts;
    out_ifaces.clear();

#if defined(_WIN32)
    ULONG buf_len = 15000;
    std::vector<char> buffer(buf_len);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    DWORD rv = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addrs, &buf_len);
    if (rv == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buf_len);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        rv = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &buf_len);
    }
    if (rv != NO_ERROR) return broadcasts;

    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            const std::uint32_t ip = sa->sin_addr.s_addr; // network order
            // OnLinkPrefixLength gives the subnet prefix (e.g. 24).
            const ULONG prefix = u->OnLinkPrefixLength;
            std::uint32_t mask_host = (prefix == 0)
                ? 0u
                : (prefix >= 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix)));
            const std::uint32_t mask = htonl(mask_host);
            const std::uint32_t bcast = ip | ~mask; // directed broadcast (net order)
            broadcasts.push_back(bcast);
            out_ifaces.push_back(ip);
        }
    }
#else
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || !ifap) return broadcasts;
    for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        auto* sa   = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        const std::uint32_t ip = sa->sin_addr.s_addr; // network order
        std::uint32_t mask = 0xFFFFFFFFu;
        if (ifa->ifa_netmask) {
            auto* nm = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
            mask = nm->sin_addr.s_addr; // network order
        }
        const std::uint32_t bcast = ip | ~mask; // directed broadcast (net order)
        broadcasts.push_back(bcast);
        out_ifaces.push_back(ip);
    }
    freeifaddrs(ifap);
#endif
    return broadcasts;
}

// Build the full list of send targets for one tick.
std::vector<SendTarget> build_send_targets(const std::string& mcast_group,
                                           bool enable_multicast) {
    std::vector<SendTarget> targets;

    std::vector<std::uint32_t> ifaces;
    const auto broadcasts = enumerate_interfaces(ifaces);

    // (1) per-interface directed broadcast.
    for (auto b : broadcasts) {
        targets.push_back(SendTarget{b, 0, false});
    }

    // (2) limited broadcast — belt and braces for single-homed hosts.
    targets.push_back(SendTarget{htonl(INADDR_BROADCAST), 0, false});

    // (3) multicast, once per interface so each NIC is covered regardless of
    //     the OS default multicast route.
    if (enable_multicast) {
        in_addr grp{};
        if (inet_pton(AF_INET, mcast_group.c_str(), &grp) == 1) {
            if (ifaces.empty()) {
                targets.push_back(SendTarget{grp.s_addr, 0, true});
            } else {
                for (auto ifc : ifaces) {
                    targets.push_back(SendTarget{grp.s_addr, ifc, true});
                }
            }
        }
    }
    return targets;
}

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

    // Enable SO_BROADCAST so sendto() to broadcast addresses works.
    int yes = 1;
    if (::setsockopt(impl_->sock, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
        Logger::warn("DiscoveryBeacon: setsockopt(SO_BROADCAST) failed; LAN discovery disabled.");
        LP_CLOSE_SOCKET(impl_->sock);
        impl_->sock = LP_INVALID_SOCKET;
        running_.store(false);
        return false;
    }

    // SO_REUSEADDR so a co-located client (same machine, same beacon port)
    // can also bind for listening without conflict.
    (void)::setsockopt(impl_->sock, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&yes), sizeof(yes));

    // Bind to the beacon port so we can RECEIVE client solicitations and
    // answer them with a unicast beacon. Binding to INADDR_ANY receives on
    // every interface. A bind failure (port busy) only disables the
    // solicitation-reply path; periodic broadcasting still works, so treat
    // it as non-fatal.
    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(cfg_.beacon_port);
    if (::bind(impl_->sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        Logger::warn("DiscoveryBeacon: bind(:{}) failed; solicitation replies "
                     "disabled (passive broadcast still active).", cfg_.beacon_port);
    } else if (cfg_.enable_multicast) {
        // Join the multicast group on every interface so multicast
        // solicitations are received regardless of which NIC they arrive on.
        std::vector<std::uint32_t> ifaces;
        (void)enumerate_interfaces(ifaces);
        in_addr grp{};
        if (inet_pton(AF_INET, cfg_.multicast_group.c_str(), &grp) == 1) {
            if (ifaces.empty()) ifaces.push_back(htonl(INADDR_ANY));
            for (auto ifc : ifaces) {
                ip_mreq mreq{};
                mreq.imr_multiaddr = grp;
                mreq.imr_interface.s_addr = ifc;
                (void)::setsockopt(impl_->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                   reinterpret_cast<const char*>(&mreq), sizeof(mreq));
            }
        }
    }

    thread_ = std::thread([this] { run_loop(); });
    Logger::success("Discovery beacon active on UDP/{} (broadcast + multicast + solicitation)",
                    cfg_.beacon_port);
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
    const std::string host = hostname_or_default();

    // Build the JSON beacon payload fresh — project name and item count
    // change at runtime.
    auto build_payload = [&]() -> std::string {
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
            Logger::warn("DiscoveryBeacon: payload build failed: {}", e.what());
            payload = json{
                {"type",       "liveplay-beacon"},
                {"name",       host},
                {"port",       cfg_.advertised_port},
                {"instanceId", cfg_.instance_id},
            };
        }
        return payload.dump();
    };

    // sendto helper that honours per-target multicast egress interface.
    auto send_one = [&](const SendTarget& t, const std::string& s,
                        std::uint16_t port) {
        if (t.is_multicast) {
            in_addr ifc{};
            ifc.s_addr = t.mcast_iface; // 0 == default
            (void)::setsockopt(impl_->sock, IPPROTO_IP, IP_MULTICAST_IF,
                               reinterpret_cast<const char*>(&ifc), sizeof(ifc));
        }
        sockaddr_in dest{};
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(port);
        dest.sin_addr.s_addr = t.addr;
        (void)::sendto(impl_->sock, s.data(), static_cast<int>(s.size()), 0,
                       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    };

    // Allow multicast to cross one router hop where IGMP snooping permits it.
    {
        int ttl = 1;
        (void)::setsockopt(impl_->sock, IPPROTO_IP, IP_MULTICAST_TTL,
                           reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    }

    using clock = std::chrono::steady_clock;
    auto next_tick    = clock::now();
    auto next_iface_refresh = clock::now();
    std::vector<SendTarget> targets =
        build_send_targets(cfg_.multicast_group, cfg_.enable_multicast);

    while (running_.load(std::memory_order_acquire)) {
        const auto now = clock::now();

        // Refresh the interface/broadcast list periodically so the beacon
        // follows the machine onto new networks (WiFi roam, VPN up/down).
        if (now >= next_iface_refresh) {
            targets = build_send_targets(cfg_.multicast_group, cfg_.enable_multicast);
            next_iface_refresh = now + std::chrono::seconds(15);
        }

        // ---- periodic announce across all delivery paths ------------------
        if (now >= next_tick) {
            const std::string s = build_payload();
            for (const auto& t : targets) send_one(t, s, cfg_.beacon_port);
            next_tick += cfg_.interval;
            if (next_tick <= now) next_tick = now + cfg_.interval; // catch up
        }

        // ---- wait for an incoming solicitation, with a timeout ------------
        // Block in select() until either a packet arrives or it's time for
        // the next announce, so solicitations get an immediate unicast reply.
        auto until = std::min(next_tick, next_iface_refresh);
        auto wait = until - clock::now();
        if (wait < std::chrono::milliseconds(0)) wait = std::chrono::milliseconds(0);
        // Cap the wait so stop() never blocks longer than 200 ms.
        auto wait_ms = std::min<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(wait).count(), 200);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(impl_->sock, &rfds);
        timeval tv{};
        tv.tv_sec  = static_cast<long>(wait_ms / 1000);
        tv.tv_usec = static_cast<long>((wait_ms % 1000) * 1000);

#if defined(_WIN32)
        const int nfds = 0; // ignored on Windows
#else
        const int nfds = static_cast<int>(impl_->sock) + 1;
#endif
        const int rc = ::select(nfds, &rfds, nullptr, nullptr, &tv);
        if (rc > 0 && FD_ISSET(impl_->sock, &rfds)) {
            char buf[1024];
            sockaddr_in from{};
#if defined(_WIN32)
            int fromlen = sizeof(from);
#else
            socklen_t fromlen = sizeof(from);
#endif
            const int n = ::recvfrom(impl_->sock, buf, sizeof(buf) - 1, 0,
                                     reinterpret_cast<sockaddr*>(&from), &fromlen);
            if (n > 0) {
                buf[n] = '\0';
                bool is_solicit = false;
                try {
                    auto j = json::parse(buf, buf + n, nullptr, false);
                    is_solicit = !j.is_discarded() &&
                                 j.value("type", "") == "liveplay-solicit";
                } catch (...) {}
                if (is_solicit) {
                    // Reply with a UNICAST beacon straight back to the asker.
                    const std::string s = build_payload();
                    (void)::sendto(impl_->sock, s.data(),
                                   static_cast<int>(s.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), fromlen);
                }
            }
        }
    }
}

} // namespace liveplay::net
