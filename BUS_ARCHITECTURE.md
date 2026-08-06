# LivePlay — Bus Architecture

> **Status:** Stages 0–3 complete on `fix/engine-config-wiring`, unmerged. **Stage 5 was done
> before Stage 4** — they are independent, the surface already promised the processing, and
> bus→bus is the risky one. The channel chain is complete: HPF, LPF, four-band EQ,
> expander/gate, compressor/limiter, section bypasses. See §0.4. **Stage 4 (bus → bus) is next**;
> plugins remain a separate future piece.
> Supersedes the "Stage 3 — Bus Mixing" sketch in `IMPROVEMENTS_PLAN.md` §6, which is now
> stale (it lists mute/solo/mixer-meters as missing; they exist).
> Ownership-model placement follows the object-ownership model discussed in issue #46.

---

## 0. Implementation status

Everything below §1 is the original design. This section records what is actually built and the
decisions taken during implementation that changed or sharpened it — read this first.

### 0.1 Done

**Stage 0 — prerequisites.** Device-override routings are released on project close and switch
(they leaked strips and master pairs for the life of the process). `mixer_accumulators_` is sized
once at `start()` for `max_mixer_channels` instead of growing on the render thread, with
`create_mixer_channel()` refusing past the cap (`--max-buses`, default 64). Strip-level REST for
gain and mute.

**Stage 1 — model.** Buses live in `document_["buses"]`, materialise onto engine strips on load,
and resolve by walking group ancestry: an item's own `busId` wins, else the nearest ancestor
group's, else Main. Full CRUD over REST with `buses_patched` broadcast. Master pairs are kept for
a bus's lifetime and returned to a free list on delete or rewire.

**Stage 2 — mixer. Complete.** A strip rail with the master pinned right, docking as a resizable
side pane, taking the full workspace, or popping out into its own Electron window
(`?mixerWindow=1`). Per-lane meters through one meter component. A full-window channel view
(§8.4) with a dedicated channel column, four-band EQ, dynamics, a plugin rack, the bus's
contributions and sends, and a metered channel-select row. `Knob.vue` + `KnobField.vue`, and pan on
mono buses — the one processing control that is real. The legacy `RoutingMatrixPanel`, the per-item
device-override control and `LiveMeterBar` are deleted.

The client files, since they are now spread across several components:

| File | Role |
|---|---|
| `MixerPanel.vue` | The rail, the master, and which view is showing |
| `MixerStrip.vue` | One rail strip; also renders the master via `master` |
| `MixerActions.vue` | Add-bus + window buttons; lives in a bottom bar, not a title bar |
| `MixerChannelDetails.vue` | The channel view's layout and its select row |
| `MixerChannelFader.vue` | The channel view's left column (its own layout, shared controls) |
| `MixerEqPanel.vue`, `MixerDynamicsPanel.vue` | Processing shells |
| `StereoMeter.vue` | Every meter in the app — cue, mixer strip, master pair |
| `CanvasFader.vue`, `Knob.vue`, `KnobField.vue`, `MeterScale.vue` | The shared controls |
| `utils/meterScale.ts` | The shared dB geometry and readout formatting |

**Stage 3 — PFL + Monitor. Complete.** Solo is gone from the engine, replaced by PFL: a
pre-fader, pre-mute tap into the Monitor strip, derived from a flag on the strip and carried in
the topology as a flat list of taps. The render loop's per-block `any_soloed` scan is retired.
Several buses can be tapped at once and the house mix never moves.

**Monitor is now the preview bus.** It owns the master pair the engine has always reserved at the
top of the bus, and cue pre-listen routes into it instead of a private "Preview" strip the mixer
never showed. So PFL'ing a bus and auditioning a cue arrive in one pair of headphones, under one
fader, on one meter — §2.4 said the reserved pair should eventually be absorbed this way, and it
now is. `ProjectState` no longer owns a preview mixer or a separately-opened preview device.

Monitor's hardware binding is the `"Monitor"` logical output when this machine maps one, else
`settings.previewDevice`. It appears in the mixer as a strip pinned beside the master, not in the
assignable rail — nothing routes *to* it.

New surface: `POST /api/buses/<id>/pfl`, `POST /api/buses/pfl/clear`, `pfl` and `bound` on
`GET /api/buses`, and a `bus_pfl_changed` / `bus_pfl_cleared` broadcast so a second mixer window
keeps up. PFL is deliberately **not** persisted — it is what the operator is listening to now, not
part of the show.

### 0.2 Decisions taken during implementation

