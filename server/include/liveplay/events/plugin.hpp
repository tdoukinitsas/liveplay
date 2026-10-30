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

#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
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
    // Pause / resume an item that is loaded. False when it has no cue.
    virtual bool pause_item(const std::string& uuid) = 0;
    virtual bool resume_item(const std::string& uuid) = 0;
    // Arm an item as "Up Next", as the operator does.
    virtual bool arm_next(const std::string& uuid) = 0;
    // A playing cue's level relative to its own volume, in dB: 0 = as set,
    // negative = ducked. nullopt when it isn't playing. Setting it changes
    // only the playing cue, never the item's saved volume.
    virtual std::optional<double> cue_offset_db(const std::string& uuid) = 0;
    virtual bool set_cue_offset_db(const std::string& uuid, double db) = 0;

    // ---- The mixer ----
    // The project's buses as the document holds them: [{ id, name, gainDb,
    // mute, pan, sends: [{ id, levelDb, tap }], output: { type, target },
    // dsp, master, preview }].
    virtual json buses() = 0;
    // Logical output names a bus can be routed to.
    virtual std::vector<std::string> outputs() = 0;
    // For the steps of a fade: gainDb, pan or dsp heard at once, but neither
    // saved nor sent to the clients. Follow with patch_bus at the end.
    virtual bool set_bus_live(const std::string& id, const json& patch) = 0;
    // A lasting change (any PATCH /api/buses field): saved in the project and
    // sent to every client. Returns "" or why the mixer refused it.
    virtual std::string patch_bus(const std::string& id, const json& patch) = 0;
    // Master gain; `final` sends the change to the clients (not every step of a fade).
    virtual double master_gain_db() = 0;
    virtual void set_master_gain_db(double db, bool final) = 0;
};

// A device found on the network by Plugin::discover().
struct Discovered {
    std::string label;                 // "Thomas's XR12 — XR12 at 192.168.1.105"
    std::string name;                  // suggested connection name ("Thomas's XR12")
    json        config = json::object();   // values for your connectionFields
};

// A live option list for a field with "optionsFrom" (Plugin::options()).
struct OptionsResult {
    json        options = json::array();   // [{ "value", "label" }, ...]
    // Shown under the field when set. Return whatever options you still can
    // (static ones, without names) alongside it: a show must stay editable
    // with the device switched off.
    std::string error;
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

    // ---- Optional capabilities (docs/EVENT_PLUGINS.md, "Capabilities") ----

    // Find devices on the network for the connection editor's "Find on
    // network" button. Declare it with "discoverable": true in the schema.
    // Return within `budget`; runs on a REST thread.
    virtual std::vector<Discovered> discover(std::chrono::milliseconds budget) {
        (void)budget;
        return {};
    }

    // Options for a field declared with "optionsFrom": "<source>", read live
    // from the device: strip names, snapshot names, the parameters of the
    // effect loaded in a slot. `connection` is the connection's config (the
    // draft one while it's being edited); `values` holds the form's other
    // values, so a list can depend on them (declare "dependsOn"). Runs on a
    // REST thread; keep it to a bounded wait.
    virtual OptionsResult options(const std::string& source, const json& connection,
                                  const json& values) {
        (void)connection; (void)values;
        OptionsResult r;
        r.error = "no option source '" + source + "'";
        return r;
    }
};

using PluginFactory = std::unique_ptr<Plugin> (*)(Host& host);

// ---- Built-in plugins (builtin_plugins.cpp lists them) ----------------------
std::unique_ptr<Plugin> make_osc_plugin(Host& host);
std::unique_ptr<Plugin> make_midi_plugin(Host& host);
std::unique_ptr<Plugin> make_http_plugin(Host& host);
std::unique_ptr<Plugin> make_liveplay_plugin(Host& host);
std::unique_ptr<Plugin> make_xair_plugin(Host& host);

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
