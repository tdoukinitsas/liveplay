# Event plugins

An **event** makes a cue drive something outside LivePlay: a lighting desk, a video server, a mixer, a web API. The operator attaches events to an audio item in the Properties panel's **Events** tab. Each event says *when* it fires, *which plugin* carries it out, *what action*, and *where to* (a **connection**). The server does all the firing, so events work with no client connected and fire exactly once, whatever the number of clients.

A **plugin** is one C++ class compiled into the server. LivePlay ships four:

| id | Does | Connection |
|---|---|---|
| `osc` | Sends an OSC 1.0 message over UDP | host, port |
| `midi` | Note on/off, control change, program change, MIDI Show Control, raw SysEx through RtMidi | output port name (on the server machine) |
| `http` | HTTP request (method, path, headers, body); plain `http://` only | base URL, headers, timeout |
| `liveplay` | Plays or stops other cues, or Stop All | none |

This guide covers everything needed to add a fifth. A typical plugin is one file of 100–250 lines, and **it needs no client code**: the client builds every form from the schema your plugin publishes.

---

## 1. How an event flows

```
 Project document                          Server
 ─────────────────                         ────────────────────────────────────────────
 eventConnections: [ {id, plugin, config} ]
 items[].events:   [ {trigger, time, plugin, action, connectionId, options} ]
                                 │
           item starts playing   ▼
        ProjectState::play_item ── snapshots the item's events ─┐
                                                                ▼
        sequencer (every 50 ms) ── transport changes → start / pause / resume / end
                                ── playhead within 75 ms of a "time" marker
                                   → schedule it for exactly when it falls due
                                                                │
        ProjectState::dispatch_event_fires                      │
            resolve connectionId → the connection's config      │
            fill {placeholders} in options and config           ▼
        events::Dispatcher ── scheduler thread holds future jobs until due
                           ── one LANE (thread) per plugin; jobs run in order
                                                                │
        YourPlugin::execute(job) ───────────────────────────────┘
                                                                │
        Result → doc_patch "event_fired" { itemUuid, eventId, ok, error } to every client
```

Code map:

| File | What |
|---|---|
| `server/include/liveplay/events/plugin.hpp` | **The plugin API**: `Plugin`, `Job`, `Result`, `Host`, `FireContext`, schema helpers and option accessors. The only header a plugin includes. |
| `server/src/events/plugins/*.cpp` | One file per plugin. Read `osc.cpp` first: it is the smallest complete example. |
| `server/src/events/builtin_plugins.cpp` | The list of plugins the server loads, in display order. |
| `server/include/liveplay/events/events.hpp`, `server/src/events/dispatcher.cpp` | Dispatcher, scheduling, lanes, placeholder substitution. Plugin authors don't touch these. |
| `server/src/core/project_state.cpp` | Sequencer integration (`track_events`, `collect_event_fires`, `dispatch_event_fires`), `EventHost`, connection storage and the `customActions` migration. |
| `client/app/components/SchemaForm.vue` | Renders the schema. If your plugin needs a field type it doesn't support, that's the one place to add it. |

---

## 2. The contract

### `Plugin`

```cpp
class Plugin {
public:
    virtual std::string id() const = 0;           // "osc". Stored in every saved event: never change it.
    virtual json        schema() const = 0;       // what the client renders (section 3)
    virtual Result      execute(const Job& job) = 0;
    virtual void        stop() {}                 // optional: release sockets/ports/threads at shutdown
};
```

### `Job`: what `execute()` receives

| Member | Contents |
|---|---|
| `action` | One of your schema's action ids. |
| `options` | The event's values for that action's fields, with `{placeholders}` already filled in. |
| `connection` | The chosen connection's `config` (values of your `connectionFields`), placeholders filled in. `{}` when `needsConnection` is false. |
| `ctx` | `FireContext`: `item_uuid`, `item_name`, `event_id`, `trigger` (`time`/`start`/`end`/`pause`/`resume`/`test`), `time`. |

You never look anything up in the project. The connection has been resolved, checked as enabled, and checked as belonging to your plugin before `execute()` runs. A missing or disabled connection is reported to the operator without reaching you.

### `Result`: what you return

```cpp
return {};                                                  // success
return Result::fail("MIDI output 'loopMIDI Port' not found"); // failure
```

