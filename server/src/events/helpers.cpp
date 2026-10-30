// =============================================================================
// events/helpers.cpp — local interfaces and value ramps (see helpers.hpp)
// =============================================================================
#include "liveplay/events/helpers.hpp"
#include "liveplay/logger.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
#else
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#include <algorithm>
#include <cstdio>

namespace liveplay::events {
namespace {

std::string dotted(std::uint32_t host_order) {
    return std::to_string((host_order >> 24) & 0xff) + "." +
           std::to_string((host_order >> 16) & 0xff) + "." +
           std::to_string((host_order >> 8) & 0xff) + "." +
           std::to_string(host_order & 0xff);
}

void add(std::vector<LocalInterface>& out, std::uint32_t ip_host, std::uint32_t mask_host) {
    LocalInterface li;
    li.address_host = ip_host;
    li.netmask_host = mask_host;
    li.address      = dotted(ip_host);
    li.broadcast    = dotted(ip_host | ~mask_host);
    out.push_back(std::move(li));
}

bool parse_ipv4(const std::string& s, std::uint32_t& host_order) {
    unsigned a, b, c, d;
    char extra;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    host_order = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

} // namespace

std::vector<LocalInterface> local_ipv4_interfaces() {
    std::vector<LocalInterface> out;
#if defined(_WIN32)
    ULONG len = 15000;
    std::vector<char> buf(len);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD rv = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &len);
    if (rv == ERROR_BUFFER_OVERFLOW) {
        buf.resize(len);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        rv = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &len);
    }
    if (rv != NO_ERROR) return out;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            const ULONG prefix = u->OnLinkPrefixLength;
            const std::uint32_t mask = prefix == 0 ? 0u
                : prefix >= 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix));
            add(out, ntohl(sa->sin_addr.s_addr), mask);
        }
    }
#else
    ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || !ifap) return out;
    for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        std::uint32_t mask = 0xFFFFFFFFu;
        if (ifa->ifa_netmask)
            mask = ntohl(reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask)->sin_addr.s_addr);
        add(out, ntohl(sa->sin_addr.s_addr), mask);
    }
    freeifaddrs(ifap);
#endif
    return out;
}

std::string local_address_for(const std::string& remote_ipv4) {
    std::uint32_t remote = 0;
    if (!parse_ipv4(remote_ipv4, remote)) return {};
    // The most specific subnet wins (a /24 LAN over a /10 VPN range).
    const auto interfaces = local_ipv4_interfaces();
    const LocalInterface* best = nullptr;
    for (const auto& li : interfaces) {
        if ((li.address_host & li.netmask_host) != (remote & li.netmask_host)) continue;
        if (!best || li.netmask_host > best->netmask_host) best = &li;
    }
    return best ? best->address : std::string{};
}

// ---- Ramps -------------------------------------------------------------------
Ramps::Ramps(std::chrono::milliseconds tick) : tick_(tick), thread_([this] { loop(); }) {}

Ramps::~Ramps() { stop(); }

void Ramps::start(const std::string& key, double from, double to, double seconds, Setter set) {
    if (!(seconds > 0.0)) {
        {
            std::lock_guard lock{mutex_};
            ramps_.erase(key);   // an instant change also ends a fade in progress
        }
        if (set) set(to);
        return;
    }
    {
        std::lock_guard lock{mutex_};
        if (stop_) return;
        ramps_[key] = Ramp{from, to, std::chrono::steady_clock::now(),
                           std::chrono::duration<double>(seconds), std::move(set)};
    }
    cv_.notify_all();
}

void Ramps::cancel(const std::string& prefix) {
    std::lock_guard lock{mutex_};
    for (auto it = ramps_.begin(); it != ramps_.end();) {
        if (it->first.rfind(prefix, 0) == 0) it = ramps_.erase(it);
        else ++it;
    }
}

void Ramps::stop() {
    {
        std::lock_guard lock{mutex_};
        if (stop_) return;
        stop_ = true;
        ramps_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Ramps::loop() {
    std::unique_lock lock{mutex_};
    while (!stop_) {
        if (ramps_.empty()) {
            cv_.wait(lock, [this] { return stop_ || !ramps_.empty(); });
            continue;
        }
        // Work out this tick's values under the lock, call setters without it
        // (a setter sends a packet, and may itself start or cancel a ramp).
        std::vector<std::pair<Setter, double>> calls;
        const auto now = std::chrono::steady_clock::now();
        for (auto it = ramps_.begin(); it != ramps_.end();) {
            const double t = std::chrono::duration<double>(now - it->second.start).count() /
                             it->second.length.count();
            if (t >= 1.0) {
                calls.emplace_back(it->second.set, it->second.to);
                it = ramps_.erase(it);
            } else {
                calls.emplace_back(it->second.set,
                                   it->second.from + (it->second.to - it->second.from) * std::max(0.0, t));
                ++it;
            }
        }
        lock.unlock();
        for (auto& [set, value] : calls) {
            try { if (set) set(value); }
            catch (const std::exception& e) { Logger::warn("events: ramp setter threw: {}", e.what()); }
        }
        lock.lock();
        cv_.wait_for(lock, tick_, [this] { return stop_; });
    }
}

} // namespace liveplay::events