| Decision | Why |
|---|---|
| **Output routing left the project entirely.** Main is a real bus; `settings.defaultOutputDevice` migrates onto it as a logical output and is dropped from the document. | A project naming a sound card cannot travel between venues. What a bus feeds is the show's business; what that output *is* in hardware belongs to the machine. |
| **`OutputMap` (`outputs.json` beside the exe), with identity fallback** — an unmapped name is treated as a device name. | A fresh install works with no configuration, and legacy device overrides migrate without moving any audio. |
| **`deviceOverride` migrates to one bus per device**, then the field is dropped. | A project should carry one routing concept, not two. |
| **A document PUT with no `buses` key means "unchanged"**, not "delete them all". | The client round-trips the whole document on every save and does not carry the bus list; taking absence literally wiped every bus on any unrelated edit. |
| **System buses (Main, Monitor) are hidden from the strip rail.** | Main has no user-facing level of its own and Monitor is the PFL destination nothing can be assigned to yet. Showing them made an unconfigured mixer look configured. |
| **Faders drive the engine live and persist on settle.** | Binding straight to `bus.gainDb` meant a PATCH plus a full refetch per drag event, and the knob fought the drag. |
| **The mixer's master fader drives output-channel gain on masters 0/1**, the same parameter as the transport bar. | Two faders both labelled master moving independently. **Consequence: the engine's global master gain now has no UI.** |
| **One dB scale shared by meter and fader.** Fader and scale span −60…+12; the meter is dBFS, tops out at 0, and its track covers only that part of the range. | 0 dBFS lands on the 0 tick, the meter keeps full resolution, and no dead strip sits above it. Only sound while both map dB to position linearly — see `client/app/utils/meterScale.ts`. |
| **Per-lane meters replace the combined read in the broadcaster.** | Both reset max-since-read; calling them in sequence would leave the second reading silence. Combined values are derived from the lanes. |
| **One meter component everywhere.** `StereoMeter` gained a mixer source, `mono`, `bare` and `showScale`; `LiveMeterBar` is deleted. | Two meter components meant two answers for the same signal: the strips painted a position-based gradient and only ever read sample peak, while the master painted a level-based zone colour and honoured the project's meter mode. Colour, peak hold, the clip latch and the readout now come from one place. |
| **The master is a `MixerStrip` with `master` set**, not bespoke markup in `MixerPanel`. | It had its own padding, spacing and row set, so the one strip that matters most looked unlike every other. Every row now exists on both — blank where it does not apply — because the faders only line up across the rail if the rows above and below them match. |
| **Bus width moved from the strip to the channel-details view.** | A button reading "M" sitting next to one reading "MUTE" is a trap, and width is a setup decision rather than a live one. The strip reports it next to the button that opens where it can be changed. |
| **The meter/fader block is the only part of a strip that flexes.** | It had a `min-height`, so a short pane overflowed instead of the fader getting shorter. Everything else is fixed height and the fader absorbs the difference, down to a floor where the pane scrolls instead. |
| **The channel view replaces the rail instead of docking under it, and drops its tabs** (§8.4). Its left column is a dedicated `MixerChannelFader`, not the rail strip. | Sharing the height left the strips half-size in the one mode with room for them, and moved the fader you were holding. Tabs put a compressor behind a click during a show. The channel column has room a rail strip does not — filters, a wider fader — so it is its own layout built from the same control components rather than the same layout twice. |
| **EQ bands are columns and parameters are rows**, and every cell is a knob *and* a typeable box. | A column is a band, which is the thing an operator reaches for; the transpose makes you read across to find one band. Ear-driven and spec-driven work both happen, so both input styles are present. |
| **Plugins**, not Inserts. | "Insert" names the routing; "plugin" names what the user is actually looking for. |
| **The mixer has no title bar.** Add-bus and the window buttons ride the bottom bar (`MixerActions`), which the channel view's select row hosts too. | A mixer is judged on how much of the window is fader. A header cost a row on every view to say "Mixer" to someone who just opened the mixer. |
| **Which channel is open is separate state from which strip is selected.** | With one value, any click on a strip threw the window out of the rail into the channel view. |
| **The detached mixer window still takes the cart window's project-data IPC**, despite needing no document to function. | Meter zone colours come from `settings.outputTargetLevels`, and theme/accent from `theme` — without them the popped-out meters would colour off the EBU defaults and disagree with the same meter in the main window. Buses, meters and fader moves do go over that window's own WebSocket. |
| **Detaching leaves `mixerOpen` alone**; the main window hides the panel while `mixerDetached` is set. | Closing the pop-out puts the panel back exactly where it was, and the header toggle can raise the window instead of opening a second copy of the same faders. |
| **Pan moves the two mixer→master send gains in place; it never rewires.** `POST /api/buses/<id>/pan` is live-only, `PATCH` persists on settle. | `route_mixer_to_master` replaces an existing send, so a pan drag drops no audio. Going through `unwire_bus`/`wire_bus` would release and re-acquire the master pair and re-open the device on every drag event. |
| **One `pan` field, two laws.** Mono buses are panned, stereo buses are balanced, and `apply_bus_pan` picks by width. | It is one knob in one place on the surface, so it is one value in the document. The distinction lives in the law, not in the storage — see §0.5 for why the laws must differ. |
| **Width lives in `dsp`, not beside `pan`.** | It is the one placement control that is genuinely per-sample (§0.5), so it needs the DSP path's live-then-persist plumbing rather than the pan endpoint's. |
| **An output-map save re-wires only the buses the edit actually moved.** `BusRouting` records what the name resolved to when it was wired; `PUT /api/outputs` re-resolves each Output-kind bus and leaves it alone unless the channels differ. | Buses are wired from the map at load, so without this a remap did nothing until the project was reloaded. Rewiring *everything* would have been the easy fix, but it drops audio on buses the edit never touched — not acceptable mid-show. A bus that failed to wire earlier records no resolution, so it compares as changed and gets retried, which is what you want right after fixing the map. |
| **Monitor IS the preview bus** — one strip, on the reserved master pair, fed by both PFL and cue pre-listen. | They were the same thing described twice: a pre-listen destination on headphones that the house never hears. Two of them meant two strips, two device handles for one pair of phones, two levels, and a preview strip the mixer never displayed. Merged, the Monitor fader is *the* headphone level and its meter shows everything you are auditioning. |
| **The PFL tap is pre-fader AND pre-mute.** | Pre-fader is the whole diagnostic use — hearing a channel with its fader down. Pre-mute is the same argument: checking a muted channel before unmuting it is exactly when you reach for PFL. Both fall out of tapping the accumulators before the strip pass, at no cost. |
| **PFL is derived from a flag on the strip, not maintained as a route by the caller.** | One source of truth. Raise the flag and the next topology snapshot carries the edge; the alternative was a route table that could disagree with the button. Width lives on the strip for the same reason — the engine needs it to place a mono strip centred rather than hard left. |
| **PFL is not persisted.** | It is what the operator is listening to right now. A project reopening with PFL latched puts signal in someone's headphones with nothing on screen to explain it. |
| **Monitor may not target the master, at the API and in the UI** (HTTP 409, and the option is absent from its dropdown). | PFL summing into the house, live, is the accident §2.4 chose PFL over solo to make impossible. Monitor goes to hardware or nowhere. |
| **Monitor gets no identity fallback when its output is unmapped** — unlike every other bus, which falls back to treating the name as a device. | `open_device_by_name()` falls back to the *default* device when a name matches nothing, so the fallback that keeps a migrated `deviceOverride` working would have put every PFL'd channel in the house. Unmapped Monitor is valid and silent (§7.5), and starts working the moment it is bound. |
| **`GET /api/buses` reports `bound`.** | The client cannot work out from the output-name list whether a bus reaches hardware: Monitor is usually bound through `settings.previewDevice`, which is not in the map at all, so inferring it put an "unmapped" warning on the one bus most likely to be working. |

### 0.2b Bugs found while building Stages 2 and 3

Each of these was reported as a UI symptom and turned out to be something else. Recorded because
the symptom is misleading in every case.

| Symptom | Actual cause |
|---|---|
| Assigning a cue to a bus emptied the whole mixer; expanding or undocking brought it back | `materialise_buses()` cleared `bus_routings_` under the lock and then released it for the entire rebuild, so a `GET /api/buses` landing in that window reported every bus with no strip — and the mixer hid buses without one. The table is now copied and swapped once at the end, and the mixer no longer filters on `mixerId`. **Verified: 2 bad reads in 13,371 polls before, 0 in 13,228 after.** |
| Cart items created in a detached window vanished on reattach, until restart | A detached window runs without the sync watchers, so nothing pushed the new item; the main window then pushed its own older copy back over IPC and the cart window cleared and repopulated from it. Detached windows now publish new items through the targeted endpoints and never author a whole-document save. |
| Dynamics appearing beside the plugin rack instead of at the top of its column | The short-window breakpoint dropped explicit grid *rows* but kept explicit *columns*, handing placement to auto-flow — whose cursor never moves backwards. |
| The window minimum "not working" (measured 1266x706 against a 1280x768 setting) | It was working. Electron's minimums describe the outer window unless `useContentSize` is set; the frame eats 14 and 62 on Windows. Every breakpoint and `vh` in this layout measures the page, so the floor now uses `useContentSize`. |
| **PFL arriving in the house** — the master read 5 dB hot the moment PFL went up | Two separate causes, both found by metering the master with PFL raised, neither visible from the code. (1) `ensure_default_routing()` took `mixers_.begin()` — an arbitrary strip out of an `unordered_map` — as "the Main mixer" and wired it to masters 0/1; sometimes that was the Monitor strip. It now skips the monitor and prefers a strip actually named Main. (2) An unmapped Monitor fell through the identity fallback to `open_device_by_name()`, which returns the *default* device on no match. Both are in the e2e harness as assertions, and both were confirmed to fail without the fix. |
| Repeat runs of the e2e harness disagreeing with the first | Two real leaks plus one bad assertion. `materialise_buses()` cleared the free master-pair pool without rewinding the allocator, so every project load abandoned its pairs and walked the counter towards the preview reserve. `rewire_buses_for_output_map()` compared Monitor against the plain output map, which never matches a binding that came from `settings.previewDevice`, so every save tore the headphone feed down and rebuilt it. And "the house level" was maxing over *every* master channel, which stopped meaning the house the moment Monitor was mapped — the reserved pair is a master channel too. |

| The gate ran but did nothing, at any setting | `ChannelDsp::advance_coeffs()` ramps the filter coefficients and originally only those, so the gate's coefficients were never copied out of the published slot into the struct the render thread reads. Every parameter change published correctly into a slot nothing looked at, and the render thread kept a default-constructed — disabled — gate. Anything added to `StripCoeffs` that is *not* ramped still has to be assigned there. The unit tests drove `GateState` directly and passed throughout; only the e2e caught it. |

### 0.3 Not done

- **Stage 4 — bus → bus.** A bus targeting another bus is accepted, warns, and stays silent.
- **Stage 5 — the compressor/limiter and plugins.** HPF, LPF, the four EQ bands and the
  expander/gate are real; §0.4 has the compressor design. The six-slot plugin rack is still a
  shell, and stays one — plugins were deferred deliberately.
- **The EQ bands are always bells.** The surface gives all four a Q control, and Q means something
  different on a shelf, so shelving LF/HF needs one more control per band to say which it is.
  `biquad_lowshelf` / `biquad_highshelf` are written and tested already.
