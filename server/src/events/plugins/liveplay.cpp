// =============================================================================
// events/plugins/liveplay.cpp — events that act on LivePlay itself
// -----------------------------------------------------------------------------
// The cue's own show, driven from its timeline: start, stop, pause or arm other
// cues; duck a playing cue; and work the internal mixer the way the X Air
// plugin works an external one: bus faders and sends with crossfades, mutes,
// routing, the bus processing and the master level.
//
// Everything goes through events::Host, so the mixer changes are the same ones
// the Mixer pane makes: saved with the project and seen by every client. A
// fade is heard step by step through the live path (set_bus_live) and saved
// once, when it lands (patch_bus).
//
// This plugin also replaces the old per-item "customActions" (play-item /
// play-index / stop-all), which ProjectState migrates into events on load.
// =============================================================================
#include "liveplay/events/helpers.hpp"
#include "liveplay/events/plugin.hpp"

#include <algorithm>
#include <cmath>

namespace liveplay::events {
namespace {

constexpr double kSilenceDb = -90.0;

// The bus processing an event may set: a path into the bus's dsp object.
struct DspParam { const char* path; const char* label; double min, max; const char* unit; bool toggle; };
constexpr DspParam kDspParams[] = {
    {"eqEnabled",      "EQ on",                  0, 1, "", true},
    {"dynEnabled",     "Dynamics on",            0, 1, "", true},
    {"hpf.freq",       "High-pass frequency",   20, 2000, "Hz", false},
    {"lpf.freq",       "Low-pass frequency",  1000, 24000, "Hz", false},
    {"gate.on",        "Gate on",                0, 1, "", true},
    {"gate.threshold", "Gate threshold",       -80, 0, "dB", false},
    {"gate.range",     "Gate range",           -80, 0, "dB", false},
    {"comp.on",        "Compressor on",          0, 1, "", true},
    {"comp.threshold", "Compressor threshold", -60, 0, "dB", false},
    {"comp.ratio",     "Compressor ratio",       1, 60, ":1", false},
    {"comp.makeup",    "Compressor make-up",   -12, 24, "dB", false},
    {"width.width",    "Stereo width",           0, 2, "×", false},
};

const DspParam* dsp_param(const std::string& path) {
    for (const auto& p : kDspParams) if (path == p.path) return &p;
    return nullptr;
}

// "comp.threshold", -20 → { "comp": { "threshold": -20 } }
json dsp_patch(const DspParam& p, double v) {
    const std::string path = p.path;
    const auto dot = path.find('.');
    const json value = p.toggle ? json(v >= 0.5) : json(v);
    if (dot == std::string::npos) return json{{path, value}};
    return json{{path.substr(0, dot), json{{path.substr(dot + 1), value}}}};
}

double dsp_current(const json& bus, const DspParam& p, double fallback) {
    const std::string path = p.path;
    const auto dot = path.find('.');
    const json dsp = bus.value("dsp", json::object());
    const json* v = nullptr;
    if (dot == std::string::npos) {
        if (dsp.contains(path)) v = &dsp[path];
    } else {
        const auto group = path.substr(0, dot), key = path.substr(dot + 1);
        if (dsp.contains(group) && dsp[group].contains(key)) v = &dsp[group][key];
    }
    if (!v) return fallback;
    if (v->is_boolean()) return v->get<bool>() ? 1.0 : 0.0;
    return v->is_number() ? v->get<double>() : fallback;
}

json find_bus(const json& buses, const std::string& id) {
    if (buses.is_array())
        for (const auto& b : buses) if (b.value("id", std::string{}) == id) return b;
    return nullptr;
}

std::string bus_label(const json& b) {
    std::string label = b.value("name", b.value("id", std::string{}));
    if (b.value("master", false)) label += " (master)";
    if (b.value("preview", false)) label += " (preview)";
    return label;
}

class LivePlayPlugin final : public Plugin {
public:
    explicit LivePlayPlugin(Host& host) : host_(host) {}

    std::string id() const override { return "liveplay"; }

