## Why

LivePlay's cart player has 16 slots but no way to trigger them from the keyboard. During live performances, reaching for the mouse to click a slot adds latency and breaks flow. Keyboard shortcuts let operators fire cues instantly without leaving the keyboard.

## What Changes

- Add configurable keyboard shortcuts for all 16 cart slots (default: 1–9 for slots 1–9, 0 for slot 10, Ctrl+1–6 for slots 11–16)
- New composable `useCartHotkeys` to own the keydown listener, mapping state, and slot triggering
- Store key mappings in the project file alongside cart items
- Add a configuration UI for rebinding keys with conflict detection and reserved-combo protection
- Display the assigned key binding label on each cart slot
- Suppress hotkeys when text inputs are focused

## Capabilities

### New Capabilities

- `cart-hotkeys`: Keyboard shortcut system for cart slots — mapping storage, keydown handling, configuration UI, and key label display

### Modified Capabilities

## Impact

- `types/project.ts` — extend `Project` interface with `cartSlotKeys` mapping
- `components/CartPlayer.vue` — integrate hotkey listener, add config UI entry point
- `components/CartSlot.vue` — display key binding label
- `composables/useCartItems.ts` — may need to expose trigger method for hotkey use
- `components/MainWorkspace.vue` — coordinate with existing F1 shortcut listener
- Project file format gains a new field (backward-compatible; missing field uses defaults)
