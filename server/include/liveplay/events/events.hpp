#pragma once
// =============================================================================
// liveplay/events/events.hpp
// -----------------------------------------------------------------------------
// The event system (issue #84): cues drive external gear. An audio item carries
// a list of events, each { trigger, plugin, action, options, connectionId }:
//
//   trigger   "time" (the playhead crosses a point in the file), "start", "end"
//             (stopped or finished), "pause", "resume"
//   plugin    who carries it out: "osc", "midi", "http", or "liveplay" (other
//             cues). Each plugin publishes a SCHEMA, and the client renders
//             every form from it, so a new plugin needs no client code.
//   connectionId  a project-level destination ("Lighting desk: OSC
//             192.168.1.50:8000") in the document's eventConnections, so moving
//             a show to another venue is one edit, not one per event.
//
// The server owns the timing: the sequencer decides WHEN, this module carries
// it out. Plugins run on per-plugin lanes, so a slow HTTP target can't hold up
// a MIDI or OSC cue that falls due at the same moment.
//
// Schema format (what GET /api/events/plugins returns per plugin):
//   { id, label, icon, description, needsConnection,
//     connectionFields: [Field], actions: [{ id, label, summary?, fields: [Field] }] }
//   Field: { id, label, type: string|text|number|integer|boolean|enum|item|list,
//            default?, required?, min?, max?, step?, placeholder?, help?,
//            options?: [{value,label}], allowCustom?, fields? (list rows),
//            showIf?: { field, equals: [...] } }
// String options may use {item.name} {item.uuid} {trigger} {time}; they are
// filled in when the event fires (substitute()).
//
// This header is the dispatcher side. Plugin authors need only plugin.hpp and
// docs/EVENT_PLUGINS.md.
// =============================================================================
#include "liveplay/events/plugin.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace liveplay::events {

using json = nlohmann::json;

// One event as stored on an item. Parsed when the item starts playing.
struct ItemEvent {
    std::string id;
    std::string trigger;        // time | start | end | pause | resume
    double      time = 0.0;     // seconds on the file timeline, trigger "time" only
    std::string plugin;
    std::string action;
    std::string connection_id;
    json        options = json::object();
    bool        triggered = false;   // "time" events: fired (or skipped) this pass

    // Enabled, well-formed events from an item's "events" array.
    static std::vector<ItemEvent> parse_list(const json& events);
};

// Fill {item.name} {item.uuid} {trigger} {time} into every string in `v`.
json substitute(const json& v, const FireContext& ctx);

class Dispatcher {
public:
    using Clock = std::chrono::steady_clock;
    using ResultCallback = std::function<void(const Job&, const Result&)>;

    Dispatcher();
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Rejects (and logs) a plugin whose schema id doesn't match id() or
    // whose id is already taken.
    void add_plugin(std::unique_ptr<Plugin> plugin);
    Plugin* find(const std::string& id) const;
    json plugins_json() const;
    bool plugin_needs_connection(const std::string& id) const;

    // Run `job` at `when` (default: now). Scheduling ahead is what makes a
    // "time" event land on its mark rather than on the sequencer's next tick.
    void enqueue(Job job, Clock::time_point when = Clock::time_point{});
    // Drop jobs for an item that haven't started yet: it paused, stopped,
    // looped or was re-fired before a scheduled marker came due.
    void cancel_item(const std::string& item_uuid);
    void cancel_all();
    // Synchronous, for the Test buttons.
    Result run_now(const Job& job);

    void set_result_callback(ResultCallback cb);
    // Joins every thread. Idempotent; the destructor calls it.
    void stop();

private:
    struct Lane {
        std::thread             thread;
        std::mutex              mutex;
        std::condition_variable cv;
        std::deque<Job>         jobs;
    };
    void scheduler_loop();
    void lane_loop(Lane& lane, Plugin& plugin);
    void hand_to_lane(Job job);
    void report(const Job& job, const Result& r);

    std::map<std::string, std::unique_ptr<Plugin>> plugins_;
    std::vector<std::string>                       order_;   // registration order, for listing
    std::map<std::string, std::unique_ptr<Lane>>   lanes_;   // one per plugin

    mutable std::mutex                    mutex_;   // scheduled_, stop_, callback_
    std::condition_variable               cv_;
    std::multimap<Clock::time_point, Job> scheduled_;
    bool                                  stop_ = false;
    ResultCallback                        callback_;
    std::thread                           scheduler_;
};

} // namespace liveplay::events