    json schema() const override {
        const auto item = [](const char* label) {
            return field("item", label, "item", {{"required", true}});
        };
        const auto bus = [](const char* id, const char* label) {
            return field(id, label, "enum",
                         {{"required", true}, {"options", json::array()}, {"optionsFrom", "buses"}});
        };
        const auto level = [](double def, double max, const char* help) {
            return field("level", "Level", "number",
                         {{"default", def}, {"min", kSilenceDb}, {"max", max}, {"step", 0.5},
                          {"unit", "dB"}, {"help", help}});
        };
        const auto fade = field("fade", "Crossfade", "number",
            {{"default", 0.0}, {"min", 0.0}, {"max", 600.0}, {"step", 0.1}, {"unit", "s"},
             {"help", "0 changes it at once; longer fades from where it is now."}});
        json params = json::array();
        for (const auto& p : kDspParams) {
            std::string label = p.label;
            if (!p.toggle) {
                char range[64];
                std::snprintf(range, sizeof range, " (%g–%g%s%s)", p.min, p.max, p.unit[0] ? " " : "", p.unit);
                label += range;
            } else {
                label += " (0/1)";
            }
            params.push_back(json{{"value", p.path}, {"label", label}});
        }

        return json{
            {"id", "liveplay"},
            {"label", "LivePlay"},
            {"icon", "queue_music"},
            {"description", "This show: start, stop and duck cues, and work the internal mixer."},
            {"needsConnection", false},
            {"connectionFields", json::array()},
            {"actions", json::array({
                json{{"id", "play-item"},   {"label", "Play cue"},   {"fields", json::array({item("Cue")})}},
                json{{"id", "stop-item"},   {"label", "Stop cue"},   {"fields", json::array({item("Cue")})}},
                json{{"id", "pause-item"},  {"label", "Pause cue"},  {"fields", json::array({item("Cue")})}},
                json{{"id", "resume-item"}, {"label", "Resume cue"}, {"fields", json::array({item("Cue")})}},
                json{{"id", "arm-next"},    {"label", "Set Up Next"}, {"fields", json::array({item("Cue")})}},
                json{{"id", "stop-all"},    {"label", "Stop all"},   {"fields", json::array()}},
                json{{"id", "duck-item"}, {"label", "Duck a cue"}, {"summary", "{level} dB"},
                     {"fields", json::array({item("Cue"),
                         level(-12.0, 0.0, "Relative to the cue's own volume: 0 brings it back, -90 silences it. The playing cue only; its saved volume doesn't change."),
                         fade})}},
                json{{"id", "bus-level"}, {"label", "Set bus fader"}, {"summary", "{bus} → {level} dB"},
                     {"fields", json::array({bus("bus", "Bus"), level(0.0, 10.0, "-90 is off (-inf)."), fade})}},
                json{{"id", "bus-mute"}, {"label", "Mute / unmute a bus"}, {"summary", "{bus}: {state}"},
                     {"fields", json::array({bus("bus", "Bus"),
                         field("state", "State", "enum",
                               {{"default", "mute"},
                                {"options", options_list({{"mute", "Mute"}, {"unmute", "Unmute"}})}})})}},
                json{{"id", "bus-send"}, {"label", "Set send level"}, {"summary", "{bus} → {send}: {level} dB"},
                     {"fields", json::array({bus("bus", "From bus"),
                         field("send", "To", "enum",
                               {{"required", true}, {"options", json::array()}, {"optionsFrom", "bus-sends"},
                                {"dependsOn", json::array({"bus"})},
                                {"help", "The bus's existing sends; add one in the Mixer first."}}),
                         level(0.0, 10.0, "-90 is off (-inf)."), fade})}},
                json{{"id", "bus-route"}, {"label", "Route a bus"}, {"summary", "{bus} → {to}"},
                     {"fields", json::array({bus("bus", "Bus"),
                         field("to", "Goes to", "enum",
                               {{"required", true}, {"options", json::array()}, {"optionsFrom", "bus-routes"}})})}},
                json{{"id", "bus-dsp"}, {"label", "Set bus processing"}, {"summary", "{bus}: {param} = {value}"},
                     {"fields", json::array({bus("bus", "Bus"),
                         field("param", "Setting", "enum", {{"default", "comp.threshold"}, {"options", params}}),
                         field("value", "Value", "number",
                               {{"default", 0.0}, {"help", "In the setting's own unit; switches are 0 (off) or 1 (on)."}}),
                         fade})}},
                json{{"id", "master-level"}, {"label", "Set master level"}, {"summary", "{level} dB"},
                     {"fields", json::array({level(0.0, 10.0, "The master gain, after every bus."), fade})}},
            })},
        };
    }

