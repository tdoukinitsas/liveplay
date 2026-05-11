## Context

E-LivePlay is an Electron 28 + Nuxt 3 desktop audio cue application. Electron 28 ships Chromium 120 and Node.js 18 (EOL). The build toolchain uses electron-builder 24.x. The app uses `contextBridge`/`ipcRenderer` properly in preload, with one exception: `navigator.clipboard.writeText()` called directly in the renderer (`PropertiesPanel.vue:633`).

Current constraints:

- SSR is disabled (`ssr: false` in nuxt.config.ts)
- No `BrowserView`, `File.path`, `registerFileProtocol`, `sendTo`, or navigation API usage found
- CI builds on GitHub Actions, triggers on `dev` branch, publishes to `Stevesibilia/liveplay`
- macOS and Windows builds produced via electron-builder

## Goals / Non-Goals

**Goals:**

- Upgrade Electron to 42.x (Chromium 148, Node.js 24)
- Upgrade electron-builder to 26.x for compatibility
- Fix all deprecated API usage (clipboard in renderer)
- Maintain working CI builds for macOS and Windows
- Keep Nuxt 3 compatibility (pinned to ~3.20.0)

**Non-Goals:**

- Nuxt 3 → 4 migration (separate change)
- TypeScript 5 → 6 migration (separate change)
- Resolving electron-builder dep tree vulnerabilities (requires electron-builder major changes beyond scope)
- Adding new Electron features (just upgrading)

## Decisions

### 1. Upgrade directly to Electron 42 (skip intermediate majors)

**Rationale**: The codebase uses very few Electron-specific APIs beyond the basics (BrowserWindow, ipcMain/ipcRenderer, contextBridge, dialog, app). Codebase audit found zero usage of APIs removed in versions 29-41. Only one deprecated pattern exists (clipboard in renderer). A direct jump is lower effort than stepping through 14 major versions.

**Alternative considered**: Step through each major version — rejected because there are no intermediate breaking changes affecting this codebase.

### 2. Migrate clipboard to preload + IPC

**Rationale**: `navigator.clipboard` in renderer is deprecated since Electron 40. Expose a `writeClipboardText(text)` method through the preload/contextBridge, backed by `clipboard.writeText()` from Electron's main process clipboard module. This is a one-line change in preload and a one-line change in the component.

**Alternative considered**: Use `document.execCommand('copy')` — rejected, also deprecated in Electron 33.

### 3. electron-builder 24 → 26

**Rationale**: electron-builder 24.x predates Electron 42's lazy binary download behavior. Version 26.x is the current stable and handles the new download mechanism. The `electron-builder install-app-deps` postinstall script is unchanged.

**Alternative considered**: Switch to `@electron/forge` — rejected, too much CI/config churn for this change.

### 4. No intermediate testing checkpoints

**Rationale**: Bump versions, fix the one clipboard deprecation, run `npm install`, test dev server + build. If native module compilation fails (C++20 requirement from Electron 33+), debug at that point. The app has no native Node addons beyond what electron-builder provides.

## Risks / Trade-offs

- **[Node.js 18 → 24 jump]** → Some dependencies may not support Node 24. Mitigation: `npm install` will surface incompatibilities immediately; the app has no exotic native modules.
- **[C++20 native module compilation]** → `electron-builder install-app-deps` may fail if system toolchain is old. Mitigation: macOS Xcode and Windows MSVC on GitHub Actions runners already support C++20.
- **[macOS minimum version bump to 12]** → Users on macOS 10.15-11 can't update. Mitigation: Acceptable — macOS 11 is EOL, 12 has <2% market share among remaining users.
- **[electron-builder 26 config changes]** → Build config in package.json `"build"` field may need updates. Mitigation: Review electron-builder 26 migration guide during implementation.
- **[Lazy binary download in CI]** → Electron binary not present after `npm install` if `ELECTRON_SKIP_BINARY_DOWNLOAD` was set. Mitigation: Remove that env var from CI; let npm install download the binary normally.
