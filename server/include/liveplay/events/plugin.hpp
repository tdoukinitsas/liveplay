#pragma once
// =============================================================================
// liveplay/events/plugin.hpp — the event plugin API
// -----------------------------------------------------------------------------
// Everything a plugin author needs, and nothing of the dispatcher. To write a
// plugin, read docs/EVENT_PLUGINS.md: it has the contract, a copy-paste
// template and the checklist. In short:
//
//   1. src/events/plugins/<name>.cpp: a class deriving from Plugin, plus
//      `std::unique_ptr<Plugin> make_<name>_plugin(Host&)`.
//   2. Declare the factory below and add it to builtin_plugins.cpp.
//   3. Add the .cpp to server/CMakeLists.txt.
// The client needs no change: it renders every form from schema().
// =============================================================================
#include <nlohmann/json.hpp>

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace liveplay::events {

using json = nlohmann::json;

// What fired the event: the source of {placeholders} and of the event_fired
// feedback. Read-only for plugins.
struct FireContext {
    std::string item_uuid;   // "" for a test fire with no item
    std::string item_name;
    std::string event_id;
    std::string trigger;     // time | start | end | pause | resume | test
    double      time = 0.0;  // the marker's file time, for "time" events
};

// One unit of work, already resolved. The plugin never looks anything up in
// the project: the connection's config and the options arrive here with every
// {placeholder} filled in.
struct Job {
    std::string plugin;
    std::string action;                        // one of your schema's action ids
    json        options    = json::object();   // values of that action's fields
    json        connection = json::object();   // values of your connectionFields ({} if none)
    FireContext ctx;
};

// What happened. A failure's `error` is shown to the operator next to the
// event, so write it for them: "MIDI output 'loopMIDI Port' not found", not
// "openPort: -1".
struct Result {
    bool        ok = true;
    std::string error;
    static Result fail(std::string why) { return Result{false, std::move(why)}; }
};

// What a plugin may ask of LivePlay itself. Thread-safe; each call may block
// briefly (playing a cue waits for its decode). Grows as plugins need more;
// keep additions small and verb-shaped.
class Host {
public:
    virtual ~Host() = default;
    // Fire an item the way GO / a cart button does (groups, toggles included).
    virtual bool play_item(const std::string& uuid) = 0;
    // Stop an item (or everything in a group) with its own stop fade.
    virtual bool stop_item(const std::string& uuid) = 0;
    // Stop All, with the project's Stop All fade.
    virtual void stop_all() = 0;
};

class Plugin {
public:
    virtual ~Plugin() = default;

    // Stable id, stored in every saved event ("osc"). Never change it.
    virtual std::string id() const = 0;

    // The schema the client builds its forms from (format: docs/EVENT_PLUGINS.md,
    // "Schema"). Its "id" must equal id(). Called on every GET
    // /api/events/plugins, so it may list live things (MIDI ports), but keep
    // it cheap and never let it throw for a missing device.
    virtual json schema() const = 0;

    // Carry out one job. Runs on this plugin's own lane (one thread, jobs in
    // order) AND on REST threads for the Test buttons, so it must be
    // thread-safe: guard any state you keep. May block for a bounded time (a
    // network round trip with a timeout); never wait indefinitely. Exceptions
    // are caught and reported as failures, but prefer returning Result::fail.
    virtual Result execute(const Job& job) = 0;

    // Release sockets, ports and threads. Called once at shutdown, after the
    // last execute() has returned. Optional.
    virtual void stop() {}
};

using PluginFactory = std::unique_ptr<Plugin> (*)(Host& host);

// ---- Built-in plugins (builtin_plugins.cpp lists them) ----------------------
std::unique_ptr<Plugin> make_osc_plugin(Host& host);
std::unique_ptr<Plugin> make_midi_plugin(Host& host);
std::unique_ptr<Plugin> make_http_plugin(Host& host);
std::unique_ptr<Plugin> make_liveplay_plugin(Host& host);

// Every plugin the server ships, in the order the client lists them.
const std::vector<PluginFactory>& builtin_plugins();

// ---- Schema helpers ---------------------------------------------------------
// field("port", "Port", "integer", {{"default", 8000}, {"min", 1}})
json field(const std::string& id, const std::string& label, const std::string& type,
           json extra = json::object());
// options_list({{"go", "GO"}, {"stop", "STOP"}}) → [{value, label}, ...]
json options_list(std::initializer_list<std::pair<const char*, const char*>> value_label);

// ---- Option accessors -------------------------------------------------------
// Read job.options / job.connection without trusting their types: a client
// may send "8000" for 8000, a hand-edited project anything at all. Each
// returns `def` when the key is missing or unusable.
std::string opt_string(const json& o, const char* key, const std::string& def = {});
double      opt_number(const json& o, const char* key, double def = 0.0);
int         opt_int(const json& o, const char* key, int def = 0);
bool        opt_bool(const json& o, const char* key, bool def = false);

} // namespace liveplay::events
