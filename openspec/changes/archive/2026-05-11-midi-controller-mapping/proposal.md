## Why

LivePlay users with MIDI controllers (e.g., Behringer X-Touch Mini) have no way to use physical buttons, encoders, and faders to control playback. Adding MIDI mapping enables tactile, eyes-free control during live performances — triggering cart slots, pausing/resuming, toggling loop, stopping all cues, and adjusting master volume from hardware.

## What Changes

- Add Web MIDI API integration in the renderer process to detect and listen to MIDI input devices
- Create a generic MIDI mapping system: any MIDI message (note on/off, CC) can be mapped to any supported action
- Supported actions: trigger cart slot 1-16, pause/resume, toggle loop, stop all, master volume (continuous CC)
- "MIDI Learn" configuration UI: user selects an action, presses/turns a control on their MIDI device, binding is captured
- MIDI mappings stored globally in app settings (`<userData>/midi-config.json`), not per-project
- Configuration accessible from a settings button (e.g., in the cart player header or app menu)

## Capabilities

### New Capabilities
- `midi-mapping`: Web MIDI API integration, MIDI learn capture, action dispatch, global config persistence

### Modified Capabilities
<!-- None -->

## Impact

- **No new npm dependencies** — Web MIDI API is built into Chromium/Electron
- **New composable:** `useMidiController.ts` — MIDI device detection, message parsing, action dispatch
- **New component:** `MidiConfigModal.vue` — MIDI learn UI with action list and binding display
- **New main-process IPC:** read/write `midi-config.json` from userData directory
- **Existing code reused:** `useCartHotkeys.triggerSlot()`, `pauseCue/resumeCue`, `stopAllCues`, `masterOutputLevel` from audio engine
