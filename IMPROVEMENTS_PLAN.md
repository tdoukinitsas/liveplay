# LivePlay — Improvements & Feature Plan

> **Audience:** This document is written for a fresh agent (or developer) with **zero prior context**.
> It describes the current architecture, then specifies six feature workstreams in implementation
> stages, with design notes, files to touch, risks, and a recommended AI model per task.
> **Nothing in this document is implemented yet.**

---

## 1. Current Architecture (read this first)

LivePlay is an open-source audio cue playback application for live sound operators
(theatre, radio, events). It is a **client/server** app:

```
┌─────────────────────────────┐        TCP 4480 (REST + WebSocket)        ┌──────────────────────────────┐
│  Client                     │ ◄───────────────────────────────────────► │  Server                      │
│  Vue 3 / Nuxt 4 / Electron  │        UDP 4481 (LAN discovery)           │  C++20, miniaudio, Crow      │
│  client/                    │                                           │  server/                     │
└─────────────────────────────┘                                           └──────────────────────────────┘
```

- The **server owns all audio**: decoding, mixing, routing, metering, limiting, the playback
  **sequencer** (auto-advance / end behaviours / timed custom actions), LTC output, project
  state, autosave/backup. The client is a remote control + editor UI. Multiple clients can
  connect; mutations are fanned out via `broadcast_doc_patch` WebSocket messages.
- The **client** is an Electron-wrapped Nuxt SPA. Electron main (`client/electron/main.js`)
  spawns/manages the bundled server binary, handles file dialogs, ffmpeg/ffprobe, yt-dlp
  YouTube import, and auto-update.

### 1.1 Repository layout

| Path | What it is |
|---|---|
| `client/app/` | Nuxt app — all UI components, composables, types |
| `client/app/types/project.ts` | **The project data model** (AudioItem, GroupItem, EndBehavior, CustomAction, CartItem, Project) |
| `client/app/composables/useProject.ts` | Project load/save/mutation, doc-patch sync (~1,860 lines) |
| `client/app/composables/useLiveplayServer.ts` | REST/WS client to the C++ server (~1,030 lines) |
| `client/app/composables/useAudioEngine.ts` | Client-side playback orchestration glue (~640 lines) |
| `client/app/composables/useMidiController.ts` | Web-MIDI input → action bindings (client-side only) |
| `client/app/components/MainWorkspace.vue` | Top-level layout: playlist + cart + properties panel |
| `client/app/components/PropertiesPanel.vue` | Per-item editor (fades, trim, end behaviour, LTC…) (~1,200 lines) |
| `client/app/components/PlaylistItem.vue` / `PlaylistView.vue` | Cue list rows / list |
| `client/app/components/CartPlayer.vue` / `CartSlot.vue` | 4×4 hot-key cart wall |
| `client/app/components/RoutingMatrixPanel.vue` | Existing routing UI (items → mixers → masters → device channels) |
| `client/electron/main.js` | Electron main: server lifecycle, dialogs, ffmpeg, yt-dlp, updater |
| `server/src/audio/engine.cpp` (+`include/liveplay/audio/engine.hpp`) | **AudioEngine**: devices, PlaybackItems, MixerChannels, routing topology (lock-free atomic snapshot), render thread, per-master Limiter+Meter |
| `server/src/audio/playback_item.cpp` | One cue = one PlaybackItem (decode, gain, fades) |
| `server/src/audio/mixer_channel.cpp` | Tier-2 mixer strip |
| `server/src/audio/limiter.cpp`, `meter.cpp` | Brickwall limiter, K-style meter (per master channel) |
| `server/src/audio/ltc_generator.cpp` | SMPTE LTC output |
| `server/src/core/project_state.cpp` (~3,480 lines) | Project JSON store, **sequencer** (end behaviours, ducking, start-next segue, custom-action dispatch), autosave |
| `server/src/net/control_server.cpp` (~2,500 lines) | Crow REST + WebSocket API (see §1.2), meter broadcast, doc-patch fan-out |
| `server/src/net/discovery.cpp` | UDP LAN discovery (port 4481) |
| `docs-site/` | Nuxt documentation website (i18n, English fallback) |
| `scripts/` | Monorepo build scripts (build server via CMake preset `vs2022` on Windows, package Electron) |

