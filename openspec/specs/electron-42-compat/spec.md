# electron-42-compat Specification

## Purpose
TBD - created by archiving change electron-update. Update Purpose after archive.
## Requirements
### Requirement: Electron 42 runtime

The application SHALL run on Electron 42.x with Chromium 148 and Node.js 24.

#### Scenario: Application starts on Electron 42

- **WHEN** the application is launched
- **THEN** it SHALL start successfully with Electron 42 runtime
- **AND** the main window SHALL render the Nuxt 3 application

#### Scenario: Dev server works with Electron 42

- **WHEN** the developer runs the dev server (`just dev`)
- **THEN** the Nuxt dev server SHALL start and Electron SHALL load the dev URL

### Requirement: electron-builder 26 compatibility

The build system SHALL use electron-builder 26.x to produce distributable packages.

#### Scenario: macOS build succeeds

- **WHEN** the CI pipeline runs on macOS
- **THEN** electron-builder SHALL produce a signed DMG and ZIP artifact

#### Scenario: Windows build succeeds

- **WHEN** the CI pipeline runs on Windows
- **THEN** electron-builder SHALL produce an NSIS installer artifact

### Requirement: Clipboard via preload

The application SHALL expose clipboard write functionality through the preload/contextBridge pattern instead of direct `navigator.clipboard` access in the renderer.

#### Scenario: Copy text to clipboard from PropertiesPanel

- **WHEN** the user triggers a copy action in PropertiesPanel
- **THEN** the text SHALL be written to the system clipboard via the preload-exposed `writeClipboardText` API

#### Scenario: Clipboard API not accessed directly in renderer

- **WHEN** any renderer code needs clipboard access
- **THEN** it SHALL use `window.electronAPI.writeClipboardText()` (or equivalent contextBridge method)
- **AND** `navigator.clipboard` SHALL NOT be called directly

### Requirement: macOS 12 minimum version

The application SHALL require macOS 12 (Monterey) or later.

#### Scenario: Build targets macOS 12+

- **WHEN** the macOS build is produced
- **THEN** the `LSMinimumSystemVersion` in Info.plist SHALL be 12.0 or the electron-builder mac target SHALL specify macOS 12 minimum

### Requirement: CI pipeline compatibility

The CI pipeline SHALL work with Electron 42's lazy binary download mechanism.

#### Scenario: CI installs Electron binary

- **WHEN** `npm install` runs in CI
- **THEN** the Electron binary SHALL be downloaded successfully without relying on `ELECTRON_SKIP_BINARY_DOWNLOAD`

#### Scenario: CI does not use removed env vars

- **WHEN** the CI workflow is executed
- **THEN** `ELECTRON_SKIP_BINARY_DOWNLOAD` SHALL NOT be referenced in any workflow file

