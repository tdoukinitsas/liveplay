# Tasks

## 1. Add reserved combos
Add `Escape`, `w`, and `s` (no modifiers) to `RESERVED_COMBOS` in `useCartHotkeys.ts`.

**File**: `app/composables/useCartHotkeys.ts`

## 2. Import stop/volume functions
Add `stopAllCues`, `setMasterGain`, `masterGainDb` to the destructured imports from `useAudioEngine()`.

**File**: `app/composables/useCartHotkeys.ts`

## 3. Add Escape → stop all handler
In `handleKeydown`, before cart slot lookup, add: if `Escape` with no modifiers → `stopAllCues()`.

**File**: `app/composables/useCartHotkeys.ts`

## 4. Add W/S → master volume handlers
In `handleKeydown`, before cart slot lookup, add: if `w` no modifiers → `setMasterGain(masterGainDb.value + 1)`, if `s` no modifiers → `setMasterGain(masterGainDb.value - 1)`.

**File**: `app/composables/useCartHotkeys.ts`