    Result execute(const Job& job) override {
        const json& o = job.options;
        const std::string a = job.action;
        if (a == "stop-all") {
            host_.stop_all();
            return {};
        }
        if (a == "master-level") {
            const double target = std::clamp(opt_number(o, "level", 0.0), kSilenceDb, 10.0);
            ramps_.start("master", host_.master_gain_db(), target, opt_number(o, "fade", 0.0),
                         [this, target](double v) { host_.set_master_gain_db(v, v == target); });
            return {};
        }

        // ---- Cues ----
        if (a == "play-item" || a == "stop-item" || a == "pause-item" || a == "resume-item" ||
            a == "arm-next" || a == "duck-item") {
            const std::string target = opt_string(o, "item");
            if (target.empty()) return Result::fail("no cue chosen");
            if (a == "play-item" && target == job.ctx.item_uuid)
                return Result::fail("a cue can't restart itself from its own event");
            if (a == "duck-item") {
                const auto from = host_.cue_offset_db(target);
                if (!from) return Result::fail("that cue isn't playing");
                const double to = std::clamp(opt_number(o, "level", -12.0), kSilenceDb, 0.0);
                ramps_.start("cue:" + target, *from, to, opt_number(o, "fade", 0.0),
                             [this, target](double v) { host_.set_cue_offset_db(target, v); });
                return {};
            }
            bool done = false;
            if (a == "play-item")        done = host_.play_item(target);
            else if (a == "stop-item")   done = host_.stop_item(target);
            else if (a == "pause-item")  done = host_.pause_item(target);
            else if (a == "resume-item") done = host_.resume_item(target);
            else                         done = host_.arm_next(target);
            return done ? Result{} : Result::fail("cue not found or not loaded");
        }

        // ---- The mixer ----
        const std::string bus_id = opt_string(o, "bus");
        if (bus_id.empty()) return Result::fail("no bus chosen");
        const json buses = host_.buses();
        const json bus = find_bus(buses, bus_id);
        if (bus.is_null()) return Result::fail("no bus '" + bus_id + "' in this project");

        if (a == "bus-level") {
            const double target = std::clamp(opt_number(o, "level", 0.0), kSilenceDb, 10.0);
            const double from = bus.value("gainDb", 0.0);
            ramps_.start("bus:" + bus_id + ":gain", from, target, opt_number(o, "fade", 0.0),
                [this, bus_id, target](double v) {
                    if (v == target) host_.patch_bus(bus_id, json{{"gainDb", v}});
                    else host_.set_bus_live(bus_id, json{{"gainDb", v}});
                });
            return {};
        }
        if (a == "bus-mute") {
            const bool mute = opt_string(o, "state", "mute") != "unmute";
            return committed(host_.patch_bus(bus_id, json{{"mute", mute}}));
        }
        if (a == "bus-send") {
            const std::string to = opt_string(o, "send");
            const json sends = bus.value("sends", json::array());
            const auto it = std::find_if(sends.begin(), sends.end(),
                [&](const json& s) { return s.value("id", std::string{}) == to; });
            if (it == sends.end()) return Result::fail("'" + bus_id + "' has no send to '" + to + "'");
            const double target = std::clamp(opt_number(o, "level", 0.0), kSilenceDb, 10.0);
            // Sends have no live path, so each step is a saved change; at 10 a
            // second that also moves the send on every client's mixer.
            slow_ramps_.start("bus:" + bus_id + ":send:" + to, it->value("levelDb", 0.0), target,
                              opt_number(o, "fade", 0.0), [this, bus_id, to](double v) {
                json now = find_bus(host_.buses(), bus_id);
                if (now.is_null()) return;
                json list = now.value("sends", json::array());
                for (auto& s : list) if (s.value("id", std::string{}) == to) s["levelDb"] = v;
                host_.patch_bus(bus_id, json{{"sends", list}});
            });
            return {};
        }
        if (a == "bus-route") {
            const std::string to = opt_string(o, "to");
            json output;
            if (to == "none")                    output = json{{"type", "none"}};
            else if (to.rfind("bus:", 0) == 0)    output = json{{"type", "bus"}, {"target", to.substr(4)}};
            else if (to.rfind("output:", 0) == 0) output = json{{"type", "output"}, {"target", to.substr(7)}};
            else return Result::fail("choose where the bus goes");
            return committed(host_.patch_bus(bus_id, json{{"output", output}}));
        }
        if (a == "bus-dsp") {
            const auto* p = dsp_param(opt_string(o, "param"));
            if (!p) return Result::fail("unknown bus setting");
            const double target = std::clamp(opt_number(o, "value", 0.0), p->min, p->max);
            if (p->toggle) return committed(host_.patch_bus(bus_id, json{{"dsp", dsp_patch(*p, target)}}));
            const double from = dsp_current(bus, *p, target);
            ramps_.start("bus:" + bus_id + ":dsp:" + p->path, from, target, opt_number(o, "fade", 0.0),
                [this, bus_id, p, target](double v) {
                    const json patch{{"dsp", dsp_patch(*p, v)}};
                    if (v == target) host_.patch_bus(bus_id, patch);
                    else host_.set_bus_live(bus_id, patch);
                });
            return {};
        }
        return Result::fail("unknown LivePlay action '" + a + "'");
    }

