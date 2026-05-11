## Why

Dragging items from the playlist to cart slots silently fails in Electron. Cart-to-cart reordering is also broken for the same reason. Chromium's security model hides custom `dataTransfer` type names (like `item-uuid` and `cart-slot`) during `dragover` events — only standard MIME types are visible. Since `handleDragOver` checks for these custom types to decide whether to accept the drop, the drop zone is never activated and the drop event never fires.

## What Changes

- **Fix `CartSlot.vue` `handleDragOver`**: Accept all drag operations unconditionally instead of checking custom `dataTransfer` type names. The existing `handleDrop` already does proper type checking by reading actual data (which works in `drop` events), so it will correctly route cart-to-cart moves, file drops, and playlist-to-cart drops.
- **No changes to `PlaylistItem.vue` or `CartSlot.handleDragStart`**: The custom type names used in `setData()` calls are fine — they are readable in `drop` events. Only the `dragover` guard logic is broken.

## Capabilities

### New Capabilities

- `drag-drop-interop`: Reliable drag-and-drop between playlist, cart slots, and external files, resilient to Chromium dataTransfer type restrictions.

### Modified Capabilities

## Impact

- `components/CartSlot.vue` — `handleDragOver` function (lines 614-627)
- No API, dependency, or data model changes
