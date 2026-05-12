# cart-hotkeys Delta Spec

## Changed Requirements

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

### Requirement: Reserved combo protection (updated)
The system SHALL also reserve `Escape`, `W`, and `S` (no modifiers) to prevent them from being assigned to cart slots.
