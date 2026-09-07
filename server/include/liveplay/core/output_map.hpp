// ============================================================================
// liveplay/core/output_map.hpp
// ----------------------------------------------------------------------------
// Server-owned binding from a project's *logical* output name ("FOH",
// "Monitors", "Comms") to the physical channels that name means on THIS
// machine.
//
// Projects never contain device names. A bus says "I go to FOH"; this map says
// what FOH is here. That separation is what lets a show move between venues —
// email the .liveplay elsewhere and it references FOH, not a particular sound
// card. One rack, many shows, one map.
//
// Lives with the server, not the project: the machine owns its own hardware.
// ============================================================================
#pragma once

#include "liveplay/audio/types.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace liveplay::core {

using json = nlohmann::json;

// The two built-in logical outputs (D26). They are names like any other —
// a real mapping in outputs.json always wins — but each has a defined meaning
// when the map says nothing:
//
//   "Main Out"    → the platform's default playback device, stereo. It is
//                   what the Master bus targets out of the box, so a fresh
//                   install makes sound with no configuration at all.
//   "Preview Out" → nothing. The Preview bus targets it out of the box, and
//                   silence is the safe answer for the bus PFL lands on: the
//                   default device is the house, and PFL in the house is the
//                   accident §2.4 chose PFL over solo to make impossible.
inline constexpr const char* kMainOutputName    = "Main Out";
inline constexpr const char* kPreviewOutputName = "Preview Out";

class OutputMap {
public:
    struct Channel {
        std::string          device;        // device display name
        audio::ChannelIndex  hw_channel = 0;
    };

    // Where outputs.json lives. Set once at startup, before load().
    void set_path(std::filesystem::path p);
    std::filesystem::path path() const;

    // Read from disk. Missing file is not an error — an unmapped install
    // still works via the identity fallback in resolve().
    bool load();
    // Atomic write (temp + rename), same contract as the project save.
    bool save() const;

    // Channels a logical name resolves to on this machine.
    //
    // A real mapping always wins. Unmapped, the built-ins have their own
    // meaning (see kMainOutputName / kPreviewOutputName above): "Main Out" is
    // the default device — an empty device name, which is what
    // AudioEngine::open_device_by_name takes to mean the platform default —
    // and "Preview Out" is no channels at all. Any other unmapped name falls
    // back to being treated as a device name, stereo on hardware channels 0/1.
    //
    // NOT the routing answer any more. That last clause — the identity
    // fallback — was withdrawn in BUS_ARCHITECTURE.md §0.8, because
    // open_device_by_name() opens the DEFAULT device when a name matches
    // nothing, so an unmapped name reached the house at a venue without that
    // hardware. Wiring goes through ProjectState::resolve_output_channels(),
    // which asks whether the device is actually present and answers silence
    // when it is not.
    //
    // What is left here is one caller: the pan path's "is there anything to
    // send to" guard. It can still be answered by the fallback, for a
    // master-role bus whose target names absent hardware — that bus holds the
    // house pair whatever its target resolves to (D27), so the guard is
    // reached, and the fallback lets the strip's sends be placed on masters
    // 0/1. Nothing is heard: in that state the house pair was never assigned
    // to a device. So the fallback cannot leak audio from here, but it is not
    // answering the current question either. Anything new that asks where
    // audio actually goes wants resolve_output_channels(), not this.
    std::vector<Channel> resolve(const std::string& name) const;

    // The built-in names, in the order the UI lists them.
    static std::vector<std::string> builtin_names();

    // True when the name has a real mapping (not the identity fallback), so
    // the UI can flag a bus pointing at an output this machine doesn't know.
    bool has(const std::string& name) const;

    void set(const std::string& name, std::vector<Channel> channels);
    void erase(const std::string& name);
    std::vector<std::string> names() const;

    json to_json() const;
    bool from_json(const json& j);

private:
    mutable std::mutex                          mutex_;
    std::filesystem::path                       path_;
    std::map<std::string, std::vector<Channel>> map_;
};

} // namespace liveplay::core
