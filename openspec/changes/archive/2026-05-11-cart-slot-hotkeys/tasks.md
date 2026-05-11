## 1. Data Model

- [x] 1.1 Add `cartSlotKeys` field to the `Project` interface in `types/project.ts`
- [x] 1.2 Create default key mapping constant (1–9 → slots 1–9, 0 → slot 10, Ctrl+1–6 → slots 11–16)
- [x] 1.3 Ensure project load/save handles missing `cartSlotKeys` by falling back to defaults

## 2. Core Composable

- [x] 2.1 Create `composables/useCartHotkeys.ts` with mapping state and `triggerSlot(index)` method
- [x] 2.2 Implement global `keydown` listener with focus suppression (skip when input/textarea/contenteditable focused)
- [x] 2.3 Wire `triggerSlot` to mirror existing cart slot click behavior (play if stopped, respect slot mode)
- [x] 2.4 Add reserved-combo list and lookup utility

## 3. Integration

- [x] 3.1 Mount `useCartHotkeys` in `CartPlayer.vue` and connect to cart item actions
- [x] 3.2 Coordinate with existing F1 shortcut in `MainWorkspace.vue` to avoid conflicts

## 4. Key Label Display

- [x] 4.1 Pass key binding label to `CartSlot.vue` and render it on each slot
- [x] 4.2 Format labels for display (e.g., "Ctrl+1", "Q", "0")

## 5. Configuration UI

- [x] 5.1 Add hotkey config button to cart player header
- [x] 5.2 Build config modal/panel showing all 16 slots and current bindings
- [x] 5.3 Implement "press any key" capture for rebinding a slot
- [x] 5.4 Add conflict detection (warn when combo already assigned to another slot)
- [x] 5.5 Add reserved-combo protection (block Ctrl+S, Ctrl+Q, etc.)
- [x] 5.6 Save updated mappings to project file on change

## 6. Testing

- [ ] 6.1 Verify default mappings apply on new/legacy projects
- [ ] 6.2 Verify hotkeys suppressed when text inputs focused
- [ ] 6.3 Verify custom bindings persist across save/load
- [ ] 6.4 Verify conflict detection and reserved-combo blocking in config UI
