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
The system SHALL block binding key combos reserved by the application or OS (e.g., Ctrl+S, Ctrl+Q, Ctrl+W, Ctrl+Z, Ctrl+N, Ctrl+O).

#### Scenario: Blocking a reserved combo
- **WHEN** the user attempts to bind Ctrl+S to a cart slot
- **THEN** the system rejects the binding and displays a message that the combo is reserved

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