- **`previewDevice` is still a device name in the project**, now as the fallback binding for the
  Monitor bus. The portable path exists — map `"Monitor"` in the output map and it wins — but the
  legacy field is still honoured, because dropping it would silently take pre-listen away from
  every project that has one configured. `ltcDevice` is untouched, being a separate feature.
- **Global master gain has no UI** (see above). A true master fader distinct from output trim is a
  real thing a desk has; needs a decision.
- **Frequency-dependent width above the bass** — widening the top independently of the middle.
  Bass-mono covers the half of this that matters (§0.5); a second crossover for the treble is a
  mastering flavour and was left out rather than doubling the control count on every strip.
- **A mono-sum audition button.** Folding a strip to mono in the Monitor bus alone would turn the
  phase-cancellation risk into something checkable rather than something the correlation number
  warns about. Cheap; not built.
- **Neither `CanvasFader` nor `Knob` is keyboard-reachable.** Deliberate, so the two behave
  identically, but it means the mixer cannot be driven without a pointer.
- **The EQ band handles are not draggable.** With no EQ behind them there is nothing to drag to;
  they are position markers on a flat curve. Making them live is Stage 5.
- **The Monitor strip has no dedicated "what am I listening to" readout.** It meters the sum of
  PFL and pre-listen, which is correct, but with three buses tapped there is nothing naming them
  except three lit PFL buttons and the count on the clear control.
- **Nothing on this branch has been driven through the GUI by me** — no browser driver is present
  and I did not add one. Every UI change was verified by build plus reasoning, and by the
  maintainer testing manually.

The **server** work is verified properly, and Stage 3 more thoroughly than the rest:

- Unit tests (`liveplay-mixer-tests`) cover PFL state on the strip, width clamping, and the
  monitor-tap arithmetic — which lane a tapped strip lands in, at what gain, that taps sum, and
  that the monitor never taps itself. `mix_monitor_taps` lives in its own header so it can be
  called without opening an audio device.
- `server/tests/e2e/pfl-e2e.js` drives a live server over REST and WebSocket with real audio
  through the real render loop, asserting the behavioural claims: pre-fader, pre-mute, the house
  never moving, mono arriving centred, PFL and pre-listen summing, and an unbound Monitor driving
  no hardware at all. It found two ways for PFL to reach the house that reading the code did not.
- Every safety assertion was confirmed to **fail** against a deliberately broken build before
  being trusted.

### 0.4 The channel chain

**The chain is fixed, not a plugin rack.** HPF → LPF → EQ ×4 → gate → compressor → width →
PFL tap → fader → pan/balance send → master. These are known blocks every strip has, always in that order, so they
are fields on `ChannelDsp` rather than an insert interface. Plugins arrive later as a separate
list alongside. It runs **regardless of mute**, because PFL is pre-mute.

Gate before compressor is the console order and the useful one: the gate removes what is below the
floor first, so the compressor is not working on noise that is about to be gated anyway. Reversed,
the compressor's makeup gain lifts that noise over the gate's threshold and holds it open.

**Where the pieces live**

| File | What |
|---|---|
| `audio/biquad.hpp` | TDF-II section + RBJ designers. Header-only, denormal-flushed, Nyquist-clamped. |
| `audio/dynamics.hpp` | Both processors: `Gate*` and `Compressor*` params / coeffs / state. |
| `audio/stereo_width.hpp` | The M/S matrix, the bass-mono side filter, and `CorrelationMeter`. |
| `audio/channel_dsp.hpp` | The chain. Double-buffered publish, per-block coefficient ramp. |
| `core/project_state.hpp` | `BusDsp`, `BusGate`, `BusComp`, `merge_bus_dsp`, `dsp_params_for`. |
| `client/utils/filterResponse.ts` | Display-only mirror of the C++ filter maths. **Must be kept in step by hand.** |
| `client/components/MixerDynamicsPanel.vue` | The surface, and a second by-hand mirror: its `compressed()` is the C++ static curve. |

**Rules that bind anything added to the chain**

- **Parameters cross to the render thread through `StripCoeffs`**, published into a double-buffered
  slot with an atomic index — *not* the topology, because a knob drag must not re-walk every route.
  Filter coefficients are ramped; dynamics coefficients are **assigned whole** in
  `advance_coeffs()`. Forgetting that assignment is what made the gate inert (§0.2b).
- **Detection is stereo-linked.** `ChannelDsp::process()` takes the lanes as a set for this reason,
  and a mono strip passes a lane count of 1. Keying per lane shifts the image; on a compressor it is
  worse than on the gate.
- **A no-op must be a genuine no-op** — bit-transparent, and reporting `needs_processing()` false so
  flat strips cost nothing. A chain going flat keeps running for a settle tail so it can ramp out.
- **Attack/release mean the opposite of the gate's.** On the compressor, attack is the gain
  *decreasing* (clamping down) and release is recovery. The gate's is the other way round and the
  notes in `_DSP_DOCS` do not say so.

**The compressor's controls** (`MixerDynamicsPanel.vue`, `compParams`): threshold −18 (−60…0 dB),
ratio 4 (1…60), makeup 0 (−12…24 dB), attack 10 (0.1…300 ms), knee 6 (0…24 dB), release 200
(5…5000 ms). Ratio, attack and release use the **log** taper; threshold, makeup and knee are
linear. Ratio 1 with a non-zero makeup stays in circuit deliberately — it is a legitimate way to
use the block as a plain gain stage, and switching it out would silently lose the level set.

**What `_DSP_DOCS/compressor_limiter.md` does not cover, and what was done instead**

1. **Soft knee**, omitted from the notes entirely and the single biggest difference between
   clinical and musical. Quadratic interpolation over a knee width `W` centred on the threshold:
   below `T − W/2` unity, above `T + W/2` the hard law, and in between
   `gain = −(1 − 1/ratio)·(x − T + W/2)² / (2W)` — continuous in value *and* slope at both joins,
   so the curve leaves unity flat and arrives already at the full ratio.
2. **The notes hard-code coefficients** (`attack_coefficent = 0.01`), which means nothing without a
   sample rate — the same constant is 0.2 ms at 48 kHz and 0.1 ms at 96. Everything derives from a
   time in milliseconds through `time_constant_coeff()`.
3. **Peak versus RMS detection**, which the notes raise and never decide. One control set covers a
   bus compressor and a limiter, so it detects peak and lets the ratio decide the character: an RMS
   detector could not do the limiter job at all.
4. **Makeup gain**, in the notes' parameter list and absent from their snippet. It rides on the
   same per-sample multiply and is deliberately kept *out* of the reported gain reduction — the GR
   meter answers "how hard is it working", and folding makeup in would show an idle compressor as
   though it were pushing.
5. **A limiter wants lookahead** to be genuinely brickwall. That means latency, which §2.7 defers,
   so this is a high-ratio compressor rather than a true brickwall — said out loud rather than
   implied. The master bus limiter is separate and unaffected.
6. **The side-chain has its own release**, capped at 15 ms rather than fixed there. A detector that
   decays much between peaks modulates the gain at the signal's own frequency, which is distortion
   on bass; but a short release setting exists precisely to be heard, and a fixed floor would
   quietly cancel it. So the detector follows the release until it would start rippling.

**Two mutation results worth keeping.** Swapping the compressor's attack/release comparison passes
every steady-state unit test and is caught only by the timing one — a compressor with them
backwards meters perfectly and destroys every transient. And deleting `active_.comp = target.comp`
from `advance_coeffs()` leaves all 43 unit tests green while turning seven e2e assertions red: the
same signature as the gate bug in §0.2b, which is why both suites exist.

### 0.5 The stereo image: balance and width

Two controls on a stereo bus, and they deliberately use **different mechanisms**, because one of
them cannot use the other's.

**Balance is send gains, like pan.** `apply_bus_pan` sets the two mixer→master send gains and
nothing else happens per sample. One `pan` field on the bus carries both meanings and
`apply_bus_pan` picks the law from the bus's width.