The `error` string is shown to the operator beside the event (a red dot with a tooltip) and logged by the server. Write it for an operator at a show, not a developer: say what is wrong and with what, e.g. *"can't resolve 'desk.local'"*, *"HTTP 404 from http://10.0.0.5/api/go"*.

### Option values are untrusted

`options` and `connection` come from a JSON document that a client, an API call or a hand-edited file may have written. Numbers may arrive as strings, fields may be missing, values may be out of range. Always read them through the accessors, which return your default when a value is missing or unusable:

```cpp
const std::string host = opt_string(job.connection, "host");
const int port         = opt_int(job.connection, "port", 0);      // "8000" works too
const double level     = opt_number(job.options, "level", 1.0);
const bool loop        = opt_bool(job.options, "loop", false);
```

Then validate and clamp (`std::clamp`), and fail with a clear message rather than sending garbage.

### `Host`: acting on LivePlay itself

A factory receives `Host&`. It is the only way a plugin may affect LivePlay:

```cpp
host.play_item(uuid);   // like GO / a cart button
host.stop_item(uuid);   // with its stop fade
host.stop_all();        // Stop All
```

These calls are thread-safe and may block briefly. Most output plugins ignore `Host`. If a plugin needs more (for example, reading the selected cue), add a small verb-shaped method to `Host` in `plugin.hpp` and implement it in `EventHost` in `project_state.cpp`. Do not hand plugins `ProjectState` itself.

### Threading rules

1. **`execute()` must be thread-safe.** It runs on your plugin's lane (one thread; your jobs run one at a time, in order) *and* on HTTP request threads when an operator presses **Test**. Guard any state you keep (sockets, open ports, caches) with a `std::mutex`. See `OscPlugin::execute` and `MidiPlugin::send`.
2. **Block only for a bounded time.** A network round trip with a timeout is fine (`http` defaults to 2 s). Waiting forever is not: it stalls every later job on your lane. Other plugins have their own lanes and are unaffected.
3. **`schema()` must be cheap and must not throw** for a missing device. It runs on every `GET /api/events/plugins`. Listing ports is fine; connecting to something is not.
4. **Exceptions** escaping `execute()` are caught and reported as failures, but prefer returning `Result::fail(...)`.
5. **`stop()`** runs once at shutdown, after every lane has finished. Close what you opened.

### Timing guarantees

- A `time` marker is scheduled from the sequencer's playhead reading up to 75 ms ahead, then handed to your lane when due. Measured delivery is within a few ms of the mark (`server/tests/e2e/events-e2e.js` asserts ±40 ms).
- Markers before where playback starts don't fire. Neither do markers jumped over by a forward seek. A loop or backward seek re-arms the markers ahead of the playhead. Pausing cancels markers that were scheduled but not yet due; resuming re-schedules them.
- `start` fires when the item is played. `end` fires once per play: at natural end, when stopped (including Stop All), or when a crossfade hands over to the next cue. `pause` and `resume` fire on transport changes.

If your action takes time to *start* (a TCP handshake, say), connect lazily on the first job and keep the connection open, so later cues don't pay that cost.

---

## 3. Schema reference

`schema()` returns:

```jsonc
{
  "id": "osc",                       // must equal id(); checked at boot (the plugin is rejected otherwise)
  "label": "OSC",                    // shown in pickers
  "icon": "settings_input_antenna",  // a Material Symbols name: https://fonts.google.com/icons
  "description": "One line: what it talks to.",
  "needsConnection": true,           // false: events have no connection picker (like `liveplay`)
  "connectionFields": [ Field, ... ],
  "actions": [
    { "id": "message", "label": "Send message",
      "summary": "{address}",        // optional one-line list summary; {fieldId} is replaced by the value
      "fields": [ Field, ... ] }
  ]
}
```

A **Field** (build it with `field(id, label, type, extra)`):

| Key | Meaning |
|---|---|
| `id` | Key in `options` / `config`. Stored in projects: never rename it. |
| `label` | Shown beside the input. |
| `type` | One of the types below. |
| `default` | Filled in when an event or connection is created, or the action changes. Always give one. |
| `required` | Marks the field in the form. Your `execute()` must still validate. |
| `min`, `max`, `step` | For `number` / `integer`. |
| `placeholder`, `help` | Input placeholder; a help line under the input. |
| `options` | For `enum`: `[{ "value", "label" }]`; build with `options_list({{"go","GO"}, ...})`. |
| `allowCustom` | For `enum`: also accept a typed value, e.g. a MIDI port that isn't plugged in right now. |
| `fields` | For `list`: the fields of one row. |
| `showIf` | `{ "field": "<sibling id>", "equals": [values...] }`: show only when that sibling has one of these values. |

