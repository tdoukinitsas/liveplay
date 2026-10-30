#pragma once
// =============================================================================
// liveplay/events/helpers.hpp — shared tools for plugin authors
// -----------------------------------------------------------------------------
//   local_ipv4_interfaces / local_address_for
//       Which of this machine's networks a device is on. Bind a UDP socket to
//       local_address_for(device) before talking to it: on a machine with a
//       VPN, a socket bound to 0.0.0.0 can send from the VPN address, and the
//       device's reply never comes back. Discovery should broadcast on every
//       interface's own broadcast address, for the same reason.
//   Ramps
//       Timed changes (crossfades) without blocking a plugin's lane: start a
//       ramp and return from execute() at once; the ramp calls your setter on
//       its own thread until it lands. See docs/EVENT_PLUGINS.md, "Helpers".
// =============================================================================
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace liveplay::events {

struct LocalInterface {
    std::string   address;     // "192.168.1.123"
    std::string   broadcast;   // "192.168.1.255"
    std::uint32_t address_host = 0;   // host byte order
    std::uint32_t netmask_host = 0;
};

// Every up, non-loopback IPv4 interface.
std::vector<LocalInterface> local_ipv4_interfaces();

// The address of the local interface whose subnet contains `remote_ipv4`, or
// "" if none does (the device is routed, or `remote_ipv4` isn't an address).
std::string local_address_for(const std::string& remote_ipv4);

// Value ramps, keyed by whatever names the target ("192.168.1.5/ch/05/mix/fader").
class Ramps {
public:
    using Setter = std::function<void(double value)>;

    explicit Ramps(std::chrono::milliseconds tick = std::chrono::milliseconds{20});
    ~Ramps();
    Ramps(const Ramps&) = delete;
    Ramps& operator=(const Ramps&) = delete;

    // Move `key` from `from` to `to` over `seconds`, linearly, calling `set`
    // every tick and finally with exactly `to`. Starting a ramp on a key that
    // is already ramping replaces it (the newest instruction wins, as on a
    // desk). seconds <= 0 calls set(to) at once, on the caller's thread.
    void start(const std::string& key, double from, double to, double seconds, Setter set);

    // Drop running ramps whose key starts with `prefix` ("" = all), where they are.
    void cancel(const std::string& prefix = {});

    // Cancel everything and join the thread. Idempotent.
    void stop();

private:
    struct Ramp {
        double from = 0.0, to = 0.0;
        std::chrono::steady_clock::time_point start;
        std::chrono::duration<double> length{0.0};
        Setter set;
    };
    void loop();

    std::chrono::milliseconds   tick_;
    std::mutex                  mutex_;
    std::condition_variable     cv_;
    std::map<std::string, Ramp> ramps_;
    bool                        stop_ = false;
    std::thread                 thread_;
};

} // namespace liveplay::events
