# LivePlay server

The C++20 server owns audio playback, the active project, hardware routing, users and network control. The desktop client is a controller.

[Build instructions](../docs/DEVELOPMENT.md) · [API reference](https://tdoukinitsas.github.io/liveplay/api/) · [Bus architecture](../docs/BUS_ARCHITECTURE.md) · [Ownership](../docs/OWNERSHIP_MODEL.md)

## Run

From the repository root:

```sh
npm run server:build
npm run server:run -- --port 4480
```

The Windows Release binary is `server/build/Release/liveplay-server.exe`; the default Ninja build produces `server/build/liveplay-server`. Use `--help` for launch syntax.

The standalone server binds to `0.0.0.0:4480` by default. HTTP and WebSocket share that listener; discovery uses UDP 4481. Use `--bind 127.0.0.1` for a local-only listener. Audio requires a usable playback device.

## Boot configuration

Precedence, highest first:

1. Command-line flags.
2. Recognized `LIVEPLAY_*` environment variables.
3. JSON configuration file.
4. Built-in defaults.

The file is selected by `--config <path>`, then `LIVEPLAY_CONFIG`, then `liveplay.json` beside the executable. It is a sparse object with `schema_version: 1`; omit keys to use defaults. Invalid keys/values are logged and ignored at boot.

All current server-config fields require a restart. The settings API distinguishes effective values, stored overrides and their provenance. Its complete field types/ranges live in the [ServerConfig schema](https://tdoukinitsas.github.io/liveplay/api/#schema-serverconfig).

| CLI flag | Environment variable | Default |
|---|---|---|
| `--port`, `-p` | `LIVEPLAY_PORT` | 4480 |
| `--bind`, `-b` | None | `0.0.0.0` |
| `--meter-hz` | `LIVEPLAY_METER_HZ` | 30 |
| `--max-upload-mb` | `LIVEPLAY_MAX_UPLOAD_MB` | 256 MiB |
| `--mix-sample-rate` | `LIVEPLAY_MIX_SAMPLE_RATE` | 48000 Hz |
| `--render-block` | `LIVEPLAY_RENDER_BLOCK` | 256 frames |
| `--ring-blocks` | `LIVEPLAY_RING_BLOCKS` | 6 |
| `--master-channels` | `LIVEPLAY_MASTER_CHANNELS` | 32 |
| `--max-buses` | `LIVEPLAY_MAX_BUSES` | 64 |
| `--master-ceiling-db` | `LIVEPLAY_MASTER_CEILING_DB` | -0.3 dB |
| `--fs-root` (repeatable) | `LIVEPLAY_FS_ROOTS` | No configured roots |
| `--cors-origin` | `LIVEPLAY_CORS_ORIGIN` | `*` |
| `--verbose`, `-v` | None | false |
| `--lock-server-config` | `LIVEPLAY_LOCK_SERVER_CONFIG` | false |
| `--silent` | None | false |

`--silent` runs the server without a console window and puts an icon in the system tray (the menu bar on macOS) instead. The icon belongs to the server, so it stays while the server runs even after the desktop app quits. Its menu shows the port and PID and has Show console, Open log folder and Stop server. On Windows, Show console brings the real console back (Hide console releases it again). On macOS and Linux it opens the live log in a terminal. On Linux the icon needs `libayatana-appindicator3` (or `libappindicator3`) and a desktop session; without them the server runs headless and logs why. The desktop app passes `--silent` when **Settings → Server → Run the server silently in the system tray** is on, or when the app itself is started with `--silent`.

Environment root lists use `;` on Windows and `:` elsewhere. In JSON, `fsRoots` is an array. `lockServerConfig` is a launch/file setting, not an API-writable field; when enabled, it blocks configuration edits even by administrators. It does not lock every other server operation.

Example local-only launch configuration:

```json
{
  "schema_version": 1,
  "bind": "127.0.0.1",
  "meterHz": 30,
  "renderBlock": 256,
  "ringBlocks": 6
}
```

The path guard applies to selected filesystem operations; it is not a comprehensive sandbox for every media reference inside projects. See [audit findings](../docs/API_AUDIT.md).

## Services

| Component | Responsibility |
|---|---|
| `src/main.cpp` | Configuration precedence, startup/shutdown, service wiring and crash resume |
| `audio/engine.cpp` | Render loop, topology snapshots, device output rings and routing |
| `audio/playback_item.cpp` | Decode buffers, cue transport, looping and envelopes |
| `audio/mixer_channel.cpp` | Mixer gain, fades and meters |
| `audio/ltc_generator.cpp` | Per-cue LTC signal |
| `core/project_state.cpp` | Show document, cue/bus materialization and transport coordination |
| `core/output_map.cpp` | Logical output names to physical device channels |
| `core/user_store.cpp` | Accounts, password hashes and signed/API tokens |
| `core/user_prefs.cpp` | Sparse per-user profiles |
| `core/server_config.cpp` | Configuration schema, validation and persisted overrides |
| `core/backup_manager.cpp` | Periodic copies of the saved project |
| `net/control_server.cpp` | HTTP handlers, WebSocket dispatch, events and meters |
| `net/discovery.cpp` | LAN announcements |
| `meta/` | Metadata and waveform computation |

Dependencies include miniaudio, Crow, TagLib, miniz, libsodium and nlohmann/json. CMake and the vcpkg manifest specify the actual dependency set.

## Threads and real-time boundaries

| Work | Execution |
|---|---|
| HTTP handlers / control commands | Crow worker threads; ProjectState serializes document access |
| WebSocket state and metering | ControlServer broadcast thread |
| Audio mix | Engine render thread |
| Device consumption | miniaudio callbacks reading output rings |
| File decoding | Playback decode workers |
| Metadata/waveforms | Handler or asynchronous work, depending on endpoint |
| Project backups | BackupManager worker |

Topology changes are constructed on the control side and published as snapshots. DSP parameters publish coefficient sets; filter changes ramp in the render path. Device output callbacks consume buffered samples. Avoid filesystem work, blocking locks and allocation in audio callbacks.

## Project lifecycle

- One project is active per server. Loading/closing changes the shared show for every controller.
- Item UUIDs are document identity; engine cue IDs are runtime identity.
- Relative `mediaPath` resolves against the project folder before an absolute `mediaServerPath` fallback.
- The actual opened/saved file determines `folderPath`; the runtime folder annotation is omitted from the saved document.
- API responses add runtime information such as cue IDs, output bindings and derived target levels.
- The header omits items and buses; paged item reads contain top-level items with nested children.
- Saving writes the document; Save As does not copy audio.
- Backups copy the saved file every 10 minutes into `backups/`, retaining at most 20 backup files in that directory.
- Supported compatibility conversions happen on project load and are reported in the reference's Migration schema. Do not treat obsolete planning documents as a file-format contract.

HTTP, WebSocket, project fields, status codes and payload examples are maintained only in the [API reference](https://tdoukinitsas.github.io/liveplay/api/).
