## Context

LivePlay's cart system supports three drag sources: playlist items, other cart slots (reorder), and external audio files. `CartSlot.vue`'s `handleDragOver` currently inspects `e.dataTransfer.types` for custom MIME types (`item-uuid`, `cart-slot`) to decide whether to accept a drag. Chromium/Electron's security model only exposes standard MIME types (`text/plain`, `text/html`, `text/uri-list`, `Files`) during `dragover` — custom types are hidden until the `drop` event. This breaks both playlist-to-cart and cart-to-cart drag.

## Goals / Non-Goals

**Goals:**
- Fix playlist-to-cart drag-and-drop
- Fix cart-to-cart drag-and-drop (same root cause)
- Solution resilient to Chromium dataTransfer restrictions

**Non-Goals:**
- Changing the data format or keys used in `setData`/`getData` calls
- Refactoring playlist-internal drag-and-drop (not affected)
- Adding new drag-and-drop capabilities

## Decisions

**Accept all drags in `handleDragOver`, let `handleDrop` discriminate.**

`handleDrop` already reads actual data with `getData()` (which works in drop events) and correctly routes to cart-move, file-import, or playlist-to-cart logic. The `dragover` guard provided no real protection — it only controlled visual feedback and `dropEffect`.

Alternative considered: Switch custom MIME types to standard ones (e.g., `text/plain` for UUID). Rejected because it would require coordinating changes across `PlaylistItem`, `CartSlot`, and `PlaylistItem`'s own `handleDragOver`, and could collide with other uses of `text/plain`.

## Risks / Trade-offs

- **Broader drop acceptance** → Cart slots will show drop-hover styling for any drag (e.g., dragging text from another app). Mitigation: `handleDrop` ignores unrecognized data, so this is cosmetic only. Acceptable trade-off for a cue playback app.
- **dropEffect hint less specific** → We default to `'copy'` since we can't distinguish cart-drag (`'move'`) from playlist-drag during dragover. Mitigation: Minimal UX impact — cursor icon may differ slightly for cart reorder.