**The two laws differ on purpose** (`audio/types.hpp`):

| | centre | hard over | why |
|---|---|---|---|
| `pan_gains_db` (mono) | −3.01 dB both | live side at unity | must hold constant power as one source sweeps an image it does not otherwise occupy |
| `balance_gains_db` (stereo) | **unity both** | other side silent | only ever attenuates — a centred stereo bus must not lose 3 dB for doing nothing, and correcting a lopsided mix must not push the loud side into the limiter |

**Width cannot be a send gain, which is why it is DSP.** The matrix expands to
`L' = a·L + b·R`, `R' = b·L + a·R` with `a = (1+w)/2`, `b = (1-w)/2` — four sends, except **`b`
goes negative above w = 1** and send gains are decibels with no polarity. Narrowing would work;
widening, the entire point, cannot be expressed. So it is a per-sample block, last in the chain.

- **The `/2` convention, not `/√2`.** The symmetric form is the one usually written down, and it
  matters only if you are metering M/S or working in both domains at equal scale. Nothing here
  does. The `/2` form needs no compensation constant anywhere.
- **Unity width is not automatically a no-op.** `fl(L+R)` and `fl(L-R)` each round, so the matrix
  is not bit-exact at w = 1 in floating point. Identity is *detected* and the block skipped —
  `width_near_identity()`, with a tolerance rather than an exact test, because these are the
  ramped coefficients and a filter's feedback terms take far longer to land exactly than to
  become inaudible. Removing that snap turns two transparency tests red.
- **No level compensation, deliberately.** The obvious `1/max(1, w)` is wrong for anything not
  already wide: a mono source has S = 0, so widening does not touch it, yet that term would still
  pull it down 6 dB at w = 2. It also scales the mid, deepening the phantom-centre loss that wide
  settings already cause. There is a unit test asserting mono material is untouched at every
  width, which is the same statement from the other side.
- **Bass-mono is a high-pass on the side signal alone**, not a crossover. The mid is untouched, so
  `L + R = 2M` is preserved exactly at every frequency and the low end is mono-safe however hard
  the rest is widened. One biquad. Parked at 20 Hz is out of circuit, the same convention the
  strip's HPF and LPF use — so neither width nor bass-mono needs an in/out switch.
- **Width sits after the dynamics**, where a mastering widener goes, so the compressor keys off
  the source image and moving width never changes how hard it works. The cost, stated rather than
  hidden: this strip's own compressor cannot catch a peak the widening creates. The strip meter
  shows it and the master limiter catches it.

**Correlation, not a goniometer.** `CorrelationMeter` publishes one number per strip — `+1`
mono-compatible, `0` wide, negative meaning a mono sum will cancel part of it. Three MACs per
sample against a vectorscope's canvas and history buffer, and it answers the question the width
control actually raises. Measured post-width so it describes what leaves. Silence and a single
live lane both read `+1`: neither is a phase problem, and reading `0` there would put a warning on
every idle strip.

**Why the PFL tap changed.** Width is upstream of the tap, so the phones hear it for free. Balance
is *downstream*, in the sends, so `append_monitor_taps` applies it itself — exactly as it already
did for mono pan. Without that a hard-balanced bus would sound centred in the phones.

---

## 1. What exists today

The engine already has a full three-tier routing graph. The UI for it does not exist.

```
PlaybackItem ──send──► MixerChannel ──send──► Master channel ──► Device:HwCh
 (per cue,             (stereo strip:         (limiter +
  own decoder)          gain, fade,            meter, 1:1
                        mute, solo,            to hardware)
                        L/R meters)
```

