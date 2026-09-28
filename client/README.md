# LivePlay client

The desktop client uses Electron, Nuxt 4 and Vue 3. It sends commands to the audio server and renders its state. Audio processing runs in the server.

[Development commands](../docs/DEVELOPMENT.md) · [Ownership guide](../docs/OWNERSHIP_MODEL.md) · [API reference](https://tdoukinitsas.github.io/liveplay/api/)

## Process boundaries

| Process | Responsibility |
|---|---|
| Nuxt renderer (`app/`) | Playlist, carts, properties, mixer, settings and meters |
| Electron main (`electron/main.js`) | Windows, native dialogs, local server lifecycle, filesystem integrations, media import and updates |
| Preload (`electron/preload.js`) | Explicit IPC bridge exposed to the renderer |
| Audio server | Playback, project authority, buses, output devices and network synchronization |

The desktop app can start/use a bundled local server or connect to a remote server. Local/remote selection and server address are machine settings. Relative project media paths belong to the server's filesystem.

Detached cart/mixer windows are additional controllers of the same server. Window geometry persists locally; reopening the app does not automatically reopen every detached window.

## Renderer map

| Location | Purpose |
|---|---|
| `app/app.vue` | Main workspace and top-level window behavior |
| `app/components/` | Playlist/cart views, mixer controls, dialogs and settings panes |
| `app/composables/useLiveplayServer.ts` | HTTP/WebSocket connection and server commands |
| `app/composables/useProject.ts` | Project state and synchronization |
| `app/composables/useAudioEngine.ts` | UI-facing playback operations |
| `app/composables/useLiveMeters.ts` | Meter delivery and display state |
| `app/composables/usePreferences.ts` | User profile retrieval/patching |
| `app/composables/useProjectSettings.ts` | Show settings |
| `app/composables/useShowControl.ts` | Show-control actions |
| `app/composables/useMidiController.ts` | MIDI input and bindings |
| `app/composables/useWorkspaceLayout.ts`, `useMixerView.ts` | Local workspace arrangement |
| `app/composables/useUiMode.ts` | Per-machine editing/show view |
| `app/composables/useLocalization.ts` | Translations and locale |
| `app/utils/` | Meter geometry, DSP display math and shared UI helpers |
| `locales/` | Application locale JSON |
| `tests/` | Window-bounds and settings-placement checks |

Settings panes are Appearance, Playback, Audio, Mixer, Outputs, Keyboard, Surfaces, Project, Server, Users and About. Keep hardware mapping in Outputs and boot policy in Server; use the ownership guide when adding a field.

## Run and package

From the repository root:

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
| `open-mixer-window` / `mixer-window-attach` | Second-window mixer surface. Carries no state of its own — that window opens its own WebSocket to the audio server. |

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
| `useCartHotkeys`       | Configurable keyboard shortcuts → transport actions and cart triggers. UI: `SettingsPaneKeyboard.vue`. Mounted by `MainWorkspace` (and by `CartPlayer` in the detached cart window), so bindings do not depend on the cart pane being visible. |
| `useMidiController`    | Web MIDI bindings → transport actions and cart triggers. UI: `SettingsPaneSurfaces.vue`. Mounted alongside `useCartHotkeys`. |
| `useSettingsPage`      | Which Settings section is showing, and the `#/settings/<section>` deep link. |
| `useProjectSettings`   | Reads `project.settings` and PATCHes it, for the Settings panes. |
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
- `MixerPanel.vue`, `MixerStrip.vue`, `MixerChannelDetails.vue` — the mixer: one channel strip per bus, with a per-bus detail view. Docks as a side pane, takes the full workspace, or pops out into its own window (`?mixerWindow=1`).
- `BusOutputSelect.vue`, `BusSendList.vue` — where a bus goes. The select is its one **output**, the whole signal at unity; the list is its **aux sends**, a copy each at its own level, taken before or after the bus's fader. Both ask the server rather than predicting it: the rules about cycles and the preview bus live there, and a 409 surfaces inline in the server's own words.
- `StereoMeter.vue`, `VUMeter.vue` — meter widgets driven by `useLiveMeters`. `StereoMeter` covers every source (cue, mixer strip, master pair) so zone colours, peak hold, the clip latch and the project's meter mode are identical wherever a level is shown.
- `SettingsPage.vue` + `SettingsPane*.vue` — the Settings page: a persistent section rail and one pane per section, deep-linkable as `#/settings/<section>`. Every user- and project-configurable value lives here; the modals that used to hold them (`ProjectSettingsModal`, `ControlConfigModal`, `ServerSettingsModal`, `OutputMapModal`, `AboutModal`) have all been retired into panes.
- `LocalServerStatus.vue`, `ConnectionLostModal.vue` — server connection management.
- `ServerFileBrowser.vue`, `ServerFilePickerModal.vue` — `GET /api/fs/list` browser, used when the client and server live on different machines.
- `AudioImportModal.vue`, `YouTubeImportModal.vue` — media import surfaces.
- `ProjectSelectionModal.vue`, `ProjectRepairModal.vue` — project management.
- `UpdateModal.vue` — auto-update UI.
- `ProgressModal.vue`, `LoadingOverlay.vue`, `LocationChoiceModal.vue` — misc.

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

The development command launches Nuxt and waits for its URL before launching Electron. Full installer builds use `npm run build`; packaging targets and bundled resources are in [package.json](package.json).

The renderer is generated into `client/.output/`. electron-builder packages it with Electron resources and the separately built server into `client/dist-electron/`; the root build collects installer artifacts into `build/`.

## State and integration

- Fetch authoritative project state after reconnect; WebSocket notifications are incremental and have no replay log.
- Store audio-affecting values in the project/server through their APIs.
- User profiles contain presentation/workflow preferences. Window placement, connection choices and local MIDI configuration live on the machine.
- UI changes to pan/DSP use live control while dragging and persisted patches for durable state.
- Server `custom_action_http` notifications are executed by connected desktop clients. Multiple clients can execute the same request; see [findings](../docs/API_AUDIT.md).
- Add desktop capabilities through the explicit preload bridge and corresponding IPC handler.
- Translation keys belong in `locales/en.json`; synchronize missing keys using `node scripts/sync-locale-keys.js` from the repository root.
- Keep EQ/dynamics visualization math aligned with the C++ DSP implementation.

Run the [targeted checks](../docs/DEVELOPMENT.md#checks) relevant to a change. The root/client manifests do not define a general lint or test script.
