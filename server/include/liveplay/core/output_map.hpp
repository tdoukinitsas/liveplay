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
    // An unmapped name falls back to being treated as a device name, stereo on
    // hardware channels 0/1. That keeps a fresh install working with no
    // configuration, and it is what lets legacy per-item device overrides
    // migrate into buses without changing where the audio goes.
    std::vector<Channel> resolve(const std::string& name) const;

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
