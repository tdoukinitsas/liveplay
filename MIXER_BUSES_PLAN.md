# MIXER_BUSES_PLAN.md — Finishing the mixer-bus / DSP work for 2.5.0

> **Audience.** This document is written for implementation agents with **zero prior context**
> and for one orchestrator agent running unattended. Every decision has already been taken and
> is recorded here (§3). If a task seems to require a decision this document does not record,
> the correct move is to STOP that task, write the question into its Notes cell in §6, mark it
> `BLOCKED`, and move on — never guess, never invent policy.
>
> **Branch discipline (absolute).** All work happens on `2.5.0-dev`. Commit per task to
> `2.5.0-dev`. **NEVER commit to `main`. NEVER push any branch.** The maintainer verifies and
> pushes manually.
>
> Written 2026-08-21 from a full audit of `2.5.0-dev` at merge `7c0607d` (PR #59,
> `experimental/mixing-and-dsp`), plus the decisions agreed in GitHub issue #58 and with the
> maintainer directly. Companion reading: [`BUS_ARCHITECTURE.md`](BUS_ARCHITECTURE.md) — the
> design record for everything already built. Its §0 status header is stale (it predates the
> merge and the last ~15 commits); this document is current.

---

## 1. Current state (audited, with evidence)

### 1.1 What is DONE and working on `2.5.0-dev`

- **Bus model + persistence + migration machinery.** Buses live in `document_["buses"]`,
  materialise onto engine strips, resolve by ancestor walk (item `busId` → nearest ancestor
  group → Main). Full REST CRUD. `server/src/core/project_state.cpp` (`load_buses_locked`
  :3764, `migrate_device_overrides_locked` :3868, `resolve_item_bus` :4690).
- **Mixer UI**: strip rail + pinned master, docked side pane / full workspace / detached
  Electron window (`?mixerWindow=1`), full-window channel view with EQ, dynamics,
  contributions, select row. Components: `MixerPanel/MixerStrip/MixerActions/
  MixerChannelDetails/MixerChannelFader/MixerEqPanel/MixerDynamicsPanel/Knob/KnobField/
  StereoMeter/MeterScale/CanvasFader` in `client/app/components/`.
- **PFL + Monitor-as-preview** (pre-fader, pre-mute; Monitor owns the reserved master pair;
  cue pre-listen and PFL sum in one strip). Solo is gone.
- **The complete fixed DSP chain** per strip: HPF → LPF → 4-band EQ (shelvable outer bands,
  draggable curve handles) → gate → compressor → M/S width (+bass-mono) → PFL tap → fader →
  pan/balance send. `server/include/liveplay/audio/{biquad,dynamics,stereo_width,channel_dsp}.hpp`.
  Parameters cross to the render thread via double-buffered `StripCoeffs`; filter coeffs ramp,
  dynamics coeffs are assigned whole in `advance_coeffs()` (forgetting that assignment is the
  historical gate bug — `BUS_ARCHITECTURE.md` §0.2b).
- **OutputMap**: server-owned `outputs.json` beside the exe, logical-name → device/channel,
  identity fallback, `GET|PUT /api/outputs` with selective rewire
  (`server/src/core/output_map.cpp`, `project_state.cpp:3992` `rewire_buses_for_output_map`).
- **Latency**: output queue cut 427 ms → ~37 ms, measured; `GET /api/engine/stats`.
- **Diagnostics for the save-pop**: seam detector on masters 0/1
  (`server/src/audio/engine.cpp:1532-1574`), `discontinuities` / `worstSeam` /
  `mutexWaitUsMax` / `topologyRebuilds` in `/api/engine/stats`.
- **Tests**: 7 unit suites (`ctest`), 15 e2e Node scripts in `server/tests/e2e/` (need a
  running server + real audio device; see `server/tests/e2e/README.md`).

### 1.2 What is INCOMPLETE or BROKEN (this plan's work)

| # | Finding | Evidence |
|---|---|---|
| 1 | **Bus→bus routing does not exist.** `output.type:"bus"` is accepted, logs a warning, stays silent. No mixer→mixer route type, no topological order, no cycle check. | `project_state.cpp:4238-4242`; no `route_mixer_to_mixer` anywhere; `engine.cpp:157-234` builds no order |
| 2 | **No output-map (hardware remap) UI.** `saveOutputs()` exists with zero call sites; `outputs.json` is hand-edit only. Unbound bus = border colour + tooltip, a dead end. | `useLiveplayServer.ts:997`; `MixerStrip.vue:437-453` |
| 3 | **Legacy 1.x / snake_case documents load with NO bus code run** — empty mixer, no Main/Monitor. The literal "nothing got routed to the mixer" from issue #58. | `project_state.cpp:5199-5296` never calls `load_buses_locked` |
| 4 | **Migration is silent** — no client warning; and a migrated project shows zero rail strips (everything is on hidden system bus Main), so it *looks* unrouted. | `project_state.cpp:3913` (info log only); `MixerPanel.vue:211` filters `system` |
| 5 | **Cross-project bus carry-over**: `PUT /api/project/document` with a bus-less doc while another project is loaded keeps the *old* project's buses; the incoming `defaultOutputDevice` is then discarded. | `project_state.cpp:1859-1863`, `:3838-3842` |
| 6 | **Save-time pop: instrumented, NOT fixed.** Localised to `replace_full_document` under load (60 items: 10–14 seams/save-with-document; 1 item: clean over 16 trials). Client sends **3 saves per edit**. | commit `3b727a3`; `engine.cpp:1532-1574` |
| 7 | **Clock-drift compensation: DSP half only.** `DriftResampler`/`DriftController` exist with unit tests but have zero references from engine code. Secondary devices drop/repeat frames; ~12 min to audible at 37 ms ring. | `drift_resampler.hpp` (unreferenced); `engine.cpp:1613-1616`, `:1213-1221` |
| 8 | **Northbound (external-control) gaps**: `/pan` + `/dsp` are live-only (desync other clients if used externally); no bus state in `/api/state/summary`; no `GET /api/buses/<id>`; no WS bus commands; `server/README.md` protocol docs predate everything on this branch; the promised docs-site "External control / API" page doesn't exist. | `control_server.cpp:1671-1673`, `:1685-1688`; `IMPROVEMENTS_PLAN.md` §4 (:249-256) |
| 9 | **Client divergences**: channel-details view uses stale name-list inference instead of `bus.bound` and lacks the option-fallback the strip has; output list never refreshes. | `MixerChannelDetails.vue:261-262`, `:112` vs `MixerStrip.vue:424-431`, `:437`; `MixerPanel.vue:240-246` |
| 10 | **No bus colour editing UI** (chip renders but nothing edits it); rename exists on the strip only. No playlist bus badge; groups can't see their inherited bus. | `MixerStrip.vue:190`; `PropertiesPanel.vue:347-351` |
| 11 | **i18n**: `mixer.*` (94 keys) + `properties.bus*` are English-only across 21 locales; the *retired* `deviceOverride` strings remain translated everywhere; 9 orphaned `mixer.*` keys. | `client/locales/*.json` |
| 12 | **Splitter drag bug**: dragging the handles separating playlist / cart player / mixer does not track the mouse position (reported by maintainer). | `MainWorkspace.vue` (owns pane widths, mixer clamp :127-140) |
| 13 | Items carrying a `busId` are wired to Main until first played (load path never calls `reroute_items_to_buses`). Audible only via PFL/meters pre-play, but wrong. | `project_state.cpp:1277`, `:3537` (never called on load) |

---

## 2. Invariants — binding on EVERY task

1. **Server owns all control and state.** Multiple clients (main window, detached mixer,
   cart window, Companion, curl) may be connected at once and must converge. No client-local
   authoritative state, ever. Every mutation that another client could see must go through the
   server and be broadcast over the WebSocket (`doc_patch` ops or dedicated frames). The only
   client-local state allowed is pure view state (which pane is open, a dismissed banner).
2. **Render-thread rules** (violating any of these is an automatic review failure):
   - No allocation on the render thread (commit `8b37fa3` removed the last ones; keep it so).
   - No unbounded locks; the engine mutex is taken via the `lock_timed()` pattern.
   - The render thread walks flat pre-computed lists; it never traverses a graph, never
     re-walks routes on a parameter change (`StripCoeffs` double-buffer is the parameter path).
   - Anything added to `StripCoeffs` that is not ramped must still be **assigned** in
     `ChannelDsp::advance_coeffs()` (§0.2b gate bug).
   - A cycle reaching the render thread = infinite loop in the audio callback. Cycles must be
     impossible by construction (rejected at API **and** dropped defensively at build).
3. **A project never names hardware.** Buses target logical output names; `outputs.json`
   (server-owned) binds names to devices. `settings.previewDevice` / `ltcDevice` are the two
   deliberate legacy exceptions — do not migrate them in this release.
4. **Never block a show from loading.** Missing hardware, unmapped outputs, legacy schemas —
   all load silently-valid with a UI flag, never an error.
5. **Match existing style**: server logs lowercase-sentence style via `Logger`; commit messages
   follow the repo convention (`feat:`/`fix:`/`perf:`/`diag:`/`docs:` + lowercase summary);
   Vue components use the existing token-driven CSS vars (`--color-*`); i18n via `t()` with
   keys added to `en.json` first.