| `type` | Input | Value in `options` |
|---|---|---|
| `string` | single-line text | string |
| `text` | multi-line text | string |
| `number` | number input | number |
| `integer` | number input, whole numbers | number |
| `boolean` | toggle | bool |
| `enum` | select (combo box with `allowCustom`) | the option's `value` |
| `item` | picker of the project's cues and groups | item uuid |
| `list` | repeatable rows, each its own sub-form | array of objects |

**Placeholders.** Every string in `options` and in the connection's `config` may contain `{item.name}`, `{item.uuid}`, `{trigger}` and `{time}` (seconds, 3 decimals). They are substituted before `execute()` sees the job, so plugins get final values. Don't explain them in a field's `help`: the client shows one translated hint under every action with text fields.

**Labels are English.** Schemas come from the server and are not localised. Keep labels short and use standard industry terms (GO, CC, Universe).

---

## 4. Adding a plugin, step by step

Say you are adding `artnet` (DMX over Art-Net).

1. **Create `server/src/events/plugins/artnet.cpp`** from the template in section 5.
2. **Declare the factory** in `server/include/liveplay/events/plugin.hpp`, beside the others:
   ```cpp
   std::unique_ptr<Plugin> make_artnet_plugin(Host& host);
   ```
3. **Register it** in `server/src/events/builtin_plugins.cpp` (the list order is the order the client offers plugins in):
   ```cpp
   make_liveplay_plugin,
   make_artnet_plugin,
   ```
4. **Compile it**: add `src/events/plugins/artnet.cpp` to the `# Event system` block of `add_executable(liveplay-server ...)` in `server/CMakeLists.txt`.
5. **Dependencies**, if any: add the vcpkg port to `server/vcpkg.json` (no `version>=` unless it is genuinely needed; the baseline pins versions). Then `find_package` it and add it to `target_link_libraries` in `server/CMakeLists.txt`. Prefer a header-only library or what is already linked: Crow brings standalone **asio**, enough for UDP/TCP (`osc.cpp` uses it). On Linux, a library that needs system dev packages must also be installed by both workflows in `.github/workflows/`. On Windows, a DLL dependency ships automatically (the installer copies every `*.dll` next to the server); check it with a packaged build.
6. **Build**: `npm run server:build`. At boot the server logs and rejects a plugin whose schema `id` doesn't match `id()`, or whose id is already registered.
7. **Try it**: open a project, add a connection under **Settings → Connections**, press **Test**, then add an event in a cue's **Events** tab and play the cue. Or use the API (section 6).
8. **Test it**: extend `server/tests/e2e/events-e2e.js` or add a sibling suite with a local listener for your protocol (the OSC and HTTP checks show the pattern: bind a UDP or TCP listener on port 0, point a connection at it, assert on what arrives). Add it to the table in `server/tests/e2e/README.md`.
9. **Document it**: add a row to the table at the top of this file. The API reference (`docs-site/public/api/reference.json`) needs no change for a new plugin; it does for a new route.

No client change is needed unless you need a new field *type*. In that case, add it to `FieldSchema['type']` in `client/app/types/events.ts`, render it in `client/app/components/SchemaForm.vue`, and handle its default in `client/app/utils/events.ts`.

---

## 5. Template

