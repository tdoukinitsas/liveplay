// ============================================================================
// liveplay/net/discovery.hpp
// ----------------------------------------------------------------------------
// LAN auto-discovery beacon. The server announces itself on a well-known UDP
// port so LivePlay clients can populate a "Servers on this network" list
// without the operator typing an IP.
//
// We use plain UDP (broadcast + multicast) rather than full mDNS because:
//   * No extra deps to vendor / link.
//   * No collision with the OS Bonjour service on port 5353.
//   * The audience for this signal is the LivePlay client UI only.
//
// Reachability is the hard part on real networks, so the beacon uses three
// delivery paths every tick and additionally answers active probes:
//
//   1. Per-interface *directed* subnet broadcast (e.g. 192.168.1.255). This
//      is the key fix for multi-homed Windows hosts: the limited broadcast
//      255.255.255.255 only egresses ONE interface chosen by the routing
//      table — frequently a Hyper-V / WSL / VPN virtual adapter — so the
//      packet never reaches the physical LAN. Sending to each interface's
//      directed broadcast guarantees the real NIC is covered.
//   2. The limited broadcast 255.255.255.255 (legacy / belt-and-braces).
//   3. An administratively-scoped multicast group, with IP_MULTICAST_IF set
//      per interface. Some managed networks pass multicast where they rate-
//      limit broadcast.
//
// On top of the periodic announce, the server LISTENS on the beacon port for
// client *solicitation* packets ({"type":"liveplay-solicit"}) and replies
// with a UNICAST beacon straight back to the asker. Unicast traverses WiFi
// AP/client-isolation and stateful firewalls far better than broadcast, and
// makes discovery feel instant instead of waiting up to one beacon interval.
//
// Beacon packet (UTF-8 JSON, fits in one MTU):
//   {
//     "type": "liveplay-beacon",
//     "name": "<hostname>",
//     "version": "<server version>",
//     "port": 4480,
//     "projectName": "<name or empty>",
//     "hasOpenProject": false,
//     "itemCount": 2,
//     "instanceId": "<uuid-ish per-run>"
//   }
//
// Solicitation packet (sent by clients): {"type":"liveplay-solicit"}
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

    // Administratively-scoped multicast group (239.0.0.0/8) for the third
    // delivery path. Must match the client's group/port.
    std::string   multicast_group = "239.255.69.80";
    bool          enable_multicast = true;
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