6. **Verification is part of every task** (§7 has the commands). Server tasks: build + all
   unit suites green + affected e2e scripts green. Client tasks: `npm run build:nuxt` passes.
   New behaviour claimed = new assertion somewhere proving it (the project's standing bar:
   safety assertions are confirmed to fail against a deliberately broken build before being
   trusted — see `BUS_ARCHITECTURE.md` §0.3).
7. **Do not remove the diagnostics** (seam detector, lock-wait timer, slow-block warning,
   stats counters). They are cheap and are the project's live tripwires.
8. **Keyboard accessibility of Knob/CanvasFader stays deferred** (maintainer decision,
   2026-08-21). Do not add tabindex/keydown handling to the shared controls in this release.

---

## 3. Decisions record (all binding; cite by number in commits/notes)

**From GitHub issue #58 (maintainer + contributor, agreed):**

- **D1 — Legacy projects route everything to Main (master), with a warning.** No migration
  wizard. Main is "just a bus" with a hidden 1:1 route to the engine's output.
- **D2 — The bus graph lives in the project; hardware binding lives in the server-owned
  OutputMap.** A bus *optionally* names a logical output; the project only flags that an
  output is expected. Bus→hardware is 1:1, owned by the engine config. (Already built.)
- **D3 — Missing hardware: default is "do nothing"** (affected outputs silently dropped, bus
  stays valid), **plus a "Remap Hardware Outputs" button** that appears when outputs are
  missing and opens a modal listing buses/logical outputs with hardware dropdowns. The
  unbound state renders as a red "Output missing" the user can click.
- **D4 — New/legacy items default to Main via inheritance** (no `busId` = inherit: nearest
  ancestor group, else Main). NOT "first bus with a valid output" — rejected as unstable.
  Groups carry `busId`; children inherit. (Already built.)
- **D5 — Stage 4 scope is output routing only**: a bus's single output may target another
  bus (PreShow→Master, Beds→Master, SFX→hardware from the issue). **Aux sends (parallel
  send-at-a-level) are deferred**; the Sends-panel copy must say so plainly.

**Taken by the maintainer in this planning session (2026-08-21):**

- **D6 — Bus→bus target rules.** Valid targets for `output.type:"bus"`: **non-system buses
  only** (not self, not Main, not Monitor). "To Master" remains the existing
  `output.type:"master"` option — one way to reach the master, not two. Monitor may never be
  a target and may never target a bus or master (existing 409 stays). Cycles: **409 at the
  API** with error text `"routing this bus would create a cycle"` **and** defensively dropped
  at topology build with a `Logger::warn` naming both buses.
