# Tasks

- [x] **1. Add reserved combos** — Add `Escape`, `w`, and `s` (no modifiers) to `RESERVED_COMBOS` in `useCartHotkeys.ts`. **File**: `app/composables/useCartHotkeys.ts`

- [x] **2. Import stop/volume functions** — Add `stopAllCues`, `setMasterGain`, `masterGainDb` to the destructured imports from `useAudioEngine()`. **File**: `app/composables/useCartHotkeys.ts`

- [x] **3. Add Escape → stop all handler** — In `handleKeydown`, before cart slot lookup, add: if `Escape` with no modifiers → `stopAllCues()`. **File**: `app/composables/useCartHotkeys.ts`

- [x] **4. Add W/S → master volume handlers** — In `handleKeydown`, before cart slot lookup, add: if `w` no modifiers → `setMasterGain(masterGainDb.value + 1)`, if `s` no modifiers → `setMasterGain(masterGainDb.value - 1)`. **File**: `app/composables/useCartHotkeys.ts`

- [x] **5. Fall back to default binding in keyboard tab** — Update `getKeyLabel` (or add an `isDefaultKey` helper) in `ControlConfigModal.vue` so that when `keyMappings.value[slotIndex]` is unset, it returns the formatted label of `DEFAULT_CART_SLOT_KEYS[slotIndex]`. Keep the `—` placeholder only when no default exists either. **File**: `app/components/ControlConfigModal.vue`

- [x] **6. Render default bindings muted** — Bind an `is-default` class on the `.slot-binding` span when the displayed binding is a default. Add a CSS rule that dims the text so users can tell defaults apart from saved bindings. **File**: `app/components/ControlConfigModal.vue`
