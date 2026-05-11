## 1. Main Process IPC

- [ ] 1.1 Add `read-midi-config` IPC handler — read `<userData>/midi-config.json`, return parsed JSON or empty object if missing
- [ ] 1.2 Add `write-midi-config` IPC handler — accept config object, write to `<userData>/midi-config.json`
- [ ] 1.3 Expose both IPC channels in `preload.js` and type declarations

## 2. MIDI Composable

- [ ] 2.1 Create `composables/useMidiController.ts` — request MIDI access, listen to all input ports, handle statechange for hot-plug
- [ ] 2.2 Implement MIDI message parsing: extract channel, type (note/cc), number, and value from raw MIDI data
- [ ] 2.3 Implement binding lookup: match incoming messages against stored config, dispatch actions
- [ ] 2.4 Implement discrete action dispatch: trigger-slot-N (0-15), pause-resume, toggle-loop, stop-all
- [ ] 2.5 Implement continuous action dispatch: master-volume (CC 0-127 → -60dB to 0dB)
- [ ] 2.6 Implement MIDI Learn mode: `startLearn(actionId)` captures next message, `stopLearn()` cancels
- [ ] 2.7 Load config from main process on mount, save config on binding changes

## 3. Configuration UI

- [ ] 3.1 Create `components/MidiConfigModal.vue` — list all actions with current bindings, MIDI Learn button per row, Clear button, Reset All
- [ ] 3.2 Display bindings as human-readable labels (e.g., "Ch1 Note 36", "Ch1 CC 9")
- [ ] 3.3 Implement conflict detection — warn when binding already assigned to another action
- [ ] 3.4 Show connected MIDI device names in the modal header
- [ ] 3.5 Style modal using existing theme CSS variables (--color-text-primary, --color-surface, etc.)

## 4. Integration

- [ ] 4.1 Mount `useMidiController` globally (in `app.vue` or `CartPlayer.vue`)
- [ ] 4.2 Add MIDI config button to cart player header (alongside hotkey config button)
- [ ] 4.3 Add localization strings to `locales/en.json`

## 5. Testing

- [ ] 5.1 Test MIDI device detection with X-Touch Mini connected (manual)
- [ ] 5.2 Test MIDI Learn capture for button and fader (manual)
- [ ] 5.3 Test cart slot triggering via MIDI button (manual)
- [ ] 5.4 Test master volume control via MIDI fader (manual)
- [ ] 5.5 Test pause/resume and stop-all via MIDI buttons (manual)
- [ ] 5.6 Test config persistence across app restart (manual)
- [ ] 5.7 Test hot-plug: connect MIDI device after app start (manual)
