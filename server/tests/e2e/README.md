# Audio and server integration checks

These scripts drive server HTTP/WebSocket interfaces. Many measure real rendered audio and need a working playback device; a silent render path cannot validate routing or dynamics.

Use a dedicated test server and disposable projects. Suites can change the active project, routing, output maps, users, preferences or boot files. Keep their console output to diagnose failures.

## Setup

```sh
# 1. Build the server.
npm run server:build

# 2. Generate the test signal (~23 MB, gitignored — do not commit it).
node server/tests/e2e/gen-signal.js /tmp/liveplay-test-signal.wav

# 3. Start a server on a spare port, then drive it.
server/build/Release/liveplay-server.exe --port 4500 &
node server/tests/e2e/pfl-e2e.js        4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/roles-e2e.js      4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/filters-e2e.js    4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/eq-analyser-e2e.js 4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/gate-e2e.js       4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/comp-e2e.js       4500 /tmp/liveplay-test-signal.wav

# The stereo-image checks need their OWN signal: the one above is identical on
# both lanes, so it has no side content for a width control to act on.
node server/tests/e2e/gen-wide-signal.js /tmp/liveplay-wide-signal.wav
node server/tests/e2e/width-e2e.js      4500 /tmp/liveplay-wide-signal.wav
node server/tests/e2e/reroute-e2e.js    4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/busbus-e2e.js     4500 /tmp/liveplay-test-signal.wav /tmp/liveplay-wide-signal.wav
# Aux sends (M1). The claim it exists for is the tap point: a pre-fader send
# must NOT follow the source's fader or mute, which is what a foldback depends
# on and what no amount of reading the code can confirm.
node server/tests/e2e/sends-e2e.js      4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/migration-e2e.js  4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/save-churn.js     4500 /tmp/liveplay-test-signal.wav <projDir> <serverLog>
node server/tests/e2e/materialise-skip.js 4500

# CORS preflight, REST cue play/stop, manual stop fades, preview state (#56 #60
# #62 #65). Writes its own short signals to a temp dir; closes the project.
node server/tests/e2e/transport-fixes-e2e.js 4500
# Loop crossfade, measured on the cue meter against a plain-loop control.
node server/tests/e2e/loop-xfade-e2e.js 4500

# Device-name matching: a renumbered Windows device is found, a mapping to an
# absent card is unbound (not the default device). Saves/restores outputs.json.
node server/tests/e2e/device-match-e2e.js 4500

# Needs no audio device and no signal — nothing here plays.
node server/tests/e2e/settings-registry-e2e.js 4500

# Writes and moves its own temp project tree, and closes the project when done.
node server/tests/e2e/project-folder-e2e.js 4500 /tmp/liveplay-test-signal.wav

# Needs a device present to have something to materialise; skips cleanly if none.
node server/tests/e2e/output-materialise-e2e.js 4500
node server/tests/e2e/absent-device-e2e.js 4500 /tmp/liveplay-test-signal.wav

# Writes outputs.json (and restores it) — do not run it against a rig whose
# output map you care about while something else is using the same server.
node server/tests/e2e/ltc-output-e2e.js 4500 /tmp/liveplay-test-signal.wav

# Opens two sockets and compares what each one is sent. Restores the
# installation locale on the way out — other suites share this server.
node server/tests/e2e/session-prefs-e2e.js 4500 /tmp/liveplay-test-signal.wav

# Start their own servers (they test boot configuration), so they take the
# binary rather than a port. Both default to the Release build.
node server/tests/e2e/fs-jail-e2e.js
node server/tests/e2e/boot-config-e2e.js
node server/tests/e2e/client-session-e2e.js

# Owns users.json beside the binary for the duration (it has to create real
# accounts), and moves any existing one aside and back. Also restarts the
# server mid-run, to prove a token issued before the restart still works.
node server/tests/e2e/auth-e2e.js

# Owns users.json AND the prefs/ directory beside the binary, and moves both
# aside and back. Restarts the server mid-run to prove a profile persists.
node server/tests/e2e/user-prefs-e2e.js

# Owns liveplay.json as well, and restarts the server four times — it is
# testing boot-time provenance, an environment override and the config lock.
node server/tests/e2e/server-config-e2e.js
```

What each one pins, in the bus-role model (round 2, D24–D36):

