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
npm install
npm run dev
npm run build:client
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