### 1.2 Server API surface (existing, in `control_server.cpp`)

REST: `/api/health`, `/api/devices[...]`, `/api/cues[...]` (register/play/stop/gain/fade/ltc),
`/api/transport/stop_all`, `/api/routing/{item_to_mixer,mixer_to_master,master_to_device}`,
`/api/mixers`, `/api/fs/list`, `/api/upload`, `/api/project/{load,save}`, `GET /api/project`.

WebSocket `/ws`: server→client `meters` (~10 Hz, client interpolates), `cue_state` transitions,
`doc_patch` (multi-client project sync); client→server transport commands.

### 1.3 Key existing concepts you will build on

- **CustomAction** (`client/app/types/project.ts`): per-item timed actions already exist —
  `{ timePoint: seconds, action }` where action ∈ `play-item | play-index | stop-all | http-request`.
  The **server sequencer** fires them when the playhead crosses `timePoint`
  (`project_state.cpp` ~line 3100, `execute_custom_action` ~line 3287). `http-request` is
  currently **delegated to a connected client** via the broadcast hook (server does not do HTTP out).
  → The per-item event system (§3) is a generalisation of this machinery.
- **MixerChannel / routing matrix / 32 master buses** already exist in the engine
  (items → mixers → masters → device channels, immutable `Topology` snapshot read lock-free
  by the render thread). → The bus/mixing feature (§6) extends this, it does not start from zero.
- **Per-master Limiter + Meter** already exist and are persistent across topology rebuilds
  (`MasterChannelState` in `engine.hpp`). → The multiband compressor and EQ (§5) slot in beside them.
- **Output Target** feature: platform loudness presets (EBU R128 etc.); the server owns the numbers.
- **Client-side MIDI input** exists (Web MIDI, `useMidiController.ts`) for triggering carts/transport.
- **Index paths**: items are addressed by `index: number[]` (nested groups); carts are `[-1, slot]`.
- **No automated tests exist anywhere in the repo** (see §8.1).

### 1.4 Build & run

- `npm run dev:all` — server + client dev concurrently. `npm run server:build` — CMake build
  (Windows preset `vs2022`, output `server/build/Release/liveplay-server.exe`).
- Dependencies: server via vcpkg (`server/vcpkg.json`); client is npm workspace `client`.
- Version bumps via `npm run bump` (syncs root, client, server, docs-site versions).

---

## 2. Stage 1 — Touch-Friendly Playback Mode

### Goal
A "show mode" toggle that strips all editing affordances and presents a pure playback surface
usable on small touch screens (tablets, rack-mounted touch displays): big targets, no
accidental edits, no misclicks.

### Design
- **Mode toggle** in the project header (and a keyboard/MIDI/API action so external controllers
  can enter/exit it). Persist per-device (localStorage), not in the project file — the same
  project may be open on an editing laptop and a touch tablet simultaneously.
- **What disappears:** PropertiesPanel, waveform trimmer, drag-to-reorder, right-click/context
  menus, inline rename, routing matrix, import buttons, delete actions.
- **What remains, enlarged:** playlist (read-only rows, min ~56 px touch height, WCAG 2.5.5
  suggests ≥44 px), GO / pause / stop-all transport bar (fixed, thumb-reachable at bottom),
  cart wall (already grid-shaped — good for touch), master fader with a **press-and-hold or
  double-tap guard** on destructive actions (stop-all should require confirmation or a 2-step
  swipe), up-next display.
- **Misclick protection:** stop-all and item-stop get either long-press (500 ms) or a two-tap
  confirm; disable browser touch gestures (`touch-action: manipulation`, no pinch zoom);
  generous spacing (≥8 px gutters) between adjacent triggers.