- `pfl-e2e.js` — PFL and pre-listen land on the **preview-role** bus (stock id `preview`, name
  "Preview", built-in output `Preview Out`, silent until mapped or given a device name); the tap
  is pre-fader / pre-mute; the house (masters 0/1) never moves; mapping `Preview Out` or naming a
  present device binds it on the reserved pair; an absent device name leaves it silent, never on
  the default device.
- `roles-e2e.js` — the role model itself: `GET /api/outputs` `builtin`, `bound` / `masters` on
  every bus, the whole 409 matrix (delete a holder, drop a role, both roles on one bus, fed bus
  as preview, bus-kind bus as a holder, feeding the preview bus, master/preview to a bus, the
  retired `type:"master"` still accepted), and the two safety claims: **moving the preview role
  moves the PFL tap and the reserved pair** (old holder off the pair and silent, pre-listen
  stopped, house untouched) and **moving the master role re-wires the house pair** (items with
  no `busId` fall back to the new holder, the house follows, the old holder sits on a pool pair).
- `busbus-e2e.js` — bus→bus routing; "to master" is a bus→bus send into the master-role bus.
- `migration-e2e.js` — a pre-bus document gets the D35 defaults; `deviceOverride` → buses; a
  round-1 document (`busSchema:1`, system Main/Monitor, a user bus of the retired `master` kind,
  `settings.previewDevice`) takes the role migration and plays out the house; `project_migrated`
  carries `rolesMigrated` / `previewDeviceMigrated` on every client.
- `width-e2e.js` — also covers mono-check on the preview bus via both `/api/monitor/mono` and
  its alias `/api/preview/mono`.
- `materialise-skip.js` — a project's own buses are the ones **without a role**; a document with
  no `busSchema` comes up on `master` + `preview` alone.
- `settings-registry-e2e.js` — the `settings` object is validated against a registry, not written
  through verbatim: unknown keys, wrong types and out-of-vocabulary enum values are dropped,
  numbers clamp, and a dropped key does **not** fire its live side effect (a rejected
  `disableLimiter` must not be heard while never being stored). The other half is that invalid
  input must not *break* anything — a stray key still returns 200 and the good keys in the same
  patch still land, because the client PATCHes its whole settings object on every edit. Also pins
  that the derived `outputTargetLevels` still arrives on every read but is never written to disk.
- `absent-device-e2e.js` — a bus whose output names hardware this machine does not have goes
  **silent**, rather than to the default device as `open_device_by_name()` would (§0.8). Measured
  on the meters, because the claim that matters is about where the audio went, not what a flag
  says. Note which assertion carries the weight: an ordinary Output-kind bus takes a *pool* pair,
  so the old fallback surfaced there and not on the house pair — the house check passes against
  the unfixed build too, and is kept only because it is the claim a reader will assume is being
  made. Includes a contrast phase (the same bus and cue re-pointed at Main Out, which must be
  loud) so "silent" cannot pass on a dead harness, an unloaded wav or a cue that never fired.
  Also pins that the preview bus stays strict even on Main Out.
- `ltc-output-e2e.js` — the same rule for timecode (§0.9, D38). `settings.ltcDevice` migrates to
  `settings.ltcOutput` and is erased, the migration is *reported*, and an output this machine
  cannot resolve makes timecode **silent** instead of handing it to the default device — which is
  the house, and an LTC squeal over the programme. The cue under test sits on a bus that is itself
  unbound, so programme audio reaches nothing and anything the meters see is the LTC channel
  alone; that separation is what makes LTC measurable at all here. Contrast phase maps a real
  output and checks timecode does arrive (reads about −6 dBFS on a pool pair), then clears the
  output and checks the feed comes down. Also pins that a legacy `ltcDevice` **patch** lands on
  `ltcOutput` without storing a second copy, and that the saved file names no sound card.
  Writes `outputs.json` and restores it on the way out.
- `client-session-e2e.js` — a connection is somebody, not a pointer in a set (U1). Two sockets are
  two sessions with their own ids and rows; a disconnect removes exactly its own; ids are never
  reused. Also pins the one refactor that could have broken a client silently — the "needs a
  playback_snapshot" flag moved from a parallel set into the session record, and nothing else here
  would notice if it were never read. The second half closes a gap S2 left: CORS does not cover
  WebSockets, so `--cors-origin` used to restrict REST while the socket stayed open to any page.
  A foreign `Origin` is now refused with 403, the configured one admitted, and an upgrade with no
  `Origin` admitted (native clients send none, and a browser cannot suppress its own).
  Starts its own servers, so it takes the binary rather than a port.

