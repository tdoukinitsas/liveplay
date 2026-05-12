# cart-hotkeys Specification

## Purpose
TBD - created by archiving change cart-slot-hotkeys. Update Purpose after archive.
## Requirements
### Requirement: Default key mappings
The system SHALL provide default keyboard mappings: keys 1–9 for cart slots 1–9, key 0 for cart slot 10, and Ctrl+1 through Ctrl+6 for cart slots 11–16. When a project file has no `cartSlotKeys` field, the system SHALL use these defaults.

#### Scenario: New project uses default mappings
- **WHEN** a project is loaded with no `cartSlotKeys` field
- **THEN** the system applies the default mapping (1→slot 1, 2→slot 2, …, 0→slot 10, Ctrl+1→slot 11, …, Ctrl+6→slot 16)

#### Scenario: Pressing a default key triggers the slot
- **WHEN** the user presses the "3" key with no modifiers and no text input is focused
- **THEN** cart slot 3 is triggered (play if stopped, respect slot mode if playing)

### Requirement: Hotkey triggers mirror click behavior
The system SHALL trigger a cart slot via hotkey identically to clicking the slot — play if stopped, and respect the slot's configured playback mode if already playing.

#### Scenario: Hotkey plays a stopped slot
- **WHEN** the user presses a bound hotkey for a slot that is stopped and loaded
- **THEN** the slot begins playback

#### Scenario: Hotkey respects slot mode on playing slot
- **WHEN** the user presses a bound hotkey for a slot that is currently playing
- **THEN** the system applies the slot's configured mode (e.g., restart, toggle stop, ignore)

### Requirement: Suppress hotkeys when text input focused
The system SHALL NOT trigger cart hotkeys when the focused element is an `input`, `textarea`, or `[contenteditable]` element.

#### Scenario: Typing in a text field does not trigger slots
- **WHEN** the user focuses a text input and presses the "1" key
- **THEN** no cart slot is triggered and the keystroke is handled normally by the input

### Requirement: Configurable key mappings
The system SHALL allow users to change the key binding for any cart slot. Mappings SHALL be stored in the project file as `cartSlotKeys: Record<number, { key: string; ctrlKey: boolean; shiftKey: boolean; altKey: boolean }>`.

#### Scenario: User rebinds a slot
- **WHEN** the user opens the hotkey config UI, selects slot 5, and presses "Q"
- **THEN** slot 5's binding updates to the "Q" key and the change is saved to the project

#### Scenario: Mappings persist across sessions
- **WHEN** the user saves a project with custom key mappings and reopens it
- **THEN** the custom mappings are restored

### Requirement: Conflict detection
The system SHALL warn the user when a key combo is already assigned to another cart slot during rebinding.

#### Scenario: Duplicate binding warning
- **WHEN** the user attempts to bind "2" to slot 5 while "2" is already bound to slot 2
- **THEN** the system displays a conflict warning identifying the existing assignment

### Requirement: Reserved combo protection
The system SHALL block binding key combos reserved by the application or OS (e.g., Ctrl+S, Ctrl+Q, Ctrl+W, Ctrl+Z, Ctrl+N, Ctrl+O, Escape, W, S with no modifiers).

#### Scenario: Blocking a reserved combo
- **WHEN** the user attempts to bind Ctrl+S to a cart slot
- **THEN** the system rejects the binding and displays a message that the combo is reserved

#### Scenario: Escape cannot be assigned to a cart slot
- **WHEN** the user attempts to bind `Escape` to a cart slot during key capture
- **THEN** the binding is rejected with a "reserved" error and the slot remains unchanged

#### Scenario: W cannot be assigned to a cart slot
- **WHEN** the user attempts to bind `W` (no modifiers) to a cart slot
- **THEN** the binding is rejected with a "reserved" error

#### Scenario: S cannot be assigned to a cart slot
- **WHEN** the user attempts to bind `S` (no modifiers) to a cart slot
- **THEN** the binding is rejected with a "reserved" error

### Requirement: Key label display on cart slots
Each cart slot SHALL display its assigned key binding as a visible label.

#### Scenario: Slot shows default key label
- **WHEN** a project with default mappings is loaded
- **THEN** slot 1 displays "1", slot 10 displays "0", slot 11 displays "Ctrl+1", etc.

#### Scenario: Slot shows custom key label
- **WHEN** the user rebinds slot 5 to "Q"
- **THEN** slot 5's label updates to "Q"

### Requirement: Configuration UI
The system SHALL provide a config UI (modal or panel) accessible from the cart player header for viewing and editing all slot key bindings using a "press any key" capture pattern.

#### Scenario: Opening the config UI
- **WHEN** the user clicks the hotkey config button in the cart player header
- **THEN** a modal or panel opens showing all 16 slots and their current bindings

#### Scenario: Press-any-key capture
- **WHEN** the user selects a slot in the config UI and presses a key combo
- **THEN** the slot's binding updates to the pressed combo (subject to conflict and reserved checks)

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

### Requirement: Default key bindings shown in keyboard config tab
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

