# cart-hotkeys Delta Spec

## ADDED Requirements

### Requirement: Stop All keyboard shortcut
The system SHALL stop all playing cues when the user presses `Escape` with no modifiers and no text input is focused.

#### Scenario: Escape stops all playing cues
- **WHEN** one or more cues are playing and the user presses `Escape` with no modifiers
- **THEN** all playing cues are stopped

#### Scenario: Escape does nothing when no cues playing
- **WHEN** no cues are playing and the user presses `Escape`
- **THEN** nothing happens

### Requirement: Master volume keyboard shortcuts
The system SHALL increase master volume by 1 dB when the user presses `W` (no modifiers, no text input focused) and decrease by 1 dB when pressing `S`. Volume SHALL be clamped to the [-60, 0] dB range.

#### Scenario: W increases master volume
- **WHEN** master volume is -10 dB and the user presses `W`
- **THEN** master volume becomes -9 dB

#### Scenario: S decreases master volume
- **WHEN** master volume is -10 dB and the user presses `S`
- **THEN** master volume becomes -11 dB

#### Scenario: Volume does not exceed 0 dB
- **WHEN** master volume is 0 dB and the user presses `W`
- **THEN** master volume remains 0 dB

#### Scenario: Volume does not go below -60 dB
- **WHEN** master volume is -60 dB and the user presses `S`
- **THEN** master volume remains -60 dB

## MODIFIED Requirements

### Requirement: Reserved combo protection
The system SHALL prevent reserved keys from being assigned to cart slots. In addition to previously reserved combos, the system SHALL also reserve `Escape`, `W`, and `S` (no modifiers).

#### Scenario: Escape cannot be assigned to a cart slot
- **WHEN** the user attempts to bind `Escape` to a cart slot during key capture
- **THEN** the binding is rejected with a "reserved" error and the slot remains unchanged

#### Scenario: W cannot be assigned to a cart slot
- **WHEN** the user attempts to bind `W` (no modifiers) to a cart slot
- **THEN** the binding is rejected with a "reserved" error

#### Scenario: S cannot be assigned to a cart slot
- **WHEN** the user attempts to bind `S` (no modifiers) to a cart slot
- **THEN** the binding is rejected with a "reserved" error

## ADDED Requirements

### Requirement: Default key bindings shown in keyboard tab
The keyboard configuration tab SHALL display the default key binding for any cart slot the user has not explicitly configured, rendered in a visually muted (dimmed) style to distinguish it from user-configured bindings. Defaults come from `DEFAULT_CART_SLOT_KEYS`.

#### Scenario: Unconfigured slot shows default binding
- **WHEN** a slot has no user-configured key binding and a default exists in `DEFAULT_CART_SLOT_KEYS`
- **THEN** the slot row displays the default binding label in muted style instead of `—`

#### Scenario: Configured slot shows user binding normally
- **WHEN** a slot has a user-configured key binding
- **THEN** the slot row displays that binding in the standard (non-muted) style

#### Scenario: Slot without default and without user binding
- **WHEN** a slot has no user-configured binding and no entry in `DEFAULT_CART_SLOT_KEYS`
- **THEN** the slot row displays the `—` placeholder
