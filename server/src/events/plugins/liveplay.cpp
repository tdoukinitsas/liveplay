// =============================================================================
// events/plugin_liveplay.cpp — events that act on LivePlay itself
// -----------------------------------------------------------------------------
// Start or stop another cue at a point in this one, or stop everything. This
// also replaces the old per-item "customActions" (play-item / play-index /
// stop-all), which ProjectState migrates into events on load.
// =============================================================================
#include "liveplay/events/plugin.hpp"

namespace liveplay::events {
namespace {

class LivePlayPlugin final : public Plugin {
public:
    explicit LivePlayPlugin(Host& host) : host_(host) {}

    std::string id() const override { return "liveplay"; }

    json schema() const override {
        const auto item = [](const char* label) {
            return field("item", label, "item", {{"required", true}});
        };
        return json{
            {"id", "liveplay"},
            {"label", "LivePlay"},
            {"icon", "queue_music"},
            {"description", "Start or stop other cues in this project."},
            {"needsConnection", false},
            {"connectionFields", json::array()},
            {"actions", json::array({
                json{{"id", "play-item"}, {"label", "Play cue"},
                     {"fields", json::array({item("Cue")})}},
                json{{"id", "stop-item"}, {"label", "Stop cue"},
                     {"fields", json::array({item("Cue")})}},
                json{{"id", "stop-all"}, {"label", "Stop all"}, {"fields", json::array()}},
            })},
        };
    }

    Result execute(const Job& job) override {
        const std::string target = opt_string(job.options, "item");
        if (job.action == "stop-all") {
            host_.stop_all();
            return {};
        }
        if (job.action != "play-item" && job.action != "stop-item")
            return Result::fail("unknown LivePlay action '" + job.action + "'");
        if (target.empty()) return Result::fail("no cue chosen");
        if (target == job.ctx.item_uuid && job.action == "play-item")
            return Result::fail("a cue can't restart itself from its own event");
        const bool done = job.action == "play-item" ? host_.play_item(target)
                                                    : host_.stop_item(target);
        if (!done) return Result::fail("cue not found or not playable");
        return {};
    }

private:
    Host& host_;
};

} // namespace

std::unique_ptr<Plugin> make_liveplay_plugin(Host& host) {
    return std::make_unique<LivePlayPlugin>(host);
}

} // namespace liveplay::events
