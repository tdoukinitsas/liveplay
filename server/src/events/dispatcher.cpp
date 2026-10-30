// =============================================================================
// events/dispatcher.cpp — scheduling and running event jobs (see events.hpp)
// =============================================================================
#include "liveplay/events/events.hpp"
#include "liveplay/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace liveplay::events {

// ---- ItemEvent ---------------------------------------------------------------
std::vector<ItemEvent> ItemEvent::parse_list(const json& events) {
    std::vector<ItemEvent> out;
    if (!events.is_array()) return out;
    for (const auto& e : events) {
        if (!e.is_object() || !e.value("enabled", true)) continue;
        ItemEvent ev;
        ev.id      = e.value("id", std::string{});
        ev.trigger = e.value("trigger", std::string{"time"});
        ev.plugin  = e.value("plugin", std::string{});
        ev.action  = e.value("action", std::string{});
        if (e.contains("time") && e["time"].is_number()) ev.time = e["time"].get<double>();
        if (e.contains("connectionId") && e["connectionId"].is_string())
            ev.connection_id = e["connectionId"].get<std::string>();
        if (e.contains("options") && e["options"].is_object()) ev.options = e["options"];
        if (ev.plugin.empty() || ev.action.empty()) continue;
        if (ev.trigger != "time" && ev.trigger != "start" && ev.trigger != "end" &&
            ev.trigger != "pause" && ev.trigger != "resume") continue;
        out.push_back(std::move(ev));
    }
    return out;
}

// ---- Placeholders ------------------------------------------------------------
namespace {
void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
}
} // namespace

json substitute(const json& v, const FireContext& ctx) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s.find('{') == std::string::npos) return v;
        char time_buf[32];
        std::snprintf(time_buf, sizeof time_buf, "%.3f", ctx.time);
        replace_all(s, "{item.name}", ctx.item_name);
        replace_all(s, "{item.uuid}", ctx.item_uuid);
        replace_all(s, "{trigger}",   ctx.trigger);
        replace_all(s, "{time}",      time_buf);
        return s;
    }
    if (v.is_array() || v.is_object()) {
        json out = v;
        for (auto& child : out) child = substitute(child, ctx);
        return out;
    }
    return v;
}

// ---- Schema / option helpers -------------------------------------------------
json field(const std::string& id, const std::string& label, const std::string& type, json extra) {
    json f{{"id", id}, {"label", label}, {"type", type}};
    if (extra.is_object()) f.update(extra);
    return f;
}

json options_list(std::initializer_list<std::pair<const char*, const char*>> value_label) {
    json out = json::array();
    for (const auto& [v, l] : value_label) out.push_back(json{{"value", v}, {"label", l}});
    return out;
}

