## 1. Fix CartSlot dragover

- [x] 1.1 Replace type-checking logic in `CartSlot.vue` `handleDragOver` (lines 614-627) with unconditional drop acceptance: always set `isDragOver = true`, call `preventDefault()`, and set a default `dropEffect`

## 2. Verify existing drop handling

- [x] 2.1 Confirm `handleDrop` correctly handles playlist-to-cart drop (reads `item-uuid`)
- [x] 2.2 Confirm `handleDrop` correctly handles cart-to-cart drop (reads `cart-slot`)
- [x] 2.3 Confirm `handleDrop` correctly handles file drop (reads `Files`)
- [x] 2.4 Confirm `handleDrop` gracefully ignores unrecognized drag data

## 3. Manual testing

- [x] 3.1 Test dragging a playlist item onto an empty cart slot (manual)
- [x] 3.2 Test dragging a playlist item onto an occupied cart slot (manual)
- [x] 3.3 Test dragging a cart item to a different cart slot (reorder) (manual)
- [x] 3.4 Test dragging an audio file from OS file manager onto a cart slot (manual)
