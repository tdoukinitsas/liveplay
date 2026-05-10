## ADDED Requirements

### Requirement: System PATH ffmpeg fallback
When the bundled ffmpeg binary fails, the system SHALL attempt to locate ffmpeg on the system PATH using `which ffmpeg` (Unix) or `where ffmpeg` (Windows) and verify it with `-version` before accepting.

#### Scenario: Bundled ffmpeg fails, system ffmpeg available
- **WHEN** the bundled `@ffmpeg-installer/ffmpeg` binary fails verification
- **AND** ffmpeg is installed on the system PATH
- **THEN** the system SHALL use the system ffmpeg path and set `ffmpegAvailable = true`

#### Scenario: Bundled ffmpeg fails, no system ffmpeg
- **WHEN** the bundled ffmpeg binary fails verification
- **AND** ffmpeg is NOT on the system PATH
- **THEN** `ffmpegAvailable` SHALL remain `false`

### Requirement: System PATH ffprobe fallback
When the bundled ffprobe binary fails, the system SHALL attempt to locate ffprobe on the system PATH and configure `fluent-ffmpeg` to use it.

#### Scenario: Bundled ffprobe fails, system ffprobe available
- **WHEN** the bundled ffprobe binary fails
- **AND** ffprobe is installed on the system PATH
- **THEN** the system SHALL set the ffprobe path via `ffmpeg.setFfprobePath()`

#### Scenario: Bundled ffprobe fails, no system ffprobe
- **WHEN** the bundled ffprobe binary fails
- **AND** ffprobe is NOT on the system PATH
- **THEN** ffprobe SHALL remain unconfigured (no error thrown)