std::string opt_string(const json& o, const char* key, const std::string& def) {
    if (!o.is_object() || !o.contains(key)) return def;
    const auto& v = o[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return std::to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return def;
}

double opt_number(const json& o, const char* key, double def) {
    if (!o.is_object() || !o.contains(key)) return def;
    const auto& v = o[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try { return std::stod(v.get<std::string>()); } catch (...) {}
    }
    return def;
}

int opt_int(const json& o, const char* key, int def) {
    const double d = opt_number(o, key, static_cast<double>(def));
    return std::isfinite(d) ? static_cast<int>(std::lround(d)) : def;
}

bool opt_bool(const json& o, const char* key, bool def) {
    if (!o.is_object() || !o.contains(key)) return def;
    const auto& v = o[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return v.get<std::string>() == "true";
    return def;
}

// ---- Dispatcher ----------------------------------------------------------------
Dispatcher::Dispatcher() : scheduler_([this] { scheduler_loop(); }) {}

Dispatcher::~Dispatcher() { stop(); }

void Dispatcher::add_plugin(std::unique_ptr<Plugin> plugin) {
    if (!plugin) return;
    const std::string id = plugin->id();
    // Caught at boot, where a plugin author will see it, rather than as forms
    // that silently never match their events.
    std::string schema_id;
    try { schema_id = plugin->schema().value("id", std::string{}); }
    catch (const std::exception& e) { schema_id = std::string{"(schema threw: "} + e.what() + ")"; }
    if (id.empty() || schema_id != id) {
        Logger::error("events: plugin '{}' not loaded: schema id is '{}'", id, schema_id);
        return;
    }
    auto lane = std::make_unique<Lane>();
    Plugin& p = *plugin;
    Lane&   l = *lane;
    {
        std::lock_guard lock{mutex_};
        if (plugins_.count(id)) {
            Logger::error("events: plugin id '{}' registered twice; keeping the first", id);
            return;
        }
        plugins_[id] = std::move(plugin);
        lanes_[id]   = std::move(lane);
        order_.push_back(id);
    }
    l.thread = std::thread([this, &l, &p] { lane_loop(l, p); });
}

Plugin* Dispatcher::find(const std::string& id) const {
    std::lock_guard lock{mutex_};
    auto it = plugins_.find(id);
    return it == plugins_.end() ? nullptr : it->second.get();
}

json Dispatcher::plugins_json() const {
    std::vector<Plugin*> list;
    {
        std::lock_guard lock{mutex_};
        // builtin_plugins() order, which is the order the client offers them in.
        for (const auto& id : order_) list.push_back(plugins_.at(id).get());
    }
    json out = json::array();
    for (auto* p : list) {
        try { out.push_back(p->schema()); }
        catch (const std::exception& e) {
            Logger::warn("events: plugin '{}' schema failed: {}", p->id(), e.what());
        }
    }
    return out;
}

bool Dispatcher::plugin_needs_connection(const std::string& id) const {
    auto* p = find(id);
    if (!p) return false;
    try { return p->schema().value("needsConnection", true); } catch (...) { return true; }
}

void Dispatcher::enqueue(Job job, Clock::time_point when) {
    if (when == Clock::time_point{} || when <= Clock::now()) {
        hand_to_lane(std::move(job));
        return;
    }
    {
        std::lock_guard lock{mutex_};
        if (stop_) return;
        scheduled_.emplace(when, std::move(job));
    }
    cv_.notify_all();
}

void Dispatcher::cancel_item(const std::string& item_uuid) {
    std::lock_guard lock{mutex_};
    for (auto it = scheduled_.begin(); it != scheduled_.end();) {
        if (it->second.ctx.item_uuid == item_uuid) it = scheduled_.erase(it);
        else ++it;
    }
}

void Dispatcher::cancel_all() {
    std::lock_guard lock{mutex_};
    scheduled_.clear();
}

Result Dispatcher::run_now(const Job& job) {
    auto* p = find(job.plugin);
    if (!p) return Result::fail("unknown plugin '" + job.plugin + "'");
    try { return p->execute(job); }
    catch (const std::exception& e) { return Result::fail(e.what()); }
}

void Dispatcher::set_result_callback(ResultCallback cb) {
    std::lock_guard lock{mutex_};
    callback_ = std::move(cb);
}

void Dispatcher::stop() {
    {
        std::lock_guard lock{mutex_};
        if (stop_) return;
        stop_ = true;
        scheduled_.clear();
    }
    cv_.notify_all();
    if (scheduler_.joinable()) scheduler_.join();
    for (auto& [_, lane] : lanes_) {
        {
            std::lock_guard lk{lane->mutex};
            lane->jobs.clear();
        }
        lane->cv.notify_all();
        if (lane->thread.joinable()) lane->thread.join();
    }
    // Every lane is joined, so no execute() is running. (A Test request could
    // still be mid-call on a REST thread; the server stops those first.)
    for (auto& [id, plugin] : plugins_) {
        try { plugin->stop(); }
        catch (const std::exception& e) {
            Logger::warn("events: plugin '{}' stop() threw: {}", id, e.what());
        }
    }
}

void Dispatcher::scheduler_loop() {
    std::unique_lock lock{mutex_};
    while (!stop_) {
        if (scheduled_.empty()) {
            cv_.wait(lock, [this] { return stop_ || !scheduled_.empty(); });
            continue;
        }
        const auto due = scheduled_.begin()->first;
        if (Clock::now() < due) {
            // Woken early by a new, earlier job or a cancel: re-evaluate.
            cv_.wait_until(lock, due);
            continue;
        }
        Job job = std::move(scheduled_.begin()->second);
        scheduled_.erase(scheduled_.begin());
        lock.unlock();
        hand_to_lane(std::move(job));
        lock.lock();
    }
}

void Dispatcher::hand_to_lane(Job job) {
    Lane* lane = nullptr;
    {
        std::lock_guard lock{mutex_};
        if (stop_) return;
        auto it = lanes_.find(job.plugin);
        if (it != lanes_.end()) lane = it->second.get();
    }
    if (!lane) {
        report(job, Result::fail("unknown plugin '" + job.plugin + "'"));
        return;
    }
    {
        std::lock_guard lk{lane->mutex};
        lane->jobs.push_back(std::move(job));
    }
    lane->cv.notify_one();
}

void Dispatcher::lane_loop(Lane& lane, Plugin& plugin) {
    for (;;) {
        Job job;
        {
            std::unique_lock lk{lane.mutex};
            lane.cv.wait(lk, [&] {
                std::lock_guard g{mutex_};
                return stop_ || !lane.jobs.empty();
            });
            {
                std::lock_guard g{mutex_};
                if (stop_) return;
            }
            job = std::move(lane.jobs.front());
            lane.jobs.pop_front();
        }
        Result r;
        try { r = plugin.execute(job); }
        catch (const std::exception& e) { r = Result::fail(e.what()); }
        catch (...) { r = Result::fail("unknown error"); }
        report(job, r);
    }
}

void Dispatcher::report(const Job& job, const Result& r) {
    if (r.ok) {
        Logger::info("EVENT: {} '{}' {}/{} ok", job.ctx.trigger, job.ctx.item_name,
                     job.plugin, job.action);
    } else {
        Logger::warn("EVENT: {} '{}' {}/{} failed: {}", job.ctx.trigger, job.ctx.item_name,
                     job.plugin, job.action, r.error);
    }
    ResultCallback cb;
    {
        std::lock_guard lock{mutex_};
        cb = callback_;
    }
    if (cb) {
        try { cb(job, r); } catch (...) {}
    }
}

} // namespace liveplay::events