- `session-prefs-e2e.js` — language and meter rate belong to the person at the surface, not the
  rig (U2). One client's `set_locale` reaches that client and **nobody else**; the installation
  default still travels, but only to sessions that never chose for themselves. A client asking for
  5 Hz gets ~10 meter frames where an unchanged one gets ~60, and a rate above the server's own
  tick is clamped and *said*. The assertion that carries the weight is the last one: a client
  thinned to 1 Hz must still hear every `cue_state` edge. Bypassing that — thinning edges along
  with samples — leaves it with **zero** transport frames, so it never learns the cue played at
  all, while every meter-count assertion still passes.
- `auth-e2e.js` — who may talk to this server, and what they may change (U3). Nearly every
  assertion here is **negative**, because a refusal that silently does not happen looks exactly
  like the feature working. It starts from the default posture — no `users.json`, everything open,
  which is what every release before 2.5 did and what an upgrade must keep doing — then creates the
  first account (possible with no credential, because there is nobody to be an administrator yet,
  and forced to `admin` however it was asked for) and pins what changes.
  Three claims carry the weight. **The socket is checked too**: Crow runs middleware for an upgrade
  and then hands over the connection regardless of what the middleware did to the response, so
  without the check in `.onaccept` the WebSocket — play, stop, bus gain, mute, selection — would be
  reachable with no token while REST was locked. **Default deny**: a path matching no route needs a
  token as well, so the route table cannot be mapped anonymously; bypassing that one line turns
  eight assertions red at once. **Tokens survive a restart**, which is the whole reason they are
  signed rather than remembered — the crash handler auto-restarts this server, and in-memory tokens
  would sign every surface out mid-show. Also pins the role split (an operator is refused
  `/api/outputs`, `/api/users` and `/api/clients` and allowed everything that runs the show),
  that a wrong password and an unknown user give the *same* reply so accounts cannot be enumerated,
  that an edited token is refused, that a password change or a deletion invalidates that user's
  live tokens, and that the last administrator cannot be deleted.
  Note when reading a red run: bypassing the socket check turns two assertions red, not one — the
  second is `/api/clients` reporting the session as `user: null`, because the principal is attached
  during the same handshake that authenticates it.
- `user-prefs-e2e.js` — what belongs to the person rather than the show (U4). Four values left the
  `.liveplay` document — the theme, the transport keymap, the meter's display unit, and
  scroll-to-playing — and the risk of the unit is entirely in the migration: the values are dropped
  from the file on the next save, so a seed that does not happen is a keymap that is simply gone.
  The suite pins that a 2.4 document still loads, that its four values are **counted and reported**
  through the existing `project_migrated` banner so the file changing shape is never silent, that
  the first read of `/api/prefs` seeds a profile from whatever project is open, that a second read
  does **not** re-import the document over a choice made since, and that `save()` then drops all
  four while `cartSlotKeys` survives — a cart wall is the show's layout, and the slot that fires
  the door slam has to be the same slot for whoever is at the desk tonight.
  Two claims are worth knowing about. **An empty seed writes nothing**: a client reads its
  preferences as soon as its socket comes up, which on the ordinary startup order is before any
  project is open, so creating a profile then would spend that person's one chance at the
  migration on an empty desk — bypassing it turns two assertions red, the second being an operator
  who never gets their keymap at all. **A profile is the caller's**: no route names a user id, an
  administrator reading `/api/prefs` gets their own, and two spellings of a per-user route are
  asserted to 404 so that adding one later fails here first.
  Also pins that the relocated settings keys are now dropped by the registry rather than refused (a
  2.4 client still works), that `PATCH /api/project/theme` is **gone** rather than answering 200 and
  losing the value at the next save, that validation drops key by key — one malformed binding costs
  that binding, not the keymap — that a null clears a key so "go back to following the project" can
  actually be said, that a profile survives a restart, and that deleting an account deletes it.
