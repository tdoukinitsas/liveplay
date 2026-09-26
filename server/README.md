# LivePlay Server — developer guide

`liveplay-server` is the headless C++20 audio engine and control surface that backs the LivePlay client. It owns the audio graph, the routing matrix, the loaded project file, and exposes a REST + WebSocket API. It runs as either a child process spawned by the desktop client (single-machine installs) or as a standalone daemon on a stage-side machine that the client connects to over the LAN.

This document is the developer's guide to the server. For end-user docs or the overall project orientation, see the [root README](../README.md). For the client-side, see [`client/README.md`](../client/README.md).

---

## Contents

- [Tech stack](#tech-stack)
- [Source layout](#source-layout)
- [Building](#building)
- [Running](#running)
- [Architecture](#architecture)
  - [Three-tier mixer](#three-tier-mixer)
  - [Multi-device routing matrix](#multi-device-routing-matrix)
  - [Brick-wall master limiter](#brick-wall-master-limiter)
  - [Per-cue LTC generator](#per-cue-ltc-generator)
  - [Real-time metering](#real-time-metering)
  - [Manual-stop fade-out contract](#manual-stop-fade-out-contract)
- [Control surface](#control-surface)
  - [Authentication](#authentication)
  - [Server configuration](#server-configuration)
  - [User preferences](#user-preferences)
  - [REST endpoints](#rest-endpoints)
  - [WebSocket frames](#websocket-frames)
- [Project state & file format](#project-state--file-format)
- [Threading model](#threading-model)
- [Adding features](#adding-features)
- [Debugging](#debugging)

---

## Tech stack

| Layer        | Library                                                                        |
|--------------|--------------------------------------------------------------------------------|
| Audio I/O    | [miniaudio](https://miniaud.io/) (header-only, vendored as `miniaudio_impl.c`) |
| HTTP / WS    | [Crow](https://crowcpp.org/)                                                   |
| Metadata     | [TagLib](https://taglib.org/)                                                  |
| JSON         | [nlohmann/json](https://github.com/nlohmann/json)                              |
| Build        | CMake (≥ 3.21) + vcpkg manifest mode (`vcpkg.json`)                            |

Native backends per platform:

| OS      | Backends (compiled in; runtime-selected)              |
|---------|-------------------------------------------------------|
| Windows | WASAPI (default), DirectSound, WinMM                  |
| macOS   | CoreAudio (frameworks: CoreAudio, AudioToolbox, AudioUnit, CoreFoundation) |
| Linux   | ALSA + PulseAudio (JACK optional, all compiled in)    |

ASIO is intentionally *not* enabled — the Steinberg SDK has redistribution terms incompatible with AGPL bundling. Build miniaudio with `MA_ENABLE_ASIO` locally if you need it.

---

## Source layout

```
server/
├── CMakeLists.txt
├── CMakePresets.json          presets: vs2022, default (Ninja), debug, macos, linux
├── vcpkg.json                 manifest — Crow, TagLib, nlohmann/json, libsodium
├── include/liveplay/
│   ├── audio/
│   │   ├── types.hpp          shared audio types (DeviceId, ChannelIndex, …)
│   │   ├── meter.hpp          VU + RMS ballistics + atomic publishers
│   │   ├── limiter.hpp        lookahead brick-wall limiter
│   │   ├── ltc_generator.hpp  procedural SMPTE LTC (24/25/29.97/30, DF + NDF)
│   │   ├── mixer_channel.hpp  Tier 2: virtual mixer strip
│   │   ├── playback_item.hpp  Tier 1: per-cue decoder + fade state
│   │   └── engine.hpp         Tier 3: master bus + device fan-out
│   ├── core/
│   │   ├── project_state.hpp  v2 project model + legacy 1.x upgrade
│   │   ├── output_map.hpp     logical output name → this machine's hardware
│   │   ├── user_store.hpp     accounts, Argon2id hashes, signed tokens
│   │   ├── user_prefs.hpp     what belongs to the person, not the show
│   │   ├── server_config.hpp  liveplay.json's schema, reader and writer
│   │   └── backup_manager.hpp on-save rotating backups
│   ├── meta/
│   │   ├── metadata.hpp       TagLib wrapper
│   │   └── waveform.hpp       offline downsample → peak JSON
│   ├── net/
│   │   ├── control_server.hpp Crow REST + WS surface
│   │   └── discovery.hpp      LAN announce / discovery (UDP)
│   ├── util/unicode_path.hpp  Windows-safe wide path helpers
│   ├── logger.hpp             ANSI-colour level logger
│   └── crash_handler.hpp      cross-platform signal/SEH crash dumps
└── src/                       implementations — same layout as headers
    ├── main.cpp               CLI parsing, banner, signal handling
    └── audio/miniaudio_impl.c the single TU that compiles miniaudio
```

---

## Building

### Prerequisites

- CMake ≥ 3.21
- A C++20 toolchain (MSVC 2022, Clang 15+, GCC 12+)
- vcpkg checkout with `VCPKG_ROOT` exported
- Ninja (recommended)
- **Linux extras**: `libasound2-dev libpulse-dev libjack-jackd2-dev libx11-dev pkg-config ninja-build`

### From the server directory

```sh
cd server
cmake --preset default                # Ninja Release; fetches vcpkg deps (~5 min first time)
cmake --build build --config Release -j
```

The binary lands at `build/liveplay-server` (or `build/Release/liveplay-server.exe` on Windows with the `vs2022` preset).

Useful presets:

| Preset   | Notes                                                                |
|----------|----------------------------------------------------------------------|
| `default`| Ninja Release. Cross-platform.                                       |
| `debug`  | Ninja Debug with assertions + symbols. Build dir: `build-debug`.     |
| `vs2022` | Multi-config Visual Studio generator. Build dir: `build`. Windows.   |
| `macos`  | Sets `CMAKE_OSX_DEPLOYMENT_TARGET` for the host arch.                |
| `linux`  | Forces Ninja + pkg-config.                                           |

### From the monorepo root

```sh
npm run server:configure         # one-time
npm run server:build             # rebuild
npm run server:run -- --verbose  # launch with debug logs
```

`npm run server:build` shells out to [`scripts/build-server.js`](../scripts/build-server.js), which selects the right preset for the host platform and is idempotent.

---

## Running

```
liveplay-server [options]
  -p, --port <port>         Port to listen on (default 4480)
  -b, --bind <addr>         Interface to bind (default 0.0.0.0)
      --pidfile <path>      Write JSON {pid,port,startedAt} after binding
      --start-delay-ms <n>  Wait <n> ms before binding (used by crash-restart)
      --meter-hz <n>        WebSocket meter push rate, 1-120 (default 30)
      --max-upload-mb <n>   Max upload size in MiB, 1-8192 (default 256)

  Engine (applied at boot — the engine cannot be re-initialised later):
      --mix-sample-rate <hz>    Mix sample rate, 8000-192000 (default 48000)
      --render-block <frames>   Render block size, 32-8192 (default 256)
      --ring-blocks <n>         Output latency in render blocks, 2-512
      --master-channels <n>     Master bus width, 4-1024 (default 32)
      --max-buses <n>           Max simultaneous mixer strips, 2-512 (default 64)
      --master-ceiling-db <db>  Limiter ceiling, -24.0-0.0 (default -0.3)

  Security (both default to the pre-2.5 behaviour, so an upgrade changes
  nothing until you set them):
      --fs-root <path>          Confine the filesystem API to <path>. Repeatable;
                                unset, the API can reach the whole filesystem
      --cors-origin <origin>    Access-Control-Allow-Origin value (default "*")

  -v, --verbose             Enable debug-level logging
  -h, --help                Show this help and exit
      --config <path>       Read boot configuration from <path> instead of
                            liveplay.json beside the executable

Environment:
  LIVEPLAY_CONFIG            Same as --config
  LIVEPLAY_PORT              Same as --port
  LIVEPLAY_MIX_SAMPLE_RATE   Same as --mix-sample-rate
  LIVEPLAY_RENDER_BLOCK      Same as --render-block
  LIVEPLAY_RING_BLOCKS       Same as --ring-blocks
  LIVEPLAY_MASTER_CHANNELS   Same as --master-channels
  LIVEPLAY_MAX_BUSES         Same as --max-buses
  LIVEPLAY_MASTER_CEILING_DB Same as --master-ceiling-db
  LIVEPLAY_METER_HZ          Same as --meter-hz
  LIVEPLAY_MAX_UPLOAD_MB     Same as --max-upload-mb
  LIVEPLAY_FS_ROOTS          Same as --fs-root, PATH-delimited (';' on Windows)
  LIVEPLAY_CORS_ORIGIN       Same as --cors-origin
  NO_COLOR=1                 Disable ANSI colour in logs
  FORCE_COLOR=1              Force colour even when stdout isn't a tty
```

Values that are unparseable or out of range are reported in the log and then
**ignored** — the built-in default stays in force rather than a typo silently
misconfiguring the engine. That holds whichever door the value came in by.

**`--cors-origin` also governs the WebSocket.** CORS is a rule browsers apply to
XHR and fetch; the WebSocket handshake is exempt from it, so a configured origin
used to restrict the REST surface while leaving the socket — which carries play,
stop, bus gain, mute and selection — open to any page the operator happened to
visit. The upgrade is now refused with **403** unless the `Origin` header matches
the configured value. Two deliberate exceptions: the default `*` admits
everything, so nothing changes for an installation that never set the flag; and
an upgrade carrying **no** `Origin` at all is admitted, because native clients,
Companion, `curl` and the Electron app send none. That is not the hole it looks
like — a browser cannot suppress its own `Origin`, which is exactly why the check
works, and an attacker who is not in a browser can reach the socket directly
either way. The check exists to stop a page the operator merely *visited* from
driving the rig.

### `liveplay.json`

Everything above can also be set in a `liveplay.json` beside the executable,
next to `outputs.json`. Precedence, lowest first:

```
built-in default  <  liveplay.json  <  environment  <  command line
```

The file is where an **installation** states its posture, the environment is
where a launcher varies it, and a flag is a person overriding both on purpose.
`--fs-root` on the command line therefore *replaces* the file's roots rather
than extending them — a flag that could only widen a jail and never narrow it
would not be an override.

```json
{
  "schema_version": 1,
  "port": 4480,
  "corsOrigin": "https://booth.example",
  "fsRoots": ["D:/Shows", "E:/Media"]
}
```

Keys are named after the flags (`port`, `bind`, `meterHz`, `maxUploadMb`,
`mixSampleRate`, `renderBlock`, `ringBlocks`, `masterChannels`, `maxBuses`,
`masterCeilingDb`, `fsRoots`, `corsOrigin`, `verbose`); `--help` prints the
list, and so does `GET /api/server/config` — all four read
`core::ServerConfig::schema()`, so a key cannot be readable and undocumented, or
editable in the settings page and dropped on the way in. `fsRoots` is an array — there is no shell here, so there is no reason to
inherit `PATH`'s separator problem.

The file is **sparse**: a key it does not mention is *not set*, which is not the
same as being set to the default. That is what lets an installation keep taking
improved defaults instead of freezing whichever ones were current the day the
file was written.

Until 2.5 the server never wrote this file, for exactly that reason — nothing it
serialised could tell a deliberate choice apart from whatever the default
happened to be that day. It writes it now, through
[`PATCH /api/server/config`](#server-configuration), and the sparseness argument
is what shapes *how*: a request names keys, and **only those keys are written**.
Editing the CORS origin in the settings page writes one key and leaves the other
twelve absent, still taking their improved defaults. What the server will not do
is serialise its whole configuration, which is the thing that would have made
every default permanent.

One bad key costs that key and nothing else: an unknown name, a wrong type and
an out-of-range number are each reported and dropped, the rest of the file still
applies, and the server still boots. A malformed file is reported and ignored
whole. A server that refused to start over a typo would be worse than one that
starts on a default and says so — it is routinely launched by a shortcut with
nobody watching the console.

Master-bus geometry is not fixed: the top two channels are always reserved for
the Preview bus, so at the default 32-wide bus preview sits on 30/31, and at a
16-wide bus on 14/15. Clients must read `masterChannels` / `previewMasterL` /
`previewMasterR` from `GET /api/state/summary` (or the `master_bus` block in the
`playback_snapshot` WebSocket frame) rather than assuming 30/31.

The control surface listens on **TCP 4480** (REST + WebSocket). Alongside it, the
[discovery beacon](include/liveplay/net/discovery.hpp) announces the server on
**UDP 4481** via subnet broadcast and multicast (group `239.255.69.80`), and answers
client solicitations with a unicast reply — so clients on the LAN can find the server
without a hand-typed address. Open both ports through any firewall when running the
server on a separate machine from the client.

If the server crashes, its crash handler spawns a fresh copy of itself with the same
flags plus `--start-delay-ms` (so the old listening socket drains before the new
instance binds) and leaves a `.crash-resume.json` behind. The restarted instance reads
that file, reloads the open project and resumes playback from where it left off — so a
crash doesn't take the show down. The desktop client tracks the live server via the
`--pidfile` it passes on launch.

On boot it prints a banner showing the LAN-reachable URL:

```
  Listening
    REST       http://0.0.0.0:4480
    WebSocket  ws://0.0.0.0:4480/ws
    LAN reach  http://192.168.1.42:4480
```

`Ctrl+C` (SIGINT / Ctrl-Break / WM_CLOSE on Windows) triggers a clean shutdown — devices closed, threads joined, project flushed if dirty.

---

## Architecture

### Three-tier mixer

Every cue's audio goes through three explicit tiers, in order, on the engine's render thread:

```
   Tier 1                Tier 2                  Tier 3
   ──────                ──────                  ──────
 PlaybackItem  ─send─►  MixerChannel  ─send─►  Master Output Bus  ─hw─►  Device:HwCh
   (one per                (group bus,            (per-channel
   live cue,               gain/mute/             brick-wall
   own decoder)            PFL/fade)              limiter)
```

- **Tier 1 — [`PlaybackItem`](include/liveplay/audio/playback_item.hpp)**: one instance per active cue, with its own `ma_decoder`, gain/fade state machine, optional LTC generator, and a per-source-channel meter. Loading the same `.wav` into two cart slots yields **two independent instances**; attenuating one never affects the other.
- **Tier 2 — [`MixerChannel`](include/liveplay/audio/mixer_channel.hpp)**: a virtual strip with gain, mute, PFL, and a smooth-fade ramp. Many items can route into one channel; one item's source channels can fan out to multiple channels. Every project **bus** (see [Buses](#buses)) is one of these strips. PFL adds a pre-fader, pre-mute tap into the strip of the bus holding the **Preview** role and changes nothing else — it replaced solo, which no UI ever reached and which cost the render thread a per-block scan of every strip.
- **Tier 3 — Master output bus** ([`engine.hpp`](include/liveplay/audio/engine.hpp)): 32 logical master channels by default, configurable from 4 to 1024 via `--master-channels`. Each carries a limiter + meter and is assigned to exactly one `(Device, HardwareChannelIndex)` tuple. Master channels 0/1 are the **house pair**: the hardware output of the bus holding the **Master** role, the clock device every other device resamples to, and what the transport bar's house meter reads. The top two are reserved for the **Preview** bus — the pre-listen destination, where both PFL and cue preview land. Every other hardware-bound bus takes a pair from the pool in between.

All three tiers run at a 256-frame block (~5.3 ms at 48 kHz). Meters and limiter envelopes update once per block.

A note on words: "master channel" below is an engine accumulator (one of the 32); "the Master bus" is the project bus that carries the master role and sits on master channels 0/1. `/api/master/*` drives the engine-wide trims; the Master bus's own fader is `PATCH /api/buses/<id>` like any bus.

### Multi-device routing matrix

LivePlay drives **multiple sound cards simultaneously** with full source-channel splitting. The matrix is sparse, JSON-serialisable, and round-trips through `/api/project`. A project expresses its routing as buses (cue → bus → output / bus → bus / bus → hardware); the server turns that into the three engine stages below, which is why an external controller should drive `/api/buses` and leave the raw matrix alone. The three stages:

| Stage              | Mapping                                       | Persisted as         |
|--------------------|-----------------------------------------------|----------------------|
| Item → Mixer       | per-source-channel sends with linear gain     | `item_routes`        |
| Mixer → Master     | per-master-channel sends with linear gain     | `mixer_routes`       |
| Master → Device    | exactly one `(device_id, hw_channel)` per master | `master_assignments` |

Example: a stereo MP3 (`L`, `R`) playing on a 4-channel cue can simultaneously feed FOH (Device A: ch 0+1) and a stage monitor mix (Device B: ch 2+3) **with different gains** by wiring `L → MixerA → Master0 → DevA:0` plus `L → MixerB → Master2 → DevB:2` and similarly for `R`.

Mutators: `POST /api/routing/item_to_mixer`, `/mixer_to_master`, `/master_to_device`.

### Brick-wall master limiter

[`limiter.hpp`](include/liveplay/audio/limiter.hpp). Defaults: −0.3 dBFS ceiling, 5 ms lookahead (~240 samples at 48 kHz), 50 ms release. The detector runs a sliding-max peak window with O(1) amortised update; the gain envelope snaps down within the lookahead window and one-pole-releases back to unity. Output is mathematically clamped to the ceiling so clipping is impossible for finite inputs.

### Per-cue LTC generator

[`ltc_generator.hpp`](include/liveplay/audio/ltc_generator.hpp). When enabled on a cue, the engine appends a synthetic **extra source channel** after the file's real channels. That synthetic channel can be routed anywhere through the matrix — to a FOH output, a dedicated 3.5 mm jack, or an entirely different device.

Supported frame rates: 24, 25, 29.97 NDF, 29.97 DF, 30. Drop-frame handling is in `LTCGenerator::timecode_for_frame`. Offset is any non-negative `chrono::nanoseconds`, so a cue can emit `01:00:00:00` at its first sample and stay sync-locked thereafter.

### Real-time metering

Every tier has its own [`Meter`](include/liveplay/audio/meter.hpp) — VU-style attack/release peak envelope plus a leaky-integrator RMS over ~300 ms. The audio thread pushes blocks via `push_block()`; the meter publishes lock-free atomics. A dedicated broadcast thread snapshots all meters at 30 Hz by default (`--meter-hz`, 1–120) and fans them out to every WebSocket client. See [WebSocket frames](#websocket-frames) below.

### Manual-stop fade-out contract

All three stop paths funnel through the same fade-out envelope:

- `PlaybackItem::stop()` — user pressed Stop, or master Stop-All; honours `fade_out_duration`.
- `PlaybackItem::stop_now()` — emergency panic, no fade.
- Natural end-of-file inside `render_block()` — honours `fade_out_duration`.

`stop()` calls `start_fade()` which feeds the same state machine the natural-end branch uses. The contract is documented inline in [`src/audio/playback_item.cpp`](src/audio/playback_item.cpp) (search for `// Manual-stop fade contract:`).

---

## Control surface

`ControlServer` (in [`net/control_server.cpp`](src/net/control_server.cpp)) hosts a Crow app on the configured port. CORS allows any origin by default (it's a LAN service); `--cors-origin` / `corsOrigin` pins it to one.

The authoritative endpoint list is the table of `CROW_ROUTE` registrations in [`src/net/control_server.cpp`](src/net/control_server.cpp). What follows is the same surface with request/response schemas.

### Conventions

- Every response — including Crow's own `404`/`405` and the `401`/`403` from authentication — carries `Access-Control-Allow-Origin` (the configured origin, `*` by default). JSON responses carry `Content-Type: application/json`.
- Every error follows `{ "error": "<message>" }` with an appropriate 4xx/5xx status code. `400` covers malformed bodies; `404` covers unknown ids/paths; `409` covers a request that's understood but refused (e.g. a bus routing rule); `413` covers oversize uploads; `500` covers internal failures.
- `OPTIONS` on any route (a CORS preflight) returns `204` with `Access-Control-Allow-Origin`, `Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS`, `Access-Control-Allow-Headers: Content-Type, Authorization` and `Access-Control-Max-Age: 600`. Crow answers preflights itself, before any route runs, so these headers are added by the `AuthGuard` middleware's `after_handle` — not by a route (#62). `OPTIONS` on a path matching no route is `404`.
- All IDs are opaque strings unless typed otherwise. `<int>` path parameters are 32-bit signed.
- `cue_id` (engine-level) ≠ `item_uuid` (project-document level). The server maintains the mapping in `ProjectState`. The WebSocket transport frames accept either (`cue_id` or `item_uuid`); REST routes are split by path — `/api/cues/<id>/…` takes a cue id, `/api/project/items/<uuid>/…` an item uuid.

### Authentication

**The default is open, and stays open.** With no `users.json` beside the executable, the server
authenticates nothing and behaves exactly as every release before 2.5 did. That is deliberate and
it is the same posture `--fs-root` and `--cors-origin` take: the knob appears, the behaviour does
not change until someone sets it, and the server states which posture it is in at boot rather than
leaving it to be discovered. A point release that started demanding a password would lock operators
out of their own rigs, quite possibly mid-show.

From the **first account onward**, every route needs a bearer token and the Server-tier routes need
an administrator.

#### The two roles

The split is the ownership model's own tier boundary, not an access scheme invented alongside it —
so "does this need an admin?" has an answer you can derive rather than memorise: it needs an admin
if it changes state belonging to the **machine** rather than to the show or to the person at it.

| Role | May |
|------|-----|
| `operator` | Run the show. Everything in the Project and User tiers: cues, buses, transport, project settings, media, uploads, their own locale and meter rate. |
| `admin`    | The above, plus the Server tier: `/api/outputs` (the logical-output map), `/api/users` (accounts), `/api/clients` (who is connected, and from where). |

Everything not on that short list is operator-level. Anything *unlisted* — including a path
matching no route at all — requires a token: the guard **defaults to deny**, so a route added later
is protected without anyone having to remember, and an anonymous caller can't map the route table
by reading which 404s come back.

#### Carrying the credential

REST takes `Authorization: Bearer <token>` and only that — never a query parameter, because a token
in a URL ends up in proxy logs, browser history and the `Referer` of anything the page then loads.

The **WebSocket** is the exception, and has to be: a browser cannot set headers on a handshake, so
`/ws` takes `?access_token=<token>`. It is named `access_token` rather than `token` so it can't be
confused with the one-shot capability the export flow puts on `/api/file/download` (that route needs
both). Crow's per-request log line is not reached on the upgrade path, and this is a session token
rather than a password — but the trade is real, which is why REST refuses the same parameter.

The socket is checked in its `.onaccept` handler rather than by the middleware. Crow *does* run
middleware for an upgrade request, but then hands the connection to the rule regardless of what the
middleware left in the response — so `res.end()` cannot refuse a socket. Two enforcement points,
because there are two doors.

#### How it is stored

`users.json` lives beside the executable, next to `outputs.json` and `liveplay.json`. Machine-owned:
a show moved to another rig does not carry accounts with it. It holds password hashes and the
token-signing secret, so on POSIX it is written `0600` (set on the temp file before the rename, so
it is never briefly world-readable at its real name). **On Windows it inherits the directory's
permissions** — doing it properly means a DACL, and a half-done ACL that looks like protection is
worse than saying so here.

Passwords are Argon2id (`crypto_pwhash_str`) at libsodium's *interactive* limits — a login has to
complete on the laptop running the show. The parameters travel inside the hash string, so raising
them later does not invalidate existing passwords.

Tokens are **stateless and signed** (`crypto_auth`, HMAC-SHA512-256) with a secret generated once
and kept in the file. That is a deliberate choice rather than the lazy one: the crash handler
*auto-restarts this server*, and tokens held only in memory would sign every connected surface out
at the exact moment things were already going wrong. The cost is that an individual token cannot be
withdrawn, so each user carries a `tokenEpoch` stamped into their tokens and checked on every
verify — **changing a password or deleting a user invalidates everything issued to them**, which
covers the two cases where immediate revocation is what anyone actually means. Tokens last 30 days.

A corrupt `users.json` **stops the server booting**. Falling back to "no users" would read as "let
everyone in", turning a damaged file into an unlocked door; an operator who wants the server open
can delete the file and mean it.

#### Bootstrapping

While the store is empty there is nobody to be an administrator, so `POST /api/users` is reachable
without a credential and the first account it creates is forced to `admin` whatever role was asked
for — a store whose only account cannot manage accounts is a locked room with the key inside. The
window is real and worth being plain about: until that first account exists, whoever reaches the
port first can claim the rig. That is not a new exposure — it is the state every release so far has
shipped in, permanently — and unlike that state, it closes.

Login is throttled per address (5 consecutive failures → 30 s), on top of Argon2id's own ~100 ms
per attempt. It is a brake on one machine grinding a word list, not a security boundary, and
nothing here pretends otherwise.

#### Endpoints

| Method · Path | Body | Response | Notes |
|---------------|------|----------|-------|
| `GET /api/auth/status` | — | `{ "authRequired": false, "userCount": 0, "setupRequired": true, "tokenTtlSeconds": 2592000 }` | **Public.** It has to be: a client cannot know whether to ask for a password until it has asked this, and requiring a token to find out whether a token is required is a loop. Reveals only whether accounts exist, never who they are. |
| `POST /api/auth/login` | `{ "name": "sam", "password": "…" }` | `{ "token": "lp1.…", "expiresIn": 2592000, "user": { "id", "name", "role" } }` · `401` · `429` when throttled (with `Retry-After`) · `409` if the server has no accounts | **Public.** A wrong password and an unknown user give the *same* message and take the *same* time (an unknown name is verified against a decoy hash), so the reply cannot be used to enumerate accounts. |
| `GET /api/auth/me` | — | `{ "authRequired": true, "user": { … } }` | Who the caller is according to their token. |
| `POST /api/auth/logout_all` | — | `{ "ok": true }` | Bumps the caller's `tokenEpoch`: invalidates every token ever issued to them, including the one making the request and the one on the tablet left at the venue. |
| `GET /api/users` | — | `[ { "id", "name", "role", "createdAt" }, … ]` | **Admin.** Never returns hashes. |
| `POST /api/users` | `{ "name", "password", "role" }` | `{ "id", "name", "role" }` · `409` name taken · `400` bad name / short password | **Admin**, except while the store is empty — see Bootstrapping. Passwords must be at least 8 characters. |
| `PATCH /api/users/{id}` | any of `{ "name", "role", "password" }` | `{ "id", "name", "role" }` · `409` name taken or last admin | **Admin.** A `password` change bumps that user's `tokenEpoch`. |
| `DELETE /api/users/{id}` | — | `{ "ok": true }` · `404` · `409` last admin | **Admin.** The last administrator cannot be deleted: that would leave a server still requiring a login with nobody able to manage it, recoverable only by editing `users.json` on the machine. Deletes that user's preferences too. |

## Server configuration

The machine's own settings have a settings page since 2.5 — every key in
[`liveplay.json`](#liveplayjson), editable by an administrator without a text
editor or a shell on the box.

| Method · Path | Body | Response | Notes |
|---------------|------|----------|-------|
| `GET /api/server/config` | — | `{ path, schemaVersion, locked, fields: [ … ] }` | **Admin.** One response renders the whole form: every key, its type and range, what it is for, what is stored, what is in force, and who set it. |
| `PATCH /api/server/config` | any subset of the schema; `null` clears a key | `{ stored, dropped, restartRequired, overriddenAtLaunch }` · `403` when locked | **Admin.** Merges. Invalid values are dropped key by key with reasons, never 400. |

Each field reports:

| Field | Means |
|-------|-------|
| `value` | what is actually in force right now |
| `stored` | what `liveplay.json` says, which may be different |
| `source` | `default` · `file` · `env` · `cli` — which tier supplied `value` |
| `overridden` | `source` is `env` or `cli`, so editing the file will not take effect |
| `appliesAt` | `restart` for everything today; the engine cannot be re-initialised while running |
| `policy` | this key is security policy rather than preference — `fsRoots` and `corsOrigin` |

**`source` is what keeps the page honest.** The desktop app always launches the
server with `--port`, so a port field that accepted an edit and said nothing
would write the file, report success, and change nothing until somebody removed
a flag they cannot see. An overridden key is still *written* — the stored value
is what applies once the flag goes away — so the PATCH response separates
`restartRequired` (waiting for a restart) from `overriddenAtLaunch` (shadowed by
something a restart will not clear). They are different facts with different
fixes, and reporting them as one would be wrong either way.

### `--lock-server-config`

Two of these keys are security policy: `fsRoots` decides how much of the
filesystem the API can reach, `corsOrigin` decides which web origins may drive
the server. Making them editable over the network moves them from *needs a shell
on the machine* to *needs an administrator's token* — a real change, since an
admin who widened `fsRoots` to the drive root could then read any file on the
box through `GET /api/fs/list`.

That is defensible — an administrator owns the Server tier by definition, and
this is that tier — but the ownership model's rule R3 says server policy must be
lockable, and this is what it was for. `--lock-server-config`,
`LIVEPLAY_LOCK_SERVER_CONFIG=1`, or `"lockServerConfig": true` in the file makes
every write refuse with `403` and the settings page render read-only, saying why.

The lock is deliberately **not writable through `PATCH`**. A lock an
administrator can turn off over the network is not a lock; a venue that sets it
means it, and clearing it is a decision to be made at the machine. It is also off
by default, the same posture `--fs-root`, `--cors-origin`, `liveplay.json` and
`users.json` all take: the knob appears, the behaviour waits to be asked for.

## User preferences

Four values used to live in the `.liveplay` document and never belonged there: the colour scheme (`theme`), the transport keymap (`playbackKeys`), the meter's display unit (`settings.meterMode`) and whether the playlist follows the playing cue (`settings.uiScrollToPlaying`). A document is a portable thing you mail to a colleague, so all four travelled with it — opening someone else's show changed your colours and silently reassigned the keys your hands already knew. They belong to the **person**, and since 2.5 that is where they live.

**Where they live depends on whether anyone is signed in.** With no accounts configured — still the default — there is no person for a preference to belong to, so nothing is stored here and the client keeps them in its own machine store. The obvious alternative, one shared "anonymous" profile, would put whatever the desk chose onto every tablet in the building: the same coupling the document had, moved to a new file and no better for the move. Sign in and they follow you to any surface instead.

Profiles are one JSON file per user under `prefs/` beside the executable, created on first sign-in and never before — an installation that leaves authentication off grows no directory. Deliberately *not* inside `users.json`: that file holds password hashes and the token-signing secret, and every theme toggle should not rewrite it. Same `0600`-on-POSIX-only admission as `users.json` (see below). A profile that cannot be parsed costs one person their colours, never the server's ability to boot, and the unreadable file is moved aside as `<id>.json.corrupt` rather than overwritten — it may be the only copy of a keymap somebody spent an afternoon on.

The store is **sparse**, like `liveplay.json`: an absent key means *not chosen*, not "off", so an installation keeps taking improved defaults instead of being frozen at whatever they were the first time someone opened a colour picker. A `null` value clears a key, which is how "stop choosing this, follow the show again" is said.

| Method · Path | Body | Response | Notes |
|---------------|------|----------|-------|
| `GET /api/prefs` | — | the caller's profile · `409` when nobody is signed in | Seeds the profile from the open project's legacy fields if it does not exist yet — this read *is* the migration. |
| `PATCH /api/prefs` | any of `{ "theme": { "mode", "accentColor" }, "meterMode", "uiScrollToPlaying", "playbackKeys", "locale" }` | the resulting profile · `409` | Merges. `theme` merges rather than replacing. Invalid values are dropped key by key, never 400. |

**Neither route names a user, and that is deliberate.** The profile acted on is always the caller's, so there is no shape of request that reads or writes somebody else's — an administrator owns the machine, not the people using it, and `playbackKeys` in particular is a description of what one person's hands do. An endpoint that took a user id is one that would eventually be called with someone else's.

`409` is not an error a client should retry: it means authentication is off, and the correct response is to keep these values locally.

### The migration

`theme`, `playbackKeys`, `settings.meterMode` and `settings.uiScrollToPlaying` are **legacy, not fatal**. A 2.4 document still opens; the values are read once, as the seed for a profile that does not have one, and `save()` then drops them. The load counts what it found and reports it as `userPrefsMigrated` on the existing `project_migrated` broadcast, so the file changing shape is something the operator is told about rather than something they discover.

Seeding runs at the moment there is finally somewhere to put the values, and never twice. One consequence is worth stating: **an empty seed writes nothing.** A client reads its preferences as soon as its socket comes up, which on the ordinary startup order is before any project is open — creating an empty profile then would permanently spend that person's one chance to inherit their real theme and keymap from a file they open a minute later.

`cartSlotKeys` deliberately did **not** move. A cart wall is the show's layout: the slot that fires the door slam is a property of this production, and it has to be the same slot for whoever is standing at the desk tonight.

`PATCH /api/project/theme` was **removed** rather than deprecated. It wrote a value `save()` now drops, so keeping it would leave a route that answers `200`, broadcasts a change, and quietly loses the input at the next save.

### One thing the server actually reads

`meterMode` is the only preference the server acts on, and only to gate true-peak and loudness metering — real DSP on the audio thread that is not worth running when nobody is looking at it. Because the meter frame is computed once and broadcast, there is no per-connection DSP to gate, so the gate takes the **union** of what every connected operator has chosen and what the project's output target implies.

That means a User-tier value spends CPU on the audio thread, which R2 would normally forbid. It is admitted for one reason: the union can only ever reach a state a single project setting could already reach on its own, so the worst case is unchanged. Per-user meter *ballistics* stay Project-owned for exactly the opposite reason — those change the numbers everyone is shown, not merely who pays for computing them.

### REST endpoints

#### Diagnostics

| Method · Path      | Body | Response | Notes |
|--------------------|------|----------|-------|
| `GET /api/health`  | —    | `{ "ok": true, "name": "liveplay-server" }` | Liveness probe. |
| `GET /api/whoami`  | —    | `{ "clientIp": "192.168.1.10", "isLocal": false }` | `isLocal` is true for loopback callers (127.0.0.0/8, `::1`). |
| `GET /api/clients` | —    | `[ { "id": 3, "remoteIp": "192.168.1.10", "user": "sam", "userId": "9f2…", "isAdmin": false, "connectedSeconds": 412, "locale": "el", "localeIsOwn": true, "meterHz": 5, "meterHzIsOwn": true }, … ]`, lowest `id` first | **Administrators only** (see [Authentication](#authentication)), because it reports the address every connected client came from. Who is connected **right now**: one row per live WebSocket. REST is stateless, so a `curl` against it is not a session — anything driving the rig holds a socket open. `id` is monotonic within a process run and never reused, so an id in a log line always means one connection. `user`/`userId` are `null` on an installation with no accounts — reported as null rather than as a name like "anonymous", so the open posture can't be confused with someone called that. |

#### Devices

| Method · Path | Body | Response |
|---------------|------|----------|
| `GET /api/devices` | — | `[ { "id": "…", "display_name": "…", "channel_count": 8, "sample_rate": 48000, "is_default": true } ]` |
| `POST /api/devices/open` | `{ "name": "…" (optional), "channels": 2 }` — empty `name` opens the default device | `{ "device_id": "…" }` · `400` if open fails |
| `POST /api/devices/close` | `{ "id": "…" }` | `{ "ok": true }` |

#### Cues (engine-direct)

This is the low-level cue surface — for normal use, prefer the project-item surface (`/api/project/items/...`), which honours `duckingBehavior`, `inPoint`, `endBehavior`, etc.

| Method · Path | Body | Response |
|---------------|------|----------|
| `GET /api/cues` | — | array of cue objects (see below) |
| `POST /api/cues` | `{ "file_path": "/abs/path.wav", "display_name": "…" (optional) }` | cue object · `400` on load failure |
| `GET /api/cues/<id>` | — | cue object · `404` if unknown |
| `DELETE /api/cues/<id>` | — | `{ "ok": true }` |
| `POST /api/cues/<id>/play` | — | `{ "ok": true }` · a cue that belongs to a project item plays through the item, exactly like `POST /api/project/items/<uuid>/play` (in-point, trim, fades, ducking, end behaviour); only an ad-hoc cue with no item is played raw. Either way a cue that had stopped starts again at its start point, never where it last stopped. |
| `POST /api/cues/<id>/stop` | `{ "fade_ms": 3000 }` (optional; empty body permitted) | `{ "ok": true }` · as `/play`: an item's cue stops through the item. `fade_ms` overrides the cue's manual-stop fade for this stop (`0` = cut). |
| `POST /api/cues/<id>/gain` | `{ "db": 0.0 }` | `{ "ok": true }` |
| `POST /api/cues/<id>/fade` | `{ "in_ms": 0, "out_ms": 0 }` | `{ "ok": true }` |
| `POST /api/cues/<id>/ltc`  | `{ "enabled": bool, "fps": 0..4, "start_timecode": "HH:MM:SS:FF" (preferred) or "offset_ns": int64 }` | `{ "ok": true }` |

`fps` indices: `0=24`, `1=25`, `2=29.97 NDF`, `3=29.97 DF`, `4=30`. `start_timecode` accepts `;` between SS and FF for drop-frame.

**Cue object shape** (returned by `GET /api/cues`, `GET /api/cues/<id>`):

```json
{
  "id": "…",
  "display_name": "…",
  "file_path": "/abs/path.wav",
  "artist": "…",
  "title": "…",
  "duration_sec": 123.4,
  "gain_db": 0.0,
  "fade_in_ms": 0,
  "fade_out_ms": 500,
  "ltc": { "enabled": false, "fps": 0, "offset_ns": 0, "start_timecode": "00:00:00:00" },
  "transport": 0,            // present iff engine has a PlaybackItem: 0=Stopped, 1=Playing, 2=FadingOut, 3=Paused
  "playhead_seconds": 0.0,
  "source_channels": 2,
  "file_loaded": true
}
```

#### Transport & master

| Method · Path | Body | Response |
|---------------|------|----------|
| `POST /api/transport/stop_all` | `{ "fade_ms": 0 }` (optional; empty body permitted) | `{ "ok": true }` |
| `POST /api/master/ceiling` | `{ "db": -0.3 }` | `{ "ok": true }` |
| `GET /api/master/gain` | — | `{ "db": float }` |
| `POST /api/master/gain` | `{ "db": float }` sets an absolute gain, or `{ "delta": float }` nudges the current gain (no read-modify-write race for a control surface). `db` wins if both are present. | `{ "ok": true, "db": float }` · `400` when the body has neither · also broadcasts `master_gain_changed` |
| `GET /api/master/limiter` | — | `{ "enabled": bool }` |
| `POST /api/master/limiter` | `{ "enabled": bool }` (omit to toggle — single-button surfaces) | `{ "ok": true, "enabled": bool }` · also broadcasts `limiter_changed` |
| `GET /api/master/channels/<int>/gain` | — | `{ "channel": int, "db": float }` |
| `POST /api/master/channels/<int>/gain` | `{ "db": float }` | `{ "ok": true, "channel": int, "db": float }` · also broadcasts `output_channel_gain_changed` |

These are engine-wide trims, not the Master bus. `/api/master/gain` is a global gain applied to *every* master channel before its limiter — the reserved Preview pair included — and `/api/master/channels/<n>/gain` is a per-master-channel trim the client no longer drives (it stays at unity; the transport bar's output faders are bus faders now). The fader on the mixer's Master strip, and the "Master" fader on the transport bar, are the Master bus's `gainDb`: `PATCH /api/buses/<id> { "gainDb": … }` on the bus whose `master` flag is `true`, or the WS `bus_gain` frame.

#### External control surface (Companion, custom remotes)

Everything below is the surface a stateless control surface (Bitfocus Companion, a Stream Deck plugin, a curl script) drives. Every mutation is broadcast as a `doc_patch`, so a control surface, the desktop client and a second control surface can never disagree about what is selected, armed or in Show Mode.

| Method · Path | Body | Response | Notes |
|---------------|------|----------|-------|
| `GET /api/state/summary` | — | see below | Compact machine-readable snapshot. Fetch once on connect (and after `project_changed`), then keep it fresh from `/ws` — no polling. |
| `GET`/`POST /api/transport/go` | — | `{ "ok": true, "uuid": "…" }` · `404` if nothing armed or derivable | Plays whatever is armed as "Up Next" (user override first, else the playing item's `endBehavior` target). `GET` is accepted so it can be fired from a browser or `curl`. |
| `GET /api/selection` | — | `{ "itemUuid": "…" }` (empty string = nothing selected) | The shared playlist selection — the control-surface equivalent of the client's arrow-key cursor. |
| `POST /api/selection` | `{ "itemUuid": "…" }` to select (empty string clears) *or* `{ "delta": -1 \| 1 }` to step through the flattened playlist | `{ "ok": true, "itemUuid": "…" }` · `400` if neither field present or `delta` is `0` · `404` if the playlist is empty | Broadcasts `selection_changed`. |
| `GET`/`POST /api/transport/arm_selected` | — | `{ "ok": true, "itemUuid": "…" }` · `404` if nothing selected | Arms the selected item as "Up Next" — the control-surface equivalent of the client's "Set As Next" context action. Broadcasts `next_item_set`. |
| `GET`/`POST /api/transport/play_selected` | — | `{ "ok": true, "itemUuid": "…" }` · `404` if nothing selected or not loaded | Triggers the selected item — the client's Enter / "Play Selected" key. |
| `GET`/`POST /api/transport/pause_toggle` | — | `{ "ok": true, "resumed": bool }` · `404` if nothing is on air | Pause/resume everything on air in one press. Resumes if anything is paused, otherwise pauses everything sounding, so a single button is never ambiguous about which way it goes. |
| `GET /api/ui/showmode` | — | `{ "enabled": bool }` | |
| `POST /api/ui/showmode` | `{ "enabled": bool }` (omit to toggle) | `{ "ok": true, "enabled": bool }` | Broadcasts `show_mode_changed`. |
| `GET /api/ui/locale` | — | `{ "locale": "…" }` | The installation **default**, not "the" locale — see below. |
| `POST /api/ui/locale` | `{ "locale": "en" }` | `{ "ok": true, "locale": "…" }` · `400` if `locale` missing/not a string | Sets the installation default. Broadcasts `locale_changed` **only to connections that have not chosen a locale of their own**. |
| `POST /api/transport/play_index` | `{ "index": [1, 11] }` — an index path descending into groups | `{ "ok": true, "uuid": "…", "index": [int, …] }` · `400` malformed path · `404` no item / not loaded | Body-addressed equivalent of `…/by-index/<path>` (see [Project items](#project-items)). |
| `GET`/`POST /api/transport/cart/<int>/play` | — | `{ "ok": true, "slot": int, "uuid": "…" }` · `404` empty slot or not loaded | Triggers whatever is bound to that cart slot. |

**`GET /api/state/summary` response** — top-level shape:

```json
{
  "buses":     [ { "id", "name", "color", "order", "width", "gainDb", "mute", "pfl", "bound", "outputDevices", "master", "preview", "masters": [l, r] | null, "output": { "type", "target" }, "monoCheck" (Preview bus only) } ],
  "project":   { "name", "itemCount", "hasOpenProject", "audioLoading" },
  "playing":   [ { "itemUuid", "cueId", "name", "color", "transport", "paused", "playheadSec", "elapsedSec", "durationSec", "remainingSec", "index"?, "triggerSeq"? } ],
  "next":      { "itemUuid", "source": "override" | "auto", "name", "color", "type", "index"? } | null,
  "selection": { "itemUuid", "name", "color", "type", "index"?, "onAir" } | null,
  "ui":        { "showMode": bool, "locale": "…" },
  "master":    { "gainDb": float, "limiterEnabled": bool },
  "cart":      [ { "slot", "itemUuid", "name", "color", "playing" } ],
  "preview":   { "active": bool, "itemUuid": "…" },
  "server":    { "version", "meterBroadcastHz", "masterChannels", "previewMasterL", "previewMasterR", "maxUploadBytes" }
}
```

`buses` is deliberately compact — no `dsp`, no `itemUuids`; a controller wants "what is it called and what state is it in", not the mixer's internals. Meters ride the separate `meters` WS broadcast, not this snapshot. `master` / `preview` flag the two role holders (exactly one of each, always present — a bare server with no project open still has its default Master and Preview buses), and `masters` is the pair of engine master channels the bus's hardware output occupies, or `null` when it is not hardware-bound; see [Buses](#buses). `master` at the top level is the engine's global trim, not the Master bus (see [Transport & master](#transport--master)).

#### Buses

The user-facing view of the mixer: every bus in display order, with the items that resolve to it (own assignment, inherited from a group, or the Master bus as the fallback). `bus_info_to_json()` in [`control_server.cpp`](src/net/control_server.cpp) is the single serialiser shared by the list and single-resource routes below, so they can never drift apart.

Every bus is an ordinary bus — there are no hidden system buses. Two of them carry a **role**:

- **Master** (`"master": true`) — the house. It is where a cue plays when neither it nor any ancestor group names a bus, its hardware output sits on engine master channels 0/1 (the clock device, the transport bar's house meter), and a sub-mix reaches the house by routing bus→bus into it.
- **Preview** (`"preview": true`) — the pre-listen bus. PFL taps and cue pre-listen (`POST /api/preview`) both land on its strip, it owns the reserved master pair at the top of the engine's bus, and it carries the mono-check. Nothing may feed it, and it never falls back to the default device: an unresolved Preview output is valid and silent.

A project has **exactly one** of each, never the same bus. Both are otherwise ordinary: rename, recolour, re-route, reorder, DSP, channel view. Roles move; they are not dropped, and a holder cannot be deleted. The defaults are ids `master` / `preview`, names "Master" / "Preview" — a bare server with no project open already has both, so `GET /api/buses` is never empty.

| Method · Path | Body | Response |
|---------------|------|----------|
| `GET /api/buses` | — | array of bus objects (see below) |
| `GET /api/buses/<id>` | — | one bus object · `404` if unknown |
| `POST /api/buses` | `{ "name": "…", "color": "…", "width": 1\|2, "gainDb": float, "mute": bool, "pan": -1..1, "order": int, "output": { "type": "output"\|"bus", "target": "…" } }` (all optional; no `output` means bus→Master) | `{ "id": "…" }` · `409` if the output is refused (see below) · `507` if no mixer strip is available | broadcasts `buses_patched` |
| `PATCH /api/buses/<id>` | any subset of the `POST` fields, plus `{ "dsp": { …partial BusDsp… } }`, `{ "master": true }` or `{ "preview": true }` to move a role here | `{ "ok": true }` · `404` unknown id · `409` if the output or the role change is refused | broadcasts `buses_patched` |
| `DELETE /api/buses/<id>` | — | `{ "ok": true }` · `404` unknown id · `409` `"this bus holds the Master role; move it first"` / `"this bus holds the Preview role; move it first"` | broadcasts `buses_patched`. Feeder buses are re-routed to the Master bus; assigned items fall back to it. |
| `POST /api/buses/<id>/pfl` | `{ "pfl": bool }` | `{ "ok": true, "pfl": bool }` · `404` unknown id (the Preview bus included — it has no PFL of its own) | broadcasts `bus_pfl_changed` |
| `POST /api/buses/pfl/clear` | — | `{ "cleared": int }` | broadcasts `bus_pfl_cleared` when `cleared > 0` |
| `POST /api/preview/mono` · `POST /api/monitor/mono` | `{ "mono": bool }` | `{ "ok": true, "mono": bool }` · `409` `"the Preview bus has no strip"` | broadcasts `monitor_mono_changed`. Folds the whole Preview bus to mono — not per-strip; PFL whichever buses you want to check, then press this. The two paths are one handler; `/api/monitor/mono` is kept for controllers written against the previous release. |

`buses_patched` always carries the full `buses` array (`{ "type": "doc_patch", "op": "buses_patched", "buses": [...] }`) rather than a diff, so a second client converges in one apply. A role move is one `PATCH` and one `buses_patched`: the previous holder loses the flag, both buses are re-wired (the new Master takes masters 0/1 and the old one goes back to a pool pair; the new Preview takes the reserved pair and the PFL taps with it) and the broadcast carries the settled state.

**Bus object** (`GET /api/buses`, `GET /api/buses/<id>`):

```json
{
  "id": "…", "name": "…", "color": "…", "order": 0, "width": 2,
  "gainDb": 0.0, "mute": false, "pan": 0.0,
  "dsp": { "eqEnabled", "dynEnabled", "hpf", "lpf", "eq": [...], "gate": {...}, "comp": {...}, "... " },
  "pfl": false,
  "monoCheck": false,
  "bound": true,
  "master": false,
  "preview": false,
  "masters": [2, 3],
  "output": { "type": "output" | "bus", "target": "…" },
  "mixerId": "…",
  "itemUuids": ["…"]
}
```

`monoCheck` is present on every bus object here (unlike the compact `summary.buses` array, which only carries it for the Preview bus) but is only ever meaningful for the Preview bus. `masters` is the pair of engine master channels the bus's hardware output occupies — `[0, 1]` for the Master bus, the reserved pair for Preview, a pool pair for any other Output-kind bus — or `null` when the bus is not hardware-bound (a bus→bus send, or an output that did not resolve); it is what the transport bar builds its output meters and faders from. `bound` is server-computed by walking the output chain to its terminal — a client renders it and never derives it itself. For an Output-kind terminal it is true when the target **resolves to at least one present device** exactly as wiring will resolve it: a mapped `outputs.json` entry counts only through channels whose device is here (a mapping to an absent card is unbound and silent — it used to report bound and play into the default device); the built-in `Main Out` unmapped is the default device; any other name counts when it names a present device (the device list is refreshed on `GET /api/devices` and on device open, never on the render thread). The Preview bus is bound only when its output actually resolved to channels. `outputDevices` lists the real device names the target resolved to (`""` = the default device), so a client never string-matches device names itself.

**Device names are matched exactly first, then modulo Windows renumbering and case** (`audio/device_name.hpp`): `OUT 3-4 (BEHRINGER UMC 404HD 192k)` in a show finds `OUT 3-4 (2- BEHRINGER UMC 404HD 192k)` on a machine that renumbered the interface — but only when exactly one present device matches, so two identical interfaces are never guessed between. A named device that cannot be found is never replaced by the default device. `dsp` is the full DSP chain (EQ bands, HPF/LPF, gate, compressor); see `bus_dsp_to_json()` in `control_server.cpp` for the exact shape.

**A bus output** is one of:

- `{ "type": "output", "target": "<logical output name>" }` — to hardware; see [Logical outputs](#logical-outputs). A plain device name is a valid target too: it resolves to that device's channels 0/1 and reports `bound` while the device is present.
- `{ "type": "bus", "target": "<bus id>" }` — bus-to-bus routing. Targeting the Master bus is how a sub-mix reaches the house.

`{ "type": "master" }` is **retired**. For one release `POST`/`PATCH` still accept it (and an omitted `output` on `POST`) and map it to `{ "type": "bus", "target": "<master bus id>" }`, logged at warn — a controller written against the previous release keeps working, but should move to the explicit form. It is never written back: the document and every response carry `output` or `bus` only.

The rules, each refused as a `409` before anything is stored (the text is the `message`):

| Refused | `409` text |
|---|---|
| Preview bus routed to another bus | `the Preview bus cannot be routed to another bus` |
| Master bus routed to another bus | `the Master bus must send to an output` |
| bus→bus to an unknown id | `no such bus to route to` |
| any bus targeting the Preview bus | `a bus cannot feed the Preview bus` |
| a bus→bus chain that loops back on itself | `routing this bus would create a cycle` |
| `{ "master": false }` / `{ "preview": false }` | `move the role to another bus instead` |
| a role granted to a bus that does not send to an output (the output may arrive in the same `PATCH`) | `a bus must send to an output to hold the Master or Preview role` |
| `{ "master": true, "preview": true }`, or granting one role to the holder of the other | `one bus cannot hold both the Master and Preview roles` |
| `{ "preview": true }` on a bus that other buses feed | `buses feed this bus; re-route them before making it the Preview bus` |
| deleting a role holder | `this bus holds the Master role; move it first` / `this bus holds the Preview role; move it first` |

**Order.** `order` is the rail position; the role holders are pinned on the surface and keep their own sort keys (`1000000` / `1000001` by default) outside the rail. `PATCH { "order": n }` is a drop onto the rail: the moved bus takes `n`, wins a tie against whatever already had it, and the rail is then renumbered `1..N` — so the `order` values in the resulting `buses_patched` are dense and may differ from what was sent. A new bus lands at the end of the rail unless `order` is given.

Two drag-only endpoints exist for live knob feedback and are **not** part of the external-control surface — external controllers use `PATCH /api/buses/<id>` instead (D16):

| Method · Path | Body | Response | Notes |
|---------------|------|----------|-------|
| `POST /api/buses/<id>/pan` | `{ "pan": -1..1 }` | `{ "ok": true }` · `404` unknown id | Live pan while the knob is being dragged: moves the send gains only, no document write, no broadcast. Client-internal. |
| `POST /api/buses/<id>/dsp` | partial `BusDsp` JSON | `{ "ok": true }` · `404` unknown id | Live tone-control coefficients while a filter knob is dragged: straight into the strip, no document write, no broadcast, no re-wire. Client-internal. |

The document carries a top-level `busSchema` version (currently `2`) alongside `buses`; each stored bus carries `master` / `preview` and an `output` of type `output` or `bus`. See [Project document](#project-document) for what a `busSchema < 2` document gets on load.

#### Logical outputs

Server-owned: what a project's output names (`"output"` targets, above) mean on *this* machine. Never part of the project document — that's what keeps a show portable between machines with different hardware.

| Method · Path | Body | Response |
|---------------|------|----------|
| `GET /api/outputs` | — | `{ "version": 1, "builtin": ["Main Out", "Preview Out"], "outputs": [ { "name": "…", "channels": [ { "device": "…", "hwChannel": int }, … ] } ] }` |
| `PUT /api/outputs` | the `GET` shape without `builtin` | `{ "version", "outputs", "rewiredBuses": [...] }` · `400` malformed map | broadcasts `outputs_changed` |

Two names are **built in** and mean something even when the map says nothing about them. A real mapping always wins; unmapped:

- **`Main Out`** → the platform's default playback device, stereo. It is what the Master bus targets out of the box, so a fresh install makes sound with no configuration.
- **`Preview Out`** → no channels at all. The Preview bus targets it out of the box; silence is the safe answer for the bus PFL lands on, because the default device is the house.
- Any **other** unmapped name → treated as a device name, stereo on hardware channels 0/1 (and, if no device of that name exists, the engine's default device). This is what lets a device chosen from the strip's output picker — or a legacy per-item device override — work without an `outputs.json` entry.

`GET /api/outputs` lists the built-ins in `builtin` so a client can show them ahead of the machine's own names; `PUT` ignores the field. Any bus routed through a remapped output is re-wired immediately (`rewiredBuses` lists which ones) — without this a remap would appear to do nothing until the project was reloaded. In the client the map is edited from the mixer header ("Remap hardware outputs") and from the "Edit hardware outputs…" row at the bottom of every strip's output picker.

#### Engine diagnostics

| Method · Path | Query | Response |
|---------------|-------|----------|
| `GET /api/engine/stats` | `?reset=1` clears the render-time peak, bounding a measurement to a window the caller controls | `{ "queuedFrames", "queuedMs", "ringCapacityFrames", "devicePeriodFrames", "devicePeriods", "deviceMs", "renderBlockUsMax", "renderBlockUsAvg", "blockBudgetUs", "blocksRendered", "underruns", "topologyRebuilds", "mutexWaitUsMax", "discontinuities", "worstSeam", "deviceCount", "devices": [ … ] }` |

Measured rather than configured — the device gets a say in the period it actually runs, and how long a block takes to render is the only thing that says whether the queue depth is buying anything.

The queue and period figures describe the **clock device**: the one carrying master channels 0/1, or the first opened if those are unassigned. Production is gated on it, so its ring depth is the engine's output latency.

`devices` is one object per open device. Every other device runs on its own crystal and is resampled to follow the clock, and this array is how that loop is watched:

| Field | Meaning |
|-------|---------|
| `name` | the device's display name |
| `isClock` | whether this is the clock device — the one nothing corrects |
| `ppm` | parts per million of correction being applied to follow the clock; `0` on the clock device. A healthy pair settles within a minute at a small steady figure; a number pinned near `2000` means whatever is wrong is not crystal drift |
| `fillPercent` | the queue depth the drift loop is regulating, smoothed. **This is the one to read against the 50% target.** On the clock device, which no loop regulates, it is the raw figure instead |
| `ringFillPercent` | raw, unfiltered ring occupancy. It jitters by a whole device period, so it is not the number to compare against the target — its job is to show a queue genuinely pinned at 0 or 100, starving or dropping, before the smoothed one admits to it |

`deviceCount` is the number of open devices. **Breaking change in 2.5.0:** `devices` was this count; it is now the array above, and the count moved to `deviceCount`.

#### Mixers

Low-level engine strips (as opposed to the document-backed [Buses](#buses) surface above). Buses are implemented on top of these strips.

| Method · Path | Body | Response |
|---------------|------|----------|
| `GET /api/mixers` | — | `[ { "id": "…", "display_name": "…", "gain_db": 0.0, "muted": false, "pfl": false } ]` |
| `POST /api/mixers` | `{ "name": "Channel" }` | `{ "id": "…" }` · `507` if the strip limit is reached |
| `DELETE /api/mixers/<id>` | — | `{ "ok": true }` · `404` if unknown |
| `POST /api/mixers/<id>/gain` | `{ "db": float }` | `{ "ok": true }` · `404` if unknown |
| `POST /api/mixers/<id>/mute` | `{ "muted": bool }` | `{ "ok": true }` · `404` if unknown |

#### Routing matrix

| Method · Path | Body |
|---------------|------|
| `POST /api/routing/item_to_mixer`    | `{ "cue": "<cue_id>", "source_channel": int, "mixer": "<mixer_id>", "gain_db": float }` |
| `POST /api/routing/mixer_to_master`  | `{ "mixer": "<mixer_id>", "master_channel": int, "gain_db": float }` |
| `POST /api/routing/master_to_device` | `{ "master_channel": int, "device": "<device_id>", "hw_channel": int }` |

All three respond `{ "ok": true }`. The master→device mapping is **one-to-one**: assigning a master channel implicitly unassigns its previous device/hw_channel.

#### Filesystem (server-side browser)

| Method · Path | Body / Query | Response |
|---------------|--------------|----------|
| `GET /api/fs/list?path=<utf8>&filter=audio\|all\|.ext,.ext` | empty `path` = "computer root" (drives on Windows, `/` on POSIX); `filter` defaults to `audio` | see below |
| `POST /api/fs/mkdir` | `{ "path": "/abs/path" }` | `{ "path": "/abs/path" }` |

`/api/fs/list` response:

```json
{
  "path":    "/abs/path",
  "parent":  "/abs",
  "is_root": false,
  "entries": [
    { "name": "song.wav",  "full_path": "/abs/path/song.wav", "kind": "file", "size": 1234567 },
    { "name": "subfolder", "full_path": "/abs/path/subfolder", "kind": "dir" },
    { "name": "C:",        "full_path": "C:\\",               "kind": "drive" }
  ]
}
```

Entries are sorted by the OS directory iterator. Hidden entries (leading `.`) are skipped. Symlink loops trip `weakly_canonical` and return `400`. File entries that fail the extension filter are omitted.

#### Uploads & media copy

| Method · Path | Body | Response |
|---------------|------|----------|
| `POST /api/upload` | `multipart/form-data` (one or more file parts); request size capped at `cfg.max_upload_bytes` (default 256 MiB) | `{ "saved": [ "/abs/path/in/media/file1", … ] }` · `413` if too large |
| `POST /api/copy_to_media` | `{ "source_path": "/abs/src.wav" }` | `{ "dest_path": "/abs/<project>/media/src.wav" }` |
| `GET /api/file/download?token=<token>` | (one-shot download token from `/api/project/export`) | `application/octet-stream` stream of the file; token consumed on success; `404` if expired/invalid |

Uploads land in `state.media_root()` (the loaded project's `media/` sub-folder). Filenames are sanitised — directory components in the multipart `filename` are stripped.

#### Metadata & waveform

| Method · Path | Body / Query | Response |
|---------------|--------------|----------|
| `GET /api/metadata?path=<utf8>` | — | `{ valid, artist, title, album, genre, year, track_number, duration_ms, sample_rate, channels, bitrate_kbps }` |
| `GET /api/waveform/<cue_id>?buckets=1000` | — | waveform object (see below); `404` if cue not registered |
| `GET /api/waveform_path?path=<utf8>&buckets=1000` | — | waveform object (no `cue_id` field) |
| `POST /api/waveform_generate` | `{ "path": "/abs/file.wav", "item_uuid": "<uuid>" }` | `{ "ok": true }` immediately; result arrives as a `waveform_ready` (or `waveform_failed`) `doc_patch` over WebSocket |

**Waveform object**:

```json
{
  "cue_id": "…",          // only on /api/waveform/<id>; absent on /api/waveform_path
  "bucket_count": 1000,
  "duration_ms": 184500,
  "sample_rate": 48000,
  "source_channels": 2,
  "channels": [
    { "peak": [0.12, 0.14, …], "rms": [0.07, 0.08, …] },
    { "peak": [...],            "rms": [...] }
  ]
}
```

Each channel's `peak` and `rms` arrays have exactly `bucket_count` floats in `[0.0, 1.0]`.

#### Preview (DJ-style pre-listening)

Plays an item into the **Preview bus** — the same strip PFL lands on, under the same fader and meter — without touching the house. Used by the cue preview buttons and the WaveformTrimmer / cue editor. There is no separate preview device: where the audition is heard is the Preview bus's `output` (see [Buses](#buses)), and the client only offers the button while that bus reports `bound`.

| Method · Path | Body | Response | Side effect |
|---------------|------|----------|-------------|
| `GET /api/preview` | — | `{ "active": bool, "itemUuid": "…", "cueId": "…" }` | — |
| `POST /api/preview` | `{ "itemUuid": "<uuid>" }` | `{ "ok": true, "itemUuid": "…", "cueId": "…" }` · `400` if the item is missing or the Preview bus has no strip | broadcasts `preview_started` |
| `DELETE /api/preview` | — | `{ "ok": true }` | broadcasts `preview_stopped` (when a preview was running) |

#### Project document

| Method · Path | Body | Response | Notes |
|---------------|------|----------|-------|
| `GET /api/project`              | — | full project JSON document | The single GET a remote client needs to render the whole project. |
| `GET /api/project/header`       | — | lightweight header `{ name, itemCount, theme, settings, cart, hasOpenProject, … }` | Hit this first so the workspace shell can paint before the items array arrives. `theme` and `playbackKeys` appear only when a legacy document is loaded that still carries them; they are the migration seed, not live state. |
| `GET /api/project/items?offset=0&limit=100` | — | `{ "offset": int, "limit": int, "total": int, "items": [...] }` | `limit` clamps to [1,1000]. Top-level items only (groups carry their children inline). |
| `GET /api/project/progress`     | — | `{ "loading": bool, "loaded": int, "total": int }` | Cheap poll for the open-project progress bar. |
| `POST /api/project/load`        | `{ "path": "/abs/file.liveplay" }` *or* `{ "document": { … } }` | header object, augmented with `needsRepair`/`repairIssues` if the document was auto-repaired on load, and `migration` if buses had to be synthesised (see below) | broadcasts `project_changed`, and `project_migrated` if anything was migrated. `400` if neither field is present or load fails. |
| `POST /api/project/close`       | — | `{ "closed": true }` | broadcasts `project_changed` |
| `PUT /api/project/document`     | full project JSON document | header object, augmented with `migration` as above | Replaces the entire in-memory document. Broadcasts `project_changed`, and `project_migrated` if anything was migrated. |
| `POST /api/project/save`        | `{ "path": "/abs/file.liveplay" (optional) }`, optionally with `{ "document": { … } }` to push an embedded document first | `{ "ok": true, "path": "…" }`, augmented with `migration` as above | Saves to the supplied path or the currently-loaded one. `400` if neither is set. Broadcasts `project_migrated` if the embedded document (if any) had to migrate. |
| `POST /api/project/repair`      | — | `{ "repaired": bool, "issues": [string], "saved": bool }` | Forces a re-save of the (already auto-repaired on load) in-memory document. |

**Bus migration** — a project document that predates buses names no routing at all, so the server invents it on load/replace: every audio item that carried no bus assignment lands on the Master bus, distinct legacy per-item `deviceOverride` values become real buses, and `settings.defaultOutputDevice` becomes the Master bus's output. That's a routing decision made without asking, so it's counted, logged, returned to whoever triggered it, and broadcast to every other connected client as `project_migrated` so nobody's mirror disagrees about where a show is routed. `migration` (in the HTTP response) and the `project_migrated` doc_patch carry the same fields: `{ "itemsToMain": int, "busesFromDeviceOverride": int, "mainOutputMigrated": bool, "previewDeviceMigrated": int, "ltcDeviceMigrated": int, "rolesMigrated": bool }` — flat on the doc_patch frame itself, not nested under `migration`.

**`busSchema`.** The document carries a top-level `busSchema`, written as `2`. A document that declares `busSchema >= 1` and *omits* the `buses` key keeps the loaded project's buses (the client round-trips the document without them). Anything else takes the full path above, and a document with `buses` but `busSchema < 2` (or none) — one from the previous release — additionally gets the **role migration**: the bus with id `main` becomes the Master bus (renamed "Master" only if it is still called "Main"), the bus with id `monitor` becomes the Preview bus (renamed "Preview" only if still "Monitor"); any bus whose output was the retired `{ "type": "master" }` (or had no output at all) becomes `{ "type": "bus", "target": "<master bus id>" }`, except the Master bus itself, which becomes `{ "type": "output", "target": "Main Out" }`; a stored `system` flag is ignored. A document lacking a Preview bus gets one synthesised; lacking a Master bus, the first Output-kind bus by `order` is promoted, else one is synthesised. The role holders are then held to the API's rules (Master and Preview must send to an output; nothing may feed Preview) and anything off disk that breaks them is corrected conservatively. All of this is logged at warn and reported as `rolesMigrated: true`.

**`settings.previewDevice` and `settings.defaultOutputDevice`** are migrated, not read. On load, `previewDevice` becomes the Preview bus's `output.target` (unless that target is already mapped in `outputs.json`) and the key is erased either way — `previewDeviceMigrated` counts it; `defaultOutputDevice` becomes the Master bus's output as before and is erased. Nothing consults either key after load, so an external controller that used to set them through `PATCH /api/project/settings` must route through the buses instead: `PATCH /api/buses/<master-id> { "output": { "type": "output", "target": "<device or logical name>" } }` for the house, the same on the Preview bus for headphones.

**`settings.ltcDevice` → `settings.ltcOutput`** closes the last of them. Timecode used to name a sound card in a document meant to travel; it now names a **logical output**, resolved exactly as a bus target is — the output map wins, an unmapped name is a device name only if that device is present, and otherwise timecode is silent. On load the old key becomes the new one (unless `ltcOutput` is already set) and is erased either way, counted as `ltcDeviceMigrated`. `PATCH /api/project/settings` still *accepts* `ltcDevice` for pre-2.5 controllers, but applies it to `ltcOutput` and stores no second copy, so there is one writer for where timecode goes.

The default device is deliberately withheld from this resolution, which is the one way it differs from an ordinary bus: unmapped `Main Out` **is** the default device, and the default device is the house. Before this, an unresolvable name went to `open_device_by_name()`, which falls back to the default device — so a show configured for an interface the venue does not have put an LTC squeal into the house rather than going quiet. It is the same rule the preview bus has always had, for the same reason. Pinned by `ltc-output-e2e.js`.

#### Project items

Mutating routes return `{ ok: true, ... }` only — the full document is **not** echoed, to keep bandwidth low on large projects. Use the `doc_patch` WebSocket fan-out to keep the local mirror in sync.

| Method · Path | Body | Response | Broadcast op |
|---------------|------|----------|--------------|
| `POST /api/project/items`              | `{ "item": { uuid, displayName, type, … }, "parentUuid": "" (root) or "<group-uuid>" }` | `{ "ok": true, "uuid": "…", "cueId": "…" }` | `item_added` |
| `PATCH /api/project/items/<uuid>`      | partial item JSON (sparse update) | `{ "ok": true, "uuid": "…" }` · `404` if missing | `item_updated` |
| `DELETE /api/project/items/<uuid>`     | — | `{ "ok": true, "uuid": "…" }` · `404` if missing | `item_removed` |
| `POST /api/project/items/reorder`      | `{ "parentUuid": "" or "<group>", "uuids": [string, …] }` | `{ "ok": true }` | `items_reordered` |
| `POST` or `GET /api/project/items/<uuid>/play` | — | `{ "ok": true }` · `404` if not loaded | — (transport edge fires `cue_state` instead) · Routes through `trigger_item`, so a **group** uuid starts the group per its `startBehavior` (it used to 404). `…/stop` on a group stops whatever is playing inside it. `stop`/`pause`/`resume`/`seek` are POST-only. |
| `POST` or `GET /api/project/items/by-index/<path>` | — | `{ "ok": true, "uuid": "…", "index": [int, …] }` · `400` invalid path · `404` no item / not loaded | — (transport edge fires `cue_state` instead) |
| `POST /api/project/items/<uuid>/stop`  | `{ "fade_ms": 3000 }` (optional; empty body permitted) | `{ "ok": true }` · `404` if not loaded · `400` if the body is not JSON | — · Fades over the item's `manualStopFade` (see below); `fade_ms` overrides it for this stop, `0` = cut. |
| `POST /api/project/items/<uuid>/pause` | — | `{ "ok": true }` · `404` if not loaded | — (REST mirror of the WS `pause` message, for stateless control surfaces) |
| `POST /api/project/items/<uuid>/resume`| — | `{ "ok": true }` · `404` if not loaded | — (REST mirror of the WS `resume` message) |
| `POST /api/project/items/<uuid>/seek`  | `{ "seconds": float }` | `{ "ok": true }` · `404` if not loaded | — |

**Triggering by index** — `…/by-index/<path>` triggers an item by its position instead of its uuid. The `<path>` is an **index path**: a zero-based list of child indices that descends into groups at each level, mirroring the client's `findItemByIndex` / `endBehavior.targetIndex`. A single number (`5`) targets the 6th top-level item; multiple components descend into groups — `1,11` means top-level item `1` (the 2nd item, a group) then its child `11` (the 12th item inside it). Both **comma- and slash-separated** forms are accepted and equivalent, so the same target can be written `…/by-index/1,11` or `…/by-index/1/11` (mixed forms like `1,2/0` work too). Like `/play`, it accepts `GET` so it can be fired from a browser or `curl`, and it routes through `trigger_item` — audio items play, group items dispatch per their `startBehavior`. Returns `400` for a malformed path, `404` when no item exists at that index or the resolved item isn't loaded into the engine.

Project UI settings such as `settings.indexDisplayStart` only change the numbers shown and entered in the client. REST by-index paths stay zero-based for backwards compatibility.

**Stop fades** — an audio item has two fade-outs, kept apart since 2.5.0 (#56):

- The **manual stop fade** is what `stop` (the Stop button, `…/stop`, the WS `stop` frame) fades over: the item's `manualStopFade` in seconds, `0` = cut. An item without the field — anything saved before 2.5.0 — keeps the old rule, `max(stopFade, fadeOutDuration)`, so an existing show's Stop button behaves exactly as it did. A stop's `fade_ms` overrides either for that one stop.
- The **end fade** is what runs when a cue reaches its out-point or the end of its file: `max(stopFade, fadeOutDuration)`, unchanged. `stopFade` still also starts that many seconds before the out-point.

**Wait before next** — an audio item may set `advanceDelay` (seconds, absent/`0` = straight away): after the cue ends, its `next` / `goto-item` / `goto-index` end behaviour waits that long before starting the target (#8). The gapless pre-roll is off for such an item, and the delay is ignored while a crossfade or Start Next is set (they overlap the next cue on purpose). While a wait runs every client is sent `advance_pending`; Stop All and closing the project cancel it. A target already on air when the wait ends is not restarted.

**Loop crossfade** — an item whose `endBehavior.action` is `loop` may set `loopCrossfade` (seconds, absent/`0` = a plain loop). The last N seconds before the loop end are blended equal-power with the first N after the in-point, and playback carries on from in-point + N, so the loop fades back into itself. N is capped at half the trimmed length. The head is decoded on a background job the first time the cue plays with a given in/out/length; until it is ready (normally milliseconds) the loop wraps without the blend. A plain loop also now fills the block that crosses the loop point from the in-point, instead of leaving the rest of it silent.

Stop All ignores both and uses its own fade (`fade_ms`, else `settings.stopAllFadeMs`, default 1000 ms). It also stops the preview audition unless `settings.stopAllStopsPreview` is `false` (absent = `true`).

#### Cart slots

| Method · Path | Body | Response | Broadcast op |
|---------------|------|----------|--------------|
| `POST /api/project/cart`         | `{ "slot": int, "itemUuid": "<uuid>" }` | `{ "ok": true, "slot": int, "itemUuid": "…" }` | `cart_slot_set` |
| `DELETE /api/project/cart/<int>` | — | `{ "ok": true, "slot": int }` | `cart_slot_cleared` |

#### Settings

| Method · Path | Body | Response | Broadcast op |
|---------------|------|----------|--------------|
| `PATCH /api/project/settings` | partial `settings` object | the resulting `settings` object | `settings_patched` |

`PATCH /api/project/theme` is **gone** — a colour scheme belongs to whoever is looking at the screen, not to the show. See [User preferences](#user-preferences).

`settings.meterMode` and `settings.uiScrollToPlaying` moved there too. They stay registered so the drop is *explained* rather than reading as "not a known setting" — a 2.4 client patching one is ignored, not refused, and its whole settings edit still succeeds.

`settings` no longer carries audio routing. `defaultOutputDevice` and `previewDevice` are migrated onto the Master and Preview buses on load and erased (see [Project document](#project-document)); patching them here changes nothing audible and the keys go on the next load. Route through `PATCH /api/buses/<id>` instead.

#### Project export / import (`.lpa` archives)

`.lpa` is a zip of a project folder. Used for transporting projects between machines or between client and server.

| Method · Path | Body | Response |
|---------------|------|----------|
| `POST /api/project/export` | `{ "folderPath": "/abs/project", "outputPath": "/abs/out.lpa" (optional), "projectName": "MyShow" (optional) }` | `{ "archivePath": "/abs/out.lpa", "size": uint64, "downloadToken": "…" (only when outputPath omitted), "downloadFilename": "MyShow.lpa" (only when outputPath omitted) }` |
| `POST /api/project/import` | **multipart** with a `file` part (the `.lpa`) and an `extractPath` text field, *or* **JSON** `{ "archivePath": "/abs/src.lpa", "extractPath": "/abs/dest" }` | `{ "extractPath": "…", "projectFiles": ["one.liveplay", …] }` |

If `outputPath` is omitted on export, the archive is staged in `<tempdir>/liveplay-exports/` and surfaced through a one-shot 10-minute `downloadToken` that's redeemed via `GET /api/file/download`. The temp file is deleted after the download completes.

`.lpa` extraction is sanitised: absolute or `..`-containing entries are rejected.

---

### WebSocket

Single endpoint at `ws://<host>:<port>/ws`. Frames are UTF-8 JSON objects with a `type` discriminator. Binary frames are silently dropped.

On connect, the server adds the connection to the broadcast set and queues a one-shot `playback_snapshot` frame for it (delivered on the next ~16 ms broadcast tick — sending it inline races the broadcast thread on the same connection and was a historical crash source).

#### Server → client frames

| `type`                | Cadence            | Payload |
|-----------------------|--------------------|---------|
| `meters`              | 30 Hz default      | per-cue / per-mixer / per-master meters (see below) |
| `cue_state`           | On transport edge  | `{ "type": "cue_state", "cue_id": "…", "transport": 0\|1\|2\|3, "playhead_seconds": float, "item_uuid": "…" (when known) }` |
| `playback_snapshot`   | On WS connect      | `{ "type": "playback_snapshot", "cues": [{cue_id,transport,playhead_seconds,item_uuid?}], "next_item_uuid": "…", "master_gain_db": float, "output_channel_gains": [{channel,db}], "preview": {item_uuid, cue_id} }` — lets a freshly-reconnected client mirror state without waiting for the next transport edge. `master_gain_db` / `output_channel_gains` are the engine-wide trims (see [Transport & master](#transport--master)); bus faders are in `GET /api/buses`. |
| `doc_patch`           | On every server-side document mutation | `{ "type": "doc_patch", "op": "<op-name>", …op-specific fields }` — see the `op` table below |
| `pong`                | On `ping`          | `{ "type": "pong" }` |
| `error`               | On malformed frame | `{ "type": "error", "message": "…" }` |

**`meters` frame**:

```json
{
  "type": "meters",
  "items": [
    {
      "cue_id":           "…",
      "transport":        1,
      "playhead_seconds": 12.43,
      "sources": [
        { "peak_db": -3.1, "rms_db": -9.2, "peak_max_db": -1.0, "true_peak_db": -2.9,
          "true_peak_max_db": -0.8, "kw_ms": 0.002, "kw_ms_s": 4.1 }
      ]
    }
  ],
  "mixer_channels": [
    {
      "mixer_id": "…", "gate_gr_db": 0.0, "comp_gr_db": -1.2, "correlation": 0.8,
      "peak_db": -6.0, "rms_db": -12.0, "peak_max_db": -4.0,
      "true_peak_db": -5.8, "true_peak_max_db": -3.9, "kw_ms": 0.001, "kw_ms_s": 2.0,
      "lanes": [ { "peak_db", "rms_db", "peak_max_db", "true_peak_db", "true_peak_max_db", "kw_ms", "kw_ms_s" } ]
    }
  ],
  "master_channels": [
    { "index": 0, "peak_db": -0.3, "rms_db": -8.1, "peak_max_db": -0.1,
      "true_peak_db": -0.2, "true_peak_max_db": -0.05, "kw_ms": 0.003, "kw_ms_s": 6.4,
      "gain_reduction_db": -1.4 }
  ]
}
```

Every `*_db` reading pairs a live value (`peak_db`, `rms_db`) with a held maximum (`peak_max_db`) plus true-peak (inter-sample) equivalents (`true_peak_db`, `true_peak_max_db`), and `kw_ms`/`kw_ms_s` are K-weighted (BS.1770) momentary/short-term loudness accumulators. `mixer_channels` covers every engine strip — which is what a bus's meters ride on: match a bus's `mixerId` (from `GET /api/buses`) against `mixer_channels[].mixer_id` to get its meters, gain-reduction and correlation. `lanes` gives one entry per physical channel of the strip (so a stereo strip reports separate L/R) with the combined fields above it derived as their per-field maxima (and `kw_ms`/`kw_ms_s` summed across lanes, per BS.1770). `gate_gr_db`/`comp_gr_db` are single figures per strip (both detectors are linked across its lanes), and `correlation` is inter-channel correlation: `+1` mono-compatible, `0` wide, negative means the lanes are cancelling and material will disappear if anything sums the strip to mono.

Stopped cues are omitted from `items`. Silent master channels (`peak_db <= -119 dB` and `peak_max_db <= -119 dB` and gain reduction `> -0.05 dB`) are omitted from `master_channels` to keep the frame small.

**Transport state values**: `0=Stopped`, `1=Playing`, `2=FadingOut`, `3=Paused`.

**`doc_patch` op vocabulary** — `op` plus the additional fields it carries:

| `op`                            | Additional fields                                          | Emitted by |
|---------------------------------|------------------------------------------------------------|------------|
| `project_changed`               | (none — clients refetch)                                   | `POST /api/project/{load,close}`, `PUT /api/project/document` |
| `project_migrated`              | `itemsToMain`, `busesFromDeviceOverride`, `mainOutputMigrated`, `previewDeviceMigrated`, `ltcDeviceMigrated`, `userPrefsMigrated`, `rolesMigrated` — flat on the frame, not nested | `POST /api/project/load`, `POST /api/project/save`, `PUT /api/project/document`, when the loaded/replaced document had to migrate |
| `item_added`                    | `uuid`, `parentUuid`, `item`, `cueId`                      | `POST /api/project/items` |
| `item_updated`                  | `uuid`, `patch`                                            | `PATCH /api/project/items/<uuid>` |
| `item_removed`                  | `uuid`                                                     | `DELETE /api/project/items/<uuid>` |
| `items_reordered`               | `parentUuid`, `uuids` (array)                              | `POST /api/project/items/reorder` |
| `cart_slot_set`                 | `slot`, `itemUuid`                                         | `POST /api/project/cart` |
| `cart_slot_cleared`             | `slot`                                                     | `DELETE /api/project/cart/<slot>` |
| `prefs_changed`                 | `prefs` (the full resulting profile)                       | `PATCH /api/prefs`. **Not a broadcast** — it reaches only the sessions belonging to the same user, which is what keeps a detached cart or mixer window in step with the desk without telling the rest of the building about somebody's colour scheme. |
| `settings_patched`              | `settings` (full resulting settings object)                | `PATCH /api/project/settings` |
| `master_gain_changed`           | `db`                                                       | `POST /api/master/gain` |
| `limiter_changed`               | `enabled`                                                  | `POST /api/master/limiter` |
| `output_channel_gain_changed`   | `channel`, `db`                                            | `POST /api/master/channels/<n>/gain` |
| `buses_patched`                 | `buses` (full resulting `buses` array — see [Buses](#buses); a role move or a reorder arrives as this one frame) | `POST /api/buses`, `PATCH /api/buses/<id>`, `DELETE /api/buses/<id>`, WS `bus_gain`, WS `bus_mute` |
| `bus_pfl_changed`               | `id`, `pfl`                                                | `POST /api/buses/<id>/pfl`, WS `bus_pfl` |
| `bus_pfl_cleared`               | (none)                                                     | `POST /api/buses/pfl/clear` (only when it actually cleared something) |
| `monitor_mono_changed`          | `mono`                                                     | `POST /api/preview/mono`, `POST /api/monitor/mono` (one handler; the op name is unchanged for compatibility) |
| `outputs_changed`               | same shape as `GET /api/outputs`, plus `rewiredBuses`      | `PUT /api/outputs` |
| `selection_changed`             | `itemUuid` (empty string clears)                           | `POST /api/selection`, WS `set_selection`, WS `select_step` |
| `show_mode_changed`             | `enabled`                                                  | `POST /api/ui/showmode`, WS `set_show_mode` |
| `locale_changed`                | `locale`                                                   | WS `set_locale` (to the sending connection alone); `POST /api/ui/locale` (to connections with no locale of their own) |
| `meter_hz_changed`              | `hz` (the effective rate after clamping)                   | WS `set_meter_hz`, to the sending connection alone |
| `preview_started`               | `itemUuid`, `cueId`                                        | `POST /api/preview` |
| `advance_pending`               | `fromUuid` (the cue that ended, `""` when no wait is running), `dueInMs` | a "wait before next" starting, firing or being cancelled |
| `preview_stopped`               | (none)                                                     | any way a preview ends: `DELETE /api/preview`, Stop All (unless `settings.stopAllStopsPreview` is `false`), the audition reaching its end, the preview role moving, the project closing |
| `next_item_set`                 | `itemUuid` (empty string clears)                           | WS `set_next_item`, and server-armed "Up Next" (auto-cue / first-item / end-of-list wrap) |
| `waveform_ready`                | `item_uuid`, `bucket_count`, `duration_ms`, `sample_rate`, `source_channels`, `channels` | waveform worker, after `/api/waveform_generate` finishes |
| `waveform_failed`               | `item_uuid`                                                | waveform worker on decode failure |
| `custom_action_http`            | `action` (project-defined HTTP action descriptor)          | `ProjectState` external-action handler — server has no HTTP client, so clients perform the fetch |

Clients apply doc_patch ops as idempotent state updates; the originating client also receives the echo (its local state already matches, so the apply is a no-op).

#### Client → server frames

Mostly mirror the REST surface so transport commands can skip the HTTP request/response overhead. The handler is in `handle_ws_message` ([`src/net/control_server.cpp`](src/net/control_server.cpp)). Most transport frames accept **either** `item_uuid` (preferred — honours `duckingBehavior` / `inPoint` / fades / sequencer auto-advance) **or** `cue_id` (raw engine target).

| `type`           | Payload | Effect |
|------------------|---------|--------|
| `play`           | `{ "item_uuid": "…" }` or `{ "cue_id": "…" }` | Starts playback. For groups (when `item_uuid` is a group), walks `startBehavior`. |
| `stop`           | same shape as `play`, plus optional `fade_ms` | Stops with the item's manual-stop fade, or over `fade_ms` when given (`0` = cut). |
| `pause`          | same shape as `play` | Holds the playhead; cue stays loaded. No-op on Stopped cues. |
| `resume`         | same shape as `play` | Resumes from the paused playhead. |
| `seek`           | `{ "item_uuid"\|"cue_id": "…", "seconds": float }` | Sets the playhead. |
| `gain`           | `{ "item_uuid"\|"cue_id": "…", "db": float }` | Sets the per-cue gain. |
| `fade`           | `{ "item_uuid"\|"cue_id": "…", "in_ms": int, "out_ms": int }` | Sets fade durations. |
| `stop_all`       | `{ "fade_ms": 0 }` | Stops every active cue — and the preview audition, unless `settings.stopAllStopsPreview` is `false`. |
| `go`             | `{}` | Same semantics as `GET /api/transport/go`. Replies with an `error` frame if nothing is armed or derivable. |
| `set_next_item`  | `{ "item_uuid": "…" }` (empty/missing clears) | Sets the user-overridden "Up Next" target. Echoed to all clients as `next_item_set`. |
| `set_selection`  | `{ "item_uuid": "…" }` (empty/missing clears) | Sets the shared playlist selection. Broadcasts `selection_changed`, including back to the sender — that's what keeps two clients from diverging. |
| `select_step`    | `{ "delta": int }` | Steps the shared selection through the flattened playlist. With nothing selected, steps from whatever is currently sounding instead of snapping to the top of the show; an explicit selection always wins. Broadcasts `selection_changed`. |
| `set_show_mode`  | `{ "enabled": bool }` (omit to toggle) | Broadcasts `show_mode_changed`. |
| `set_locale`     | `{ "locale": "en" }` | **This connection only.** Replies `locale_changed` down the same socket and to nobody else. |
| `set_meter_hz`   | `{ "hz": 5 }` (`0` = follow the server's rate) | **This connection only.** Thins the `meters` stream for this client; replies `meter_hz_changed` with the *effective* rate, clamped to the server's own tick rate. `cue_state` and `playback_snapshot` are never thinned. |
| `bus_gain`       | `{ "busId": "…", "gainDb": float }` | Same code path as `PATCH /api/buses/<id>`, so it persists and broadcasts `buses_patched` identically. `error` frame to the sender only (no broadcast) if `busId` is missing or unknown. |
| `bus_mute`       | `{ "busId": "…", "mute": bool }` (omit `mute` to toggle the bus's current state) | Same code path as `PATCH /api/buses/<id>`; broadcasts `buses_patched`. `error` frame to the sender only if unknown. |
| `bus_pfl`        | `{ "busId": "…", "pfl": bool }` (omit `pfl` to toggle) | Same code path as `POST /api/buses/<id>/pfl`, including its broadcast shape (`bus_pfl_changed`). `error` frame to the sender only if unknown. |
| `ping`           | `{}` | Server replies with `{ "type": "pong" }`. |

Unknown `type` values get a `{ "type": "error", "message": "unknown type" }` reply.

#### What is shared, and what is yours

Most WS state is deliberately **shared**: selection, Show Mode, bus gain and mute
are the show's state, so a change broadcasts to everyone — that is what keeps two
operators and a Companion surface from diverging, and why a client joining
mid-show adopts what it finds instead of imposing its own stale copy.

**Language and meter rate are not.** They are presentation preferences, and they
belong to the person at the surface. `set_locale` used to write one server-global
string and broadcast it, so one operator switching to Greek switched every other
client and every control surface with them; `set_meter_hz` did not exist, so a
tablet on Wi-Fi paid for the desk's 60 Hz meters. Both are now per connection:

```
built-in default  <  POST /api/ui/locale  <  this connection's set_locale
   "en"              (installation)           (the person)
```

An installation default still exists and still travels — but only to connections
that have expressed no preference, so a client that chose for itself is never
dragged back by the house changing its default. `GET /api/clients` reports each
session's effective values along with `localeIsOwn` / `meterHzIsOwn`, which is
the distinction that decides whether a default change will reach it.

**Theme, meter unit, scroll-to-playing and the transport keymap are not shared
either**, and since 2.5 they are not in the show file at all. They are the same
kind of value as the language — the person's, not the rig's and not the
document's — and they now persist in a [user profile](#user-preferences) rather
than merely lasting as long as a connection. `prefs_changed` is the one
`doc_patch` op that is *not* a broadcast: it reaches only the sessions belonging
to the same user, which keeps a detached cart or mixer window in step with the
desk without telling the rest of the building about somebody's colour scheme.

Meter thinning applies to the `meters` frame **only**. `cue_state` and
`playback_snapshot` are edges, not samples: dropping a sample costs resolution,
while dropping a transition costs the client a fact it will never be told again,
leaving its transport display wrong until something else happens to move.

**Who** the connection is joins the same record. Once accounts exist, the
handshake establishes a principal and the session carries it for its lifetime —
which is what `GET /api/clients` reports and what a log line needs in order to
stay true about a connection that has since ended. It is deliberately *not* the
authority on what anyone may do: a token carries only a user id and an epoch, so
every REST request looks the role up in the store as it stands at that moment,
and a demotion takes effect on the caller's very next request rather than at
their next login. No WebSocket message is admin-gated — the socket carries show
control, which is the operator tier in full — so nothing reads the session's
cached role to decide anything.

### Network event lifecycle (cue trigger)

```
1. User presses Cart #3.
2. CartSlot.vue → useLiveplayServer().play(cueId)
3. WebSocket frame { "type": "play", "cue_id": "…" } sent to server.
4. ControlServer::handle_ws_message → engine_.play(cueId) — O(1) atomic state flip.
5. AudioEngine render thread, next block:
     PlaybackItem.render_block() decodes + applies fade-in envelope.
     Mixer summing through the matrix.
     Master limiter + meter on per-device output buffers.
     Buffers pushed into each device's miniaudio ring buffer.
6. Per-device miniaudio callback drains the ring → hardware DAC.
7. Render thread also pushes amplitude into the 3-tier meters.
8. Broadcast thread, ~16 ms later: snapshot all meters → JSON → fan out to every WS client.
9. Client's LiveMeterBar.vue reflects the new levels in the next frame paint.
```

Round-trip from keypress to first sample at the DAC is dominated by the chosen render-block size + the device's hardware buffer — typically well under 20 ms on Windows with default WASAPI settings.

---

## Project state & file format

`ProjectState` ([`core/project_state.hpp`](include/liveplay/core/project_state.hpp)) owns the canonical, in-memory project document. It's stored as a single nlohmann/json tree; mutations are dispatched through narrow helpers that also emit `doc_patch` broadcasts so clients stay in sync.

### File format

A `.liveplay` project is a folder containing a JSON document plus a `media/` sub-folder. The schema is **v2** of the format, but legacy 1.x files are auto-upgraded on load by `ProjectState::upgrade_legacy_document` ([`src/core/project_state.cpp`](src/core/project_state.cpp)):

- Walks legacy `carts` / `playlist` arrays and reconstructs the v2 `cues` list with the same names, file paths, gains, and fade durations.
- Synthesises stereo master assignments: master channel 0 → default device hw ch 0, master channel 1 → default device hw ch 1.
- Runs the same bus load as a modern document (see [Project document](#project-document)), so a 1.x file comes up with a Master bus on `Main Out` and a Preview bus on `Preview Out` — everything it plays goes out of the house.
- Auto-creates one per-cue mixer channel so each cue still has independent gain/fade.

Result: existing `.liveplay` projects open and play identically. Operators can then open the new Routing UI to split source channels or send to additional devices.

### Backups

`BackupManager` ([`core/backup_manager.hpp`](include/liveplay/core/backup_manager.hpp)) keeps rotating timestamped copies of the project file on every save, so a corrupt write or bad mutation can be recovered.

### Repair

`POST /api/project/repair` walks the document and attempts to fix structural issues (orphaned references, missing routes, dangling cue → file links). It's surfaced in the UI through `ProjectRepairModal.vue`.

---

## Threading model

The server runs ~5 threads:

| Thread             | Owns                                                             |
|--------------------|------------------------------------------------------------------|
| Main               | Crow's I/O reactor, REST handlers, lifecycle, signal handling.   |
| Engine render      | `AudioEngine::render_block()` driven by miniaudio per-device callbacks. Lock-free; no allocations, no exceptions, no syscalls. |
| Meter broadcast    | Snapshots meters at 30 Hz by default and pushes JSON to every WS client. |
| Waveform worker    | Drains an async queue of `/api/waveform_generate` requests off-thread (so REST stays responsive). |
| Discovery          | UDP broadcaster announcing this server on the LAN.                |

Inter-thread communication is lock-free atomics where it's on the audio path; everywhere else, a `std::mutex` guarding the relevant section is fine. **Do not call any potentially-blocking API from the engine render thread.**

---

## Adding features

### A new REST endpoint

1. Register the route in `install_routes()` inside [`src/net/control_server.cpp`](src/net/control_server.cpp).
2. Validate inputs via nlohmann/json — return `crow::status::BAD_REQUEST` on malformed bodies.
3. If the handler mutates `ProjectState`, build a JSON Patch and call `broadcast_doc_patch()` so connected clients stay in sync.
4. Add the matching call on the client in `client/composables/useLiveplayServer.ts`.

### A new audio feature

1. Decide which tier owns it: per-cue (`PlaybackItem`), per-mixer-channel (`MixerChannel`), or master (`AudioEngine`).
2. Hot params (anything the audio thread reads) must be `std::atomic<…>`. Don't add new locks to the render path.
3. If it has visible state (peak, gain reduction, frame index), publish it through a `Meter`-style atomic so the broadcast thread can read it without sync.
4. Unit-testing audio code is awkward — write a small driver that calls `engine.render_block()` directly with synthetic inputs and asserts on the output buffer.

### A new persisted field

1. Add it to the relevant struct in `core/project_state.hpp` (or one of the audio tier classes for non-persistent state).
2. Update the `to_json`/`from_json` adaptors so it round-trips.
3. If old projects need a sensible default, add a fallback in `upgrade_legacy_document`.

---

## Debugging

- `--verbose` enables `DBUG`-level logs. The logger ([`logger.hpp`](include/liveplay/logger.hpp)) is ANSI-coloured by default; set `NO_COLOR=1` for log aggregators.
- The `debug` CMake preset enables assertions + symbols and builds into `build-debug/`.
- `crash_handler.cpp` installs a cross-platform signal/SEH handler that dumps a backtrace on fatal errors. Useful when bug reports come in from operators in the field.
- Smoke-test the binary in CI by running `liveplay-server --help` (`build-server.yml` does exactly this).
- For audio-thread bugs, prefer adding atomic counters / ring-buffer logs rather than `printf` from inside `render_block()`.

For deeper context on the client side of the protocol, see [`client/README.md`](../client/README.md).
