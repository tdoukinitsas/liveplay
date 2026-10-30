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
// =============================================================================
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

// What fired: the source of {placeholders} and of the event_fired feedback.
struct FireContext {
    std::string item_uuid;
    std::string item_name;
    std::string event_id;
    std::string trigger;
    double      time = 0.0;
};

// A unit of work for a plugin: the event with its connection resolved.
struct Job {
    std::string plugin;
    std::string action;
    json        options    = json::object();   // placeholders already filled in
    json        connection = json::object();   // the connection's config ({} if none)
    FireContext ctx;
};

struct Result {
    bool        ok = true;
    std::string error;
    static Result fail(std::string why) { return Result{false, std::move(why)}; }
};

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual std::string id() const = 0;
    // Built per request, so live lists (MIDI ports) are current.
    virtual json schema() const = 0;
    // Runs on this plugin's lane, or on a REST thread for /api/events/test, so
    // it must be thread-safe. May block briefly (a network round trip).
    virtual Result execute(const Job& job) = 0;
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
    std::map<std::string, std::unique_ptr<Lane>>   lanes_;   // one per plugin

    mutable std::mutex                    mutex_;   // scheduled_, stop_, callback_
    std::condition_variable               cv_;
    std::multimap<Clock::time_point, Job> scheduled_;
    bool                                  stop_ = false;
    ResultCallback                        callback_;
    std::thread                           scheduler_;
};

// ---- Built-in plugins -------------------------------------------------------
std::unique_ptr<Plugin> make_osc_plugin();
std::unique_ptr<Plugin> make_midi_plugin();
std::unique_ptr<Plugin> make_http_plugin();

// "liveplay": act on other cues. The owner supplies the verbs.
struct LivePlayHooks {
    std::function<bool(const std::string& uuid)> play_item;
    std::function<bool(const std::string& uuid)> stop_item;
    std::function<void()>                        stop_all;
};
std::unique_ptr<Plugin> make_liveplay_plugin(LivePlayHooks hooks);

// ---- Field helpers shared by the plugins' schema() ---------------------------
json field(const std::string& id, const std::string& label, const std::string& type,
           json extra = json::object());
json options_list(std::initializer_list<std::pair<const char*, const char*>> value_label);

// Option accessors that tolerate numbers sent as strings and vice versa.
std::string opt_string(const json& o, const char* key, const std::string& def = {});
double      opt_number(const json& o, const char* key, double def = 0.0);
int         opt_int(const json& o, const char* key, int def = 0);
bool        opt_bool(const json& o, const char* key, bool def = false);

} // namespace liveplay::events
