## Why

Electron 28 is over 18 months old, uses Chromium 120 and Node.js 18 (EOL). Upgrading to Electron 42 brings Chromium 148, Node.js 24, security patches, and performance improvements. The companion electron-builder 24.x is also outdated and won't handle Electron 42's lazy binary download behavior.

## What Changes

- **BREAKING**: Upgrade Electron from 28 to 42 (Chromium 120 → 148, Node.js 18 → 24)
- **BREAKING**: Upgrade electron-builder from 24.x to 26.x (required for Electron 42 compatibility)
- **BREAKING**: Minimum macOS raised from 10.15 (Catalina) to 12 (Monterey)
- Migrate `navigator.clipboard.writeText()` in renderer to preload + contextBridge pattern (deprecated in Electron 40)
- Update CI workflow for Electron 42's lazy binary download (no more `postinstall` for binary, `ELECTRON_SKIP_BINARY_DOWNLOAD` removed)
- Native modules now require C++20 compilation (Electron 33+)
- Ensure app code-signing for macOS notifications (Electron 42 uses UNNotification)

### Low-risk (no affected code found in codebase)

- `ipcRenderer.sendTo()` removed (v28) — not used
- `BrowserView` deprecated (v30) — not used
- `File.path` removed (v32) — not used
- `registerFileProtocol` deprecated (v33) — not used
- Navigation APIs moved to `navigationHistory` (v32) — not used

## Capabilities

### New Capabilities

- `electron-42-compat`: Electron 42 compatibility layer — updated preload APIs, lazy binary download support, C++20 native module compilation, macOS 12+ minimum

### Modified Capabilities

_(none — no existing specs have requirement-level changes)_

## Impact

- **`electron/main.js`**: Minimal changes expected — current API usage is compatible
- **`electron/preload.js`**: May need clipboard API exposure if renderer clipboard usage is retained
- **`components/PropertiesPanel.vue:633`**: `navigator.clipboard.writeText()` deprecated in renderer — move to IPC
- **`package.json`**: Electron ^42.0.0, electron-builder ^26.0.0
- **`.github/workflows/build-release.yml`**: Update for lazy binary download, possibly remove `ELECTRON_SKIP_BINARY_DOWNLOAD`
- **Native deps**: `electron-builder install-app-deps` must compile with C++20
- **macOS**: Minimum OS version bump to 12 (Monterey) — users on older macOS can't upgrade
- **Node.js 24**: Test all runtime dependencies for compatibility