- **Implementation shape:** a global `uiMode: 'edit' | 'playback'` ref in a new composable
  `useUiMode.ts`; components branch with `v-if`/CSS classes. Prefer a dedicated
  `TouchPlaybackView.vue` that reuses existing composables (`useAudioEngine`,
  `useLiveplayServer`, `useCartItems`) rather than sprinkling hundreds of `v-if`s through
  `MainWorkspace.vue` — the playback surface is simple enough that a purpose-built layout is
  less risky and easier to make touch-perfect.
- Add an "exit playback mode" affordance that is deliberate (e.g. long-press on a corner icon
  or a keyboard shortcut) so operators can't fall out of show mode mid-show.

### Files to touch
`client/app/components/MainWorkspace.vue`, new `TouchPlaybackView.vue`, new
`composables/useUiMode.ts`, `ProjectHeader.vue` (toggle), `PlaybackControls.vue` (big-button
variant), `CartPlayer.vue`/`CartSlot.vue` (size variant), locales in `client/locales/`.

### Risks / notes
- Client-only feature; no server changes.
- Test on an actual touch device — Electron on Windows fires pointer events differently than
  mouse; use pointer events (`pointerdown`) not `mousedown`/`click` mixes.
- Keep i18n: every new string goes through `useLocalization`, and translation stubs must be
  added to all locale files (repo has had translation-stub fixes before).

**Recommended model:** Claude Sonnet (UI/CSS/Vue work, well-bounded, lots of iteration) — use
Opus/Fable for the initial UX layout decisions if you want stronger one-shot design judgement.

---

## 3. Stage 1 — Per-Item Event System (timed markers → external protocols)

### Goal
Users add markers at any time point in an item; when the playhead crosses a marker the server
fires an event. Event types: **HTTP GET/PUT/POST/PATCH, OSC, MIDI, Art-Net, MSC (MIDI Show
Control), raw HEX payloads, TCP, UDP**, plus a **plugin architecture** for custom device
definitions.

### Design principle
**Extend, don't replace, the existing `CustomAction` machinery.** The sequencer already fires
`customActions` at time points server-side (`project_state.cpp` ~3100). Two changes:

1. **Widen the action type** in `client/app/types/project.ts` and the server dispatcher:

```ts
export type CustomActionType =
  | { type: 'play-item'; uuid: string }
  | { type: 'play-index'; index: number[] }
  | { type: 'stop-all' }
  | { type: 'http-request'; request: HttpRequest }        // extend methods with PATCH
  | { type: 'osc';    host: string; port: number; address: string; args: OscArg[] }
  | { type: 'midi';   port: string; message: number[] }   // raw bytes, or structured note/cc
  | { type: 'msc';    port: string; deviceId: number; commandFormat: number; command: number; cue?: string }
  | { type: 'artnet'; host: string; universe: number; channels: Record<number, number> }
  | { type: 'tcp';    host: string; port: number; payload: string; encoding: 'utf8' | 'hex' }
  | { type: 'udp';    host: string; port: number; payload: string; encoding: 'utf8' | 'hex' }
  | { type: 'plugin'; pluginId: string; actionId: string; params: Record<string, unknown> };
```

2. **Move execution fully server-side** so events fire even with no client connected (the
   current `http-request` client-delegation is a wart; keep it as fallback but the server
   should own outbound I/O). New server module: `server/src/events/event_dispatcher.{hpp,cpp}`
   with one sender per protocol:
   - HTTP: vcpkg `cpr` (libcurl wrapper) — add to `server/vcpkg.json`.
   - OSC: tiny — hand-roll the OSC 1.0 binary encoding over UDP (~150 lines) or vcpkg `oscpack`.
   - UDP/TCP/HEX: raw sockets (Crow already brings ASIO — reuse `asio::ip::udp/tcp`).
   - Art-Net: it's just a UDP packet format (ArtDMX opcode 0x5000, universe, 512 ch) — hand-roll.
   - MIDI / MSC: needs a MIDI out port — vcpkg `rtmidi`. MSC is a SysEx payload
     (`F0 7F <devId> 02 <cmdFmt> <cmd> ... F7`) over that port.
   - **Timing:** the sequencer tick drives dispatch; senders must be **fire-and-forget on a
     worker queue** (never block the sequencer or audio threads). One `EventDispatcher` thread
     with an MPSC queue is enough; HTTP goes further onto its own pool.

