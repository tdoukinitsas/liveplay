## Why

MIDI controllers can map pause/resume, toggle loop, stop all, and master volume actions, but keyboard controls cannot. This creates an inconsistency where users without MIDI hardware have no keyboard access to these essential playback controls. During live shows, having quick keyboard shortcuts for these actions is critical.

## What Changes

- Add keyboard hotkey support for: stop all (`Escape`) and master volume up/down (`W`/`S`)
- Pause/resume (`Space`) and toggle loop (`Right Shift`) are already implemented — no change needed
- These are hardcoded defaults (not user-configurable for now), matching the MIDI action set
- In the keyboard configuration tab, display the default key binding (from `DEFAULT_CART_SLOT_KEYS`) in muted/greyed style for any slot the user has not configured, instead of the current `—` placeholder

## Capabilities

### New Capabilities

### Modified Capabilities

- `cart-hotkeys`: Add stop-all and master volume up/down keyboard shortcuts to the existing hotkey handler

## Impact

- `app/composables/useCartHotkeys.ts` — add new key handlers in `handleKeydown` for stop-all (`Escape`) and master volume (`W`/`S`)
- Reserved combos list needs `Escape`, `w`, `s` added to protect the new bindings from cart slot assignment
- `app/components/ControlConfigModal.vue` — `getKeyLabel` falls back to the default binding when a slot is unmapped; slot row marks the binding as `is-default` so it can be styled with muted/dimmed text