- `server-config-e2e.js` — the machine's own settings, editable from the settings page (P3a).
  S3 gave `liveplay.json` a reader and said the server would never write it; this adds a writer,
  which is only not a contradiction because of how it behaves. The suite pins that **sparse stays
  sparse**: patching one key writes one key, so a file with two keys in it does not become a file
  with thirteen and an installation keeps taking improved defaults for everything nobody chose.
  A `null` **clears** a key, which is how "stop pinning this" is said — without it a value could be
  changed but never un-chosen, and the file would fill up one edit at a time until every default
  was frozen.
  The claim that carries the unit is that **the page cannot lie about provenance**. The desktop app
  always launches the server with `--port`, so a port field that accepted an edit and said nothing
  would write the file, report success and change nothing; every field reports which tier supplied
  the value in force, and bypassing the flag's own record of that turns three assertions red.
  Being overridden does not make a field read-only — the stored value is what applies once the flag
  goes, so the response separates "waiting for a restart" from "shadowed at launch", two facts with
  different fixes.
  **The lock is a lock (R3).** `--lock-server-config` refuses every write with 403 including from an
  administrator, and cannot be turned off through the API it locks — a lock an admin can pick over
  the network is not one. Bypassing it turns three red.
  Also pins that this is **admin-only**: an operator is refused, and bypassing that gate lets one
  rewrite `corsOrigin`, which is the escalation the gate exists to stop. Note when reading that red
  run: "reading the config needs a token at all" still passes, because `access_for`'s default-deny
  is a second layer underneath the admin rule.
- `client-session-e2e.js` note when reading a red run: bypassing the `onclose` erase does not
  merely leave a stale row —
  the broadcast thread then writes to freed connections and the server **crashes**, which takes
  several later assertions with it. The erase is load-bearing against a use-after-free, not
  bookkeeping.
- `output-materialise-e2e.js` — pointing a bus at a device present on this host adds a logical
  output of that name to `outputs.json` instead of leaving the document holding a bare device
  reference (O2). Also pins the three cases it must *not* fire on: a repeat of the same pick, a
  name matching no present device (inventing an entry for absent hardware would be a lie about
  the machine — since §0.8 that name resolves to silence, not to a device), and a built-in,
  whose unmapped meaning D26 defines. Restores the map it found.
- `project-folder-e2e.js` — `folderPath` is derived from where the `.liveplay` file is found and
  is **not** written into it, so a project that has been moved, mailed or unzipped somewhere else
  still resolves its media. Pins the saved file having no such key, a moved project reporting and
  resolving against its new location, a save-as re-anchoring the folder *and* `media_root` (before
  this, the next import landed in the folder just saved away from), and a pre-2.5 file that still
  carries the field being corrected rather than believed. Deliberately does not pin what a save-as
  does to media: it does not copy the media folder, so the copy's relative `media/…` points at
  nothing — true before this change too, and a question nobody has answered.
- `fs-jail-e2e.js` — the filesystem allow-list (`--fs-root`) and the configurable CORS origin
  (`--cors-origin`). Two halves, and the second matters as much as the first: that a confined
  server refuses paths outside its roots — including a traversal *out* of a root and a sibling
  directory whose name merely shares the root's prefix, the two cases a naive prefix test gets
  wrong — and that an **unconfigured** server still reaches the whole filesystem and still sends
  `*`, because those defaults are deliberate and a test that only proved the jail works would not
  notice if they had silently become restrictive.
- `boot-config-e2e.js` — `liveplay.json` and the precedence around it: a file alone configures a
  server with no flags at all, and `default < file < environment < command line` holds on every
  tier. Two halves are easy to miss. `--fs-root` must *replace* the file's roots, not extend them,
  or a flag could only ever widen an installation's jail; and every complaint has to be **said** —
  an unknown key, a wrong type, an out-of-range number, a malformed file and a `--config` naming
  nothing each get a named log line, because a silently ignored config file is worse than none at
  all, the operator believing the posture is set. Also pins that one bad key costs one key: the
  server still boots and the good keys in the same file still land.

The server holds the wav open while a project referencing it is loaded, so **stop the server
before regenerating the signal** or the write fails with EBUSY and the old file is used silently.

Exit code is non-zero if anything failed; each assertion prints PASS/FAIL with the levels it
measured, so a failure says *how far* out it was rather than just that it was.