    OptionsResult options(const std::string& source, const json&, const json& values) override {
        OptionsResult r;
        const json buses = host_.buses();
        if (source == "buses") {
            for (const auto& b : buses)
                r.options.push_back(json{{"value", b.value("id", std::string{})}, {"label", bus_label(b)}});
            return r;
        }
        if (source == "bus-sends") {
            const json bus = find_bus(buses, opt_string(values, "bus"));
            if (bus.is_null()) return r;
            for (const auto& s : bus.value("sends", json::array())) {
                const std::string id = s.value("id", std::string{});
                const json dest = find_bus(buses, id);
                r.options.push_back(json{{"value", id},
                    {"label", (dest.is_null() ? id : bus_label(dest)) +
                              (s.value("tap", std::string{"post"}) == "pre" ? " (pre-fader)" : "")}});
            }
            return r;
        }
        if (source == "bus-routes") {
            r.options.push_back(json{{"value", "none"}, {"label", "Nowhere (sends only)"}});
            for (const auto& b : buses) {
                if (b.value("preview", false)) continue;   // nothing may feed the preview bus
                r.options.push_back(json{{"value", "bus:" + b.value("id", std::string{})},
                                         {"label", "Bus · " + bus_label(b)}});
            }
            for (const auto& name : host_.outputs())
                r.options.push_back(json{{"value", "output:" + name}, {"label", "Output · " + name}});
            return r;
        }
        r.error = "no option source '" + source + "'";
        return r;
    }

    void stop() override {
        ramps_.stop();
        slow_ramps_.stop();
    }

private:
    static Result committed(const std::string& err) { return err.empty() ? Result{} : Result::fail(err); }

    Host& host_;
    Ramps ramps_;
    Ramps slow_ramps_{std::chrono::milliseconds{100}};
};

} // namespace

std::unique_ptr<Plugin> make_liveplay_plugin(Host& host) {
    return std::make_unique<LivePlayPlugin>(host);
}

} // namespace liveplay::events
