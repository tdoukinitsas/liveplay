## Context

LivePlay runs in Electron 28 (Chromium 120) which supports the Web MIDI API natively. The X-Touch Mini (and similar controllers) send standard MIDI messages: Note On/Off for buttons, Control Change (CC) for encoders and faders. The app already has a keyboard hotkey system (`useCartHotkeys.ts`) that maps key events to actions — MIDI mapping follows the same pattern but with MIDI messages instead of keyboard events.

Master volume is dB-based (-60 to 0) via `masterOutputLevel` in `useAudioEngine`. The audio engine exposes `stopAllCues()` for panic stop, `pauseCue/resumeCue` for pause/resume, and `activeCues` for tracking playing state.

## Goals / Non-Goals

**Goals:**
- Generic MIDI mapping: any MIDI input → any supported action
- MIDI Learn UX: select action, press control, binding captured
- Support both discrete actions (buttons → note on) and continuous actions (fader/encoder → CC value)
- Global persistence (not per-project) since the MIDI controller is hardware-specific
- Works with any class-compliant MIDI controller, not just X-Touch Mini

**Non-Goals:**
- MIDI output (sending LED feedback to the controller) — future enhancement
- OSC or HID protocol support
- Per-project MIDI configs
- Mapping MIDI to individual track volumes (only master volume for now)

## Decisions

### 1. Web MIDI API in renderer process

**Decision:** Use `navigator.requestMIDIAccess()` directly in the renderer. No main-process involvement for MIDI I/O.

**Rationale:** Web MIDI API is fully supported in Chromium 120. Running in the renderer avoids IPC overhead for real-time MIDI messages. Electron doesn't block MIDI access by default.

**Alternatives considered:** Node.js `midi` package in main process (requires native compilation, adds complexity); `webmidi.js` wrapper library (unnecessary abstraction over a simple API).

### 2. MIDI message identification

**Decision:** Identify a MIDI binding by `{ channel, type, note_or_cc }` where type is `'note'` or `'cc'`, and note_or_cc is the MIDI note number (0-127) or CC number (0-127). Channel is 0-15.

**Rationale:** This uniquely identifies any button or knob on any MIDI device. The X-Touch Mini uses channel 0, notes 0-23 for buttons, CC 1-8 for encoders, CC 9 for fader.

### 3. Action types

**Decision:** Two action categories:
- **Discrete** (triggered on note-on or CC value > 63): `trigger-slot-N` (0-15), `pause-resume`, `toggle-loop`, `stop-all`
- **Continuous** (maps CC 0-127 to a range): `master-volume` (CC 0→-60dB, CC 127→0dB, linear-in-dB mapping)

**Rationale:** Discrete actions fire once on press. Continuous actions update in real-time as the fader/encoder moves. The dB mapping is linear in dB space which feels natural for volume control (each step is equal perceived loudness change).

### 4. Global config persistence

**Decision:** Store MIDI mappings in `<userData>/midi-config.json` via main-process IPC (`read-midi-config` / `write-midi-config`).

**Rationale:** MIDI controller setup is hardware-specific and shouldn't change between projects. Using userData keeps it outside the project file. Main-process IPC is needed because renderer can't access the filesystem directly.

### 5. MIDI Learn capture flow

**Decision:** In the config modal, user clicks an action row → UI enters "listening" mode → next MIDI message received becomes the binding → binding saved immediately.

**Rationale:** Identical UX to the keyboard hotkey config. Simple, intuitive, standard pattern for MIDI mapping software.

### 6. Composable architecture

**Decision:** Create `useMidiController.ts` composable that:
- Requests MIDI access on mount
- Listens to all MIDI input ports for messages
- Looks up bindings in the config and dispatches actions
- Exposes `startLearn(actionId)` and `stopLearn()` for the config UI

**Rationale:** Mirrors `useCartHotkeys` pattern. Single composable owns the lifecycle, mounted once in `app.vue` or `CartPlayer.vue`.

## Risks / Trade-offs

- **[MIDI permission]** → Chromium may prompt for MIDI access permission. Mitigation: Electron typically auto-grants; if not, handle the permission request gracefully.
- **[Hot-plug]** → MIDI devices connected after app start. Mitigation: Web MIDI API fires `statechange` events on the MIDIAccess object; listen and reconnect.
- **[Multiple devices]** → User may have multiple MIDI devices. Mitigation: listen to ALL input ports; bindings include channel which disambiguates.
- **[Encoder behavior]** → Some encoders send relative values (increment/decrement) rather than absolute CC. Mitigation: start with absolute CC support (X-Touch Mini default mode); relative mode can be added later.
