// output_map.cpp — see output_map.hpp.
#include "liveplay/core/output_map.hpp"

#include "liveplay/logger.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <fstream>

namespace liveplay::core {

namespace fs = std::filesystem;

void OutputMap::set_path(fs::path p) {
    std::lock_guard lock{mutex_};
    path_ = std::move(p);
}

fs::path OutputMap::path() const {
    std::lock_guard lock{mutex_};
    return path_;
}

bool OutputMap::load() {
    fs::path p = path();
    if (p.empty()) return false;
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        // Not an error: an install with no mapping still resolves by identity.
        Logger::debug("OutputMap: no map at '{}', using identity resolution",
                      util::path_to_utf8(p));
        return false;
    }
    try {
        std::ifstream in{p, std::ios::binary};
        json j;
        in >> j;
        if (!from_json(j)) {
            Logger::warn("OutputMap: '{}' is not a valid output map",
                         util::path_to_utf8(p));
            return false;
        }
        Logger::info("OutputMap: loaded {} logical output(s)", names().size());
        return true;
    } catch (const std::exception& e) {
        Logger::warn("OutputMap: failed to read '{}': {}", util::path_to_utf8(p), e.what());
        return false;
    }
}

bool OutputMap::save() const {
    fs::path p = path();
    if (p.empty()) return false;
    try {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        // Write-temp-then-rename so a crash mid-write never destroys a good
        // map — same contract as ProjectState::save().
        const fs::path tmp = p.string() + ".tmp";
        {
            std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
            if (!out) return false;
            out << to_json().dump(2);
            out.flush();
            if (!out) return false;
        }
        fs::rename(tmp, p, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        Logger::warn("OutputMap: failed to write '{}': {}", util::path_to_utf8(p), e.what());
        return false;
    }
}

std::vector<OutputMap::Channel> OutputMap::resolve(const std::string& name) const {
    if (name.empty()) return {};
    {
        std::lock_guard lock{mutex_};
        auto it = map_.find(name);
        if (it != map_.end() && !it->second.empty()) return it->second;
    }
    // Identity fallback — the name is taken to be a device name, stereo.
    return {Channel{name, 0}, Channel{name, 1}};
}

bool OutputMap::has(const std::string& name) const {
    std::lock_guard lock{mutex_};
    auto it = map_.find(name);
    return it != map_.end() && !it->second.empty();
}

void OutputMap::set(const std::string& name, std::vector<Channel> channels) {
    if (name.empty()) return;
    std::lock_guard lock{mutex_};
    map_[name] = std::move(channels);
}

void OutputMap::erase(const std::string& name) {
    std::lock_guard lock{mutex_};
    map_.erase(name);
}

std::vector<std::string> OutputMap::names() const {
    std::lock_guard lock{mutex_};
    std::vector<std::string> out;
    out.reserve(map_.size());
    for (const auto& [n, _] : map_) out.push_back(n);
    return out;
}

json OutputMap::to_json() const {
    std::lock_guard lock{mutex_};
    json arr = json::array();
    for (const auto& [name, channels] : map_) {
        json ch = json::array();
        for (const auto& c : channels) {
            ch.push_back(json{{"device", c.device}, {"hwChannel", c.hw_channel}});
        }
        arr.push_back(json{{"name", name}, {"channels", std::move(ch)}});
    }
    return json{{"version", 1}, {"outputs", std::move(arr)}};
}

bool OutputMap::from_json(const json& j) {
    if (!j.is_object() || !j.contains("outputs") || !j["outputs"].is_array()) return false;
    std::map<std::string, std::vector<Channel>> parsed;
    for (const auto& o : j["outputs"]) {
        if (!o.is_object()) continue;
        const auto name = o.value("name", std::string{});
        if (name.empty()) continue;
        std::vector<Channel> channels;
        if (o.contains("channels") && o["channels"].is_array()) {
            for (const auto& c : o["channels"]) {
                if (!c.is_object()) continue;
                Channel ch;
                ch.device     = c.value("device", std::string{});
                ch.hw_channel = c.value("hwChannel", audio::ChannelIndex{0});
                if (ch.device.empty()) continue;
                channels.push_back(std::move(ch));
            }
        }
        if (!channels.empty()) parsed[name] = std::move(channels);
    }
    std::lock_guard lock{mutex_};
    map_ = std::move(parsed);
    return true;
}

} // namespace liveplay::core
