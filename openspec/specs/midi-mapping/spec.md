# midi-mapping Specification

## Purpose
TBD - created by archiving change midi-controller-mapping. Update Purpose after archive.
## Requirements
### Requirement: Detect MIDI input devices
The system SHALL request Web MIDI API access on startup and listen to all connected MIDI input ports for incoming messages. The system SHALL handle device hot-plug by listening for `statechange` events.

#### Scenario: MIDI device connected at startup
- **WHEN** the app starts with a MIDI controller connected
- **THEN** the system detects the device and begins listening for MIDI messages

#### Scenario: MIDI device connected after startup
- **WHEN** a MIDI controller is connected while the app is running
- **THEN** the system detects the new device and begins listening for its messages

#### Scenario: No MIDI devices available
- **WHEN** no MIDI devices are connected
- **THEN** the system operates normally without MIDI functionality and the config UI shows "No MIDI devices detected"

### Requirement: Map MIDI messages to actions
The system SHALL support mapping MIDI messages (note on, control change) to discrete actions (trigger cart slot 1-16, pause/resume, toggle loop, stop all) and continuous actions (master volume).

#### Scenario: Button press triggers cart slot
- **WHEN** a MIDI note-on message matching a configured binding for "trigger slot 5" is received
- **THEN** cart slot 5 is triggered (play if stopped, pause if playing, resume if paused)

#### Scenario: Button press triggers pause/resume
- **WHEN** a MIDI note-on message matching a configured binding for "pause-resume" is received
- **THEN** the currently playing/selected item is paused or resumed

#### Scenario: Button press triggers stop all
- **WHEN** a MIDI note-on message matching a configured binding for "stop-all" is received
- **THEN** all playing cues are stopped

#### Scenario: Fader controls master volume
- **WHEN** a MIDI CC message matching a configured binding for "master-volume" is received with value V
- **THEN** the master output level is set to the corresponding dB value (0→-60dB, 127→0dB)

#### Scenario: Unmapped MIDI message received
- **WHEN** a MIDI message is received that has no configured binding
- **THEN** the message is ignored with no side effects

### Requirement: MIDI Learn capture
The system SHALL provide a "MIDI Learn" mode where the user selects an action and the next MIDI message received becomes the binding for that action.

#### Scenario: Capturing a button binding
- **WHEN** the user selects "Trigger Slot 3" in the config UI and presses a button on their MIDI controller
- **THEN** the MIDI note message is captured and assigned as the binding for "Trigger Slot 3"

#### Scenario: Capturing a fader binding
- **WHEN** the user selects "Master Volume" in the config UI and moves a fader on their MIDI controller
- **THEN** the MIDI CC message is captured and assigned as the binding for "Master Volume"

#### Scenario: Cancel MIDI Learn
- **WHEN** the user presses Escape during MIDI Learn mode
- **THEN** the learn mode is cancelled without changing any binding

### Requirement: Conflict detection
The system SHALL warn the user when a MIDI message is already assigned to another action during MIDI Learn.

#### Scenario: Duplicate binding warning
- **WHEN** the user attempts to bind a MIDI message that is already assigned to another action
- **THEN** the system displays a conflict warning identifying the existing assignment and asks the user to confirm or cancel

### Requirement: Global config persistence
The system SHALL store MIDI mappings in a global config file (`<userData>/midi-config.json`) that persists across sessions and projects.

#### Scenario: Mappings persist across app restart
- **WHEN** the user configures MIDI mappings and restarts the app
- **THEN** the mappings are restored from the config file

#### Scenario: Config file missing
- **WHEN** the app starts with no existing MIDI config file
- **THEN** the system starts with no MIDI mappings (empty config)

### Requirement: MIDI configuration UI
The system SHALL provide a configuration modal accessible from the app UI showing all available actions, their current MIDI bindings (displayed as "Ch1 Note 36" or "Ch1 CC 9"), and a MIDI Learn button for each action.

#### Scenario: Opening the MIDI config
- **WHEN** the user clicks the MIDI config button
- **THEN** a modal opens showing all mappable actions with their current bindings

#### Scenario: Clearing a binding
- **WHEN** the user clicks "Clear" on an existing binding
- **THEN** the binding is removed and the action is no longer triggered by MIDI

#### Scenario: Reset all bindings
- **WHEN** the user clicks "Reset All"
- **THEN** all MIDI bindings are cleared

