## Why

MIDI controllers can map pause/resume, toggle loop, stop all, and master volume actions, but keyboard controls cannot. This creates an inconsistency where users without MIDI hardware have no keyboard access to these essential playback controls. During live shows, having quick keyboard shortcuts for these actions is critical.

## What Changes

- Add keyboard hotkey support for: stop all (`Escape`) and master volume up/down (`W`/`S`)
- Pause/resume (`Space`) and toggle loop (`Right Shift`) are already implemented — no change needed
- These are hardcoded defaults (not user-configurable for now), matching the MIDI action set

## Capabilities

### New Capabilities

### Modified Capabilities

- `cart-hotkeys`: Add stop-all and master volume up/down keyboard shortcuts to the existing hotkey handler

## Impact

- `app/composables/useCartHotkeys.ts` — add new key handlers in `handleKeydown` for stop-all (`Escape`) and master volume (`W`/`S`)
- Reserved combos list needs `Escape`, `w`, `s` added to protect the new bindings from cart slot assignment