3. **Marker UI:** markers drawn on the waveform (`WaveformCanvas.vue` /
   `WaveformTrimmer.vue`) — draggable, coloured by event type; an "Events" section in
   `PropertiesPanel.vue` listing markers with per-type parameter forms; a test-fire button per
   event (calls a new `POST /api/events/test` endpoint so users can verify a rig without playing).

4. **Plugin architecture** (custom device definitions): start **declarative, not code**.
   A plugin is a JSON/YAML file in `userData/plugins/` (client) mirrored to the server:
   ```jsonc
   {
     "id": "hyperdeck", "name": "BMD HyperDeck", "version": 1,
     "actions": [{
       "id": "play", "name": "Play clip",
       "params": [{ "key": "host", "type": "string" }, { "key": "clip", "type": "number" }],
       "emit": { "type": "tcp", "host": "{{host}}", "port": 9993, "payload": "play: clip id: {{clip}}\n" }
     }]
   }
   ```
   i.e. plugins are **templates over the built-in transports** with `{{param}}` substitution.
   This covers 90 % of devices (projectors, video servers, lighting desks) with zero sandboxing
   or scripting-engine risk. A later phase can add a JS scripting hook (the Electron side or an
   embedded interpreter) if genuinely needed — do not start there.

### Migration
Existing projects have `customActions` with the old 4 types — the widened schema is a strict
superset, so no migration needed; but bump the project `version` and keep the server dispatcher
tolerant of unknown types (log + skip).

### Files to touch
`client/app/types/project.ts`, `PropertiesPanel.vue` (events editor), `WaveformCanvas.vue` /
`WaveformTrimmer.vue` (marker rendering/drag), new `client/app/components/EventEditor.vue`;
server: new `src/events/*`, `project_state.cpp` (`execute_custom_action` → delegate to
dispatcher), `control_server.cpp` (test-fire endpoint, plugin listing endpoints),
`vcpkg.json` (+cpr, +rtmidi), `CMakeLists.txt`.

### Risks
- Never block the sequencer thread (worker queue mandatory).
- MIDI out on Linux/macOS/Windows via rtmidi differs in port naming — expose port enumeration
  through a new `GET /api/midi/outputs`.
- Art-Net floods networks if sent per-tick — events are one-shot sends, fine; document that.

**Recommended model:** Claude Fable/Opus for the server-side dispatcher + protocol encoders
(threading, binary protocols, C++ — highest defect cost), Sonnet for the Vue marker/editor UI.

---

## 4. Stage 2 — Bitfocus Companion Support (Stream Deck)

### Goal
Control LivePlay and view its status from a Stream Deck via Bitfocus Companion.

### Design
Companion integrations are **TypeScript modules living in their own repo** published to the
Companion module registry (`companion-module-<name>`, base package
`@companion-module/base`). The module is a *client* of LivePlay's existing API — so this
workstream is 20 % LivePlay changes, 80 % a new small repo.

1. **New repo** `companion-module-tdoukinitsas-liveplay` (scaffold with
   `yo @companion-module/generator` or copy an existing module; check current Companion v3+
   docs at github.com/bitfocus/companion-module-base — verify current API, knowledge may be stale).
   - **Config:** host + port (default 4480). Optionally use LivePlay's UDP 4481 discovery for a
     "found instances" dropdown.
   - **Actions:** play/stop/pause item by index path or UUID, GO (play-next), stop-all (with
     fade ms), trigger cart slot 1–16, master gain set/step, toggle limiter, load project.
   - **Feedbacks:** item playing/paused (button turns green/red), cart slot active, connection
     state, limiter engaged.
   - **Variables:** current item name, elapsed/remaining time, up-next name, master gain,
     project name, LUFS/meter values (throttled).
   - Transport: reuse the WebSocket `/ws` for push state (`cue_state`, `meters`) — poll nothing.
