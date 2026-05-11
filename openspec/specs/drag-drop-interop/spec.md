## ADDED Requirements

### Requirement: Cart slots SHALL accept all drag operations during dragover
Cart slot drop zones SHALL unconditionally accept all drag operations during the `dragover` event by calling `preventDefault()` and setting `isDragOver` to `true`. Type discrimination SHALL occur only in the `drop` handler where `getData()` is available.

#### Scenario: Playlist item dragged over a cart slot
- **WHEN** a user drags a playlist item over a cart slot
- **THEN** the cart slot SHALL display drop-hover styling and allow the drop

#### Scenario: Cart item dragged over another cart slot
- **WHEN** a user drags a cart item over a different cart slot
- **THEN** the cart slot SHALL display drop-hover styling and allow the drop

#### Scenario: External audio file dragged over a cart slot
- **WHEN** a user drags an audio file from the OS file manager over a cart slot
- **THEN** the cart slot SHALL display drop-hover styling and allow the drop

#### Scenario: Unrecognized drag over a cart slot
- **WHEN** a user drags unrecognized content (e.g., plain text from another app) over a cart slot
- **THEN** the cart slot SHALL display drop-hover styling, but the drop handler SHALL ignore the content with no side effects

### Requirement: Drop handler SHALL discriminate drag sources by reading data
The `handleDrop` function SHALL determine the drag source by calling `getData()` for known keys (`cart-slot`, `item-uuid`, `Files`) and route accordingly. This existing behavior MUST be preserved unchanged.

#### Scenario: Playlist item dropped on cart slot
- **WHEN** a playlist item is dropped on a cart slot
- **THEN** the drop handler SHALL read `item-uuid` from dataTransfer and assign that item to the slot

#### Scenario: Cart item dropped on another cart slot
- **WHEN** a cart item is dropped on a different cart slot
- **THEN** the drop handler SHALL read `cart-slot` from dataTransfer and perform the move/insert operation
