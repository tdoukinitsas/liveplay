## Approach

Add two new handlers to the existing `handleKeydown` function in `useCartHotkeys.ts`:

1. **Escape → Stop All**: Call `stopAllCues()` from `useAudioEngine`
2. **W → Volume Up / S → Volume Down**: Call `setMasterGain(masterGainDb.value + step)` with a 1 dB step per keypress

Both are handled before the cart slot lookup so they take priority. The composable already imports from `useAudioEngine` for play/pause/stop — we just need to add `stopAllCues`, `setMasterGain`, and `masterGainDb` to the destructured imports.

## Key Bindings

| Key | Action | Modifier constraints |
|-----|--------|---------------------|
| `Escape` | Stop all cues | No modifiers |
| `W` | Master volume +1 dB | No modifiers |
| `S` | Master volume -1 dB | No modifiers |

These are hardcoded, not user-configurable. All three keys are added to `RESERVED_COMBOS` to prevent cart slot assignment conflicts.

## Volume Step

1 dB per keypress. `setMasterGain` already clamps to [-60, 0] range, so no bounds checking needed in the handler.

## Components

`PlaybackControls.vue` already reactively displays `masterGainDb`, so keyboard-driven volume changes reflect in the OUT slider automatically.

`ControlConfigModal.vue` needs two changes for default-mapping display:

1. **`getKeyLabel`** returns a `{ label, isDefault }` shape (or a parallel `isDefaultKey(slot)` helper). When `keyMappings.value[slotIndex]` is unset, fall back to `DEFAULT_CART_SLOT_KEYS[slotIndex]` and flag it as default. The current `—` placeholder is removed.
2. **Slot row template** binds an `is-default` class when the displayed value is a default. A `.slot-binding.is-default` CSS rule renders the text in muted/dimmed color (e.g. `opacity: 0.5` or a `--color-muted` token) to visually distinguish defaults from user-saved bindings.

MIDI tab is out of scope — it keeps the existing `—` placeholder for unmapped actions.