2. **LivePlay server additions** (small): a stable **external-control surface** —
   - `GET /api/state/summary` — compact machine-readable transport state (playing items with
     index, name, elapsed/duration, up-next) so Companion doesn't need the whole project JSON.
   - Ensure every action listed above is reachable via REST/WS with **index-path addressing**
     (`play-index` exists in the custom-action vocabulary; expose it as a first-class endpoint,
     e.g. `POST /api/transport/play_index { index: [2,0] }`, `POST /api/transport/go`).
   - Document the protocol in `docs-site` under a new "External control / API" page — this also
     serves the event system (§3) and any future integration.
3. Publish the module via Bitfocus' PR process; meanwhile users can sideload it
   (Companion dev-modules folder).

### Files to touch
LivePlay: `server/src/net/control_server.cpp` (+summary/transport endpoints), docs-site.
New repo: full Companion module (TS).

### Risks
- Companion module API versions move quickly — **verify against current
  `@companion-module/base` docs before coding**, don't trust training data.
- Meter variables must be throttled (≤2 Hz) or Companion UI chokes.

**Recommended model:** Claude Sonnet (the module is well-templated TS following extensive
existing examples; fetch a real module like `companion-module-figure53-qlab` as reference).
Use Fable/Opus only for designing the `/api/state/summary` contract.

---

## 5. Stage 3 — Multiband Compressor (per output channel) + Pro-Q-style EQ (outputs & tracks)

### Goal
1. A multiband compressor on each output (master) channel.
2. A graphical parametric EQ with **unlimited bands**, FabFilter Pro-Q-style interaction
   (screenshot provided by the user: dots on a frequency-response curve over a live spectrum
   analyzer, drag to set freq/gain, scroll for Q, per-band type/slope, floating band toolbar),
   available on **output channels and individual tracks**.

### Where DSP goes
All DSP is server-side in the render path (`engine.cpp` / new files under `server/src/audio/`).
The engine already has the exact seam: `MasterChannelState { limiter, meter }` per master
channel — add `eq` and `multiband_comp` there (chain: **EQ → multiband comp → master gain →
limiter → meter**). For tracks, `PlaybackItem` gets an optional EQ processing stage after
decode/fade.

### DSP components (write from scratch, they're standard)
- **EQ band:** RBJ biquad cookbook filters (bell, low/high shelf, low/high cut with selectable
  slope 6–96 dB/oct via cascaded biquads, notch, band-pass). A band = type + freq + gain + Q +
  enabled + channel placement (stereo/L/R/M/S — M/S can be a later phase). "Infinite" bands =
  `std::vector<Band>`; recompute coefficients on param change (control thread), publish to the
  audio thread the same way the engine already publishes topology (atomic shared_ptr snapshot —
  **follow the existing `AtomicSharedPtr` pattern in `engine.hpp`**). Smooth coefficient
  changes over one block to avoid zipper noise.
- **Multiband compressor:** 3–6 bands, Linkwitz-Riley 4th-order crossovers (LR4 = two cascaded
  Butterworth biquads, sums flat), per-band: threshold/ratio/attack/release/makeup, envelope
  follower per band, optional lookahead later. Per-band + global bypass, per-band gain-reduction
  metering (extend `MeterSnapshot`).
- **Spectrum analyzer** (the Pro-Q background curve): server computes FFT (e.g. 2048-pt, Hann,
  ~30 Hz update) per EQ instance **only while a client has that EQ UI open** (subscription
  message over WS, otherwise wasted CPU) and streams magnitude bins over the existing meter
  broadcast channel. vcpkg: `kissfft` (tiny) — or hand-roll radix-2.

