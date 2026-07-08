# LivePlay Client — developer guide

The LivePlay client is a Vue 3 + Nuxt 3 application wrapped in Electron. It is a **remote control** for the LivePlay audio server: it owns no audio decoding, no playback, no Web Audio nodes. Every user action turns into a REST call or WebSocket frame sent to `liveplay-server`, and every meter / waveform / state update comes back the same way.

This document is the developer's guide to the client. For the audio engine, see [`server/README.md`](../server/README.md). For the overall project, see the [root README](../README.md).

---

## Contents

- [Tech stack](#tech-stack)
- [Source layout](#source-layout)
- [Running](#running)
- [Architecture](#architecture)
  - [The renderer ↔ Electron-main split](#the-renderer--electron-main-split)
  - [The renderer ↔ liveplay-server link](#the-renderer--liveplay-server-link)
  - [Local server lifecycle](#local-server-lifecycle)
- [Composables](#composables)
- [Components](#components)
- [Localisation (20 languages, RTL)](#localisation-20-languages-rtl)
- [Theming](#theming)
- [Auto-updates](#auto-updates)
- [Packaging](#packaging)
- [Adding features](#adding-features)

---

## Tech stack

| Layer            | Library                                                         |
|------------------|-----------------------------------------------------------------|
| Shell            | Electron 28                                                     |
| Renderer         | Nuxt 3 (Vue 3 Composition API, TypeScript, SCSS)                |
| UI primitives    | Material Symbols icons; in-house components — no UI framework   |
| Local server     | C++ `liveplay-server` binary spawned as a child process         |
| Media tooling    | `@ffmpeg-installer/ffmpeg`, `@ffprobe-installer/ffprobe`        |
| YouTube import   | `yt-dlp-wrap` + `youtube-search-api`                            |
| Updates          | `electron-updater` (GitHub releases provider)                   |
| File transport   | `archiver` + `extract-zip` for `.lpa` project archives          |

Audio playback, waveform extraction, metering and routing all live in the C++ server. The renderer never decodes audio.

---

## Source layout

```
client/
├── electron/
│   ├── main.js                  Electron main process: window, menu, IPC, server lifecycle, REST shim
│   ├── preload.js               contextBridge → `window.electronAPI`
│   └── preload-state-viewer.js  separate preload for the state-viewer popup window
├── components/                   Vue SFCs (all in one flat folder, ~33 files)
├── composables/                  Reactive state + server bindings
│   ├── useLiveplayServer.ts     singleton REST + WebSocket client — every component goes through this
│   ├── useLiveMeters.ts          meter subscription helpers
│   ├── useProject.ts             project CRUD as exposed by the server
│   ├── useAudioEngine.ts         transport facade (play / stop / fade / seek / ducking)
│   ├── useCartItems.ts           cart-grid state
│   ├── useCartHotkeys.ts         keyboard hotkey bindings → cart triggers
│   ├── useMidiController.ts      Web MIDI → cart triggers
│   ├── useStateViewer.ts         feeds the diagnostics popup window
│   └── useLocalization.ts        20-language i18n + RTL handling
├── plugins/                      Nuxt plugins (client.ts: auto-connect on boot)
├── locales/                      JSON locale files (21 files — en + 20 translations)
├── types/                        TypeScript DTOs (server.ts, project.ts, global.d.ts)
├── utils/                        Pure helpers (audio.ts: dB/RMS math, indexDisplay.ts: playlist index formatting)
├── public/                       Static assets — screenshots, fonts, icons
├── assets/                       Bundled styles (main.scss, variables.scss)
├── scripts/                      Workspace-local utilities (locale sync, etc.)
├── nuxt.config.ts                Nuxt configuration
├── tsconfig.json
└── package.json                  Electron + electron-builder configuration
```

---

## Running

From the monorepo root, `npm run dev` (or `npm run dev --workspace=client`) does the full loop:

1. [`scripts/ensure-server.js`](../scripts/ensure-server.js) checks whether the C++ server is built; if not, builds it.
2. `nuxt dev` starts on `http://localhost:3000` with HMR.
3. `wait-on` waits for Nuxt, then `electron .` launches with DevTools open.

To work on just the renderer (no Electron shell): `npm run dev:nuxt` and visit `http://localhost:3000` in a browser. The renderer still tries to talk to a running `liveplay-server`; start one separately with `npm run server:run` from the monorepo root.

Production build (Nuxt static generate + electron-builder):

```sh
npm run build:electron        # in client/, or `npm run build:client:electron` at the root
```

Outputs land in `client/dist-electron/`. The root `npm run build` script copies the installers from there into `build/` at the repo root.

---

## Architecture

### The renderer ↔ Electron-main split

The renderer is a sandboxed Nuxt SPA with `nodeIntegration: false` and `contextIsolation: true`. The Electron main process exposes a controlled surface via [`electron/preload.js`](electron/preload.js) → `window.electronAPI`. Anything that needs Node.js (file dialogs, child processes, OS integration, auto-update, native menu) goes through an IPC handler in [`electron/main.js`](electron/main.js).

Key IPC channels (non-exhaustive):

| Channel                               | Purpose |
|---------------------------------------|---------|
| `liveplay-server:get-config` / `set-config` | Read/write the persisted server connection settings. |
| `liveplay-server:get-status` / `ensure-running` / `restart` / `shutdown` | Manage the bundled server child process. |
| `liveplay-discovery:start` / `list`   | Browse for `liveplay-server` instances on the LAN. |
| `select-project-folder` / `select-project-file` / `select-audio-files` | Native file pickers. |
| `read-file` / `write-file` / `copy-file` / `read-audio-file` | Project filesystem helpers (binary + text). |
| `export-project` / `import-project` / `import-lpa-file` | `.lpa` archive round-trip (zip-based project bundle). |
| `check-for-updates` / `download-update` / `install-update` / `get-app-version` | `electron-updater` controls. |
| `update-menu-language` / `get-system-locale` / `get-available-locales` / `get-locale-data` | Dynamic menu localisation. |
| `open-folder` / `open-external` / `app:relaunch` / `app:exit` | OS integration. |
| `open-cart-player-window` / `cart-player-window-attach` / `sync-project-data` | Second-window cart-player surface. |

The audio data path is **not** via IPC — it's directly between the renderer and `liveplay-server` over HTTP + WebSocket. IPC is used only for things Electron needs to do as a desktop application.

### The renderer ↔ liveplay-server link

`composables/useLiveplayServer.ts` is the single source of truth. It is a Vue singleton — every component that calls `useLiveplayServer()` receives the **same** WebSocket connection and the **same** reactive state. The contract:

- The server URL is read from `localStorage` (`liveplay.serverUrl`, default `http://127.0.0.1:4480`). Change it via the **Server Settings** modal.
- On boot, the [`plugins/liveplay-server.client.ts`](plugins/liveplay-server.client.ts) plugin connects. The connection is lazy-retried if it drops (showing `ConnectionLostModal` in the meantime).
- REST calls return promises; WebSocket frames update reactive refs.
- Outbound frames are mostly transport commands (`play`, `stop`, `seek`) that take a fast WS path to avoid the HTTP round-trip; everything mutating goes through `PATCH /api/project/...` so the server can echo a `doc_patch` to all connected clients.

For the full REST and WebSocket surface, see [`server/README.md`](../server/README.md#control-surface).

### Local server lifecycle

When LivePlay is installed as a desktop app, [`electron/main.js`](electron/main.js) is also responsible for spawning the bundled server. The recipe:

1. `electron-builder` copies `liveplay-server[.exe]` into `resources/server-bin/` via `extraResources` (see the `build` block in `package.json`).
2. On first launch, main resolves the binary path and spawns it as a detached child process bound to `127.0.0.1:<port>`.
3. A lockfile records the PID so subsequent launches reattach to the running instance rather than spawning a duplicate.
4. The server is shut down cleanly (Ctrl-Break / SIGTERM, with a hard kill fallback) when the last LivePlay window closes.
5. The "Server Settings" UI can be pointed at a remote server, in which case the local child process is killed and the client connects over the LAN instead.

`liveplay-discovery:*` IPC channels run a UDP listener that picks up announce broadcasts from `liveplay-server` instances on the LAN, so the connection UI can present a one-click list.

---

## Composables

All composables are Vue `setup()`-time helpers, typed in TypeScript.

| Composable             | Responsibility |
|------------------------|----------------|
| `useLiveplayServer`    | REST + WS singleton. Holds connection state, project document, server config. Every other composable builds on this. |
| `useLiveMeters`        | Subscribes to the `meters` WS frame and exposes per-cue / per-mixer / per-master reactive refs at 60 Hz. Drives `LiveMeterBar`, `StereoMeter`, `VUMeter`. |
| `useProject`           | Project CRUD as exposed by the server (new, open, save, close, item add/remove/move/patch). Wraps `useLiveplayServer` calls into ergonomic methods. |
| `useAudioEngine`       | Transport facade: `playCue`, `stopCue`, `stopAllCues`, `seek`, `setVolume`, ducking mode helpers. All implemented by forwarding to the server — no audio runs in the renderer. |
| `useCartItems`         | The cart grid model (slot → cue mapping). |
| `useCartHotkeys`       | Configurable keyboard shortcuts → cart triggers. See `CartHotkeyConfig.vue` for the UI. |
| `useMidiController`    | Web MIDI bindings → cart triggers. See `ControlConfigModal.vue`. |
| `useStateViewer`       | Feeds the live diagnostics popup window (project doc + connection + server status). |
| `useLocalization`      | i18n (20 languages, RTL). See [Localisation](#localisation-20-languages-rtl). |

**Rule of thumb**: components don't import `useLiveplayServer` directly unless they're presenting a low-level diagnostic. They use one of the facades above so the surface stays small and testable.

---

## Components

The component tree is intentionally flat — every SFC lives directly in [`components/`](components/). The big ones to know:

- `WelcomeScreen.vue` — project picker before a project is loaded.
- `MainWorkspace.vue` — top-level layout once a project is loaded.
- `PlaybackControls.vue`, `ActiveCueItem.vue` — top-of-screen transport.
- `PlaylistView.vue`, `PlaylistItem.vue` — recursive playlist tree.
- `CartPlayer.vue`, `CartSlot.vue` — 16-slot cart grid (cart-player can pop out into its own window).
- `PropertiesPanel.vue` — properties for the selected item (gain, fades, behaviours, ducking).
- `WaveformCanvas.vue` — canvas-rendered waveform fetched from `GET /api/waveform/<cueId>`.
- `WaveformTrimmer.vue` — interactive in/out trimming + normalise.
- `RoutingMatrixPanel.vue` — the 3-tier routing matrix UI.
- `LiveMeterBar.vue`, `StereoMeter.vue`, `VUMeter.vue` — meter widgets driven by `useLiveMeters`.
- `ServerSettingsModal.vue`, `LocalServerStatus.vue`, `ConnectionLostModal.vue` — server connection management.
- `ServerFileBrowser.vue`, `ServerFilePickerModal.vue` — `GET /api/fs/list` browser, used when the client and server live on different machines.
- `AudioImportModal.vue`, `YouTubeImportModal.vue` — media import surfaces.
- `ProjectSelectionModal.vue`, `ProjectSettingsModal.vue`, `ProjectRepairModal.vue` — project management.
- `UpdateModal.vue` — auto-update UI.
- `AboutModal.vue`, `ProgressModal.vue`, `LoadingOverlay.vue`, `LocationChoiceModal.vue` — misc.

Style: Composition API + `<script setup lang="ts">`, scoped SCSS, CSS variables for theming (see [Theming](#theming)).

---

## Localisation (20 languages, RTL)

LivePlay ships with [`client/locales/*.json`](locales/) — one file per language. Currently shipped: **en, ar, bn, de, el, es, fa, fr, hi, it, ja, ko, no, pt, ro, ru, sq, sv, tr, ur, zh**. Arabic, Farsi and Urdu use RTL layout.

### Using translations in a component

```vue
<script setup lang="ts">
const { t, currentLocale, getDirection } = useLocalization();
</script>

<template>
  <button>{{ t('menu.newProject') }}</button>
  <p>{{ t('welcome.subtitle') }}</p>
</template>
```

Keys are dot-notation paths into the locale JSON tree. Missing keys silently fall back to English.

### Locale-file structure

Each locale starts with a `_metadata` block:

```json
{
  "_metadata": { "code": "en", "name": "English", "nativeName": "English", "direction": "ltr" },
  "app": { … },
  "menu": { … },
  "welcome": { … },
  …
}
```

`direction: "rtl"` triggers `dir="rtl"` on the root element and the RTL CSS rules in `assets/styles/main.scss`.

### Adding a language

1. Copy `locales/en.json` to `locales/<code>.json`.
2. Update `_metadata` (`code`, `name`, `nativeName`, `direction`).
3. Translate the values; don't change the keys.
4. From the monorepo root: `node scripts/sync-locale-keys.js` to backfill any keys you missed from `en.json`.
5. The language appears automatically in the **View → Language** menu — `useLocalization` discovers locales at runtime, and the Electron menu is generated from the same source via the `get-available-locales` / `get-locale-data` IPC channels.

### Persistence

The chosen locale is stored in `localStorage` under `liveplay-locale` and synced into the Electron menu on launch.

---

## Theming

All colours and spacing flow through CSS custom properties in [`assets/styles/main.scss`](assets/styles/main.scss):

```scss
[data-theme='dark'] {
  --color-background: #161616;
  --color-surface:    #262626;
  --color-text-primary: #f4f4f4;
  --color-accent: var(--color-accent-custom, #da1e28);
}
```

Custom accent colours are set at runtime:

```ts
document.documentElement.style.setProperty('--color-accent-custom', '#0066FF');
```

Theme mode + accent colour are persisted on the project, not per-user — every operator opening the same project sees the same look. The UI lives in `ProjectSettingsModal.vue`.

---

## Auto-updates

In production builds, the client checks GitHub releases on startup via `electron-updater` (3 second post-launch delay). If a newer version exists:

1. `UpdateModal.vue` shows the current and new version, with the release notes.
2. The user picks **Download and install** (immediate progress + restart) or **Install on exit** (silently downloads, applies on next quit).
3. Auto-update is **disabled in development** to prevent false notifications during local builds.

For the release to be installable, the `latest.yml` / `latest-mac.yml` / `latest-linux.yml` metadata files must be attached to the GitHub release — `electron-builder` produces these automatically and the [release workflow](../.github/workflows/build-release.yml) uploads them along with the installers.

Update IPC: `check-for-updates`, `download-update`, `install-update`, `get-app-version` (see [The renderer ↔ Electron-main split](#the-renderer--electron-main-split)).

---

## Packaging

The `build` block in [`package.json`](package.json) drives `electron-builder`:

- `appId`: `com.liveplay.app`
- `productName`: `LivePlay`
- `files`: includes `.output/`, `electron/`, `assets/`, `locales/`. Locales must be listed explicitly or they don't ship.
- `extraResources`: copies the C++ server binary into `resources/server-bin/`.
- `asarUnpack`: the ffmpeg/ffprobe installers can't run from inside an asar, so they're unpacked.
- `fileAssociations`: registers the `.liveplay` extension.

To build locally:

```sh
npm run build:electron               # Nuxt generate + electron-builder for the host
npm run electron:build -- --win --x64    # explicit platform/arch flags
```

For multi-platform builds, use the [GitHub Actions release workflow](../.github/workflows/build-release.yml) — cross-compiling Electron locally is unreliable.

---

## Adding features

### A new component

1. Create `components/MyThing.vue` with `<script setup lang="ts">`.
2. If it needs server state, use a composable (not `useLiveplayServer` directly) — add the new method to the composable rather than calling REST inline.
3. Add translation keys to `locales/en.json`; run `node ../scripts/sync-locale-keys.js` to backfill the other languages.
4. Use CSS variables for colours and spacing; scoped SCSS for styling.

### A new server-backed action

1. Add the REST/WS handler on the server side (see [`server/README.md`](../server/README.md#adding-features)).
2. Add the matching method to `composables/useLiveplayServer.ts`.
3. Expose it through one of the facade composables (`useProject`, `useAudioEngine`, …) if it's user-facing.
4. Wire it into a component.

### A new IPC handler

Use IPC **only** for capabilities that genuinely need the Electron main process (file dialogs, OS integration, the local server child process, updater). Anything that touches audio or the project document goes through `liveplay-server`.

1. `ipcMain.handle('my-channel', async (event, …args) => { … })` in [`electron/main.js`](electron/main.js).
2. Expose it via `contextBridge.exposeInMainWorld('electronAPI', { … })` in [`electron/preload.js`](electron/preload.js).
3. Add the type to `types/global.d.ts`.
4. Call `window.electronAPI.myChannel(...)` from the renderer.

### Debugging

- **Renderer**: open DevTools in the running Electron app (Ctrl/Cmd+Shift+I); or hit `http://localhost:3000` in a browser during `npm run dev`.
- **Main process**: `console.log` lands in the terminal that started Electron.
- **IPC tracing**: add a log inside the handler and inside the call site — handy when wiring up a new channel.
- **Server traffic**: open the WebSocket frame inspector in DevTools (Network → WS).
- **Live state**: open the diagnostics popup via the View menu — driven by `useStateViewer` and its separate preload.

For the audio/transport side of any bug (timing, fades, routing, meters), the issue is almost always server-side — see [`server/README.md`](../server/README.md).
