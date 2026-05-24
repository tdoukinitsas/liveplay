// ============================================================================
// liveplay/net/discovery.hpp
// ----------------------------------------------------------------------------
// Tiny LAN auto-discovery beacon. The server periodically broadcasts a
// small JSON UDP packet on a well-known port; LivePlay clients listen on
// the same port to populate a "Servers on this network" list.
//
// We use a fixed UDP broadcast rather than full mDNS because:
//   * No extra deps to vendor / link.
//   * No platform-specific multicast quirks (mDNS on Windows often
//     collides with the OS Bonjour service on port 5353).
//   * The audience for this signal is the LivePlay client UI only —
//     showing up in OS-level service browsers isn't a requirement.
//
// Packet shape (UTF-8 JSON, fits in one MTU):
//   {
//     "type": "liveplay-beacon",
//     "name": "<hostname>",
//     "version": "<server version>",
//     "port": 4480,
//     "projectName": "<name or empty>",
//     "hasOpenProject": false,
//     "clients": 2,
//     "instanceId": "<uuid-ish per-run>"
//   }
// ============================================================================
#pragma once

#include "liveplay/core/project_state.hpp"
#include "liveplay/net/control_server.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace liveplay::net {

struct DiscoveryConfig {
    std::uint16_t beacon_port      = 4481;   // separate from REST 4480
    std::uint16_t advertised_port  = 4480;   // the REST/WS port to publish
    std::chrono::milliseconds interval{3000};
    std::string instance_id;                 // generated if empty
};

class DiscoveryBeacon {
public:
    explicit DiscoveryBeacon(core::ProjectState& state, DiscoveryConfig cfg = {});
    ~DiscoveryBeacon();

    DiscoveryBeacon(const DiscoveryBeacon&) = delete;
    DiscoveryBeacon& operator=(const DiscoveryBeacon&) = delete;

    bool start();
    void stop();

private:
    core::ProjectState& state_;
    DiscoveryConfig     cfg_;
    std::atomic<bool>   running_{false};
    std::thread         thread_;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    void run_loop();
};

} // namespace liveplay::net
