# LivePlay — Object Ownership Model

> **Status:** Adopted. This is the normative description of where a parameter lives and why.
>
> **Provenance:** written in July 2026 as a proposal against a pre-bus tree, as an attachment to
> [Discussion #54](https://github.com/tdoukinitsas/liveplay/discussions/54) and a companion to
> [#46](https://github.com/tdoukinitsas/liveplay/issues/46). Adopted into the repository in
> September 2026 with §5 rewritten against the tree as it actually is, §6 and §7 reconciled against
> what shipped, and §8's answered questions answered. **§1–§4 are unchanged** — the model itself
> needed no correction, which is the most useful thing this document can report about itself.
>
> **Audience:** a developer or agent with no prior context. Read §1–§4 for the model, §5 for where
> every parameter lands, §5.7 before adding a project setting, §6–§8 for users and auth.

> **A note on citations.** The original cited `file.cpp:1234` throughout. Nearly every one of those
> line numbers was wrong within two months. This version cites **files and symbol names**, which
> survive the edits line numbers do not. Where a line number appears it was checked against the tree
> at the time of adoption and is marked as such.

---

## 1. Why this document exists

LivePlay grew organically, and a parameter's owner used to be decided by whichever layer happened to
need it first:

- Engine behaviour that integrators need to tune (sample rate, render block, discovery ports,
  limiter defaults, backup cadence) was a **compile-time constant** — changing it meant a rebuild.
- Presentation preferences (theme, locale, Show Mode) lived in **browser `localStorage`**, bound to
  a machine, lost on reinstall, and impossible to manage centrally.
- Some show-critical values existed in **two places at once**, with an implicit inheritance rule
  written down nowhere.
- The server had **no notion of who was connected**, so nothing could be attributed, permissioned or
  personalised — and the REST API was consequently wide open.

Issue #46 proposed a server config file and correctly fenced off "project" and "client" parameters
as out of scope. That fence is the right instinct, but it only works if the fence lines are written
down. This document defines those lines **once**, so that every future parameter has an obvious home
and we stop re-litigating it per feature.

The design goal is two-sided:

> **Invisible to the operator who just wants to play cues. Sufficient for the integrator deploying
> LivePlay into a rack in a building they will never revisit.**

Most of the past tense above is earned: §7 records which of the fourteen migration steps have
shipped. The parts that have not are named there too, rather than left to be discovered.

---

## 2. The model: three authorities and one store

#46's discussion converged on a 4-tier model — Server, Client, User, Project. This document refines
it to **Server, User and Project as authorities, plus a Machine store that is deliberately not an
authority.**

"Client" was doing two unrelated jobs:

1. Holding *personal preferences* (theme, locale, hotkeys, "don't ask me again"). These are
   properties of a **person**, and once a user concept exists the person should own them. Keeping
   them on the client means the same operator gets a different app on the booth machine than on
   their laptop, for no reason.
2. Holding *machine facts* (window geometry, which server this install last connected to, the local
   MIDI port, local import folders). These are properties of **one physical installation** and are
   meaningless anywhere else.

Only (1) belongs in the ownership chain. (2) is not something any other layer inherits from,
overrides or resolves against — nothing in LivePlay ever asks "what is the effective window width,
considering the server default and the project override". So it is a **store**, not a tier.

### 2.1 The three authorities

| Tier | Owns | Scope | Lives in | Written by |
|---|---|---|---|---|
| **Server** | How the engine process runs, binds and protects itself; policy ceilings | One server process; all projects, all users | `liveplay.json` beside the executable, plus `outputs.json` and `users.json` | An administrator, via config file or the Settings page |
| **User** | Who a person is, what they may do, and how the app presents itself to them | One person, across every machine they sign in on | `prefs/<id>.json` in the server's data directory | The user themselves; an admin for role and password |
| **Project** | The show: content, structure, and every value that changes what the audience hears | One `.liveplay` document | The `.liveplay` document | Any signed-in user |

### 2.2 The one store

| Store | Holds | Scope | Lives in |
|---|---|---|---|
| **Machine / Installation** | Facts about one physical install that cannot travel: window bounds, pane layout, last-connected server, local MIDI port, local folder paths | One OS user on one machine | Electron `userData`, and the browser's `localStorage` for the web client |

**The defining test for the Machine store:** *if this value were silently deleted, could the app
regenerate or re-ask for it without the user losing any work or intent?* If yes, it is Machine
state. Window bounds pass. A hotkey map does not.

> **Naming note.** Calling this "3+1" rather than "4-tier" is not pedantry — it encodes a
> constraint. Because Machine is not an authority, **no show-affecting value may ever live there**,
> and no resolution logic may ever read it. That one rule prevents the most likely failure mode of
> the 4-tier model: a show that behaves differently depending on which laptop is driving it.

### 2.3 Where "Client" went

Nothing is lost. Every former client-owned parameter maps to exactly one of:

- **→ User** — theme, locale, meter units, hotkeys, confirmation-dialog suppressions.
- **→ Machine** — window bounds, pane layout, Show Mode, last server address, local MIDI device
  name, local file dialog paths.
- **→ Server** — anything naming the *engine's* hardware, and **all control I/O**. This one
  surprises people, so it is worth stating as a principle: the process that opens the sound card is
  the process that owns what the sound card is.

Two of those placements were refined by building them, and the refinements are worth recording
because the original text would now be wrong:

- **Show Mode is Machine, not User.** The original filed it under User. It is per-device: the same
  project is open on an editing laptop and a touch tablet at once, and each should remember its own
  answer. `useUiMode` holds it in `localStorage`. The *broadcast* — "put the whole rig into Show
  Mode now" — is a command and live session state (§5.5), not a stored preference.
- **Pane layout is Machine, not User.** Likewise per-device: the widths that fit a console display
  are wrong on a laptop carried to rehearsal. `useWorkspaceLayout` and `useMixerView` hold them.
  This is the same reasoning as Show Mode, and both follow from the §2.2 test — a deleted pane width
  costs nobody any intent.

---

## 3. Resolution: the override chain

Tiers are not silos — most show-affecting values resolve down a chain, highest precedence last:

```
built-in default  →  server config  →  user preference  →  project  →  item / cue
      (code)          (liveplay.json)    (prefs/<id>.json)  (.liveplay)  (.liveplay)
```

Four rules govern it. R1–R3 are from the original; **R4 was discovered while writing §5.2.4 and is
promoted here**, because it decides cases R1–R3 get wrong.

**R1 — Exactly one writer per value.** A parameter has one owning tier. Other tiers may supply a
*default for* it or an *override of* it, but the direction is declared once, in §5, and never
inferred.

**R2 — The User tier may never affect audio output.** User preferences are presentation, input
mapping and workflow only. If who is signed in can change what the audience hears, the show is not
reproducible. This resolves most apparent ambiguity by itself: "should fade defaults be a user
preference?" — no, because a different operator would produce a different show.

**R3 — Server policy can be locked.** A server value may be published as a *default* (projects may
override) or a *policy* (projects may not). A broadcast facility must be able to pin a ceiling no
incoming show file can exceed. Mechanically one boolean per key, and the API must report *why* a
value is immutable so the client can explain rather than silently fail. Shipped as
`--lock-server-config`, which refuses even an administrator.

**R4 — Values that form shared vocabulary between the people running a show stay Project-owned, even
when they are display-only.** R2 alone would have put cue numbering in the User tier; two people
seeing different cue numbers is a communications failure, not a preference.

> **R2 has exactly one admitted exception, and it is documented rather than hidden.** `meterMode` is
> a User value that reaches the audio thread, because the server gates true-peak and loudness DSP on
> the **union** of what every connected session asks for (`ProjectState::set_user_meter_modes`). It
> was admitted because the union can only ever reach a state that one project setting could already
> reach alone — it spends CPU, it does not change the programme. Any *second* exception should be
> refused: this one is load-bearing precisely because it is the only one.

### 3.1 Sparse vs materialised storage

The tiers deliberately store values differently:

- **Server and User store only deltas from the built-in default.** A key absent from
  `liveplay.json` means "whatever this version's default is", so improved defaults ship for free on
  upgrade and the files stay readable. The same rule gives user profiles a third state beyond
  on and off: **absent means "never chose"**, which is how a theme mode of `"system"` stays
  distinguishable from having expressed no opinion at all.
- **Project stores show-affecting values materialised (explicit), even when they equal the default.**
  A show must sound the same after an upgrade. If a project relied on an implicit default and we
  improve that default, the show changes — exactly what a show file must never do.

This asymmetry is the single most important storage decision in this document.

> **Sparseness survived its hardest test.** S3 stated that the server would *never write*
> `liveplay.json`, precisely to protect this. P3a then had to give it a writer for the Server pane.
> The reconciliation: the writer only ever touches keys a request **names**, so editing one key
> leaves the other twelve absent and still taking improved defaults. `null` clears a key, which is
> not a detail — without it a value could be changed but never un-chosen, and the file would fill up
> one edit at a time until it had frozen everything after all.

### 3.2 Portability test

For any parameter, ask: **"I email this `.liveplay` to another venue. Must this value come with
it?"**

- Must come with it → **Project** (fades, routing intent, cue structure, cart bindings).
- Must *not* come with it → **Server or Machine** (ports, device names, absolute paths, log paths).
- Should follow the *person*, not the file → **User** (theme, locale, hotkeys).

The original noted that the model had at least one failure of this test: absolute audio paths and
hardware device names inside the document. **Both are now fixed** — see §5.3.2 and §5.3.3.

---

## 4. Placement decision tree

For any new parameter, in order — first match wins:

1. **Does changing it require re-initialising the audio engine, or does it define the process's
   identity on the host (ports, binds, paths, logs, crash policy)?** → **Server**
2. **Does it name a piece of hardware, or a window, on one specific machine, and would it be
   meaningless elsewhere?** → **Machine**
3. **Would two people operating the same show simultaneously reasonably want different values, with
   no effect on what the audience hears?** → **User**
4. **Would the same person want different values for two different shows, or does the value change
   what the audience hears?** → **Project**
5. **Is it a per-cue variation of a project value?** → **Item**, with the project value as its default
6. **Still ambiguous?** → **Project**, with a Server locked-or-default entry. Project is the safe
   default because it is reproducible and portable; a wrong guess there is recoverable, whereas a
   show-affecting value stranded in User or Machine is not.

Then check R4: if the value is display-only but two people would need to agree on it out loud, it is
Project regardless of what step 3 said.

---

## 5. Parameter registry

This section places every parameter that exists today. It is the normative part of the document: if
a parameter is not listed here, §4 decides, and the result gets added here.

**Unit references** (`O1`, `S3`, `U4`, `P3d`, `M1`, …) identify the shipped change that put a value
where it is. They are the stack's own names for its units and appear in the commit history.

### 5.1 Server tier

The original opened this section by observing that `EngineConfig` was unreachable from outside the
process and that there was no security boundary at all. Both have been addressed: **S3** gave the
server a boot configuration file with the precedence chain
`default < liveplay.json < env < CLI`, and **S1/S2/U1/U3** gave it a filesystem allow-list, a
configurable CORS origin, an origin check on the WebSocket, and authentication.

#### 5.1.1 Process and network identity — Server-owned, no override

`port` and `bind` are S3 config keys, reachable through `liveplay.json`, the environment or the
command line per its precedence chain. `pidfile` and `start_delay_ms` stay **command-line only**,
deliberately: Electron passes a `pidfile` path under its own `userData`, which is a runtime path the
client chooses and cannot be expressed in a file that sits beside the executable.

**The discovery beacon's port, interval, multicast group, TTL and interface refresh are still
constants**, set in `DiscoveryConfig` in code rather than exposed as configuration. Nothing has
asked for them, and the original listed them as *Server-owned*, which they are — being Server-owned
and being a knob are different claims, and only the first is a requirement of this model.

**`bind` is load-bearing for security, not merely operational** — it is the one Server key where a
careless change widens the attack surface silently. Called out in the config schema for that reason.

As adopted, `liveplay.json` has **13 keys**: `port`, `bind`, `meterHz`, `maxUploadMb`,
`mixSampleRate`, `renderBlock`, `ringBlocks`, `masterChannels`, `maxBuses`, `masterCeilingDb`,
`fsRoots`, `corsOrigin`, `verbose`. Each carries its equivalent flag, its range, and whether it
applies at restart, which is what lets the Server pane report provenance per field.

#### 5.1.2 Engine — Server-owned (requires engine re-init), no override

`mix_sample_rate`, `render_block`, `master_channels`, `ring_headroom_blocks`, and the limiter's
lookahead and release.

Not knobs — architectural invariants or spec tables, listed so nobody mistakes them for
configuration: `kMixerLanes` = 2, the true-peak oversampler taps, the `kMinSignal` denormal guard,
the `kRampMs` de-click slew, and the LTC frame-rate tables.

`mix_sample_rate`, `render_block`, `master_channels` and `ring_headroom_blocks` are S3 config keys
now rather than the unwired struct fields the original found; the limiter's lookahead and release
remain constants.

The original recorded two defects here. **Both are resolved**, and the second is why the first
could be:

1. The split default — `kDefaultMasterChannels` at 64 against `EngineConfig::master_channels` at 32,
   which made the "fall back to 64 if zero" branch unreachable dead code — was unified under **O5**.
   Two competing defaults for one value is exactly the ambiguity R1 forbids.
2. The preview channel pair was `30/31`, correct only because the bus happened to be 32 wide, while
   device-override allocation *did* recompute its reserved base from the live bus width — so the two
   would disagree the moment `master_channels` became configurable. **This was the stated blocker
   for making it configurable at all.** `audio::preview_master_base(bus_width)` is now the single
   derivation, and `masterChannels` is a config key because of it.

#### 5.1.3 Server policy — locked-or-default (per R3)

R3 has two distinct mechanisms, and they are easy to conflate:

- **`--lock-server-config` refuses *every* write to `liveplay.json` through the API**, even from an
  administrator, and is itself not settable through the API. Off by default. Editing `fsRoots` from a
  UI turns a shell requirement into an admin-token one, and an admin who widens it to the drive root
  can read any file on the machine through the filesystem API — so the flag exists to take that off
  the table entirely for installations that want it off.
- **Two keys are marked `policy` in the schema** — `fsRoots` and `corsOrigin` — which is what the
  Settings page reads to label them as policy rather than preference. They are the two where the
  distinction earns its keep.

| Key | Default | Project may override? |
|---|---|---|
| `masterCeilingDb` | −0.3 | Yes — a project's output target sets its own; this is the boot default |
| `maxUploadMb` | 256 | No |
| `corsOrigin` | `*` | No — **policy** |
| `fsRoots` | *(empty — unrestricted)* | No — **policy** |

Still constants rather than configuration, listed so the gap is visible rather than assumed closed:
the accepted audio extensions, the download-token TTL, and the master and output gain clamps.

**The two security defaults are deliberately permissive**, and that is the same posture the rest of
this document takes: the knob appears, the behaviour does not change until somebody sets it, and the
server states which posture it is in at boot rather than leaving it to be discovered. A point
release that started refusing connections would break deployments mid-show.

`fs_browse_roots` has one non-obvious rule worth keeping: **`--fs-root` replaces the tier below it
rather than appending to it.** It used to extend the environment's list, which meant a flag could
only ever widen a jail — the opposite of what a flag on a lockdown key should be able to do.

#### 5.1.4 Reliability and logging — Server-owned, no override

Crash policy (`max_consecutive`, `healthy_sec`, `restart_delay_ms`, `resume_timeout_sec`,
`max_logs_kept`, `max_stack_frames`) and logging (`log.file`, `max_bytes`, `ring_capacity`,
`verbose`, plus the standard `NO_COLOR` / `FORCE_COLOR` convention).

`crash.restart_delay_ms` was hardcoded in three places. **O5 gave it one owner**, along with the
cart prime duration and the seamless-segue lead. The original called this "a small thing, but the
shape of problem this document is meant to prevent: a value with no owner ends up copied" — which is
exactly why it became a unit of its own.

> **O5 deliberately did not expose these as configuration.** S3 could have made any of them a config
> key; none was asked for, and a restart delay an operator can shorten is a way to break restart.
> One owner is not the same as one knob.

#### 5.1.5 Control I/O surfaces — Server-owned

All northbound (control *into* LivePlay) and southbound (control *out of* LivePlay) I/O is
server-owned per §2.3. **This is the largest part of the model that has not been built.**

| Surface | Direction | Owner | Status |
|---|---|---|---|
| LTC output | South | **Server** (which output), Project (does this show emit LTC) | **Shipped.** O3 made it a logical output name |
| MIDI input | North | **Server** | **Not moved.** Still client-side Web MIDI (`useMidiController.ts`) |
| MIDI output / feedback | South | **Server** | Does not exist |
| OSC in/out | Both | **Server** | Does not exist |
| HTTP / WS automation | North | **Server** | Exists, and is now authenticated (U3) |
| GPIO / tally / relays | Both | **Server** | Does not exist |

Per surface the server owns whether it is enabled, which physical port or device it binds, its
protocol parameters, and its bindings to *app actions*. What it does **not** own is bindings that
reference project content — see §5.2.3.

Two consequences to design for:

- **A surface exercises permissions, so it needs an identity — but what kind depends on its
  transport.** A MIDI controller screwed into a rack has no password and cannot have one; its trust
  comes from physical access. Anything arriving over IP is a different case and must not inherit the
  wire's exemption. See §6.2.
- **Surface config is installation config, not user config.** Two operators sharing a booth share
  the one controller. Its bindings are a property of the room.

> **What P2 actually shipped, and what it did not.** The Control Surfaces pane moved MIDI
> *configuration* into Settings and fixed a real bug on the way — `useMidiController.mount()` was
> called only from `CartPlayer.vue`, so every MIDI binding died whenever the cart pane was closed.
> But the MIDI *transport* is still Web MIDI in the renderer. Moving it server-side remains step 11
> of §7 and still needs a `vcpkg.json` dependency.

#### 5.1.6 Server default → Project override

The inheritance pattern this document describes, in the places that implement it:

| Value | Server default | Project override |
|---|---|---|
| Stop-All fade | 1000 ms | `settings.stopAllFadeMs` |
| Meter ballistics | preset table (`digital-ppm`, `ppm-i`, `ppm-ii`, `vu`, `instant`) | `settings.meterBallistics`, or `meterBallisticsCustom` |
| Media root | *(no server opinion)* | derived from the project's location (O4) |

#### 5.1.7 Moved from Server to Project, or to User

The original proposed six moves. Their state:

| Value | Proposed | State |
|---|---|---|
| `backup.interval_minutes` | Project, server default | Server config key (S3); not yet a project override |
| `backup.max_keep` | Project, server default | Server config key (S3); not yet a project override |
| `seamless_lead_sec` | Project | One owner (O5); not yet a project setting |
| `limiter.ceiling_db` | Project, under server ceiling | Server policy (§5.1.3) |
| `meter_broadcast_hz` | **User** (per-connection) | **Shipped — U2.** `set_meter_hz` per connection, surfaced in the Server pane by P3b |
| `log.verbose` | Server, per-session togglable | Server config key; still host-wide |

`meter_broadcast_hz` was the original's "good early test of the User tier", and it was: it is the
first parameter negotiated per connection at connect time rather than configured, and the shape it
established — per-connection state hanging off `ClientSession` — is what per-connection locale then
reused.

### 5.2 User tier

Shipped as **U1–U4**. Profiles are one JSON file per user under `prefs/` beside the executable,
created on first sign-in and never before, so an installation that leaves authentication off grows
no directory at all.

> **Profiles are deliberately NOT inside `users.json`.** That file holds password hashes and the
> token-signing secret; a theme toggle must not rewrite it. Separate files mean separate blast
> radius: a corrupt profile costs one person their colours, while a corrupt `users.json` refuses to
> boot (§6.1).

#### 5.2.1 Identity and permissions — User-owned

`id`, `name`, `role`, `tokenEpoch`, `createdAt`, and the Argon2id password hash — which no accessor
exposes and which never travels in any API response. `tokenEpoch` is what makes a stateless token
revocable: it is stamped into each token and compared on every verification, so a password change or
a deletion invalidates everything issued to that person at once.

The original proposed a `permissions{}` map overriding role defaults. **Not built**, and the reason
is §6.2: the role model collapsed from four roles to two, which removes most of what per-user grants
were for.

#### 5.2.2 Presentation preferences — User-owned, no project override

| Value | Where it lives now |
|---|---|
| Theme mode + accent colour | **User profile** (U4). `mode` may be `light`, `dark` or `system` |
| Meter display unit (`meterMode`) | **User profile** (U4) — the admitted R2 exception, see §3 |
| `uiScrollToPlaying` | **User profile** (U4) |
| `playbackKeys` | **User profile** (U4); see §5.2.3 |
| Meter push rate | **Per connection** (U2), not stored |
| Locale | **Machine** (`localStorage`) plus a per-connection override (U2). The profile has a slot for it that the client does not read yet |
| Show Mode | **Machine** (`localStorage`) — see the correction in §2.3 |
| Confirmation-dialog suppressions | Still no persistence |
| Recent projects | Machine file of server filesystem paths |

**Locale was the clearest single argument for the User tier**, and it is the one value that ended up
only half-migrated. It used to be *server-global* — one `ui_locale_` shared by every connected
client, so two operators who did not share a language could not both read the UI. U2 replaced that
with a three-level chain: built-in `"en"` < the installation default (`POST /api/ui/locale`) < a
connection's own choice, with the default's fan-out skipping sessions that have chosen for
themselves. What remains is making the *profile* the source, so your language follows you to the
tablet. The storage is already there and validated.

> **One client-side rule worth stating, because it looks inconsistent and is not.** The snapshot's
> locale is adopted only when the client has no stored preference, or a reconnect would undo the
> operator's language. Selection and Show Mode take the **opposite** rule on purpose: they are the
> *show's* state, and a joiner must adopt them.

#### 5.2.3 Input mapping — two axes, not one

This is the subtlest area, and single-axis answers are wrong. Two independent questions decide the
owner:

1. **Where does the input physically arrive?** In a focused client window (keyboard) → client-side,
   so it can be user-scoped. At the engine (MIDI, OSC, GPIO) → server-side per §2.3, so it *cannot*
   be user-scoped, because a rack-mounted controller belongs to no one.
2. **What does the binding reference?** An app action that exists in every project (go, stop-all,
   pause) → the tier that owns the surface. *This project's* cart slot 7 → **Project**, always,
   because the binding is meaningless without this project's content.

| Binding | Owner | Why |
|---|---|---|
| `playbackKeys` (keyboard → transport) | **User** | Keyboard arrives at the client; points at app actions; muscle memory follows the person |
| `cartSlotKeys` (keyboard → cart slot) | **Project** | References this project's slots |
| MIDI/OSC → transport actions | **Server** | The surface is a fixture of the room |
| MIDI/OSC → cart slots | **Project** | References this project's slots |
| MIDI/OSC port, channel, device | **Server** | The engine binds it (§5.1.5) |
| MIDI `masterVolumeMultiplier` | **Server** | A property of that controller's fader resolution, a fixture too |

The answer for keyboard and MIDI is deliberately *different*. The asymmetry is not an inconsistency
— it falls straight out of where the electrons land.

> **One proposal here was rejected during U4, and the rejection matters.** The plan had the project's
> `playbackKeys` acting as an override that wins over the user's. That would mean opening a
> colleague's show silently replaced your transport keys — the exact hazard the User tier exists to
> remove. **The user's map wins outright**; a document's copy is a migration seed and nothing more.

**MIDI Learn should become a server-side flow** when §5.1.5 is built: "arm learn → the next inbound
message on surface X binds to action Y" is a small protocol addition and works from any client,
including a tablet with no MIDI hardware of its own. Today it still captures in the browser.

#### 5.2.4 Claims rejected for the User tier

Several settings look like personal preferences and should not be. Recorded so the argument is not
reopened:

| Value | Not User because | Correct owner |
|---|---|---|
| `defaultTransitionMode` | Changes what the audience hears (R2) | Project |
| `disableAutoVolumeAndTrim` | Changes gain and trim applied on import | Project |
| `stopAllFadeMs`, fade defaults | Audio-affecting | Project |
| `meterBallistics` | Computed once server-side and broadcast to all clients; per-user ballistics is an architecture change, not a re-filing (§8) | Project |
| `indexDisplayStart` | Display-only, but cue numbers are shared vocabulary — **R4** | Project |
| `disableSilenceWarning` | "This show has intentional silence" is a fact about the show, not a nag preference | Project |
| `autoSave` | Governs writes to a file everyone shares | Project |

> **Ballistics versus unit is the distinction to keep hold of**, because they look like the same kind
> of thing and are not. The meter *unit* is how one person reads a number, so it is theirs. The
> *ballistics* change the number everybody sees, so they are the show's. The two sitting side by side
> on one pane was the confusing part; the Appearance pane took the unit and left the ballistics on
> Audio deliberately.

### 5.3 Project tier

#### 5.3.1 Correctly Project-owned

Show data proper: items and their gain, trim, fades, segue markers, end behaviours, ducking, custom
actions and LTC flag; groups and their children and behaviours; cart bindings; the signal-flow
graph; `outputTarget` and `outputTargetLevels`; `disableLimiter`; and the bus graph — names,
colours, routing, sends, DSP and role assignments.

`GroupItem.isExpanded` deserves a note: it is UI state persisted in the document and broadcast to
every client, so all operators' views start identically. Under R2 that looks like a leak. It is
deliberate and should stay — expanded and collapsed state *is* how a cue list is authored, and it is
R4's kind of value.

#### 5.3.2 The device-name leak — **fixed**

The original called this "the most serious ownership violation in the codebase": `defaultOutputDevice`,
`previewDevice`, `ltcDevice`, per-item `deviceOverride` and master→device assignments all stored
**hardware device name strings inside the portable show document**.

The fix landed as **O2, O2b and O3**, in the shape the original proposed:

- The project names outputs **semantically**; buses and items reference those logical names.
- The **machine** owns `outputs.json`, mapping logical name → physical device and channel offset.
- Picking a device from a strip **materialises** a logical output of that name and points the bus at
  it (O2) — the same two clicks for the operator, and it happens server-side in `patch_bus` so
  Companion and `curl` get it too.
- `ltcDevice` became `ltcOutput`, a logical name resolved the same way (O3), which took the last raw
  device name out of a portable document.

Two hardening passes were needed after the first, and both are worth knowing about:

- **O2b withdrew the identity fallback.** An output naming absent hardware now goes **silent** and
  says so, rather than falling back to the default device. Silence is recoverable; a sub-mix in the
  house is not.
- **A later fix closed the other half.** O2b covered *unmapped* outputs, but a **mapped** channel
  naming a device that is not present was still handed to `open_device_by_name()`, which falls back
  to the default device — so a stale `outputs.json` played a sub-mix into the house *while reporting
  `bound: true`*. Mappings are now filtered to devices actually present, and
  `audio::normalise_device_name` lets a renumbered interface (`2-` → `3-`) still match, guarded so
  that an **ambiguous** match resolves to nothing rather than to the wrong card.

> **`bound` is a claim about audibility and has to stay true.** When M1 let a bus leave by its sends
> alone, `bound` had to become a search over outputs **and** sends — a bus with no output but a
> post-fader send into the house is perfectly audible, and reporting it unbound would put "this bus
> is silent" on a bus the room can hear. That is how operators learn to ignore a warning, and take
> the true ones with it.

#### 5.3.3 Absolute host paths in the document — **fixed**

`folderPath` was an absolute host path persisted in the document. **O4 removed it**: it is derived
from where the file was loaded, every time. A path stored inside a file, describing where that file
is, can only ever go stale.

`mediaServerPath` (an absolute import-time path per item) is still mitigated rather than removed:
`relativize_media_paths()` rewrites absolute to relative at save time and `resolve_media_path()`
prefers the relative form.

> **Known defect, recorded not fixed:** `relativize_media_paths()` does not check that the file
> exists before rewriting an absolute path into a relative one, so it can replace a working
> reference with one that resolves nowhere — while its own doc comment says it acts only "when the
> file actually lives inside that folder".

#### 5.3.4 Schema note

Two schemas coexist and are easy to conflate. `document_` is the persisted, client-shaped document
and is what `save()` writes. `to_json()` is a separate snake_case engine-facing shape used as the
legacy upgrade target. `full_document()` also glues a synthetic `server{ … }` block onto responses.

That last one is a pattern to keep: **server facts are attached to the response, not stored in the
document.** The device-name fix in §5.3.2 is the same move applied to routing, and buses being
absent from the client's document snapshot is the same move again — the server holds them, so a
client that never sends them cannot clobber them.

Any tier gaining a config file needs its own `schema_version` and migration path, and the existing
version fields must not be conflated in that work.

### 5.4 Machine store

Not an authority. Nothing resolves against it.

| Value | Where |
|---|---|
| Server connection mode / `remoteUrl` / `localPort` | `userData/liveplay-server.json` |
| Web fallback server URL, auth token | `localStorage` |
| Recent servers, recent projects | `userData/liveplay-recent-*.json` |
| Server lock (pid/port), firewall marker | `userData` |
| **Window bounds and maximized state, all four windows** | `userData/liveplay-window-bounds.json` (**P4**) |
| **Pane layout** — cart/mixer widths, properties height, which panes are showing | `localStorage['liveplay-workspace-layout']` (**P4**) |
| **Mixer docked mode** (side / full) | `localStorage['liveplay-mixer-mode']` |
| **Show Mode** | `localStorage['liveplay-ui-mode']` |
| Locale | `localStorage['liveplay-locale']` (§5.2.2) |
| The output map (logical → physical) | `outputs.json` beside the server — the machine's, but server-side |

> **Two things P4 deliberately does not persist**, both for the same reason. Which windows were
> *open* is not restored, only their geometry; and the detached flags for the mixer and cart are not
> stored at all. A flag saying "detached" restored without the window it names would hide the pane
> *and* hide the header button that brings it back. Recreating the window instead would put one on
> screen because of a choice made days ago, and would need the renderer to learn about a window it
> never asked for.

`midi-config.json` is absent from this table deliberately: all of it — bindings, `preferredDevice`,
`masterVolumeMultiplier` — moves to the server under §5.1.5, and the file becomes a migration source
only (§7, step 11).

### 5.5 Session state — deliberately not a tier

One category resists all four tiers, and the code gets it right, so it is documented rather than
"fixed". Show Mode's broadcast and the shared selection are held by the server, broadcast to every
client, and deliberately **not** persisted into the document.

Shared selection is not a personal preference: it exists so that Companion, a booth screen and a
stage-side tablet all agree on which cue is next. That is **live session state** — ephemeral,
server-held, shared, owned by no config tier. The only correction this document made was to locale,
which was mis-filed as session state when it belongs to the person.

The connection list (`GET /api/clients`) is the same category made visible: one row per live
WebSocket, monotonic ids that are never reused, and `user`/`userId` reported as `null` rather than
as a name like "anonymous" on an installation with no accounts — so the open posture cannot be
confused with somebody called that.

### 5.6 Where each setting is edited

Every value above is reachable from one page. The Settings page has eleven panes, and the rail's
order is deliberate rather than alphabetical:

| Pane | Tier | Holds |
|---|---|---|
| Appearance | **User** | Language, theme mode, accent, meter unit, scroll-to-playing |
| Playback | Project | Transition mode, auto-cue, Stop-All fade and scope |
| Audio & Metering | Project | Output target, limiter, ballistics, timecode output |
| Mixer | **Machine** | Which view the mixer opens in |
| Hardware outputs | **Machine** (server-side) | The output map, and the venue-changeover remap tool |
| Keyboard | **User** / Project | Transport keys (user), cart slot keys (project) |
| Control Surfaces | Server *(client-side today)* | MIDI bindings and devices |
| Project | Project | Autosave, cue numbering, silence warning |
| Server | **Server** | Port, bind, policy, meter rate — with each field's provenance |
| Accounts | **Server** | Users, roles, and whether a login is required at all |
| About | — | Version and licence |

Two of those are worth their own note:

- **The Server pane keeps two owners apart on purpose.** *Which server this app talks to* stays on
  Electron's own IPC — not laziness about using the new API, but because it has to work while
  disconnected (it is how you fix being disconnected) and it writes to `userData`, which is writable
  where the directory beside the server binary may not be. *What that server does* goes through the
  config API. A field the environment or a flag is supplying renders **read-only and says which tier
  is winning**, because offering an edit that cannot take effect until somebody removes a flag they
  cannot see from this screen is worse than not offering it.
- **The Mixer pane is the only Machine-tier pane on the client side**, and it is not on Appearance
  for exactly that reason: everything on Appearance follows the operator between desks, and a mixer
  view must not.

### 5.7 Adding a project setting — the registry

> **Read this before adding a key to `settings`.** The registry is the mechanism O1 introduced to
> replace an open bag that accepted any key of any type, and it is the reason a settings key cannot
> arrive by accident.

`settings_registry()` in `server/src/core/project_state.cpp` (line 435 at adoption) is a
`key → SettingSpec` map. **Adding a setting is one line in it.** A key that is not in the map is not
a setting, and `patch_settings()` will not store it.

**The eight kinds**, in `SettingKind`:

| Kind | Accepts | Notes |
|---|---|---|
| `Bool` | `true` / `false` | |
| `Enum` | a string from `allowed` | Anything else drops |
| `Number` | a double | **Clamped** to `[min, max]`, not rejected |
| `Integer` | a whole number | Clamped |
| `StringOrNull` | a free string, or `null` to clear | |
| `Ballistics` | the `meterBallisticsCustom` object | |
| `Derived` | **nothing** | Computed server-side and injected into every read; accepting it is what used to persist a stale copy |
| `Relocated` | **nothing** | Moved to a user profile (U4). Read on load, never stored |

**Numbers clamp rather than drop**, matching the convention every other client-supplied number in
that file follows. A typo'd value becomes a legal one instead of vanishing silently.

**Unknown keys are DROPPED, not rejected with a 400**, and this is deliberate rather than lax. The
client watches its whole settings object and PATCHes *all of it* back whenever any part changes, and
the object it holds is the one `full_document()` decorated with the derived `outputTargetLevels`.
Answering a stray key with 400 would therefore fail **every settings edit in the app**. Dropping
buys the guarantee that actually matters — the document only ever contains registered keys — without
breaking that round trip.

**`Relocated` is the kind worth understanding**, because it is the one that exists for people rather
than for data. `meterMode` and `uiScrollToPlaying` moved to user profiles in U4. They could have been
deleted from the registry; instead they are registered as `Relocated` so that the drop is *explained*
rather than reading as "not a known setting". A 2.4 document still carries them and a 2.4 client
still patches them, and both should be handled rather than merely tolerated.

The same reasoning keeps `defaultOutputDevice`, `previewDevice` and `ltcDevice` registered as
legacy: all three migrate at load and are erased from the document there, and `patch_settings()`
rewrites `ltcDevice` to `ltcOutput` on the way in, so the live key keeps **exactly one writer** (R1).

As adopted, the registry holds **19 keys**: five audio and metering, six playback, two UI, three
legacy, two relocated, one derived.

> **The registry got the only real test it could have.** A later contributor added a new project
> setting — `stopAllStopsPreview` — without being told the registry existed, and it went in *through*
> the registry rather than round the side. That is O1 working as designed.

---

## 6. The users model

Shipped as **U1–U4**, with account management in **P3c**. The original's framing is preserved below
where it still holds, and corrected where building it changed the answer.

### 6.1 It was also a security fix

The server had no authentication and no filesystem confinement, and the two compounded: any client
that could reach the port could enumerate every drive on the host, read any absolute path, copy any
file into a downloadable media root, extract an archive anywhere, and drive transport over an
unconditionally accepted WebSocket. CORS was a hardcoded `*`, so a browser on the same LAN could do
all of it cross-origin.

The original's sequencing recommendation — **ship the filesystem allow-list before the user model**
— was followed, and was right: it is a fraction of the work and it helps even with no users.

Three findings from building it that the original could not have predicted:

- **CORS does not cover WebSockets.** `Access-Control-Allow-Origin` is a rule browsers apply to XHR
  and fetch; the WebSocket handshake is exempt. So from S2 until U1, an installation started with a
  configured origin had its REST surface restricted and its socket **wide open** — and the socket is
  not the lesser surface, since it carries play, stop, bus gain, mute and selection. `.onaccept` now
  refuses a mismatched `Origin` with 403. Two deliberate exemptions: the default `*` admits
  everything, and an upgrade carrying *no* `Origin` is admitted, because native clients, Companion,
  `curl` and Electron send none — and a browser cannot suppress its own, which is exactly why the
  check works.
- **A Crow middleware cannot refuse a WebSocket upgrade.** Middleware *is* run for an upgrade, but
  the response is ignored — `http_connection.h` calls the middlewares and then `handle_upgrade`
  unconditionally. So authentication is enforced in **two** places: `AuthGuard` for REST, `.onaccept`
  for `/ws`. `/ws` is listed as Public in the access table *deliberately*, not by omission.
- **The `onclose` erase guards a use-after-free**, not tidiness. Leave a dead `connection*` in the
  map and the broadcast thread writes to freed memory on the next tick; because that is not a C++
  exception, the `catch(...)` around `send_text` does not save it.

**A corrupt `users.json` stops the server booting.** Falling back to "no users" would turn a damaged
file into an open door, which is the one failure mode a store like this must not have.

### 6.2 Roles — two, not four

The original proposed Admin, Operator, Editor and Viewer. **The shipped model has two**, and the
split is the ownership model's own tier boundary rather than an access scheme invented beside it:

| Role | Owns |
|---|---|
| **Operator** | The Project and User tiers — cues, buses, transport, project settings, media, their own preferences |
| **Admin** | The above, plus the **Server tier**, which is exactly three paths: `/api/outputs`, `/api/users`, `/api/clients` |

So "does this need an admin?" has an answer that can be **derived rather than remembered**: it needs
an admin if it changes state belonging to the machine rather than to the show or to the person at
it.

Enforcement is server-side, through a Crow middleware rather than per-handler guards, because there
are around ninety routes and **`access_for()` defaults to deny** — so a route added next year is
protected without anyone remembering, and a path matching no route at all needs a token too, which
stops an anonymous caller mapping the route table by reading which 404s come back.

Two judgment calls are recorded in `access_for()`'s own comment so they read as decisions rather than
gaps: `POST /api/ui/locale` stays operator-level (it is shared presentation state and cannot silence
anything) even though it is installation-scoped; and `PATCH /api/buses` can write `outputs.json`
through O2's materialisation, which is the show's routing rather than a way around the admin gate on
editing the venue's named outputs.

#### Turning authentication off again

Authentication turns **on** implicitly, the moment a first account exists. Until recently there was
no way back: `auth_required()` was "the store is not empty", and the store refuses to delete the last
administrator — so the guard that stops an operator locking themselves out of account management also
made authentication permanent, and the documented recovery was deleting `users.json` by hand.

`PATCH /api/auth/required` stores an explicit choice in `users.json` and **keeps the accounts**.
What protects it is worth stating precisely, because the obvious answer is wrong:

- **Not the access table.** While authentication is off the guard short-circuits and every admin
  route is open. The route verifies **an administrator's name and password in the body itself**, in
  both directions, and *that* is the boundary.
- **Re-entry rather than the session token**, because tokens here are long-lived, signed and cross
  the LAN with no TLS — a token can be read off the wire; a password is asked for at the moment of
  the act.
- **Turning it back on needs the password too**, or anyone on an open LAN could lock a desk
  mid-show. A denial of service rather than a breach, but free to close.
- **It shares the login throttle**, or it would be an unthrottled password oracle beside a throttled
  front door.

Lockout is unrepresentable rather than merely discouraged: an empty store is open *before* the stored
choice is consulted, and turning authentication on with no accounts is refused.

#### Principals that are not people

Moving control I/O into the server (§5.1.5) means things that can fire cues will not all have
logins. The dividing line is **not** "is this principal a person" — it is the transport:

> **Anything crossing the network authenticates, northbound or southbound. Anything arriving on a
> physical wire cannot, so its trust derives from physical access instead.**

There is no "it is only the LAN" exemption. A show LAN routinely carries a lighting desk, a video
server and somebody's laptop.

| Principal | Transport | Authenticates with | Typical permissions |
|---|---|---|---|
| **User** | HTTP/WS | Password → signed token | Per role |
| **API client** (Companion, automation) | HTTP/WS over IP | Revocable long-lived token — **not built** | Scoped: transport and state read, never filesystem |
| **Physical surface** (MIDI DIN/USB, GPIO) | Wire | Nothing — no handshake exists | Fixed by an admin at configuration time |
| **OSC / UDP inbound** | IP, but unauthenticatable | Nothing meaningful | Config-gated, not authenticated |

**Inbound OSC should not pretend to authenticate.** OSC/UDP has no handshake, no session, trivially
spoofable source addresses and no encryption; a shared secret in the address pattern is obfuscation.
It should be off by default, enabled per install, bound to a named interface, restricted by IP
allowlist and pinned to a fixed permission scope — documented as a trusted-segment feature. Anyone
needing *authenticated* network control should use the HTTP/WS API with a token.

Attribution should record the **principal**, not just a user id, so "who fired that cue" can answer
"the booth controller" as readily as "Dave".

#### Southbound credentials need a secret store

Southbound control means LivePlay holds *other systems'* credentials. None of the existing files is a
safe home:

- **Never in the project document** — it is portable by design, so emailing a show would email the
  credentials.
- **Never in logs.** The crash handler dumps a 16,384-line ring buffer, so any credential that ever
  touched a log line lands in crash reports users email around.
- **Server tier, but a separate file from `liveplay.json`.** That file is what integrators bake into
  rack images and paste into issue reports; secrets must not ride along.

### 6.3 The single-operator install must not regress

The live concern in #46 was a user model "overcomplicated for the vast majority of installs".

**The shipped answer is simpler than the original's proposal, and better.** The original proposed a
loopback exemption — a client on the same machine needs no login — with the caveat that `--bind`
defaults to `0.0.0.0`, so "trust the local address" is a weak check, and that local mode should
actually bind `127.0.0.1` to convert the exemption from a policy into a property of the socket.

What shipped instead: **no accounts means no authentication, for everybody.** An installation that
has not been locked down behaves exactly as every release before 2.5 did, and the server says which
posture it is in at boot. That needs no exemption, no `isLocal` check and no reliance on the bind
address — and it is the same posture `--fs-root`, `--cors-origin` and `liveplay.json` all take: the
knob appears, the behaviour waits to be asked for.

The bind-address observation still stands and is recorded in §5.1.1.

### 6.4 Implementation shape

The original's research was accurate and is preserved because it explains the shape of the code:

- **The REST choke point was cheap.** Routes are ~90 individual `CROW_ROUTE` lambdas, not one
  dispatch function, but Crow supports a middleware chain and `Impl::app` had none attached.
  Re-templating gave a `before_handle` hook covering every route without touching a handler body.
- **WS needed its own choke point.** `.onaccept` fires before `.onopen` and can inspect the upgrade
  request — see the correction in §6.1 about why this is a separate enforcement point rather than a
  convenience.
- **Per-connection identity needed a data-structure change.** `ws_clients` was an
  `unordered_set<connection*>` with nowhere to hang an identity; it became a map to `ClientSession`,
  which is also what unlocked per-connection locale and meter rate.
- **Persistence reused what existed.** `ProjectState::save()`'s atomic write-temp-then-rename is
  directly reusable and is what `users.json` and the profiles use.
- **Crow runs multithreaded** with a separate broadcast thread, so any store needs its own mutex.

One Crow-specific hazard found while building: **`userdata` is safe to allocate into from
`.onaccept`, but only after every refusal path**, because `websocket.h` assigns it only when the
handler leaves the response empty.

### 6.5 The two blockers

1. **No crypto dependency was vendored.** Resolved: `libsodium` is in `vcpkg.json`, and passwords
   are Argon2id at interactive limits via `crypto_pwhash_str`. Tokens are signed with `crypto_auth`
   (HMAC-SHA512-256). Nothing here rolls its own crypto.
2. **No TLS.** **Unresolved, and shipped anyway as a stated limitation.** Tokens and passwords cross
   the LAN in clear text. On a trusted show LAN this is a smaller risk than the completely open
   filesystem API it replaced — but it must be a documented limitation rather than an oversight, and
   it wants a decision before anything is exposed beyond such a LAN.

Two further limitations are recorded in `server/README.md` rather than fixed:

- **`users.json` is `0600` on POSIX only.** On Windows it inherits the directory's ACL. Doing it
  properly means a DACL, and a half-done one that looks like protection is worse than an honest note.
- **The login throttle is per-IP and in-memory** (5 failures → 30 s, table capped then cleared). It
  is a brake on one machine grinding a word list, not a boundary — a spray from many addresses walks
  past it.

---

## 7. Migration

The original's fourteen ordered steps, with what actually happened.

| # | Step | State |
|---|---|---|
| 1 | Filesystem allow-list + configurable CORS | ✅ **S1 + S2** |
| 2 | Server config file (#46) | ✅ **S3** — `liveplay.json`, `default < file < env < CLI` |
| 3 | Derive the preview channel pair from bus width | ✅ Landed with the bus work |
| 4 | De-duplicate split defaults | ✅ **O5** |
| 5 | Logical outputs — project names, server maps | ✅ **O2 + O2b + O3** (§5.3.2) |
| 6 | Drop `folderPath` from the document | ✅ **O4** |
| 7 | Users file + auth middleware + `.onaccept` | ✅ **U3** — no accounts means no auth, rather than a loopback exemption (§6.3) |
| 8 | Per-connection identity; per-connection locale and meter rate | ✅ **U1 + U2** |
| 9 | Move theme / meterMode / uiScrollToPlaying → User; migrate `playbackKeys` | ✅ **U4** |
| 10 | API tokens for northbound automation; local mode binds loopback | ❌ **Not built** |
| 11 | Server-side control I/O — MIDI in/out, MIDI Learn as a server flow | ❌ **Not built.** Needs a MIDI dependency |
| 12 | Surface principals — per-surface permission sets | ❌ **Not built** (depends on 10, 11) |
| 13 | Server admin UI | ✅ **P1–P3e** — a whole Settings page, not a modal |
| 14 | OSC and further surfaces | ❌ **Not built** (depends on 11, 12) |

Steps 1–9 and 13 are done. **What remains is control I/O and the principals that come with it** —
steps 10, 11, 12 and 14, which are one body of work with MIDI at its root.

> **Step 9's data migration had a timing hole that the test suite found, not review.** Seeding a
> profile happens on the first read of `/api/prefs`, from whatever project is open — but a client
> reads its preferences *the moment its socket comes up*, which on the ordinary startup order is
> before any project exists. The first implementation wrote an **empty** profile at that moment,
> permanently spending that person's one chance to inherit their real theme and keymap from a 2.4
> file opened a minute later. Two changes close it: an empty seed writes **nothing**, so "no profile
> yet" survives, and the client reads again once a document lands. Then the same hole reappeared by a
> different route — `load()` injected a default theme into every document, so the seed was never
> actually empty. Documents no longer gain a theme they did not have.

**Step 11's data migration, when it is built:** read `userData/midi-config.json` once and offer to
import its bindings into the server's surface config, splitting them per §5.2.3 — transport bindings
to the surface, cart-slot bindings to the open project. Then stop writing the file. A client whose
MIDI map silently vanishes is a bad upgrade for exactly the power users most likely to have one.

---

## 8. Open questions

1. **Per-user meter ballistics.** Still open. Ballistics are computed once server-side and broadcast,
   so per-user ballistics means either per-connection meter state on the server or sending raw
   peak/RMS and doing ballistics client-side. The second is cleaner and makes meter push rate a pure
   bandwidth knob, but moves DSP into the client. §5.2.4 parks ballistics in the Project tier until
   this is decided deliberately.
2. **Does the Editor role need a transport lockout?** **Moot** — the role model is two roles (§6.2).
   Reopening it means reopening the role split first.
3. **Config file format.** **Answered: JSON**, matching the existing stack and the Settings page.
4. **Hot reload.** Still open. #46 proposed boot-time only, which is right for engine parameters, but
   some Server keys are safely live-reloadable (meter rate, log verbosity, backup cadence,
   `fs_browse_roots`). The suggestion stands: mark each key `boot` or `live` in the schema rather
   than choosing one policy for all. The Server pane already reports `restartRequired` per field,
   which is most of the mechanism.
5. **Do projects get stable ids?** Still open. Required before recent-projects can be user-portable,
   and useful for attribution and for permissions scoped to a project.
6. **TLS.** **Answered: shipped without, documented loudly** (§6.5).
7. **Multi-user concurrent editing.** Still open, and now *possible* to address: everything is
   last-write-wins, but identity exists, so attribution and soft locks are buildable. A separate
   design.
8. **Which MIDI library?** Still open, and now the gating question for steps 11–14. RtMidi and
   libremidi remain the candidates.
9. **Does a keyboard-only client still need a local fallback?** Still open, and still worth
   confirming: if all control I/O is server-side, a client that loses its connection can no longer
   trigger anything locally. For a system where the server owns all audio that is probably already
   true, since a disconnected client cannot make sound either way.

---

## 9. What this document is not

It does not describe the **bus and mixer architecture** — see [`BUS_ARCHITECTURE.md`](../BUS_ARCHITECTURE.md),
whose D-numbered decisions are the normative record for routing, roles, sends and the output map.
Where the two meet — logical outputs, `bound`, the master and preview roles — this document states
the *ownership* rule and `BUS_ARCHITECTURE.md` states the *audio* rule.