### API & model
- Project schema: `eq?: { bands: EqBand[] }` on `AudioItem`, and a new per-master-channel DSP
  section in project settings (`outputDsp: Record<masterIndex, { eq, multibandComp }>`).
- REST/WS: `POST /api/cues/{id}/eq`, `POST /api/master/{ch}/eq`, `POST /api/master/{ch}/mbcomp`
  with full-state payloads (bands array) — param changes stream over WS for drag smoothness
  (throttled ~30 Hz, coalesced).

### UI (`client/app/components/EqEditor.vue` — a large canvas component)
- Log-frequency axis 10 Hz–30 kHz, dB axis ±30. Layers: grid → spectrum (filled polyline) →
  per-band response curves (tinted, like the screenshot's magenta/blue/green) → **summed
  response curve** (yellow) → band handles (dots).
- Interactions: double-click/tap empty space = add band; drag dot = freq/gain; scroll/alt-drag =
  Q; right-click = band menu (type, slope, channel, delete); floating toolbar for the selected
  band(s) (the screenshot's bottom panel: freq/gain/Q knobs, stereo placement, band on/off);
  multi-select with shift.
- Compute the drawn curves **client-side** (same biquad math in TS, evaluate |H(e^jω)| at ~200
  log-spaced points) so dragging is 60 fps locally; server state is the authority, client
  optimistically renders and reconciles.
- Multiband comp UI: crossover handles on a spectrum + per-band GR meters and knobs.

### Suggested implementation order
1. Biquad/EQ DSP + unit tests offline (feed sine sweeps, assert magnitude response) — this is
   where a test harness (§8.1) pays for itself.
2. Engine integration (master EQ only) + REST + minimal UI (sliders, no canvas).
3. Canvas EQ editor + spectrum stream.
4. Track EQ (PlaybackItem stage + per-item UI entry point in PropertiesPanel).
5. Multiband compressor DSP + UI.

### Risks
- **Real-time safety:** no allocation/locks on the render thread; snapshot-swap pattern for
  coefficient sets; denormal protection (FTZ/DAZ or add tiny DC offset).
- CPU: N tracks × M bands of biquads is cheap, but the spectrum FFTs are not — subscribe/
  unsubscribe strictly.
- The EQ canvas is the largest single UI component in the app; budget accordingly.

**Recommended model:** Claude Fable/Opus for all DSP + engine integration and for the EQ canvas
interaction model (numerically subtle, threading-sensitive, high polish bar). Sonnet is fine
for the parameter forms, serialization plumbing, and locale strings.

---

## 6. Stage 3 — Bus Mixing, Workspaces / Detachable Panels, VST Plugins

### Goal
Items assignable to named buses; buses mixed independently (fader/mute/solo/meter/DSP);
eventually VST plugin hosting; UI reorganised into workspaces or detachable panels to fit the
new mixer surface.

### What already exists (important!)
The engine **already has the full 3-tier routing architecture**: PlaybackItems → MixerChannels
→ 32 master buses → device channels, with a routing matrix API (`/api/routing/*`,
`RoutingMatrixPanel.vue`). "Buses" ≈ MixerChannels. What's missing is:
1. **Project-level bus semantics:** named, ordered, persistent buses in the project file
   (`buses: [{ id, name, color, gainDb, mute, solo, masterSends }]`), an `AudioItem.busId`
   assignment (default "Main"), and ProjectState code that materialises these into engine
   mixer channels + routes on load (it already does something similar for routing).
2. **Mixer strip state in the engine:** MixerChannel likely has gain; add mute/solo (solo =
   control-thread computed mute-others, don't put solo logic in the render thread) and
   **per-mixer metering** (currently meters are per-master only — add `Meter` to
   `MixerChannel`, extend the WS meter broadcast with a `mixers` section).
3. **Mixer UI:** new `MixerPanel.vue` — vertical strips (fader = existing `CanvasFader.vue`,
   mute/solo, meter = existing `LiveMeterBar.vue`, bus DSP button opening the §5 EQ/comp for
   that bus), plus a bus selector dropdown on each playlist/cart item (PropertiesPanel + a
   compact badge on `PlaylistItem.vue`).
4. **Workspaces / detachable panels:** two options —
   - *Workspaces (recommended first):* named layout presets (Playlist / Mixer / Editing /
     Show) switching which panels are visible in `MainWorkspace.vue`, persisted per-device.
     Cheap, no window management.
   - *Detachable panels:* Electron `BrowserWindow` per detached panel loading the same Nuxt
     app with a `?panel=mixer` route/query; state stays consistent because **all state lives
     on the server** and every window is just another WS client — this architecture makes
     detachment unusually easy for this app. Do workspaces first, detach second.
5. **VST hosting (last, and by far the hardest):**
   - Legal/practical: VST2 SDK is unavailable (licensing) — target **VST3** (GPL3-compatible
     SDK, fine with this repo's AGPL) and consider **CLAP** (MIT, much simpler API) and LV2 on
     Linux. Recommendation: implement **CLAP first** (clean C API, permissive), VST3 second.
   - **Out-of-process hosting is strongly recommended:** a separate `liveplay-plugin-host`
     process per plugin (or per chain) with shared-memory audio rings, so a crashing plugin
     can't take down the show. This is what Bitwig/Reaper do. In-process is easier but one
     bad plugin kills live audio.
   - Plugin **editor UIs** are native windows — they must be opened by a process on the
     server machine. For the common local case (client+server same machine) fine; for remote
     servers show generic parameter sliders instead (CLAP/VST3 expose parameter lists).
   - Insert points: per-bus (MixerChannel) inserts first; per-item later.
   - This is a multi-month workstream on its own; keep it strictly last and behind an
     "experimental" flag.

### Suggested order
buses in project schema → engine mute/solo/mixer-meters → MixerPanel UI → workspaces →
detachable panels → CLAP hosting (out-of-proc) → VST3.

### Risks
- Solo semantics with ducking/stop-all interactions need care (define: solo affects monitoring
  only, never the sequencer).
- Project-file version bump + migration (older projects get one implicit "Main" bus).
- VST: sample-rate/block-size negotiation, plugin latency compensation (report latency, delay
  dry paths) — defer PDC to a later iteration but leave room in the render graph design.

**Recommended model:** Claude Fable/Opus for engine changes, the detachable-window architecture
and everything VST/CLAP (hardest work in this document). Sonnet for MixerPanel.vue and workspace
layout persistence.

---

## 7. Cross-Cutting: Recommended Additional Improvements (agent's suggestions)

### 7.1 Stage 1 (do early — they de-risk everything above)

| Improvement | Why / What |
|---|---|
| **Automated test infrastructure** | There are currently **zero tests**. Add: (a) C++ unit tests (Catch2 via vcpkg) for DSP (limiter, upcoming EQ/comp), sequencer end-behaviour logic, project (de)serialisation; (b) Vitest for composables (`useProject` mutations, index-path math); (c) one API smoke test that boots the server headless and exercises REST. Without this, the DSP work in §5 is not verifiable. **Model: Sonnet** (mechanical, high volume). |
| **Undo/redo** | An editor for live shows without undo is dangerous. `useProject.ts` centralises mutations — add a command/patch history (the doc-patch mechanism is already patch-shaped; record inverse patches). Multi-client makes global undo hard: scope it to *local* edits first. **Model: Fable/Opus** (concurrency semantics), UI trivial. |
| **Show-mode safety lock** | Complements §2: while any cue is playing, guard destructive edits (delete item, close project) behind confirmation. Cheap, high value. **Model: Sonnet.** |
| **API documentation page** | Document REST/WS in docs-site (needed by §4 Companion and §3 plugins anyway). **Model: Sonnet/Haiku.** |

### 7.2 Stage 2

| Improvement | Why / What |
|---|---|
| **Remote web client** | The client is a Nuxt SPA talking to a network server — serve the built SPA from the C++ server (Crow static routes) so any browser/tablet on the LAN can open `http://server:4480` with the touch UI from §2. Huge operational win for ~small effort (audit Electron-only codepaths behind `window.electron` guards — they exist in `global.d.ts`). **Model: Fable/Opus** for the audit, Sonnet for execution. |
| **PFL/preview bus** | Pre-listen exists (`settings.previewDevice`); formalise it as a proper cue/PFL bus in the §6 bus model with its own device assignment. **Model: Sonnet** once §6 lands. |
| **Streaming decode for long files** | Verify whether `PlaybackItem` fully decodes into RAM (likely); 2-hour show files × many cues = GBs. Add chunked/streaming decode with a decoded ring ahead of the playhead. **Model: Fable/Opus** (audio-thread correctness). |
| **Crash-recovery UX** | Server has crash handler + backup manager; add a "restore session (items that were playing)" flow on reconnect. **Model: Sonnet.** |

### 7.3 Stage 3

| Improvement | Why / What |
|---|---|
| **Timeline / multi-track view** | Once events (§3) and buses (§6) exist, a horizontal timeline showing overlapping cues, start-next segues and event markers becomes the natural power-user view (theatre workflows). Big UI project. **Model: Fable/Opus.** |
| **Cue-list scripting hooks** | The declarative plugin format (§3) plus a JS hook stage ("on cue start/end run script") — after the event system proves itself. **Model: Fable/Opus** (sandboxing decisions). |
| **Accessibility pass** | Keyboard-only operation audit, ARIA on custom widgets (faders, canvas EQ needs a parameter-list fallback), contrast in both themes. **Model: Sonnet** with the a11y-debugging tooling. |

---

## 8. Sequencing Summary & Model Cheat-Sheet

Recommended overall order (respects dependencies):

1. **Test infrastructure (§7.1)** — unblocks safe iteration on everything.
2. **Touch mode (§2)** — isolated, client-only, ships value fast.
3. **Event system (§3)** — server dispatcher first, then marker UI, then declarative plugins.
4. **API docs (§7.1) → Companion module (§4)** — docs feed the module.
5. **Remote web client (§7.2)** — multiplies the value of §2 and §4.
6. **EQ DSP → EQ UI → multiband comp (§5)** — needs tests from step 1.
7. **Buses & mixer UI → workspaces → detachable panels (§6)**.
8. **CLAP/VST hosting (§6, experimental)** — last; largest risk.

| Task type | Recommended model |
|---|---|
| C++ DSP, audio-thread/lock-free code, plugin hosting, undo semantics, streaming decode | **Claude Fable 5 / Opus** — highest reasoning need, mistakes are audible or crash shows |
| Server protocol encoders (OSC/Art-Net/MSC), event dispatcher threading | **Fable/Opus** |
| Vue UI (touch mode, mixer panel, forms, modals), Companion TS module, tests, docs-site | **Claude Sonnet** — fast, cheap, strong on well-patterned web code |
| Locale stubs, mechanical refactors, doc formatting | **Claude Haiku** |
| EQ canvas editor (interaction design + numerics) | **Fable/Opus** for the core, Sonnet for polish |

### Conventions any implementing agent must follow
- All user-visible strings go through `useLocalization`; add stubs to **every** locale file in `client/locales/`.
- The server owns state; the client mutates via REST/WS and receives `doc_patch` fan-out — never fork state client-side.
- New engine state read by the render thread must use the snapshot/atomic pattern (`AtomicSharedPtr`, see `engine.hpp`) — no locks or allocation on the audio path.
- Project-schema additions must be optional-with-defaults so old project files load unchanged; bump `Project.version` when semantics change.
- Version bumps via `npm run bump`; Windows server build via `npm run server:build` (CMake preset `vs2022`); dev loop via `npm run dev:all`.