## The latency probes

`latency-probe.js` and `control-latency-probe.js` are **measurements, not pass/fail harnesses** —
they print numbers and exit 0. They exist because the engine's output latency is not readable from
its configuration: the ring's depth is the latency, the device overrides the period it was asked
for, and whether any of it is safe depends on how long a render block actually takes.

```sh
npm run server:build
node server/tests/e2e/gen-signal.js test-signal.wav
node server/tests/e2e/gen-wide-signal.js test-wide.wav
npm run server:run -- --port 4500
```

In another terminal, supply the port and absolute signal path:

```sh
node server/tests/e2e/pfl-e2e.js 4500 /absolute/path/test-signal.wav
node server/tests/e2e/width-e2e.js 4500 /absolute/path/test-wide.wav
```

On Windows, use a path such as `C:/Tests/test-signal.wav`. The ordinary signal has identical stereo lanes; width checks need the separate wide signal. Do not commit generated audio.

## Suites using a running server

Unless shown otherwise, arguments are `<port> <wavPath>`.

| Script | Coverage / special arguments |
|---|---|
| `pfl-e2e.js` | Pre-fader/pre-mute listening and program isolation |
| `roles-e2e.js` | Master/Preview roles, binding and role moves |
| `filters-e2e.js` | Filter processing |
| `eq-analyser-e2e.js` | EQ and analyser delivery |
| `gate-e2e.js` | Gate behavior and timing |
| `comp-e2e.js` | Compression and gain reduction |
| `width-e2e.js` | Stereo width, balance and mono-check; use wide signal |
| `reroute-e2e.js` | Cue rerouting |
| `busbus-e2e.js` | Bus graph; `<port> <wavPath> <widePath>` |
| `sends-e2e.js` | Auxiliary send taps |
| `migration-e2e.js` | Supported project compatibility paths |
| `absent-device-e2e.js` | Missing hardware stays unbound |
| `ltc-output-e2e.js` | LTC destination binding; saves/restores output map |
| `project-folder-e2e.js` | Project folder/media resolution; creates and moves temp files |
| `session-prefs-e2e.js` | Per-connection locale/meter settings |
| `materialise-skip.js` | Bus materialization; `<port>` |
| `settings-registry-e2e.js` | Settings validation; `<port>`, no audio needed |
| `output-materialise-e2e.js` | Output materialization; `<port>`, skips without a device |
| `device-match-e2e.js` | Device identity; `<port>`, saves/restores output map |
| `transport-fixes-e2e.js` | Transport, manual fades, preview and CORS; `<port>`, creates signals |
| `loop-xfade-e2e.js` | Loop crossfade; `<port>`, creates signals |

These scripts generally assume an unauthenticated test server unless they explicitly manage authentication.

## Suites that start their own server

Each accepts an optional server executable path. Their default points at the Windows Release build; pass `server/build/liveplay-server` for a Ninja build.

| Script | Coverage / affected state |
|---|---|
| `fs-jail-e2e.js` | Configured filesystem roots |
| `boot-config-e2e.js` | Launch configuration |
| `client-session-e2e.js` | Connection tracking and handshake behavior |
| `auth-e2e.js` | Accounts, roles, tokens and restart; temporarily owns users file |
| `user-prefs-e2e.js` | Profiles and restart; temporarily owns users and prefs |
| `server-config-e2e.js` | Stored/effective configuration and lock; temporarily owns configuration |

Run these serially. Read their setup/cleanup code before using a binary directory that contains real installation data.

## Diagnostic probes

Probes print measurements for investigation; their results depend on hardware and workload.

| Script | Arguments |
|---|---|
| `control-latency-probe.js` | `<port> [eventsPerSec] [seconds]` |
| `latency-probe.js` | `<port> <wavPath> [maxItems] [soakSeconds]` |
| `ui-churn-probe.js` | `<port> <wavPath> [editsPerSec] [seconds] [items]` |
| `seam-bisect.js` | `<port> <wavPath> [repeats]` |
| `save-churn.js` | `<port> <wavPath> <projectDir> <serverLog>` |

For deterministic C++ checks, see the [CTest instructions](../../../docs/DEVELOPMENT.md#checks). Protocol contracts live in the [API reference](https://tdoukinitsas.github.io/liveplay/api/), not this test guide.