**`MixerChannel` is already a bus.** It has gain with a cosine fade ramp, `set_mute()`,
`set_solo()`, and a per-lane meter
([mixer_channel.hpp:36-93](server/include/liveplay/audio/mixer_channel.hpp#L36-L93)). Mute and
solo are honoured on the render thread
([engine.cpp:1059-1071](server/src/audio/engine.cpp#L1059-L1071)) and round-trip through
`ProjectState` ([project_state.cpp:397-398](server/src/core/project_state.cpp#L397-L398), :3877,
:4009). The full routing matrix is exposed over REST: `POST /api/mixers`,
`/api/routing/item_to_mixer`, `/mixer_to_master`, `/master_to_device`
([control_server.cpp:1559-1634](server/src/net/control_server.cpp#L1559-L1634)).

**The only UI for any of it is dead code.** `RoutingMatrixPanel.vue` implements all three tiers
and is imported by nothing — grepping the client tree finds only the component's own file. It is
not behind a dev flag; it was simply never mounted. Its backing composable functions
(`routeItemToMixer`, `createMixerChannel`, …,
[useLiveplayServer.ts:807-836](client/app/composables/useLiveplayServer.ts#L807-L836)) are called
from nowhere else.

So the reachable UI offers exactly two routing controls:

| Control | Where | Effect |
|---|---|---|
| Project default output device | [ProjectSettingsModal.vue:300](client/app/components/ProjectSettingsModal.vue#L300) | Sets `settings.defaultOutputDevice` |
| Per-item device override | [PropertiesPanel.vue:126-144](client/app/components/PropertiesPanel.vue#L126-L144) | Sets `item.deviceOverride` |

That is why it looks like audio can only go to alternate outputs: **that is literally the only
thing the UI can express.**

### 1.1 `deviceOverride` is a degenerate bus

Each distinct device name lazily gets one mixer channel named `"Output: <device>"`, a dedicated
master pair, and a device assignment
([project_state.cpp:3310-3369](server/src/core/project_state.cpp#L3310-L3369)). Every item
pinned to that device shares it. That is a bus in all but name — keyed by hardware rather than by
anything the user names. **The bus system should subsume this mechanism, not sit beside it.**

### 1.2 Three findings that change the plan

**Nothing about routing is persisted.** The comment at
[project_state.cpp:1611-1615](server/src/core/project_state.cpp#L1611-L1615) says it outright:
server-side routing tables "don't get written to disk here; they're rebuilt from the document on
next load." For the client `.liveplay` format, `load_from_json` explicitly *clears* `mixers_`,
`item_routes_`, `mixer_routes_` and `master_assignments_` and never repopulates them
([project_state.cpp:3781-3785](server/src/core/project_state.cpp#L3781-L3785)); routing is
re-derived at play time from `deviceOverride` / `defaultOutputDevice` plus an "auto-route
unrouted cues to Main" fallback. Only *device name strings* survive a save. A user-defined bus
layout is therefore **new persistence from zero**, not an extension of something existing.

**Groups have no audio meaning whatsoever.** `GroupItem` is `children` + `startBehavior` +
`endBehavior` + `isExpanded` ([types/project.ts:63-69](client/app/types/project.ts#L63-L69)) —
no gain, no mute, no routing. All 18 server-side `type=="group"` checks are tree-walking for
sequencing. Group bus assignment is entirely new work.

**There is a real leak.** `device_routings_` and `next_override_master_` are never cleared —
`new_project`/`reset` clears `cues_`, `mixers_`, `item_routes_` but not these
([project_state.cpp:1002-1006](server/src/core/project_state.cpp#L1002-L1006)). Device-override
busses and their master pairs accumulate for the life of the *process*, across project switches,
until restart. Open enough projects with different overrides in one session and master-channel
allocation fails. This should be fixed regardless of the bus work, and gets worse with it.

---

## 2. Target model

### 2.1 Bus object (Project tier)

```jsonc
{
  "id":      "bus-<uuid>",
  "name":    "FOH",
  "color":   "#4A9",
  "order":   0,
  "width":   2,                       // channel count, user-chosen — §2.5
  "gainDb":  0.0,
  "mute":    false,
  "pfl":     false,
  "output":  { "type": "master" | "bus" | "output", "target": "…" },
  "sends":   [],                      // explicit lane map, §2.5.1; empty = default rule
  "inserts": []                       // §2.6
}
```

Two **system busses** always exist and cannot be deleted:

- **Main** — the default destination for everything. `output → master`.
- **Monitor** — the PFL / pre-listen destination, routed to the preview device. §2.4.

`output.type`:

| Type | `target` | Meaning |
|---|---|---|
| `master` | *(unused)* | Into the master bus — inherits the master limiter |
| `bus` | another bus id | Submix feeding another bus (§2.3) |
| `output` | a **logical output name** | Direct to hardware, bypassing master — §2.1.2 |

**A bus never names a device.** `output.target` is always a logical name; the binding to physical
hardware lives in a separate map (§2.1.1) that the project does not contain and never sees. This
is what keeps a show portable: email a `.liveplay` to another venue and it references `"FOH"`,
not `"Focusrite Scarlett 18i20 — Analogue 3"`.

#### 2.1.1 The logical output map (separate, and not in the project)

```jsonc
// Server-owned. Lives with the server config, NOT in the .liveplay.
"outputs": [
  { "name": "FOH",      "channels": [ {"device": "…", "hwCh": 0}, {"device": "…", "hwCh": 1} ] },
  { "name": "Monitors", "channels": [ {"device": "…", "hwCh": 2}, {"device": "…", "hwCh": 3} ] },
  { "name": "Comms",    "channels": [ {"device": "…", "hwCh": 4} ] }
]
```

A logical output has a **width** of its own (the length of `channels`), which need not match the
width of the bus feeding it — see §2.5.1. One rack, many shows, one map: opening a project whose
logical outputs are not all mapped prompts once, on the server, and the mapping is reused for
every subsequent show in that rack.

#### 2.1.2 Direct-to-hardware bypasses the master limiter

A bus with `output.type = "output"` goes straight to hardware, skipping the per-master-channel
limiter and meter chain entirely ([engine.cpp:1092-1111](server/src/audio/engine.cpp#L1092-L1111)).

This is a deliberate capability — a record bus or a comms feed usually *should not* be
brick-walled — but it voids the guarantee the README currently makes, that "clipping is
impossible for finite inputs." That guarantee holds only for signal that transits the master bus.

**Recommendation:** a direct-out bus gets a limiter insert enabled by default, which the user can
remove deliberately. The alternative — silently unprotected hardware outputs — is the kind of
thing that is discovered during a show. Whichever way this goes, the README's claim needs
qualifying once direct-out ships.

### 2.2 Assignment and inheritance

`AudioItem.busId?` and `GroupItem.busId?`. Both optional; absent means *inherit*.

Resolution is **nearest ancestor wins**, evaluated at trigger time:

```
item.busId  ?? nearest ancestor group with busId  ?? Main
```

Item beats group automatically, because the item is the first thing checked. Nested groups fall
out for free — you walk up until something is set. Note this needs a parent lookup the tree walk
does not currently provide, since `for_each_item` descends without tracking ancestry.

**`busId` is the whole of an item's routing.** There is no per-item output device, no per-item
send, no per-item matrix — one property, chosen from a dropdown, and the item is done. Everything
about where that audio then goes belongs to the bus and is edited in the mixer (§8). This is why
the existing per-item `deviceOverride` control disappears rather than being kept alongside:
"play this cue out of the other sound card" becomes "assign this cue to a bus that outputs there",
which is both the console idiom and the only version that stays portable (§2.1).

### 2.3 Bus → bus, and the one genuinely hard part

Today the graph is inherently two hops and needs no traversal order — `Topology` is
`{ items, masters }` with items carrying sends into mixers and masters carrying sends from mixers
([engine.hpp:131-165](server/include/liveplay/audio/engine.hpp#L131-L165)). Allowing a bus to
feed a bus makes it a directed graph, which brings two requirements:

1. **Cycle rejection.** Reject the wire at the API with a clear error, *and* defensively drop the
   offending edge at topology-build time with a log line. A cycle that reaches the render thread
   is an infinite loop in the audio callback — the worst possible failure in this program.
2. **A processing order.** Busses must be processed in topological order so a bus's inputs are
   complete before it is read. Computed **on the control thread** in `rebuild_topology_locked()`
   and stored in the snapshot as a plain index list. The render thread iterates that list; it
   never traverses the graph.

Both belong to the control thread. The render thread's contract stays "walk a flat list."

### 2.4 PFL instead of solo

**Decision: replace solo with PFL.** A PFL'd bus adds one send into the Monitor bus. Nothing is
muted, the house output is untouched, multiple PFLs sum — standard console behaviour.

This is strictly better than solo-in-place here:

- **Safety.** Solo-in-place means one mis-click silences everything except one bus, live, in
  front of an audience. PFL cannot do that.
- **Simplicity.** Solo is a *global audibility* problem — with bus→bus it becomes a graph walk
  (keep upstream feeders and downstream destinations audible, honour solo-safe) recomputed on
  every graph or solo change. PFL is one extra edge.
- **It deletes code.** The render thread's per-block `any_soloed` scan
  ([engine.cpp:1059-1071](server/src/audio/engine.cpp#L1059-L1071)) goes away, along with the
  divergence from `IMPROVEMENTS_PLAN.md`'s own instruction to keep solo logic off the render
  thread.

Mute is unaffected and stays per-strip. Muting a bus zeroes its contribution and everything
downstream follows naturally — no graph knowledge needed.

**Tap point: pre-fader.** You need to hear a bus *with its fader down* — that is the entire
diagnostic use case. Cost: the render loop applies strip gain in place on the accumulator
([engine.cpp:1072](server/src/audio/engine.cpp#L1072)), so a pre-fader tap must copy the lane
buffer before the gain multiply — one buffer copy per PFL'd bus per block. AFL would be free
(tap after), but is the less useful control.

**The Monitor bus should eventually replace the reserved preview pair.** Preview is currently a
hardcoded pair at the top of the master bus. Once PFL and cue pre-listen both feed a real
Monitor bus, `preview_master_base()` can retire. Both feeding one bus means previewing a cue and
PFL'ing a bus sum in the headphones — correct console behaviour, but state it deliberately.

### 2.5 Bus width — mono or stereo

**Scope: `width ∈ {1, 2}` only. Multi-channel busses are explicitly deferred** to a separate
design conversation. Everything below is written so that deferral costs nothing later.

**Storage stays two lanes per bus.** `kMixerLanes = 2` remains a compile-time constant
([types.hpp:43](server/include/liveplay/audio/types.hpp#L43)) and accumulators keep their
`mixerIdx * 2 + lane` addressing
([engine.cpp:1050-1076](server/src/audio/engine.cpp#L1050-L1076)). A mono bus uses lane 0 and
leaves lane 1 idle; `width` is a **semantic property** that governs metering, UI, and the summing
rules — not the buffer layout.

This is a deliberate reversal of the earlier plan to introduce a per-bus offset table up front.
With only mono and stereo in scope, the case for it collapses:

- **The waste is negligible.** One idle lane is `render_block × sizeof(float)` = 1 KB per mono bus
  at the default 256-frame block. Not worth touching the render thread's hottest indexing for.
- **It keeps the riskiest code untouched.** The accumulator addressing is in the per-block inner
  loop; leaving it byte-identical means mono support cannot possibly regress stereo playback.
- **Nothing is foreclosed.** If multi-channel is taken up later, the offset-table refactor is the
  same mechanical change it would be today, and `width` is already a per-bus property by then.

Only if multi-channel becomes near-term does the offset table earn its place in this stage. Then,
and only then, these change together: accumulator indexing → `offset[mixerIdx] + lane`;
`MixerChannel::meters_` → `std::vector<Meter>` sized on the control thread
([mixer_channel.hpp:114](server/include/liveplay/audio/mixer_channel.hpp#L114)); WS meter payload
→ variable length.

What *does* change now, regardless: a send carries `(srcLane, dstLane, gain)` rather than a
destination lane alone, because 2→1 and 1→2 need to express a real mapping.

#### 2.5.1 Summing rules

Four cases, all of them concrete:

| Source → Dest | Default |
|---|---|
| 2 → 2 | Straight through, L→L, R→R, unity |
| 1 → 1 | Lane 0 → lane 0, unity |
| 1 → 2 | Lane 0 feeds both destination lanes, subject to pan (§2.5.3) |
| 2 → 1 | Sum L+R with a downmix attenuation (§2.5.2) |

Overridable per send; the defaults exist so the common case needs no matrix. `kAllMixerLanes`
keeps working as sugar for "feed every lane of the destination."

#### 2.5.3 Pan is now well-defined

With only mono and stereo in play, pan stops being an open question. **Pan is a property of a
mono→stereo send, not of a strip** — it is the position of lane 0 between the destination's two
lanes, which is exactly what a console pan pot does on a mono channel.

- A **mono bus** feeding a stereo destination gets a pan control.
- A **stereo bus** gets balance, or nothing at all for v1 — balance is a nicety, pan is not.
- Neither needs a strip-level pan field on `MixerChannel`, so no engine state is added.

> **Superseded in part (§0.5).** Balance is built, and it shares the `pan` field rather than
> getting its own — but under a *different law*, because a constant-power pan applied to a stereo
> bus loses 3 dB at centre and adds gain at the extremes. A strip-level field did turn out to be
> needed after all: the PFL tap sits upstream of the sends, so the strip carries its own copy to
> place itself in the phones. Stereo buses also gained M/S width, which is not a send gain at all.

The pan law is the same −3 dB question as §2.5.2, and should use the same answer so a mono source
panned centre and a stereo source folded to mono behave consistently.

#### 2.5.2 Downmix law — decided: −3 dB

Summing two correlated channels into one adds +6 dB. Two conventions:

- **−3 dB (power-preserving).** Correct for uncorrelated material, standard pan law on most
  consoles. Correlated (mono-ish) content still comes out +3 dB hot.
- **−6 dB (amplitude-preserving).** Correct for correlated material, standard for broadcast L+R
  → mono fold-down. Uncorrelated content comes out 3 dB quiet.

**Decision: −3 dB**, matching console pan-law expectations. The same value governs both
stereo→mono folding and a centre-panned mono source (§2.5.3), so the two stay consistent.

**It must be one named constant, not a literal.** Define it once:

```cpp
// audio/types.hpp — power-preserving pan/downmix law.
inline constexpr float kDefaultDownmixDb = -3.0f;
```

Every fold and every centre-pan reads that symbol. The intent is that it becomes a **Server-tier
config key** (`--downmix-law`, per `OWNERSHIP_MODEL.md` §5.1) once the server config file exists,
so a facility can standardise on −6 dB without a rebuild. Writing `-3.0f` inline in the summing
code would make that a find-and-replace across the render path instead of a config wire-up — the
exact "copied constant with no owner" pattern the ownership doc exists to prevent.

Per-send gain remains available to override the law case by case.

### 2.6 DSP insert scaffolding

An insert chain per bus, built and configured on the control thread, published in the topology
snapshot as a `shared_ptr`. **Nothing about a processor may allocate on the render thread.**

```cpp
class BusProcessor {
public:
    virtual ~BusProcessor() = default;
    virtual void configure(SampleRate, FrameCount max_block, ChannelCount lanes) = 0;
    virtual void process(Sample* const* lanes, ChannelCount lane_count,
                         FrameCount frames) noexcept = 0;
    // Reported from day one; not yet compensated. See §2.7.
    virtual FrameCount latency_frames() const noexcept { return 0; }
    virtual void reset() noexcept {}
};
```

Ship the interface with a passthrough and one real processor (a biquad EQ band) to prove the
contract. EQ / compressor / expander follow as `BusProcessor` implementations. This is also the
insert point a future CLAP/VST3 host plugs into, per `IMPROVEMENTS_PLAN.md` §6.

### 2.7 Latency is a known, deferred gap

There is **no latency compensation anywhere in the codebase**, and the master limiter's 5 ms
lookahead is already uncompensated today. Inserts make this worse and more visible.

Recommendation: **report latency from the first commit, compensate later.** Having
`latency_frames()` in the interface from the start costs nothing and means PDC is a later
addition rather than a refactor. Document it as a known limitation instead of discovering it
during a show.

### 2.8 Ownership placement

Per `OWNERSHIP_MODEL.md`:

| Thing | Tier | Why |
|---|---|---|
| Bus definitions, names, colours, order | **Project** | Show structure; travels with the file |
| Bus gain, mute, insert settings | **Project** | Affects what the audience hears (R2) |
| Item / group `busId` | **Project** | Show structure |
| `output.target` logical name | **Project** | Semantic intent, portable |
| Logical name → physical device | **Server** | The engine opens the sound card (§2.3 of the ownership doc) |
| PFL engaged | **Session** | Transient monitoring state, not saved |
| Max bus count | **Server** policy | Resource ceiling |

**`output` should reference logical names, not device names, from day one.** Storing device name
strings is the portability bug the ownership doc calls the worst violation in the codebase —
email a show elsewhere and it references sound cards that do not exist. Busses are exactly where
that gets fixed, and doing it now avoids migrating the same data twice.

---

## 3. Engine changes

1. **New route type** `MixerChannel → MixerChannel` in `PendingRoute` and `Topology`
   ([engine.hpp:358-383](server/include/liveplay/audio/engine.hpp#L358-L383)).
2. **`Topology` gains a processing order** — a topologically sorted list of bus indices, built in
   `rebuild_topology_locked()` ([engine.cpp:152-213](server/src/audio/engine.cpp#L152-L213)).
3. **Render loop restructure** ([engine.cpp:1057-1090](server/src/audio/engine.cpp#L1057-L1090)):
   iterate busses in order; per bus — advance fade, apply mute + gain, run inserts, meter, then
   fan lanes into the destination bus or master accumulators. Replaces the current fixed
   "mixer pass then master pass."
4. **PFL tap** — copy lane buffers into the Monitor accumulator before the gain multiply.
5. **Pre-size `mixer_accumulators_` on the control thread.** It is currently grown lazily *on the
   render thread* ([engine.cpp:1006-1009](server/src/audio/engine.cpp#L1006-L1009)) — an existing
   real-time violation that user-controlled bus counts would make routine rather than rare.
6. **Per-bus `Limiter` is optional**, not automatic. Limiters exist only per master channel today
   ([engine.hpp:390-394](server/include/liveplay/audio/engine.hpp#L390-L394)); a bus limiter
   should be an insert, not a fixed fitting.

## 4. API surface

New REST (none of this exists — there is currently **no endpoint at all** for mixer gain, mute or
solo):

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/buses` | List with live state |
| POST | `/api/buses` | Create |
| PATCH | `/api/buses/<id>` | name, colour, gainDb, mute, pfl, output |
| DELETE | `/api/buses/<id>` | Delete; reassign orphans to Main |
| POST | `/api/buses/<id>/route` | Set output target; **rejects cycles** |

Item/group assignment rides the existing `PATCH /api/project/items` as a `busId` field. WS gains
a `buses` section in the meter broadcast and a `bus_patched` doc-patch op.

## 5. Persistence and migration

Bus definitions and `busId` assignments go in the `.liveplay` document — the first routing
information ever persisted there. Schema version bump required.

Migration on load:

1. No `buses` array → synthesize **Main** (and **Monitor**), assign nothing. Behaviour identical
   to today.
2. Items with `deviceOverride` → synthesize one bus per distinct device name, `output` pointing
   at that device, and set those items' `busId`. `deviceOverride` becomes a legacy field read on
   load and no longer written. This retires §1.1's shadow bus system rather than running two in
   parallel.
3. Persisted `solo: true` on a mixer channel → migrate to `pfl` and stop writing `solo`. Low risk
   — solo was never reachable from the UI, so real data is unlikely.

## 6. Staging

Each stage is independently shippable. Value lands early; the graph work lands last.

| Stage | Work | Risk |
|---|---|---|
| **0 — Prerequisites** | Fix the `device_routings_` / `next_override_master_` leak (§1.2). Pre-size `mixer_accumulators_` off the render thread. Add REST for bus gain/mute. | Low. Pure bug fixes, valuable with or without busses. |
| **1 — Model + assignment** | Bus schema (incl. `width` ∈ {1,2}), persistence, migration, ancestor-walk resolution, materialisation into engine mixer channels on load. Logical output map + logical-name resolution. Summing rules + mono→stereo pan. **No bus→bus yet.** UI is only the bus dropdown in PropertiesPanel plus a bus badge on playlist items — per §8.0 that is the entirety of per-item routing. | Medium. New persistence, but the render loop's accumulator addressing is untouched (§2.5). |
| **2 — Mixer surface** | `MixerPanel.vue`: conventional vertical channel strips, one per bus (§8.2). Per-bus meters in the WS broadcast. Channel-details page (§8.4) with Overview + Output real and EQ/Dynamics/Inserts as placeholders, including the persistent mini-strip bank. New `Knob.vue`; `width` prop on `CanvasFader`. **Delete `RoutingMatrixPanel.vue` and the per-item `deviceOverride` control.** | Low-medium for the strips; the details page is mostly shell. |
| **3 — PFL + Monitor bus** | Monitor bus as a real bus; PFL send with pre-fader tap; retire the `any_soloed` render-thread scan. | Medium. Touches the render loop. |
| **4 — Bus → bus** | New route type, topological order, cycle rejection at API and build. | **High.** The only stage that changes the fundamental graph shape. |
| **5 — Inserts** | `BusProcessor` interface, chain plumbing, passthrough + one EQ band. New `Knob.vue` (§8.2). Latency reported, not compensated. | Medium, then open-ended as processors are added. |

Stage 0 is worth doing immediately regardless of whether the rest proceeds.

## 7. Open questions

1. **Bus count ceiling.** Busses feeding the master are cheap (2 lanes × block × float + two
   meters). Busses bound to *hardware* consume a master pair each, and the master bus is
   4–1024 wide with two reserved for preview. Propose `--max-buses` as a Server-tier policy,
   default 64.
2. **Should ducking become bus-aware?** It currently pokes every *other* `PlaybackItem`'s gain
   from the control thread ([project_state.cpp:2497-2522](server/src/core/project_state.cpp#L2497-L2522)).
   "Duck this bus" is the more natural control once busses exist, but it is a behaviour change.
4. **Does deleting a bus with children reassign to Main, or refuse?** Proposed: reassign, with a
   confirmation naming the affected item count.
5. **What happens when a logical output has no binding on this machine?** Proposed: the bus stays
   valid and silent, the UI flags it, and the server prompts once to map it. It must never be an
   error that stops a show from loading.
6. **Can a direct-to-hardware bus be limiter-less?** §2.1.2. Proposed: limiter insert on by
   default, removable deliberately.

---

## 8. Mixer UI

Laid out along the lines every mixing desk and DAW mixer has converged on — a bank of vertical
channel strips, and a per-channel detail view. Operators already have these conventions in their
hands; deviating from them buys nothing and costs familiarity, so this section describes the
standard shapes rather than inventing new ones.

### 8.0 Two corrections to earlier drafts

**`RoutingMatrixPanel.vue` is retired, not repurposed.** An earlier draft proposed keeping it as
the Routing tab of the channel-details page. That was wrong: it exposes the engine's three-tier
matrix directly — source channel → mixer → master → device, wired one row at a time — which does
not scale past a handful of cues and asks the operator to think in the engine's terms rather than
the desk's. It should be deleted once the mixer replaces it.

**Routing leaves the item entirely.** Per-item routing UI is not simplified, it is *removed*. An
item or group carries one property, `busId`, and nothing else about where audio goes. Everything
else — what a bus feeds, its level, its inserts — lives in the mixer window. This also retires the
per-item `deviceOverride` dropdown in `PropertiesPanel.vue`: "send this cue to the other sound
card" becomes "assign this cue to a bus that outputs there."

### 8.1 Strips are buses, not cues

A DAW mixer gives every track a strip. LivePlay should not: a "track" here is a playing cue, which
is transient — cues start and stop constantly during a show, and a mixer whose strips appear and
vanish mid-show is unusable. **The mixer shows buses only**, plus the master. What feeds a bus is
shown *inside* the channel-details page (§8.4), which is where the console analogue of an input
list belongs.

### 8.2 Strip anatomy

Top to bottom, in the conventional order. Note the name sits at the **bottom**, where the scribble
strip is on a physical desk — this contradicts the earlier draft in this document, which put it at
the top.

```
┌────────────────┐
│ INSERTS        │  4 slots. Stage 2: placeholders. Stage 5: real.
│ [ ---- ]       │
│ [ ---- ]       │
├────────────────┤
│ OUTPUT         │  Master │ Bus N │ a logical output.  The most
│ [ Master   ▾]  │  important control on the strip.
├────────────────┤
│ [ST]      pan  │  width badge; pan only when mono → stereo
│           ( ◠) │
├────────────────┤
│  [PFL]  [MUTE] │
├────────────────┤
│  12 ┌──┬─────┐ │
│   6 │▓▓│  ▮  │ │  meter + fader share the dominant vertical
│   0 │▓▓│  ▮  │ │  space, with a dB scale down the left edge
│  10 │▓▓│  ▮  │ │
│  20 │▓▓│  ▮  │ │
│  40 └──┴─────┘ │
├────────────────┤
│     -3.0       │  numeric dB, click to type
├────────────────┤
│      dyn       │  insert-active indicator (PT's "dyn" row)
├────────────────┤
│ ▮ FOH          │  colour chip + name — double-click to rename
└────────────────┘
```

The **master strip pins to the right**, visually separated, and uses `StereoMeter` rather than
`LiveMeterBar` — it already has the clip latch, peak hold and gain-reduction sub-track that belong
on a master.

Buses whose logical output has no binding on this machine (§7.5) render with a warning state on
the output selector rather than looking healthy.

### 8.3 Where the mixer lives

There is **no vue-router and no `pages/` directory** — the whole app is `app.vue` swapping between
`WelcomeScreen` and `MainWorkspace`, with every "view" a conditional panel driven by a `ref`. The
mixer is therefore a panel swap inside `MainWorkspace.vue`, following the pattern `cartFullscreen`
/ `cartClosed` already use ([MainWorkspace.vue:96-166](client/app/components/MainWorkspace.vue#L96-L166)),
with a toggle in `ProjectHeader.vue`.

### 8.4 Channel view

Opening a channel **replaces** the strip rail rather than sharing the window with it. The earlier
draft had it as a drawer under the rail, which was wrong twice over: it left the strips at half
height in the one mode that has room for them, and the fader you were adjusting ended up somewhere
other than where you grabbed it.

The zoning follows what a large-format live console does on its channel screen, and each part of
that shape carries a reason:

| Zone | LivePlay |
|---|---|
| Header | Colour chip, name, width + output summary, delete, back to the rail |
| Left column | `MixerChannelFader` — ‹ name ›, meters, fader, mute/PFL, HPF/LPF, pan. Full height |
| Work area, column 1 | **EQ**, full height |
| Work area, column 2 | **Dynamics** taking the height, **Plugins** rack beneath it at the height its slots need |
| Work area, column 3 | **Contributions** above **Sends**, full height |
| Bottom | Channel select row, each tile carrying a live meter, with ‹ › arrows |

**The channel column is not the rail strip.** A rail strip is a dense summary sized to sit twenty
across; this is one channel with the height to carry filters and a bigger fader. They share every
control component — `StereoMeter`, `MeterScale`, `CanvasFader`, `Knob` — so the parts stay identical
even though the arrangement does not. Channel stepping sits either side of the name, where the
thing being stepped is.

**No tabs.** An operator reaching for a compressor mid-show should not have to find a tab first.

**EQ runs bands across, parameters down.** One column per band, rows for frequency, gain and Q. A
column is then a band — the thing you actually reach for — and comparing one parameter across bands
is a glance along a row. Every cell is a knob *and* a typeable box (`KnobField`), because an EQ set
by ear and an EQ set from a spec sheet are both real jobs. The frequency axis on the curve is
logarithmic, so an octave takes the same width everywhere.

**EQ never scrolls its controls.** The band grid keeps its natural height and the graph takes
whatever is left, with a floor so it cannot collapse. It has no ceiling: EQ owns a full-height
column, so growing it starves nothing, and a curve display is one of the few things that keeps
improving with height. (It was clamped while EQ shared a row with dynamics, when growing it *did*
steal from the panels underneath.) An EQ you can see but not adjust is worse than one you have to
scroll to reach.

**The rows are `1fr auto`, not the reverse.** The plugin rack takes only the height its six slots
need and dynamics absorbs the slack above it. Sizing the rack row instead would hand the leftover
height to six empty slots.

**Dynamics packs its knobs rather than spreading them.** The control grids are `repeat(3, auto)`
justified to the start, not `1fr` columns — `1fr` spread six controls across whatever width was
going, which left a group reading as scattered dots instead of a block you take in at once.

**The gate and the compressor share one transfer graph, but not their meters.** They act on the
same axis — input level in, output level out — and one curve is how you see what the pair actually
does; two graphs would show two halves of one answer. Gain reduction is the opposite case: how far
each one is pulling is exactly what tells them apart, so they get a meter each. Those are
deliberately *not* `StereoMeter` — that measures signal level against the project's output target,
and this measures how far a processor is pulling down. Different quantity, different scale.

**EQ and dynamics sit side by side, not stacked.** Stacking them made both too short to use.
Beneath each sits the thing that belongs to it and needs no height of its own: the plugin rack
under EQ (three across, two down — six slots in the shortest arrangement), the connection panels
under dynamics.

**The window has a floor of 1280x720 of *page*** (`MIN_WINDOW` in `client/electron/main.js`),
applied to the main window and the detached mixer but not the cart player, which is a pad grid and
is legitimately used shrunk into a corner. `useContentSize` is the load-bearing part: without it
Electron's minimums describe the outer window, so a 1280x768 floor left the page at 1266x706 on
Windows — and every breakpoint and `vh` unit in this layout measures the page. Measured outer size
is about 1295x784, so the app wants a display with ~800px of usable height. The breakpoints below
stay as a safety net for display scaling and for the docked side pane, but the channel view is sized
against that floor: at 720 the EQ panel lands near 350px, leaving the row beneath it roughly 270 for
the plugin rack and the bus lists.

**Two escapes, one per axis, and they are not the same escape.** Below 1180px *wide* the grid
collapses to a single column. Below 660px *tall* it keeps both columns and only stops dividing the
height — a short window is usually a wide one, and collapsing there would waste the width it does
have. Only `grid-template-rows` changes in the height case: an earlier version also set
`grid-row: auto` while leaving the explicit columns, which handed placement back to auto-flow, and
because the auto-placement cursor never moves backwards, dynamics landed in column 2 *row 2* beside
the plugin rack instead of at the top.

**Contributions above Sends, in signal order** — what arrives, then where it goes. Sends has fixed
content and takes only what it needs at the bottom; contributions takes the rest and scrolls its
list inside itself. A bus can feed a hundred cues, so that list must never be what decides the
column's height. The count in the contributions heading is there to give the length without
scrolling to find it.

**The select row along the bottom carries meters.** The point of the row is knowing which channel to
go to, and on a desk that judgement is made by watching level, not by reading names. No scale — at
that size it would be unreadable — but the same meter component as everywhere else, so the colours
mean the same thing.

**Selection and "which channel is open" are separate.** When they were one value, any click on a
strip threw the window out of the rail and into the channel view. Opening is now something you ask
for — the strip's button, or the select row.

**The right-hand column answers "what is connected to this."** On a desk that is sends and bus
assignment. LivePlay's equivalent is the bus's own output *plus* the list of items and groups
assigned to it — because in LivePlay, inputs are cues, and that list is the only place the operator
can see the consequence of all those per-item bus assignments in one view.

Stage 2 ships this shell with Overview and Output real, EQ/Dynamics/Inserts labelled placeholders.
Stage 5 fills panels rather than inventing navigation late.

### 8.5 Components: what exists, what must be built

| Need | Use | Notes |
|---|---|---|
| Fader | [`CanvasFader.vue`](client/app/components/CanvasFader.vue) | Canvas, vertical, dB-domain, drag / shift-fine / wheel / dbl-click reset. **Hardcoded 20px** — needs a `width` prop before a detached mixer can scale up |
| Strip meter | [`LiveMeterBar.vue`](client/app/components/LiveMeterBar.vue) | `vertical` prop, sources its own data via `source: 'mixer'` + id — pass props, no wiring |
| Master meter | [`StereoMeter.vue`](client/app/components/StereoMeter.vue) | 68px, clip latch, peak hold, GR sub-track |
| PFL / Mute | [`ActionButton.vue`](client/app/components/ActionButton.vue) | Square icon toggle with `isActive` + `highlightColor` |
| Button variants | `.qm-btn` in [`QuitConfirmModal.vue:125-158`](client/app/components/QuitConfirmModal.vue#L125-L158) | The canonical token-driven set |
| Detail page tabs | `.properties-tabs` / `.tab-btn` from [`PropertiesPanel.vue`](client/app/components/PropertiesPanel.vue) | `ProjectSettingsModal.vue:451` documents this as the shared convention |
| Icons | `<span class="material-symbols-rounded">name</span>` | No wrapper component exists |
| Touch sizing | `useUiMode()` | Show Mode multiplier is ~1.6× ([CartSlot.vue:1225-1259](client/app/components/CartSlot.vue#L1225-L1259)) |

**Must be built:**

- ~~**`Knob.vue`**~~ — **built.** Canvas, 270-degree travel with the gap at the bottom, anchored
  vertical drag over a fixed 140px (a knob is too small to map its own height to the range),
  shift for fine, wheel, double-click to reset, and CSS custom properties read at draw time so it
  re-themes without a remount. The value is a plain number in the caller's unit — nothing about dB
  or pan is baked in, so EQ can reuse it. `origin` decides where the value arc grows from, which is
  what makes it read as a bipolar deflection for pan and a fill for a unipolar parameter.
- **A `width` prop on `CanvasFader`** — 20px suits a dense docked strip but is too small for a
  detached mixer window on a large display.
- **`EqCurve.vue` / `DynamicsCurve.vue`** — the graph displays. Placeholders in Stage 2.

### 8.6 Detaching it — built

`createMixerWindow()` spawns a `BrowserWindow` on `index.html?mixerWindow=1`, following the cart
grid's pop-out, and `app.vue` branches on the flag to render `MixerPanel` alone in `mode="full"`.
`open-mixer-window` / `mixer-window-attach` mirror the cart's IPC pair, and `mixer-window-opened` /
`mixer-window-closed` drive `mixerDetached` in the main window.

What that window needs from the main process turned out to be less than the cart, but not nothing.
Buses, meters and fader moves all go over its own `useLiveplayServer` socket — every window opens
one, and the Nuxt plugin points it at the right URL before the app mounts. But meter zone colours
come from `settings.outputTargetLevels` and the look from `theme`, both of which live in the
project, so the window subscribes to the cart's existing project-data broadcast and applies it. The
sync watchers in `useProject` are skipped in *any* secondary window (`cartWindow` or `mixerWindow`);
without that the window would diff its IPC copy against itself and push phantom edits.

Consequence worth remembering: everything the detached mixer draws that is *not* bus state degrades
to defaults when the main window has no project open. That is the right failure — the faders still
work, because they never depended on the document.

A detached window is also where larger faders matter most — see the `width` prop above; still 20px.

### 8.7 Localisation

Add `mixer.*` and `mixerChannel.*` namespaces to
[`client/locales/en.json`](client/locales/en.json). Keys must exist there or `t()` returns the raw
key; the other 20 locales fall back to English, so they can follow later without blocking the UI.

