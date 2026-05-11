## Context

LivePlay is an Electron + Nuxt 3 audio cue playback app. The cart player renders a 4×4 grid of 16 slots (`CartPlayer.vue` / `CartSlot.vue`). Currently slots are triggered only by mouse click. `MainWorkspace.vue` already has a global keydown listener for F1. The `Project` interface in `types/project.ts` defines the project file shape, and `useCartItems` composable manages cart state.

## Goals / Non-Goals

**Goals:**
- Keyboard shortcuts for all 16 cart slots with sensible defaults
- User-configurable bindings stored in the project file
- Visual key labels on each slot
- Conflict detection and reserved-combo protection in the config UI

**Non-Goals:**
- MIDI or gamepad input (future work)
- Global OS-level hotkeys (only in-app when the window is focused)
- Hotkeys for non-cart actions (transport, markers, etc.)

## Decisions

### 1. New composable `useCartHotkeys.ts`

**Decision:** Create a dedicated composable rather than adding logic to `useCartItems` or `CartPlayer.vue`.

**Rationale:** Single-responsibility — the composable owns the keydown listener lifecycle, mapping state, and `triggerSlot(index)` dispatch. It can be tested independently and reused.

**Alternatives considered:** Embedding in `CartPlayer.vue` (rejected — mixes UI and input concerns); Nuxt plugin (rejected — overkill for a component-scoped feature).

### 2. Data model on Project

**Decision:** Add `cartSlotKeys: Record<number, { key: string; ctrlKey: boolean; shiftKey: boolean; altKey: boolean }>` to the `Project` interface. Slot indices 0–15 are keys.

**Rationale:** Storing modifiers as booleans is explicit, serializable, and matches the `KeyboardEvent` API directly. Using the slot index as the record key aligns with the existing `cartItems` array indexing.

**Alternatives considered:** Array of 16 entries (rejected — sparse record is cleaner when only some slots are bound); string-encoded combos like `"Ctrl+1"` (rejected — requires parsing and is error-prone).

### 3. Listener placement and focus suppression

**Decision:** Register a single global `keydown` listener in `useCartHotkeys`, called from `CartPlayer.vue` on mount. Suppress when `document.activeElement` is an `input`, `textarea`, or `[contenteditable]`.

**Rationale:** A single listener is simpler than per-slot listeners. Focus check prevents hotkeys from interfering with text editing.

### 4. Trigger behavior

**Decision:** `triggerSlot(index)` mirrors the existing click handler on `CartSlot` — play if stopped, respect the slot's configured mode (restart, toggle, etc.) if playing.

**Rationale:** Consistency — users expect keyboard and mouse to behave identically.

### 5. Config UI — "press any key" capture

**Decision:** Small modal/panel accessible from cart player header. User clicks a slot row, presses desired key combo, binding updates live. Conflict detection warns if the combo is already assigned to another slot. Reserved combos (Ctrl+S, Ctrl+Q, Ctrl+W, Ctrl+Z, Ctrl+N, Ctrl+O, etc.) are blocked.

**Rationale:** "Press any key" is the standard UX pattern for keybinding and avoids users needing to type combo strings.

## Risks / Trade-offs

- **[Electron vs. browser key handling]** → Some key combos may be intercepted by Electron before reaching the renderer. Mitigation: test default bindings on all platforms; reserved-combo list blocks known problematic combos.
- **[Project file migration]** → Older project files won't have `cartSlotKeys`. Mitigation: default to the standard mapping when the field is missing (backward-compatible).
- **[Modifier key differences across OS]** → Ctrl on Windows/Linux vs. Cmd on macOS. Mitigation: initially use Ctrl on all platforms; consider a `metaKey` option later if needed.