```cpp
// =============================================================================
// events/plugins/example.cpp — <one line: what it talks to and over what>
// -----------------------------------------------------------------------------
// <Why it exists / what gear speaks this. Any protocol quirks worth knowing.>
// =============================================================================
#include "liveplay/events/plugin.hpp"

#include <mutex>

namespace liveplay::events {
namespace {

class ExamplePlugin final : public Plugin {
public:
    std::string id() const override { return "example"; }

    json schema() const override {
        return json{
            {"id", "example"},
            {"label", "Example"},
            {"icon", "extension"},
            {"description", "What it talks to."},
            {"needsConnection", true},
            {"connectionFields", json::array({
                field("host", "Host", "string",
                      {{"default", "127.0.0.1"}, {"required", true}}),
                field("port", "Port", "integer",
                      {{"default", 9000}, {"min", 1}, {"max", 65535}}),
            })},
            {"actions", json::array({
                json{{"id", "go"}, {"label", "Go"}, {"summary", "cue {cue}"},
                     {"fields", json::array({
                         field("cue", "Cue", "string", {{"default", "1"}}),
                     })}},
            })},
        };
    }

    Result execute(const Job& job) override {
        const std::string host = opt_string(job.connection, "host");
        const int port = opt_int(job.connection, "port", 0);
        if (host.empty() || port <= 0 || port > 65535)
            return Result::fail("Example connection needs a host and a port");

        if (job.action == "go") {
            const std::string cue = opt_string(job.options, "cue");
            if (cue.empty()) return Result::fail("no cue number");
            std::lock_guard lock{mutex_};   // execute() also runs on Test threads
            // ... send it; on failure:
            // return Result::fail("couldn't reach " + host + ": " + why);
            return {};
        }
        return Result::fail("unknown Example action '" + job.action + "'");
    }

    void stop() override {
        std::lock_guard lock{mutex_};
        // close sockets / ports
    }

private:
    std::mutex mutex_;
};

} // namespace

std::unique_ptr<Plugin> make_example_plugin(Host&) { return std::make_unique<ExamplePlugin>(); }

} // namespace liveplay::events
```

Style: match the existing plugins. Use a header block that says what the plugin talks to and why. Comments explain *why*, not what. Validate early and fail with operator-readable errors.

---

## 6. Testing through the API

All routes need the usual credentials when accounts are on (`Authorization: Bearer <token>`).

```sh
# Schemas, as the client sees them
curl http://127.0.0.1:4480/api/events/plugins

# Fire one event now, against an unsaved connection
curl -X POST http://127.0.0.1:4480/api/events/test -H 'content-type: application/json' -d '{
  "event": { "plugin": "osc", "action": "message",
             "options": { "address": "/test", "args": [{ "type": "i", "value": 1 }] } },
  "connection": { "id": "draft", "plugin": "osc", "config": { "host": "127.0.0.1", "port": 9000 } }
}'
# → { "ok": true }  or  { "ok": false, "error": "..." }

# Connections of the open project
curl http://127.0.0.1:4480/api/events/connections
curl -X PUT http://127.0.0.1:4480/api/events/connections -H 'content-type: application/json' \
     -d '{ "connections": [ { "id": "c1", "name": "Desk", "plugin": "osc", "config": { "host": "10.0.0.5", "port": 8000 } } ] }'
```

Use a spare port and a scratch data directory for a test server (`liveplay-server --port 4517 --data-dir <tmp>`), never 4480: that may be someone's running show. Every fire is logged as `EVENT: <trigger> '<item>' <plugin>/<action> ok|failed: <why>`, and broadcast to clients as the `event_fired` doc_patch.

---

## 7. Compatibility rules

Saved projects store your plugin's `id`, action ids, field ids and option values. Once released:

- **Never rename or remove** a plugin id, action id or field id. To retire one, keep accepting it in `execute()`, and drop it from the schema only when nothing can still be using it.
- **Add** fields with a `default`, and treat a missing value as that default in `execute()` (the accessors' `def` argument): older events won't have it.
- **Change a meaning** only by adding a new action or field.
- Connection `config` follows the same rules.

---

## 8. Data shapes (reference)

```jsonc
// Project root
"eventConnections": [
  { "id": "uuid", "name": "Lighting desk", "plugin": "osc", "enabled": true,
    "config": { "host": "192.168.1.50", "port": 8000 } }
]

// On an audio item
"events": [
  { "id": "uuid", "name": "Chorus", "enabled": true,
    "trigger": "time",            // time | start | end | pause | resume
    "time": 62.5,                 // file seconds, "time" only (same timeline as inPoint/outPoint)
    "plugin": "osc", "action": "message", "connectionId": "uuid",
    "options": { "address": "/eos/cue/12/go", "args": [] } }
]
```

Before 2.6, items could carry `customActions` (`play-item`, `play-index`, `stop-all`, `http-request`). The server migrates them into `events` on load; an HTTP target becomes a connection per origin. See `migrate_custom_actions` in `project_state.cpp`.

REST and WebSocket details: `GET/PUT /api/events/connections`, `GET /api/events/plugins` and `POST /api/events/test`, plus the doc_patch ops `event_fired` and `event_connections_changed`, are all in the API reference (`docs-site/public/api/reference.json`, published with the docs site).