- **D7 — Processing order** is a topological sort (Kahn's algorithm) over strip→strip edges,
  computed on the **control thread** in `rebuild_topology_locked()`, stored in the `Topology`
  snapshot as a flat ordered index list. The render thread iterates that list only.
- **D8 — Bus→bus lane mapping reuses the mixer→master laws**: width-aware pan/balance send
  gains (`apply_bus_pan` semantics), `kDefaultDownmixDb` (−3 dB, `audio/types.hpp`) for 2→1
  folds. No new law.
- **D9 — Deleting a bus**: assigned items fall back to Main (existing behaviour); **feeder
  buses (whose output targeted the deleted bus) are retargeted to `master`**, persisted, and
  announced via `buses_patched`. Never silent-orphaned.
- **D10 — `bound` for a Bus-kind bus** = walk the output chain to its terminal; bound iff the
  terminal reaches hardware (master counts as bound). Server computes; client renders only.
- **D11 — Document versioning for buses.** `write_buses_to_document_locked()` also writes a
  top-level `"busSchema": 1`. `replace_full_document()` may carry the previous project's
  buses into an incoming document **only** when the incoming document has `busSchema >= 1`
  and simply omits the `buses` key (i.e. it is a client round-trip of a bus-era project). A
  document **without** `busSchema` takes the full migration path — synthesise + migrate, no
  carry. This fixes finding §1.2-5 deterministically.
- **D12 — Migration surfacing.** When migration changes anything on load/replace, the server
  (a) logs at `warn` level, (b) broadcasts `doc_patch` op `project_migrated` with
  `{itemsToMain, busesFromDeviceOverride, mainOutputMigrated}` counts, and (c) includes the
  same object in the HTTP response. The client shows a dismissible, non-blocking banner:
  "This project was created before mixer buses. All cues now route to the Master bus." with
  an "Open Mixer" action. Dismissal is per-client view state (allowed under invariant 1).
- **D13 — Save debouncing.** One document PUT per edit burst: trailing debounce **300 ms**,
  with an immediate flush on project close, app quit, and explicit "save as". Targeted
  endpoints (item add/update, cart ops) are unchanged. Three-saves-per-edit must become one.
- **D14 — The save-pop must be root-caused and fixed**, proven by the existing seam counter:
  0 discontinuities across the ui-churn probe's save-with-document scenario at 60 items.
  Symptom-hiding (e.g. output muting during saves) is not acceptable.
- **D15 — Clock device = the house.** The device carrying master channels 0/1's assignment is
  the clock; fallback: first-opened device. Production gates on the clock device; every other
  device runs through a `DriftController`-driven `DriftResampler` (target fill 0.5). No new
  CLI flags. Per-device `{name, ppm, fillPercent}` appears in `/api/engine/stats`. Scratch
  buffers are pre-allocated at device open (invariant 2).
- **D16 — Northbound semantics.** Every new feature is REST-controllable with
  persist-and-broadcast semantics. `POST .../pan` and `POST .../dsp` stay live-only drag
  endpoints and are *documented* as client-internal (external controllers must use `PATCH`).
  Add: `GET /api/buses/<id>`; a `buses` block in `GET /api/state/summary`; WS client→server
  commands `bus_gain {busId, gainDb}`, `bus_mute {busId, mute?}` (omit = toggle),
  `bus_pfl {busId, pfl?}` (omit = toggle) with identical semantics to the REST equivalents.
- **D17 — Multi-client is a feature requirement, not a nice-to-have** (maintainer, this
  session): all control/state server-side so several clients can drive one session. Every
  task's acceptance implicitly includes "a second connected client converges".
- **D18 — Bus rename + colour.** Renaming: inline scribble-strip rename exists on the strip
  (`MixerStrip.vue:197`); add the same inline rename to the channel-details header. Colour:
  clicking the colour chip in the channel-details header opens a swatch popover using the
  existing `PRESET_COLORS` pattern (`PropertiesPanel.vue:36-44`, `types/project.ts`);
  `PATCH /api/buses/<id> {color}` persists; `buses_patched` broadcasts. Bus colour then
  propagates: strip chip (exists), channel-select row tiles, playlist badge (D19).
- **D19 — Playlist bus badge.** Playlist rows (items and groups) with an **explicit** `busId`
  show a small dot in the bus's colour with the bus name in the tooltip; inherited routing is
  NOT badged in the list (it is shown in PropertiesPanel's "Inheriting: …" readout, which
  must be fixed to work for groups via a client-side ancestor walk of the local tree).
- **D20 — i18n**: machine-translate all new/existing `mixer.*`, `properties.bus*`, and every
  key added by this plan into the 20 non-English locales; delete `properties.deviceOverride`
  / `deviceOverrideHelp` from all 21 locales; delete the orphaned `mixer.*` keys
  (`tabOverview`, `tabInserts`, `eqPlaceholder`, `dynPlaceholder`, `processing`, `channel`,
  `systemBus`) everywhere. The docs-site API page stays English-only (locale fallback covers
  the rest).
- **D21 — Explicitly deferred (do NOT build, do reference as deferred where UI copy exists):**
  aux sends; the plugins rack (stays a shell); keyboard accessibility of Knob/CanvasFader;
  master-strip channel view (master DSP); multi-channel (>2) buses; frequency-dependent
  width; a UI for the engine's global master gain; `previewDevice`/`ltcDevice` migration;
  the Bitfocus Companion module repo; mDNS discovery.
- **D22 — Branch discipline**: commits to `2.5.0-dev` only, one commit per task, repo message
  convention, **no pushes, never main**.
- **D23 — Bus meters**: per-lane bus meters already ride the `meters` WS broadcast; no new
  meter surface is added by this plan.

---

## 4. Workstreams and task specifications

Every task below states: files, exact required behaviour, and acceptance. Line numbers are
from the audit at `7c0607d` — re-locate by searching for the named symbols if drifted.

---

### Workstream A — Bus→bus routing (Stage 4)

The only remaining stage of `BUS_ARCHITECTURE.md` (§2.3 is the design; read it first). The
current render pass is strictly two-hop: items → bus accumulators → (DSP → gain) → master
accumulators. Bus→bus makes the middle a DAG.

#### A1 — Engine: mixer→mixer route type, topological order, render-loop restructure

*Files:* `server/include/liveplay/audio/engine.hpp`, `server/src/audio/engine.cpp`,
`server/include/liveplay/audio/monitor_tap.hpp` (read its :35-37 comment — it predicted this
task).

1. **Engine API**: add `route_mixer_to_mixer(src_id, dst_id, lane_gains)` and
   `unroute_mixer_to_mixer(src_id)` (one output per source strip — D5 means a strip has at
   most one downstream strip). Extend `PendingRoute` so routes requested before `start()`
   replay, exactly as the existing route types do (`engine.hpp:358-383` region).
2. **Topology**: add to the snapshot (a) per-strip optional destination
   `{dst_strip_index, lane_gains}` and (b) `std::vector<size_t> strip_order` — indices into
   the snapshot's strip list, topologically sorted (Kahn) so every strip appears before the
   strip it feeds. Built in `rebuild_topology_locked()` (`engine.cpp:157-234`) on the control
   thread. **If a cycle survives to build time, drop the offending edge(s), log
   `Logger::warn` naming both strips, and continue** — the render thread must never see a
   cycle (invariant 2). Monitor is excluded from `strip_order` (it is processed after taps,
   as today).
3. **Render loop restructure** (`render_one_block`, `engine.cpp:1304-1625`). Current order:
   item pass → DSP pass (all strips) → `mix_monitor_taps` → monitor chain → gain/mute/fade
   pass (all strips) → mixer→master pass. New order:
   - Item pass unchanged (items feed bus accumulators).
   - **Per strip, in `strip_order`**: run its DSP chain → capture its monitor tap (if
     PFL'd) from the post-DSP, pre-gain accumulator (this preserves today's tap semantics —
     the tap point is post-chain, pre-fader, pre-mute) → apply gain/mute/fade in place →
     meter + correlation → if it has a bus destination, **add** its lanes × `lane_gains`
     into the destination strip's accumulator. Topological order guarantees the destination
     has not run yet.
   - Then: fold captured taps into Monitor, run Monitor's chain, monitor gain/meter.
   - Then the mixer→master pass, unchanged (reads finalised accumulators).
   - `mix_monitor_taps` in `monitor_tap.hpp` takes all strips at once today; refactor to a
     per-strip append (keep it header-only and unit-testable without a device). Its existing
     width/pan placement rules must be preserved bit-for-bit — the unit tests in
     `mixer_channel_test.cpp` pin them.
4. **Lane mapping** (D8): destination send gains are computed exactly as mixer→master sends
   are today — source width + `pan` field decide (`apply_bus_pan` laws, `pan_gains_db` /
   `balance_gains_db` in `types.hpp`), 2→1 uses `kDefaultDownmixDb`. Do not invent a matrix.
5. **No allocation on the render thread**: everything new is sized at topology build /
   `start()` time. The existing cached strip-list mechanism (`engine.cpp:1319-1338`,
   generation counter) must incorporate the order list.

*Acceptance:* server builds; **all seven unit suites green**; a new unit test file
`server/tests/topo_order_test.cpp` (+ CMake target `liveplay-topo-tests`) covering: order is
topological for chains and diamonds; cycle edges dropped deterministically with the rest of
the graph still ordered; Monitor excluded. E2e verification is A3's job, but
`node server/tests/e2e/pfl-e2e.js` and `width-e2e.js` must still pass unchanged (they pin the
tap semantics you are moving).

#### A2 — Server: ProjectState wiring, validation, `bound`, delete-retarget

*Files:* `server/src/core/project_state.cpp`, `server/include/liveplay/core/project_state.hpp`,
`server/src/net/control_server.cpp`.

1. **`wire_bus` Bus branch** (`project_state.cpp:4238-4242` currently warns-and-returns):
   resolve the target bus id → its strip; call `route_mixer_to_mixer`; record the resolved
   target in `BusRouting` (so `unwire_bus` :3963-3990 gets a Bus branch that unroutes the
   mixer→mixer edge).
2. **Validation at the API** (in `patch_bus` / `create_bus`, surfacing as HTTP 409 through
   the existing error path in `control_server.cpp:1653+`): target must exist; target must be
   a non-system bus (D6 — not self, not `main`, not `monitor`); the new edge must not create
   a cycle (walk `output` edges from the target; if the walk reaches the source, 409 with
   `"routing this bus would create a cycle"`). Monitor-as-source keeps its existing rules.
3. **`bound` reporting** (D10): for Bus-kind, walk the chain to the terminal;
   `bound = terminal is Master-kind, or an Output-kind with non-empty wired_channels`.
   (`project_state.cpp:4765-4766` is where `bound` is computed today.)
4. **Delete retarget** (D9): in `delete_bus` (`:4667` region), any bus whose output targets
   the deleted bus is set to `{type:"master"}`, persisted via
   `write_buses_to_document_locked`, wired, and included in the `buses_patched` broadcast.
5. **Materialise + rewire**: `materialise_buses()` wires Bus-kind edges after all strips
   exist (two passes: create strips, then wire edges). `rewire_buses_for_output_map`
   (`:3992`) stays Output-only, but Bus-kind `bound` must recompute for reporting after a
   map change (it is derived, so no extra work if computed on read — verify it is).
6. Remove the now-false warning string path and the client copy that says bus→bus is
   unimplemented (coordinate with A4 for the client side).

*Acceptance:* build green; unit suites green; manual REST smoke (§7): create buses A and B,
`PATCH` A `{output:{type:"bus",target:"<B id>"}}` → 200 and `GET /api/buses` shows A bound
iff B is bound; `PATCH` B to target A → **409**; `DELETE` B → A reports `{type:"master"}`.

#### A3 — Tests: bus→bus unit + e2e

*Files:* new `server/tests/e2e/busbus-e2e.js`; extend `server/tests/mixer_channel_test.cpp`
only if a pinned law moved (it should not).

The e2e follows the house style (see `pfl-e2e.js` — REST + WS against a live server, real
audio, assertions that were confirmed to fail against a broken build). Must assert:
1. Audio routed item→A, A→B, B→master is audible on the master meters at the expected level
   (chain of unity gains ⇒ same level as direct-to-master, within meter tolerance).
2. A's fader attenuates the chain; B's fader attenuates the chain; muting B silences it.
3. A mono bus feeding a stereo bus lands per the pan law; a stereo bus feeding the chain
   folds per `kDefaultDownmixDb` when the terminal is mono.
4. Cycle `PATCH` returns 409 and the audio is undisturbed (seam counter unchanged).
5. Deleting the middle bus retargets the feeder to master and audio continues (no silence).
6. PFL on A still taps pre-fader/pre-mute (fader down, still audible in Monitor; house
   unchanged) — the Stage 3 guarantees hold across the restructure.

*Acceptance:* script passes against the A1+A2 build; at least assertions 1 and 6 are
demonstrated to FAIL against a build with the relevant piece stubbed (record how in Notes).

#### A4 — Client: bus targets in the output selectors

*Files:* `client/app/components/MixerStrip.vue`, `MixerChannelDetails.vue`,
`client/locales/en.json`, `client/app/types/project.ts` (types only if needed).

1. Both output `<select>`s (strip :36-59, details :108-114) gain a "Buses" option group:
   value scheme `bus:<id>` alongside the existing `master` and `out:<name>`. Candidates:
   non-system buses, excluding self and excluding any bus whose output chain reaches this
   bus (client-side walk of the loaded bus list — cheap, and the server 409 remains the
   authority).
2. On a server 409, revert the select to the previous value and surface the server's error
   text inline using the existing `det__warn` style (details view) / tooltip + brief warn
   border (strip). No blocking dialogs.
3. Remove `mixer.busToBusUnsupported` usage; update the Sends panel line
   (`mixer.auxSendsPending`) to say aux sends are a future feature (D5), not "arrive with
   bus-to-bus routing".
4. A Bus-kind bus's warn state now follows `bus.bound` (unbound chain terminal ⇒ warn).
5. Strip output summary text for a bus target shows the target bus's name (e.g. "→ Beds").

*Acceptance:* `npm run build:nuxt` green; manual flow against a live server: assign, see
`buses_patched` update in a second window (D17), attempt a cycle and see the revert +
message.

---

### Workstream B — Remap Hardware Outputs (output-map UI)

#### B1 — Server: `outputs_changed` broadcast

*Files:* `server/src/net/control_server.cpp`.

After a successful `PUT /api/outputs` (`:1802-1817`), broadcast `doc_patch` op
`outputs_changed` carrying the new map (same JSON as `GET /api/outputs`) and the
`rewiredBuses` count. Add the op to the WS docs comment block if one exists in the handler
region.

*Acceptance:* build green; two connected WS clients both receive the op on a `PUT` (curl +
`websocat`/node one-liner is fine; record method in Notes).

#### B2 — Client: `OutputMapModal.vue`

*Files:* new `client/app/components/OutputMapModal.vue`; `MixerActions.vue`,
`MixerStrip.vue`, `MixerChannelDetails.vue`, `MixerPanel.vue`,
`useLiveplayServer.ts` (wire the existing dead `saveOutputs` :997), `en.json`.

The modal is the D3 "Remap Hardware Outputs" surface **and** the general output-map editor:

1. **Contents**: one row per logical output: name (inline-editable), its channel list — each
   channel a device `<select>` (from `GET /api/devices`, refetched on modal open) + hardware
   channel number input — add/remove channel, add/delete logical output. Below: an
   "Unmapped bus targets" section listing every bus whose `output.type === 'output'` target
   has no map entry and is not `bound`, each with a one-click "map now" that seeds a row with
   that name. Deleting a logical output that buses reference shows the affected bus names
   before confirming (no orphan surprises).
2. **Save**: single `PUT /api/outputs` of the whole map (the endpoint is whole-map replace);
   on success close; on 400 show the error inline. No partial saves.
3. **Entry points**: (a) a button in `MixerActions.vue` (always available, icon + tooltip
   "Remap hardware outputs"), (b) clicking the red/warn output state on a strip or in the
   channel details opens the modal (the click affordance replaces today's dead-end tooltip).
4. **Live refresh**: `MixerPanel` refetches outputs on `outputs_changed` and on WS
   reconnect (fixes the never-refreshed `outputNames`, finding §1.2-9). The modal itself also
   listens while open (a second client may save).
5. Works in the detached mixer window (it has its own socket; verify).
6. Strings: `mixer.outputMap*` namespace in `en.json`; follow existing modal markup/style
   conventions (`ProjectSettingsModal.vue` is the reference).

*Acceptance:* `npm run build:nuxt` green; manual: create map entry for an unbound bus →
strip warn state clears without reload (the server rewires + `buses_patched`/
`outputs_changed` land); a second window converges (D17).

#### B3 — Client: fix the details-view divergences

*Files:* `client/app/components/MixerChannelDetails.vue`.

1. `:261-262` — replace the name-list inference with `props.bus.bound === false` (matching
   `MixerStrip.vue:437-438`; the type documents this at `types/project.ts:322-327`).
2. `:112` — add the current-target fallback option exactly as `MixerStrip.vue:424-431` does,
   so a bus pointing at an unmapped name never renders a blank select.

*Acceptance:* `npm run build:nuxt` green; with a bus targeting an unmapped name, strip and
details agree and neither select is blank.

---

### Workstream C — Legacy migration correctness + surfacing

#### C1 — Server: fix the legacy load paths

*Files:* `server/src/core/project_state.cpp`.

1. **Snake_case / 1.x branch** (`load_from_json` else-branch, `:5199-5296`): after the
   document is populated, run the same sequence the client branch runs — `load_buses_locked()`,
   `write_buses_to_document_locked()`, and `materialise_buses()` (mirror the call sites at
   `:5159-5160`, `:5183`). A 1.x project must come up with Main + Monitor and items resolving
   to Main (D1). Note this branch currently resets `document_` to `default_empty_document()`
   at `:5215` before populating — make sure buses are loaded/written **after** population.
2. **`is_client_document` heuristic** (`:1073-1092`): an empty client project
   (`items: []`, no cart keys) currently falls into the legacy branch and gets its document
   replaced by the default. Fix: any document containing an `items` **array** is a client
   document. Add whatever additional key checks the existing heuristic needs to stay correct
   for genuine 1.x docs (`carts`/`playlist`/`cues_legacy` per `is_legacy_document` :5060).
3. **Route on load**: after `materialise_buses()` on every load path, re-route items that
   carry a `busId` (call `reroute_items_to_buses` (`:3537`) or make the mirror pass
   bus-aware) so pre-play wiring matches `resolve_item_bus` (finding §1.2-13).
4. Remove the two `defaultOutputDevice: null` re-injections (`:5162-5168`, `:1864-1870`) —
   they resurrect the field the migration exists to remove.

*Acceptance:* build + unit suites green; manual: load a genuine 1.x document (construct one
from `upgrade_legacy_document`'s expectations) → `GET /api/buses` returns Main + Monitor,
`system:true`, items resolve to Main; load an empty client project → document NOT replaced;
`save-churn.js` still green.

#### C2 — Server: `busSchema`, carry rule, migration broadcast

*Files:* `server/src/core/project_state.cpp`, `server/src/net/control_server.cpp`.

1. **`busSchema` (D11)**: `write_buses_to_document_locked()` (`:3918`) also sets
   `document_["busSchema"] = 1`. In `replace_full_document()` (`:1845-1923`), the
   carry-previous-buses branch (`:1858-1863`) becomes conditional on the incoming document
   having `busSchema >= 1`; otherwise the incoming document goes through full migration
   (`load_buses_locked` handles it) with **no carry**. This kills the cross-project
   carry-over and un-discards `defaultOutputDevice` for legacy PUTs (§1.2-5).
2. **Migration summary (D12)**: `load_buses_locked` + `migrate_device_overrides_locked`
   count what they changed: `itemsToMain` (items with no `busId` when the doc had no
   `buses` key — i.e. the whole project for a legacy doc), `busesFromDeviceOverride`,
   `mainOutputMigrated` (bool). When any is non-zero on a load/replace: log at **warn**
   (upgrading the current info logs at `:3845`, `:3913`), broadcast `doc_patch` op
   `project_migrated` with the counts, and include a `migration` object in the HTTP
   response of the load/replace endpoint that triggered it.
3. Idempotence: a second load of the already-migrated document reports zero counts and
   broadcasts nothing.

*Acceptance:* build + suites green; manual: PUT a legacy doc (with `deviceOverride`s) over a
loaded project → response carries `migration`, WS carries `project_migrated`, buses are the
legacy doc's own (not the previous project's); PUT a bus-era doc without `buses` → buses
carried, no migration broadcast.

#### C3 — Client: migration banner + mixer empty state

*Files:* `MainWorkspace.vue`, `MixerPanel.vue`, `useLiveplayServer.ts` (handle
`project_migrated` in the `doc_patch` switch, `:336+`), `en.json`.

1. On `project_migrated`: show a dismissible banner at the top of the workspace (existing
   warn styling/tokens): headline "This project was created before mixer buses.", body
   "All cues now route to the Master bus." (+ "N cues were moved onto buses created from
   their device overrides." when `busesFromDeviceOverride > 0`), actions: "Open Mixer"
   (sets `mixerOpen`) and dismiss. Dismissal is local view state; the banner reappears on a
   fresh migration event only.
2. Mixer rail empty state: when `userBuses` is empty (`MixerPanel.vue:211`), render copy in
   the rail: "All cues route to Master. Add a bus to split your mix." (key
   `mixer.emptyRail`) above the existing add-bus control — this fixes the "looks unrouted"
   half of issue #58's report.

*Acceptance:* `npm run build:nuxt` green; manual with C2 server: open a legacy project →
banner appears in every connected window, dismisses independently; empty rail shows copy.

#### C4 — Migration e2e

*Files:* new `server/tests/e2e/migration-e2e.js`.

Drive a live server: (1) PUT a synthetic 1.x doc → Main+Monitor exist, items resolve to
Main, `project_migrated` observed on WS; (2) PUT a 2.4-style client doc with two distinct
`deviceOverride`s → two Output-kind buses, items assigned, overrides erased from the
document (`GET /api/project/document`), audio from an override item plays out the mapped
bus; (3) PUT the same (now-migrated) doc again → zero-count, no broadcast; (4) PUT a
bus-era doc omitting `buses` → prior buses intact. House e2e style; assertions verified to
fail against a build with C2 reverted.

*Acceptance:* script green; noted which assertions were fail-tested.

---

### Workstream D — Save pipeline: debounce + the residual pop

#### D1 — Client: one save per edit

*Files:* `client/app/composables/useProject.ts` (the save/sync machinery; per-section
debounced timers exist around `:1361`), possibly `PropertiesPanel.vue` callers.

The maintainer's server log showed **three** document PUTs within one second for a single
edit (commit `16816de` message). Find the three triggers (the per-section debounced sync
timers + direct `saveProject()` calls at e.g. `:465`, `:534`, `:1194` are the suspects) and
coalesce: one trailing-debounced (300 ms, D13) document save per edit burst, with immediate
flush on project close / quit / save-as (`:1931` force path exists). Constraints:
- Targeted endpoints (item add/update/delete, cart ops) keep their current timing — they are
  what keeps other clients live; only the whole-document PUT is debounced.
- Detached windows already never author document saves (`BUS_ARCHITECTURE.md` §0.2b) — keep.
- No edit may be lost if the app quits within the debounce window (flush on quit path).

*Acceptance:* `npm run build:nuxt` green; manual: single property edit while watching the
server log ⇒ exactly one `PUT /api/project/document`; quit immediately after an edit ⇒ the
edit is on disk. Note the before/after PUT counts in Notes.

#### D2 — Server: root-cause and fix the residual save-time pop

*Files:* `server/src/core/project_state.cpp` (`replace_full_document` :1845 and everything
it calls), `server/src/audio/engine.cpp` only if the fix requires it.

**Context (read first):** commit messages `3b727a3`, `13bbdec`, `16816de`, `df29957`,
`8b37fa3` (`git log`), and `server/tests/e2e/README.md`. Everything already ruled out BY
MEASUREMENT: underruns, render-thread cost, lock contention (2–5 µs), topology rebuilds (0),
cart re-priming, Phase-3 property re-application. The fault signature: a value changes under
playing audio inside `replace_full_document`, only when the PUT carries a document, only
under load (60 items: 10–14 seams, worst 0.483 vs signal max-step 0.10; 1 item: clean × 16
trials). The seam detector (`engine.cpp:1532-1574`) + `GET /api/engine/stats`
(`discontinuities`, `worstSeam`) attribute it per action.

**Method (bounded):**
1. Reproduce with `node server/tests/e2e/ui-churn-probe.js` against a 60-item project (the
   probe's README documents setup; `gen-signal.js` makes material).
2. Bisect *inside* `replace_full_document` by temporarily gating its phases (mirror
   retirement, per-item re-mirror, gain/fade re-application, settings apply, bus reload,
   reroute) and reading the seam counter per gate. The commit `3b727a3` predicts "the next
   round is arithmetic rather than archaeology" — do the arithmetic.
3. Candidate mechanisms to check explicitly: a per-item `set_gain`/fade re-application that
   snaps instead of ramps; item mirror replacement swapping a decoder/reader under a playing
   cue; route replacement (unroute+route) briefly zeroing a send; `load_buses_locked` →
   `materialise_buses` touching live strips on an unchanged bus list.
4. Fix the root cause so the mutation either no-ops when the value is unchanged, ramps, or
   happens on a snapshot swap — whatever the mechanism demands. Remove all temporary gates.
5. **Stop condition:** if the fix demands a restructure larger than ~2 days of work (e.g.
   full copy-on-write document mirroring), STOP, write the full findings + proposed design
   into Notes and `BUS_ARCHITECTURE.md` §0.6-adjacent, mark `BLOCKED` for maintainer review.

*Acceptance (D14):* ui-churn probe, 60 items, save-WITH-document windows: **0
discontinuities**, repeated ×3 runs; `seam-bisect.js` still clean; all unit suites + the
e2e set (`pfl`, `width`, `comp`, `gate`, `filters`, `reroute`, `save-churn`,
`materialise-skip`) green. The seam detector stays in the build (invariant 7).

---

### Workstream E — Clock-drift compensation (wire the existing DSP)

#### E1 — Engine: clock device, controller feedback, per-device resampling, stats

*Files:* `server/src/audio/engine.cpp`, `server/include/liveplay/audio/engine.hpp`,
`server/include/liveplay/audio/drift_resampler.hpp` (exists, tested, unreferenced),
`server/src/net/control_server.cpp` (stats JSON).

Commit `13bb625` states the remaining work precisely: "choosing the clock device, gating
production on it, and running each other device through a resampler."

1. **Clock device (D15)**: the device carrying master channels 0/1's assignment
   (`MasterRouteEntry::destination` for masters 0/1); fallback `devices_.front()`.
   Production gating (`engine.cpp:1213-1221`) moves from `devices_.front()` to the clock
   device. Recompute the clock when device assignments change (control thread).
2. **Per non-clock device**: a `DriftController` (target fill `kDriftTargetFill = 0.5`) fed
   once per block with that device's ring `available_read` (the pattern at `:1281` shows the
   read); its ratio drives a per-device `DriftResampler` applied to the interleaved block
   before `ma_pcm_rb_acquire_write` in the dispatch loop (`:1586-1624`). The clock device
   keeps the plain `memcpy` path.
3. **Allocation**: resampler state + a scratch sized `DriftResampler::max_output(render_block)`
   are allocated when the device opens / at `start()` — never on the render thread
   (invariant 2). Device close tears them down on the control thread.
4. **Stats**: `EngineStats` + `GET /api/engine/stats` gain a `devices` array:
   `{name, isClock, ppm, fillPercent}` (ppm from `DriftController::ppm()`). The existing
   drop-on-full comment (`:1613-1616`) should now be a genuinely cold path — keep the break
   as a last resort.
5. `server/tests/e2e/latency-probe.js` prints the new fields (it reads `/api/engine/stats`).

*Acceptance:* build + all unit suites green (`liveplay-drift-tests` already pins the
components); single-device behaviour bit-identical (clock device path unchanged — assert by
running `seam-bisect.js` and `pfl-e2e.js`); with two devices open, `/api/engine/stats` shows
a non-clock device converging to fill ≈ 0.5 with |ppm| sane (< 500), and no underrun/drop
log lines over a 10-minute soak (script or manual; record the soak result in Notes).
**Sequencing: starts only after A1 is merged** (both restructure `render_one_block`).

---

### Workstream F — Northbound control + protocol docs

#### F1 — Server: external-control API completion

*Files:* `server/src/net/control_server.cpp`, `server/include/liveplay/net/control_server.hpp`
(its header comment is the endpoint index — update it), `server/src/core/project_state.cpp`
(`state_summary` :3085).

1. `GET /api/buses/<id>` — single-resource read, same shape as one element of
   `GET /api/buses`.
2. `GET /api/state/summary` gains a `buses` array (D16 fields: `id, name, color, order,
   width, gainDb, mute, pfl, bound, output{type,target}`, plus `monoCheck` on Monitor).
   Keep it compact — no `dsp` block, no `itemUuids` (Companion reads names/levels/flags).
3. WS client→server commands in `handle_ws_message` (`:860-1075`): `bus_gain {busId,
   gainDb}`, `bus_mute {busId, mute?}` omit = toggle, `bus_pfl {busId, pfl?}` omit =
   toggle — each delegating to the same code paths as the REST equivalents (persist where
   REST persists, broadcast what REST broadcasts). Unknown bus → the existing `error` frame.
4. Document-in-code: the comments at `:1671-1673` / `:1685-1688` already mark `/pan` +
   `/dsp` as live-only; extend those comments to say "client-internal; external controllers
   use PATCH" (D16).

*Acceptance:* build green; curl smoke of both GETs; a WS `bus_mute` toggle observed to
broadcast `buses_patched` to a second client (D17). **Starts after A2** (summary includes
bus-target outputs).

#### F2 — Docs: bring `server/README.md` current

*Files:* `server/README.md` (§"Control surface" ~:262, WS protocol ~:494-581).

Audit found the protocol section documents none of: `/api/buses*`, `/api/outputs`,
`/api/engine/stats`, `/api/transport/go|play_index|pause_toggle|arm_selected|play_selected`,
`/api/selection`, `/api/ui/showmode|locale`, `/api/master/limiter|gain|channels/<n>/gain`,
`/api/monitor/mono`, `/api/state/summary` (as a surface), nor the WS ops `buses_patched`,
`bus_pfl_*`, `monitor_mono_changed`, `limiter_changed`, `master_gain_changed`,
`output_channel_gain_changed`, `selection_changed`, `show_mode_changed`, `locale_changed`,
`outputs_changed`, `project_migrated`, nor the client→server frames beyond the original set.
Rewrite the two sections from the *code* (`control_server.cpp` route registrations + the
`doc_patch` emit sites are ground truth), including the live-only status of `/pan`+`/dsp`
and the richer `meters` payload fields (`control_server.cpp:461-467`). Table style stays as
the file has it.

*Acceptance:* every route registered in `control_server.cpp` appears exactly once; every
`doc_patch` op emitted appears exactly once; no documented route/op that doesn't exist.
Spot-check 10 random entries against code in review.

#### F3 — Docs-site: "External control / API" section

*Files:* `docs-site/app/app.vue`, `docs-site/public/locales/en.json`.

The site is a single-page `app.vue` with locale JSON (no content/ dir, no router). Add an
"External control / API" section following the existing section pattern (anchor-navigable,
keys under a new `api.*` namespace in `en.json` **only** — other locales fall back, D20).
Content: what the surface is (REST + WS on TCP 4480, discovery beacon on UDP 4481), how to
find the server (discovery JSON shape), the headline endpoints for external controllers
(`/api/state/summary`, `/api/transport/*`, `/api/selection`, `/api/buses` + bus commands,
`/api/master/*`), the WS push model (`cue_state`, `meters`, `doc_patch`), and a pointer to
`server/README.md` for the full reference. Marketing-adjacent tone consistent with the
page; this fulfils `IMPROVEMENTS_PLAN.md` §4's promised page.

*Acceptance:* `npm run build --workspace=docs-site` (or the docs-site build script) green;
section renders with working anchors; non-English locales fall back without key leakage
(spot-check one).

---

### Workstream G — UI polish, i18n, docs

#### G1 — Client: bus colour editing + rename in the channel view

*Files:* `MixerChannelDetails.vue`, possibly `MixerStrip.vue` (colour propagation),
`en.json`.

Per D18: the colour chip in the channel-details header (`:21-38`) becomes a button opening
a small popover with the `PRESET_COLORS` swatch grid (pattern: `PropertiesPanel.vue:36-44`;
colours from `types/project.ts`); picking one emits the existing `patch` flow →
`PATCH /api/buses/<id> {color}` → `buses_patched`. The header bus name becomes
inline-renameable exactly like the strip scribble strip (`MixerStrip.vue:197-198` is the
reference implementation). Verify colour propagates live to: strip chip
(`MixerStrip.vue:190`), channel-select row tiles (`MixerChannelDetails.vue:143-191`), and
(after G2) the playlist badge, in all windows.

*Acceptance:* `npm run build:nuxt` green; rename + recolour from the details view update a
second window without reload.

#### G2 — Client: playlist bus badge + group inheritance readout

*Files:* the playlist row component (`PlaylistItem.vue` / `PlaylistView.vue` — locate by
grep), `PropertiesPanel.vue`, `en.json`.

1. **Badge (D19)**: rows (items and groups) whose own `busId` is set render a small dot in
   the assigned bus's colour (buses come from `useLiveplayServer().buses`), tooltip
   "Routed to <name>". No badge for inherited routing. Unknown `busId` (stale) renders the
   dot in the disabled colour with tooltip "Routed to a missing bus".
2. **Group inheritance readout**: `PropertiesPanel.vue:347-354` (`effectiveBusName`)
   early-returns for non-audio items, so groups show nothing. Fix with a client-side
   ancestor walk of the local project tree (mirror `resolve_item_bus` semantics: own
   `busId`, else nearest ancestor group's, else Main) so both cues and groups show
   "Inheriting: <name>" when unset. Show "Inheriting: Master" for the default case.

*Acceptance:* `npm run build:nuxt` green; badge appears/disappears with assignment, colour
tracks G1 recolours; a nested group shows the correct inherited name.

#### G3 — i18n sweep (after ALL string-adding tasks)

*Files:* all 21 `client/locales/*.json` (`ar bn de el en es fa fr hi it ja ko no pt ro ru
sq sv tr ur zh`).

Per D20: (1) delete `properties.deviceOverride` + `properties.deviceOverrideHelp` from all
21 locales; (2) delete orphaned keys `mixer.tabOverview`, `tabInserts`, `eqPlaceholder`,
`dynPlaceholder`, `processing`, `channel`, `systemBus` from `en.json` (verify each is
reference-free by grep first — if referenced, keep and note); (3) translate the full
`mixer.*` namespace, `properties.bus*`, and every key added by A4/B2/C3/G1/G2 into the 20
non-English locales, matching each file's existing tone/formality (compare neighbouring
keys), keeping placeholders (`{name}`, `{count}`) intact, and keeping console jargon that
each language conventionally leaves in English (PFL, EQ, dB, mute where locally
conventional — check how each locale already handles `playback.*`/meter strings for
precedent). JSON must remain valid and key-order consistent with each file's layout.

*Acceptance:* every locale parses (`node -e "require('./client/locales/xx.json')"` loop);
key sets identical across all 21 files for the touched namespaces; `npm run build:nuxt`
green; spot-check 3 languages for placeholder integrity.

#### G4 — Docs: refresh `BUS_ARCHITECTURE.md` (final task)

*Files:* `BUS_ARCHITECTURE.md`, `IMPROVEMENTS_PLAN.md` (only its §6 stale-pointer note if
needed).

Update: the status header (Stages 0–5 complete, merged, Stage 4 done by this plan); §0.1
gains Stage 4 + the drift wiring + the output-map UI + migration hardening; §0.3 rewritten
to the new truth (remaining deferrals = D21 list); §0.2 gains one-line entries for
decisions D6–D16 (cite this file); §0.6 updated for the drift fix; the save-pop story
(§0.2b-adjacent) records the D2 root cause and fix. Keep the document's voice — it explains
*why*, not just *what*.

*Acceptance:* no claim in §0 contradicts the code (spot-check every changed claim against
the merged work); supersession notes kept rather than history rewritten.

#### G5 — Client: splitter drag offset bug

*Files:* `client/app/components/MainWorkspace.vue` (pane sizing: `mixerWidth` clamp
`:127-140`; the playlist/cart splitters live in the same component — locate the
pointer/mouse handlers).

Reported by the maintainer: dragging the handles separating playlist / cart player / mixer
does not track the mouse — the divider moves but not to the pointer position. Reproduce,
then fix so every splitter tracks the pointer 1:1 during the whole drag. The usual causes
in this shape of code: computing width from `movementX` deltas that accumulate error, or
from `clientX` without subtracting the container's `getBoundingClientRect()` offset, or a
clamp applied to the wrong edge. Check all three splitters (playlist/cart boundary,
cart/mixer boundary, docked-mixer width) and both directions, at different window sizes and
with the window not at the screen origin. Do not change the clamping *ranges* — only make
position tracking honest.

*Acceptance:* `npm run build:nuxt` green; manual: for each splitter, the handle stays under
the pointer for the full drag at two window sizes; clamps still hold at the extremes.

---

## 5. Sequencing and conflict map

```
A1 ──► A2 ──► A3
        │ └──► A4
        └────► F1 ──► F2, F3
A1 ──────────► E1          (both restructure render_one_block — strictly serial)
C1 ──► C2 ──► C3, C4
C2 ──────────► D2          (both edit replace_full_document — strictly serial)
B1 ──► B2
B1 ──► B3
D1, G1, G2, G5             (independent, any time)
A4, B2, C3, G1, G2 ──► G3  (i18n sweep last among string tasks)
everything ───────────► G4 (doc refresh truly last)
```

**Same-file serialization (hard rule for the orchestrator):** never run two tasks
concurrently that both touch any of: `engine.cpp`/`engine.hpp` (A1, E1, D2-maybe),
`project_state.cpp` (A2, C1, C2, D2), `control_server.cpp` (A2, B1, C2, F1),
`MixerChannelDetails.vue` (A4, B2, B3, G1), `en.json` (A4, B2, C3, G1, G2 — small merge
risk; serialize or rebase carefully). Safe parallel lanes at the start: **A1 ∥ C1 ∥ B1 ∥ D1
∥ G5**.

---

## 6. Task table

Status values: `TODO` → `IN PROGRESS` → `DONE` (implemented, self-verified) → `VERIFIED`
(orchestrator re-checked build/tests) | `BLOCKED` (question in Notes). Implementing agents
fill Notes with: what was done, deviations, verification evidence, and anything the next
task needs to know.

| ID | Task | Agent | Thinking | Effort | Depends on | Status | Notes |
|----|------|-------|----------|--------|------------|--------|-------|
| A1 | Engine: mixer→mixer routes, topo order, render-loop restructure | Fable | On | High | — | VERIFIED | Adds StripRouteEntry {strip, dst_strip, lane_sends} and strip_order to the Topology snapshot; header-only compute_strip_order (Kahn, smallest-ready-index tie-break, deterministic cycle-edge drop, exclude index for Monitor) so it unit-tests without a device. route_mixer_to_mixer(src, dst, vector<MixerLaneGain{src_lane,dst_lane,gain_db}>) / unroute_mixer_to_mixer(src); gains are dB, converted internally like route_mixer_to_master; one send per source (D5), re-routing replaces in place. PendingRoute::MixerToMixer replays pre-start routes. remove_mixer_channel cleans both directions. Render loop is now one ordered pass: DSP chain, PFL tap (post-chain pre-fader pre-mute, unchanged point), fader/mute/fade/meter/correlation, then multiply-add into the destination accumulator. mix_monitor_taps keeps its exact signature (mixer_channel_test still pins it); new mix_strip_monitor_taps is the per-strip append. Cycle edges surviving to build are dropped with a Logger::warn naming both display names; Monitor-touching edges likewise. No StripCoeffs change, so advance_coeffs needed none. Seam detector, lock-wait timer, slow-block warning and stats counters all untouched. New topo_order_test.cpp + liveplay-topo-tests (ctest name 'topo'). Verified by orchestrator: build clean, ctest 8/8 including topo, and on a fresh server with a real device width-e2e and reroute-e2e ALL PASS and pfl-e2e ALL PASS. NOTE: pfl-e2e fails its first assertion on a SECOND run against the same server (monitor retains ~-27 dBFS); confirmed pre-existing test hygiene, not a regression, by building HEAD in a scratch worktree and reproducing the identical pass/fail alternation there. IMPORTANT FOR A2: the engine silently ignores unknown src/dst and only warn-refuses self-routes and Monitor-touching routes — it never returns an error, so all 409s (cycle, system-bus target, Monitor rules) must be enforced at the API before calling. |
| A2 | ProjectState: bus-output wiring, cycle 409, `bound` chain, delete-retarget | Opus | On | High | A1 | VERIFIED | wire_bus Bus branch resolves target to strip, records BusRouting::wired_bus_target, and lets apply_bus_pan issue route_mixer_to_mixer so a pan drag replaces gains without dropping the edge; unwire_bus gets the matching Bus branch. bus_to_bus_lane_gains implements D8 exactly (balance_gains_db stereo, pan_gains_db mono, 2 to 1 fold at kDefaultDownmixDb). validate_bus_output_locked enforces D6 with a hop-capped forward walk; patch_bus parses and validates the whole output patch BEFORE any mutation so a refused patch is never stored/persisted/wired, and create_bus validates inside the lock so a refusal leaves no bus behind. bound is derived on every read by bus_reaches_hardware_locked, hop-capped. delete_bus retargets every feeder to master inside the same lock as the document write (D9). materialise_buses is now two passes, create all strips then wire all edges, with the fresh table passed in because bus_routings_ still names the outgoing project's removed strips. control_server maps refusals via a new bus_output_refusal_text helper; POST keeps 507 only for a full desk. TWO DEVIATIONS accepted by orchestrator, both in-scope: Monitor to bus now gets its own 409 (D6 says Monitor may never target a bus; before A2 the case was unreachable), and rewire_bus_feeders re-issues ARRIVING sends when a bus's width changes, without which a stereo feeder into a newly-mono bus keeps a lane-for-lane map and loses its right channel, contradicting D8. Verified by orchestrator: engine.cpp confirmed byte-identical (temporary instrumentation fully reverted), build clean, ctest 8/8, the agent's a2-smoke re-run ALL PASS with the exact cycle text 'routing this bus would create a cycle' plus self/main/monitor/unknown/monitor-to-master/monitor-to-bus all 409 with distinct messages and bound tracking a 3-hop chain both ways, a2-audio ALL PASS with real audio (destination reads -6.0 dBFS once routed, master still -6.0 so it arrives once, stereo into mono reads -3.0, un-route empties it), delete retarget seen by a second WS client, and pfl (fresh server) / width / reroute / save-churn / materialise-skip all ALL PASS. Bus-kind output JSON is {type:'bus',target:'<bus id>'}; bus ids are slugified names. |
| A3 | Bus→bus unit tests + `busbus-e2e.js` | Sonnet | On | Medium | A2 | VERIFIED | New busbus-e2e.js, 27 assertions, all six required claims. Fail-tested as the standing bar demands: stubbing the bus-to-bus multiply-add in render_one_block made assertion 1 fail (chain -35.3 vs direct -6.0 dBFS, 10 assertions down, unrelated ones still passing), and gating out the per-strip mix_strip_monitor_taps call made exactly the two PFL/Monitor checks fail at -120.0 while the house-goes-quiet half still passed. engine.cpp restored byte-identically, confirmed by an empty git diff, re-verified by the orchestrator. THREE FINDINGS worth keeping: (1) peak-domain fold prediction is wrong for decorrelated content — decorrelated peaks essentially never coincide, so the first version failed by ~6 dB; RMS adds in power and matched to <0.1 dB, and the reasoning is in the script's comments so nobody reintroduces it; (2) peak-hold ballistics need ~1200 ms to settle after a fader change, not 300 ms, or the drop under-measures (6.8 dB instead of 12); (3) playing three signals at once engages the master limiter and its release corrupts the NEXT section's readings, so the chain item is paused during the fold section with an explicit back-to-baseline checkpoint. No pinned law moved: kDefaultDownmixDb is still -3.0f and mixer_channel_test.cpp needed no edit. Unlike pfl-e2e it establishes its own clean state, so it passes on a repeat run against the same server. Verified by orchestrator: ctest 8/8 and busbus-e2e ALL PASS (0) on a fresh server — unity chain reads -6.0 direct vs -6.0 chained, pan-law centre lands at -3 dB, the 2-to-1 fold matches prediction to 0.0 dB, seam counter 0 to 0 across the refused cycle, and PFL still taps pre-fader and pre-mute. |
| A4 | Client: bus targets in output selectors, 409 handling, copy updates | Sonnet | On | Low | A2 | VERIFIED | Both output selects gain a Buses optgroup with the bus:<id> value scheme; candidates exclude self, system buses, and any bus whose existing chain already reaches this one, via a reachesBus walk guarded by a visited set. onOutputChange now calls server.patchBus directly rather than the fire-and-forget emit so it can await and catch a 409, then restores the previous selection by resetting the select element's .value (Vue's :value binding alone will not repaint, since the bound value never changed) and shows the server's error text inline for 6s — no dialog. Strip warn now follows bus.bound === false for any non-master bus; the hardware-remap click affordance still appears only for an unmapped output-kind target, and a bus-kind unbound chain gets its own non-clickable warn line. Strip output summary shows the target bus's NAME. mixer.busToBusUnsupported usage removed; auxSendsPending reworded to D5's future-feature wording. types/project.ts needed no change — BusOutput.type already covers 'bus'. New en.json keys: mixer.busesGroup, mixer.busRouteUnbound; mixer.auxSendsPending value changed in place. FOR G3: mixer.busToBusUnsupported is now orphaned and should be deleted in the sweep — it is not in the plan's stated orphan list. Verified by orchestrator: build:nuxt green, en.json parses, and the agent confirmed against a live server that A to B is 200 with bound true on both, B to A is 409 with the exact cycle body, target 'main' is 409, and unbinding B flips bound false on both (D10). Revert-on-409 and two-window convergence verified by analysis. |
| B1 | Server: `outputs_changed` broadcast on PUT /api/outputs | Sonnet | Off | — | — | VERIFIED | Broadcasts doc_patch op outputs_changed after a successful PUT /api/outputs, payload {type, op, version, outputs, rewiredBuses} — outputs/version identical to GET /api/outputs, rewiredBuses the same count the PUT response carries. Uses the existing broadcast_doc_patch helper with the same key layout as buses_patched/monitor_mono_changed; no central WS doc block exists in the header, so the explanatory comment is inline at the emit site as the neighbouring ops do. Verified by orchestrator: build green, ctest 8/8, and the agent's two-socket node script showed both clients receiving one identical frame. B2 listens for op === 'outputs_changed'. |
| B2 | Client: `OutputMapModal.vue` (Remap Hardware Outputs) | Sonnet | On | Medium | B1 | VERIFIED | New OutputMapModal.vue (532 lines, ProjectSettingsModal style, deliberately not teleported so scoped styles survive a production build): rows of logical output name + channel list (device select from GET /api/devices, hw channel number), add/remove channel and output, delete-confirm listing the affected bus names, and an Unmapped bus targets section keyed off server-computed bound === false (D10) with a one-click map-now. Save is one whole-map PUT /api/outputs through the previously dead saveOutputs(), no partial saves, 400 shown inline, and nothing applied locally on success — the broadcast is what repaints. Entry points: always-on button in MixerActions, click affordance on the strip's warn state and on the details warn text (replaces the dead-end tooltip). MixerPanel now refetches output names on mount, on outputs_changed and on WS reconnect (fixes finding 1.2-9, the never-refreshed list) and mounts the modal outside the rail/details branch so it works in the detached window. DEVIATION accepted: one extra line in useLiveplayServer.ts doc_patch switch — outputs_changed also triggers fetchBuses(), because the server rewires bound on a remap but B1 only broadcasts outputs_changed, so without it the strip warn state would not clear without a reload. 25 new en.json keys under mixer.outputMap* plus mixer.outputMissing/outputMissingHint. Verified by orchestrator: build:nuxt green, en.json parses, vue-tsc showed no new errors, and the save path and refresh wiring read as specified. Two-window convergence verified by analysis, not by driving two windows. |
| B3 | Client: details-view `bound` + option-fallback fixes | Haiku | Off | — | B1 | VERIFIED | Both divergences fixed and now character-identical to MixerStrip: unmapped is bus.output.type === 'output' && bus.bound === false (was inferring from the outputNames list), and the output select renders outputOptions, a copy of the names with the current target appended when it is not in the map, so it never renders blank. Verified by orchestrator: build:nuxt green and both expressions diffed against MixerStrip.vue outputOptions/outputUnmapped. |
| C1 | Server: legacy/snake_case load-path fixes, heuristic, route-on-load | Opus | On | High | — | VERIFIED | Legacy/1.x branch now runs load_buses_locked + write_buses_to_document_locked under the lock after the document is populated, then materialise_buses with the lock released; adds a D1 warn naming the cue count. is_client_document inverted: exclusions first (schema_version, carts/playlist/cues_legacy), then ANY items array counts, so an empty client project is no longer replaced by default_empty_document. Route-on-load implemented in start_async_mirror (reroute_items_to_buses over just the uuids that pass loaded, after apply_default_device_routing) rather than at the materialise_buses call sites, where item_uuid_to_cue_ is still empty and the call would no-op; covers all three load paths since each ends in the mirror. Both defaultOutputDevice null re-injections removed. Verified by orchestrator on a fresh server with a real device: the agent's c1-verify.js re-run ALL PASS — 1.x doc yields system Main+Monitor with live strips and 2 cues, no settings.defaultOutputDevice, empty client project keeps name/theme/items, and a never-played item carrying busId reads -6.0 dBFS on Alt and -120.0 on Main. save-churn and materialise-skip ALL PASS. Two pre-existing issues flagged by the agent, NOT addressed: a 1.x doc still produces no client items (upgrade_legacy_document leaves document_ at the default; making 1.x synthesise items is an unrecorded policy call), and reset() loads buses without materialising strips. |
| C2 | Server: `busSchema`, carry rule, `project_migrated` broadcast | Opus | On | Medium | C1 | DONE | Implemented; orchestrator holding the commit pending a tightening. Agent found that strict D11 breaks ordinary saving: the client never sends busSchema (buildDocumentSnapshot in useProject.ts is a field whitelist and the Project type has no such field), so under strict D11 every ordinary save is a doc without busSchema and without a buses key, which would wipe the bus list and re-materialise every strip mid-show — the exact regression 16816de prevents, and it fails save-churn and materialise-skip. Agent's interim fix adds a fallback: carry also when the doc does NOT carry pre-bus routing fields (settings.defaultOutputDevice or any item deviceOverride). ORCHESTRATOR JUDGEMENT: that is a heuristic in the very load path whose heuristics caused finding 1.2-5, and D11 plainly presupposes the client round-trips busSchema. Directing the faithful fix instead — client emits busSchema, then the rule becomes strict D11 and the heuristic is deleted. Deferred only until A4 releases types/project.ts and A3 finishes its deliberate engine.cpp stub. Also closed the asymmetry C1 flagged: the snake_case branch now calls release_device_routings_locked(). |
| C3 | Client: migration banner + mixer empty-state copy | Sonnet | Off | — | C2 | TODO | |
| C4 | `migration-e2e.js` | Sonnet | On | Medium | C2 | TODO | |
| D1 | Client: one document save per edit (debounce + flush) | Sonnet | On | Medium | — | VERIFIED | D13 done. Module-scoped trailing 300ms debounce over the whole-document save, shared waiter list so every caller in a burst gets one save and one result; force bypasses it; flushes wired into closeProject() and app.vue runQuitFlow() step 0. Cart-only mirroring and autosave gating stay synchronous on every call (they feed the item diff-watcher, not the disk write). NOTE for D2: the whole-document write is POST /api/project/save carrying {document} (which is what reaches replace_full_document and what save-churn.js exercises), NOT PUT /api/project/document as the spec text says; PUT /api/project/document has only 3 call sites (initial load, reconnect overlay, and a rare 800ms fallback watcher for cartSlotKeys/playbackKeys/name, left alone). No separate Save As flow exists; File>Save uses force and flushes. Verified: build:nuxt green, call sites enumerated. Residual, not a regression: flushPendingSave() returns early if the timer already fired and the save is still in flight. |
| D2 | Server: root-cause + fix the residual save-time pop | Fable | On | Max | C2 | TODO | |
| E1 | Engine: clock-device drift compensation wiring + stats | Opus | On | High | A1 | TODO | |
| F1 | Server: `GET /api/buses/<id>`, summary buses block, WS bus commands | Sonnet | On | Medium | A2 | TODO | |
| F2 | Docs: `server/README.md` protocol refresh | Sonnet | On | Low | F1 | TODO | |
| F3 | Docs-site: "External control / API" section | Sonnet | On | Low | F1 | TODO | |
| G1 | Client: bus colour swatch popover + details rename | Sonnet | Off | — | — | VERIFIED | D18 done. Details-header colour chip is now a button opening a PRESET_COLORS swatch popover; header name inline-renameable (dblclick, Enter/blur commit, Esc cancel), system buses excluded to match MixerStrip. Both go through the existing patch emit to PATCH /api/buses/<id>; no client-local authoritative state. MixerStrip.vue untouched: its chip already renders bus.color from the shared buses ref refreshed on buses_patched, so colour propagates by construction. New en.json key: mixer.busColor; reuses existing mixer.renameHint. Verified by orchestrator: build:nuxt green, PRESET_COLORS export + patch emit signature + useLocalization t() all confirmed in code. D17 convergence verified by analysis, not by driving two windows. |
| G2 | Client: playlist bus badge + group inheritance readout | Sonnet | On | Low | — | VERIFIED | D19 badge on PlaylistItem rows (items and groups alike, busId is on BaseItem): a dot coloured from the shared server buses ref when the row has its own busId, disabled-colour dot with a 'missing bus' tooltip for a stale id, no badge for inherited routing. PropertiesPanel effectiveBusName no longer early-returns for non-audio items — it was reading the server's per-bus itemUuids, which only attributes leaf audio items, so groups showed nothing; replaced with resolveEffectiveBusId, a client-side ancestor walk mirroring resolve_item_bus. New en.json keys: playlist.busRouted {name}, playlist.busRoutedMissing; reuses the existing properties.busEffective. DEVIATION accepted by orchestrator: the plan text says show 'Inheriting: Master' but the system bus is named 'Main' (id 'main') everywhere in the code and D1 calls it Main; the agent resolves the name from the live bus list rather than hardcoding either, so it reads 'Inheriting: Main' and tracks a rename. Verified: build:nuxt green, en.json parses, badge colour confirmed to read from the shared buses ref so G1 recolours repaint it. |
| G3 | i18n: translate new keys ×20 locales, delete dead keys ×21 | Sonnet | Off | — | A4,B2,C3,G1,G2 | TODO | |
| G4 | Docs: `BUS_ARCHITECTURE.md` status refresh | Sonnet | On | Low | all others | TODO | |
| G5 | Client: splitter drag offset fix | Sonnet | On | Low | — | VERIFIED | Two real bugs, both fixed. (1) Offset: startResize measured the cart pane's right edge as the container's right edge, but with the mixer docked the row is playlist/handle/cart/mixer-handle/mixer, so the divider trailed the pointer by the mixer's width. Now measures the live .mixer-resize-handle left edge, falling back to the container edge when undocked. (2) Slope: .cart-section had default flex-shrink 1 while playlist+cart already request 100% before the mixer's handle and panel, so the rendered cart width was cartWidth*(1-k) and the divider tracked at a fractional slope; .cart-section is now flex-shrink 0 and playlist absorbs the overflow. startMixerResize was already 1:1 (mixer-section is flex 0 0 auto). Clamp ranges, snap thresholds and min-width 30% untouched. Verified: build:nuxt green; tracking verified by arithmetic (slope 1, offset 0, docked and undocked), not by dragging. OPEN QUESTION FOR MAINTAINER: with cart flex-shrink 0, the playlist min-width 30% floor and the cart maxWidth 95% clamp are mutually inconsistent in a narrow band near the far-left of the drag when the mixer is docked; making tracking honest there needs a mixer-aware cart clamp, which SS5 forbids changing. Not addressed. |

Model-choice rationale (for the orchestrator when re-planning): **Fable** for the two tasks
where a wrong judgement is expensive and evidence must drive design (the render-loop
restructure; the pop root-cause). **Opus + thinking high** for server tasks inside the
concurrency/load-path minefield. **Sonnet** for well-specified client/API/docs work
(thinking on where there is any judgement; off for mechanical edits). **Haiku** only for
B3, which is a two-line change with the exact lines cited.

---

## 7. Build, run, verify (Windows, this machine)

```powershell
# Server — configure once (VS2022 preset, vcpkg manifest; needs VCPKG_ROOT):
cmake --preset vs2022 -S server -DLIVEPLAY_BUILD_TESTS=ON
# Build:
cmake --build server/build --config Release -j
# Binary: server/build/Release/liveplay-server.exe
# Unit tests (all seven suites):
ctest --test-dir server/build -C Release --output-on-failure

# e2e (needs the server running with a REAL audio device — this machine has one):
server\build\Release\liveplay-server.exe --port 4480   # separate terminal / background
node server/tests/e2e/pfl-e2e.js                       # etc. — see server/tests/e2e/README.md

# Client compile check:
npm run build:nuxt
# Client dev (spawns/uses the server):
npm run dev

# Docs-site:
npm run build --workspace=docs-site
```

Notes: `npm run server:build` is the cross-platform wrapper. The debug preset builds into
`server/build-debug` with assertions. E2e scripts assume port 4480 and print their own
setup requirements in their headers; two of them (`latency-probe`, `control-latency-probe`)
are measurements, not pass/fail.

---

## 8. Orchestrator

**Agent:** Opus. **Thinking:** On, effort **Medium** (escalate own effort to High only when
re-planning after a `BLOCKED`). The orchestrator implements nothing itself; it dispatches,
serializes per §5, verifies, commits, and keeps §6 truthful.

**Operating procedure:**

1. **Session start:** read this file top to bottom. Run the §7 build + unit tests once to
   establish a green baseline; if the baseline is red, fix nothing — record and stop.
2. **Dispatch** the frontier of unblocked `TODO` tasks (initial frontier: A1, C1, B1, D1,
   G5), respecting §5's same-file serialization absolutely. One sub-agent per task, with the
   model/thinking/effort from §6. Each sub-agent's prompt must contain: the full text of its
   task spec from §4, the invariants (§2), the decisions it cites, and the §7 commands — do
   not make sub-agents read the repo to rediscover what this file already states.
3. **On task completion:** re-run the task's acceptance checks yourself (build, suites,
   named e2e, `build:nuxt` as applicable). Only then set `VERIFIED`, update Notes, and
   commit that task's changes as **one commit** to `2.5.0-dev` with the repo's message
   style (e.g. `feat: bus→bus routing, topological order on the control thread`) ending
   with the task ID in the body. **Never push. Never touch main.**
4. **On failure or ambiguity:** one retry with the failure evidence added to the sub-agent
   prompt; second failure → `BLOCKED` + full Notes, move on. Any needed decision not in §3
   → `BLOCKED`, never invent policy (D-numbers are the whole of policy).
5. **Keep §6 current in the file itself** (edit + include in the task's commit) — the table
   is the single source of progress truth for a resumed session.
6. **End state:** all tasks `VERIFIED` (or `BLOCKED` with questions), G4 last, then a final
   full pass: server build + all suites + e2e set green, `build:nuxt` green, docs-site build
   green. Write a closing summary at the bottom of this file under "## 9. Run log".

**Initial prompt (paste verbatim to start the orchestrator):**

> You are the orchestrator for the LivePlay 2.5.0 mixer-bus completion work. The repository
> is `m:\Github\liveplay`, branch `2.5.0-dev` (already checked out). Read
> `MIXER_BUSES_PLAN.md` at the repo root in full — it contains the audit, every binding
> decision (§3), complete task specifications (§4), the dependency/conflict map (§5), the
> task table you must keep updated (§6), build/verify commands (§7), and your operating
> procedure (§8). Follow §8 exactly: establish a green baseline, dispatch the initial
> frontier (A1, C1, B1, D1, G5) as sub-agents with the models, thinking modes and effort
> levels from §6, serialize same-file tasks per §5, verify every acceptance criterion
> yourself before marking VERIFIED, commit one commit per task to `2.5.0-dev`, and NEVER
> push or touch `main`. If anything requires a decision not recorded in §3, mark the task
> BLOCKED with the question in its Notes cell and continue with other tasks. Work until
> every task is VERIFIED or BLOCKED, finishing with G4 and the final full verification
> pass, then append the run log per §8.6.
