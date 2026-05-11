## 1. Dependency Upgrades

- [x] 1.1 Bump `electron` from `^28.0.0` to `^42.0.0` in package.json
- [x] 1.2 Bump `electron-builder` from `^24.9.1` to `^26.0.0` in package.json
- [x] 1.3 Run `npm install` and resolve any dependency conflicts
- [x] 1.4 Verify `electron-builder install-app-deps` succeeds (C++20 native module compilation)

## 2. Clipboard Migration

- [x] 2.1 Add `writeClipboardText` IPC handler in `electron/main.js` using `clipboard.writeText()`
- [x] 2.2 Expose `writeClipboardText` in `electron/preload.js` via contextBridge
- [x] 2.3 Add type declaration for `writeClipboardText` in `types/global.d.ts`
- [x] 2.4 Replace `navigator.clipboard.writeText()` with `window.electronAPI.writeClipboardText()` in `components/PropertiesPanel.vue:633`

## 3. Build Configuration

- [x] 3.1 Review electron-builder 26 migration notes and update `"build"` config in package.json if needed
- [x] 3.2 Verify macOS build target specifies minimum macOS 12 (Monterey)
- [x] 3.3 Remove any references to `ELECTRON_SKIP_BINARY_DOWNLOAD` in `.github/workflows/build-release.yml`

## 4. Verification

- [x] 4.1 Start dev server (`just dev`) and confirm Electron window loads
- [x] 4.2 Test clipboard copy in PropertiesPanel
- [x] 4.3 Run production build locally (`npm run build`) and verify packaged app launches
- [x] 4.4 Push branch and verify CI builds succeed for macOS and Windows
