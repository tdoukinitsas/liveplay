# LivePlay

![Main liveplay user interface, with playlist editor, cue cart and properties panel](client/public/screenshots/liveplay_screenshot.jpg)

LivePlay is a free, open-source audio playback system for live sound operators who need reliable, flexible cue management. As of v2.0 it ships as a **decoupled client/server** application:

- **`/server`** — a headless, cross-platform C++ application that owns the audio engine, project state, REST/WebSocket control plane, and metadata/waveform analysis.
- **`/client`** — the existing Vue 3 / Nuxt 3 / Electron desktop application, refactored to act as a remote control over WebSocket and REST. No audio is played in the renderer process.

Splitting the audio engine out of the renderer process gives LivePlay direct access to ASIO/Core Audio/ALSA, true multi-device output routing, native DSP (limiter, LTC, metering at audio-callback rate), and independent per-cue playback instances — none of which were achievable through Chromium's Web Audio sandbox.

**Available in 20 languages** with full RTL support.

---

## Table of Contents

- [Architecture](#architecture)
  - [Topology](#topology)
  - [The 3-Tier Internal Mixer](#the-3-tier-internal-mixer)
  - [Multi-Device Routing Matrix](#multi-device-routing-matrix)
  - [LTC Generation](#ltc-generation)
  - [Manual-Stop Fade-Out Contract](#manual-stop-fade-out-contract)
  - [Brick-Wall Master Limiter](#brick-wall-master-limiter)
  - [Real-Time Metering (3 Stages)](#real-time-metering-3-stages)
  - [Network Event Lifecycle](#network-event-lifecycle)
  - [Project File Backwards Compatibility](#project-file-backwards-compatibility)
- [Repository Layout](#repository-layout)
- [Local Development](#local-development)
  - [Prerequisites](#prerequisites)
  - [Server (C++)](#server-c)
  - [Client (Vue / Nuxt / Electron)](#client-vue--nuxt--electron)
  - [Running both concurrently](#running-both-concurrently)
- [Production Build & Packaging](#production-build--packaging)
- [CI / GitHub Actions](#ci--github-actions)
- [Contributing](#contributing)
- [License](#license)

---

## Architecture

### Topology

```
+-----------------------------------+      WebSocket (ws://host:4480/ws)      +-----------------------------------+
|  /client                          | <---------------- meters @ ~60 Hz ----> |  /server  (liveplay-server)       |
|  Electron + Nuxt 3 + Vue 3        | <--- transport / route commands -----   |  C++20, miniaudio, Crow, TagLib   |
|                                   |       REST  (http://host:4480/api/*)    |                                   |
|  + UI, cart grid, properties,     | <--- list / load / upload / waveform -> |  + AudioEngine                    |
|    routing matrix UI              |                                         |  + ProjectState                   |
|  + WaveformCanvas (from /api/     |                                         |  + ControlServer (REST + WS)      |
|    waveform/:cueId)               |                                         |  + Metadata / Waveform services   |
|  + LiveMeterBar  (from /ws meter  |                                         |                                   |
|    broadcasts)                    |                                         |  Native devices:                  |
|                                   |                                         |    Win → WASAPI                   |
|  No Howler. No WaveSurfer.        |                                         |    Mac → CoreAudio                |
|  No Web Audio decoding.           |                                         |    Linux → ALSA / Pulse           |
+-----------------------------------+                                         +-----------------------------------+
```

Client and server can run on the **same machine** (a single-machine LivePlay install bundles both) or on **different machines** on the same network (e.g. show laptop runs the client, a stage-side mini-PC runs the server with audio interfaces plugged in).

### The 3-Tier Internal Mixer

The C++ engine routes audio through three explicit tiers. Every cue's audio is processed through each tier in sequence:

```
   Tier 1                Tier 2                  Tier 3
   ──────                ──────                  ──────
 PlaybackItem  ─send─►  MixerChannel  ─send─►  Master Output Bus  ─hw─►  Device:HwCh
   (one per                (group bus,            (per-channel
   live cue,               gain/mute/             brick-wall
   own decoder)            solo/fade)             limiter)
```

- **Tier 1 — `PlaybackItem`** ([server/include/liveplay/audio/playback_item.hpp](server/include/liveplay/audio/playback_item.hpp))
  Each cue gets its own `PlaybackItem` with its own `ma_decoder`, gain/fade state, LTC generator, and per-source-channel meter. Loading the same `.wav` into two cart slots yields **two independent instances** — this is the fix for the LivePlay 1.x state-sharing bug where attenuating one cart silently attenuated the other.
- **Tier 2 — `MixerChannel`** ([server/include/liveplay/audio/mixer_channel.hpp](server/include/liveplay/audio/mixer_channel.hpp))
  A virtual mixer strip with gain, mute, solo, and a smooth-fade ramp. Multiple items can route into the same channel; multiple source channels of one item can route to different channels.
- **Tier 3 — Master Output Bus** ([server/include/liveplay/audio/engine.hpp](server/include/liveplay/audio/engine.hpp))
  Up to 64 logical master channels (configurable). Each carries a brick-wall limiter + meter and is assigned to exactly one `(Device, HardwareChannelIndex)` tuple.

All three tiers run on the engine's render thread at a 256-frame block (≈ 5.3 ms at 48 kHz). Meters and limiter envelopes update once per block.

### Multi-Device Routing Matrix

LivePlay supports **multiple sound cards simultaneously** with full source-channel splitting. The matrix is sparse and serialisable; it round-trips through `/api/project`.

The matrix entries are:

| Stage | Mapping | Persisted as |
|---|---|---|
| Item → Mixer  | per-source-channel sends with linear gain | `item_routes` |
| Mixer → Master | per-master-channel sends with linear gain | `mixer_routes` |
| Master → Device | exactly one `(device_id, hw_channel)` per master | `master_assignments` |

Example: a stereo MP3 (`L`, `R`) on a 4-channel cue can simultaneously feed FOH (Device A: ch 0+1) and a stage monitor mix (Device B: ch 2+3) **with different gains** by wiring `L → MixerA → Master0 → DevA:0` plus `L → MixerB → Master2 → DevB:2` and similarly for `R`.

UI: see [`client/components/RoutingMatrixPanel.vue`](client/components/RoutingMatrixPanel.vue). Backed by REST routes `/api/routing/item_to_mixer`, `/api/routing/mixer_to_master`, `/api/routing/master_to_device`.

### LTC Generation

Per-cue procedural SMPTE Linear Timecode ([server/include/liveplay/audio/ltc_generator.hpp](server/include/liveplay/audio/ltc_generator.hpp)). When enabled on a cue, the engine adds a synthetic **extra source channel** appended after the file's real channels. That channel can be routed anywhere through the matrix — to one of FOH's outputs, to a dedicated 3.5 mm jack, to a different device entirely.

Supported frame rates: 24, 25, 29.97 NDF, 29.97 DF, 30. Drop-frame handling is implemented in `LTCGenerator::timecode_for_frame`.

Offset: any non-negative `chrono::nanoseconds`. So a cue can be set up to emit `01:00:00:00` at its first sample, sync-locked to the cue's playhead afterwards.

### Manual-Stop Fade-Out Contract

The 1.x client only honoured the user-defined fade-out on **natural** end-of-file. LivePlay v2 funnels all three stop paths through the same envelope:

- `PlaybackItem::stop()` (user pressed Stop, or master Stop-All) — honours `fade_out_duration`.
- `PlaybackItem::stop_now()` — emergency panic, no fade.
- Natural end-of-file inside `render_block()` — honours `fade_out_duration`.

The implementation enforces the contract: `stop()` calls `start_fade()` which feeds the same state machine the natural-end branch uses. See [server/src/audio/playback_item.cpp](server/src/audio/playback_item.cpp) (search for the `// Manual-stop fade contract:` comment).

### Brick-Wall Master Limiter

Each master channel carries a lookahead brick-wall limiter ([server/include/liveplay/audio/limiter.hpp](server/include/liveplay/audio/limiter.hpp)). Default settings:

- Ceiling: **−0.3 dBFS**
- Lookahead: **5 ms** (≈ 240 samples at 48 kHz)
- Release: **50 ms**

The detector runs a sliding-max peak window with O(1) amortised update; the gain envelope snaps down within the lookahead and one-pole-releases back to unity. Output is mathematically clamped to the ceiling so clipping is impossible for finite inputs. This replaces the legacy "reduce every cue by 6 dB" hack from 1.x.

### Real-Time Metering (3 Stages)

Every tier carries its own `Meter` ([server/include/liveplay/audio/meter.hpp](server/include/liveplay/audio/meter.hpp)) — VU-style ballistics with attack/release peak envelope plus a leaky-integrator RMS over ~300 ms. Audio thread pushes blocks via `push_block()`; the meter publishes lock-free atomics that the broadcast thread reads at 60 Hz.

The WebSocket frame the client receives:

```json
{
  "type": "meters",
  "items": [
    {
      "cue_id": "…",
      "transport": 1,
      "playhead_seconds": 12.43,
      "sources": [
        { "peak_db": -3.1, "rms_db": -9.2 },
        { "peak_db": -4.2, "rms_db": -9.5 }
      ]
    }
  ],
  "mixer_channels": [
    { "mixer_id": "…", "peak_db": -6.0, "rms_db": -12.0 }
  ],
  "master_channels": [
    { "index": 0, "peak_db": -0.3, "rms_db": -8.1, "gain_reduction_db": -1.4 }
  ]
}
```

The 1.x "cheat" — sampling the static waveform thumbnail's amplitude at the playhead — is gone. Live metering is real audio-callback amplitude.

### Network Event Lifecycle

A typical cue trigger from the operator's point of view:

```
1. User presses Cart #3.
2. CartSlot.vue → useLiveplayServer().play(cueId)
3. WebSocket frame { "type": "play", "cue_id": "…" } sent to server.
4. ControlServer::handle_ws_message → engine_.play(cueId) — O(1) atomic state flip.
5. AudioEngine render thread, next block:
     PlaybackItem.render_block() decodes + applies fade-in envelope.
     Mixer summing through the matrix.
     Master limiter + meter on per-device output buffers.
     Buffers pushed into each device's ma_pcm_rb (ring buffer).
6. Per-device miniaudio callback drains the ring → hardware DAC.
7. Render thread also pushes amplitude into the 3-tier meters.
8. Broadcast thread, ~16 ms later: snapshot all meters → JSON → fan out to every WS client.
9. Client's LiveMeterBar.vue reflects the new levels in the next frame paint.
```

Total round-trip from keypress to first sample at the DAC is dominated by the chosen `render_block` size + the device's hardware buffer — typically well under 20 ms on Windows with default WASAPI settings.

### Project File Backwards Compatibility

The legacy 1.x `.liveplay` project file format is auto-upgraded on load by `ProjectState::upgrade_legacy_document` ([server/src/core/project_state.cpp](server/src/core/project_state.cpp)). The translator:

- Walks legacy `carts` / `playlist` arrays and reconstructs the v2 `cues` list with the same names, file paths, gains, and fade durations.
- Synthesises stereo master assignments: master channel 0 → default device hw ch 0, master channel 1 → default device hw ch 1.
- Auto-creates one per-cue mixer channel so each cue still has independent gain/fade.

The result: existing `.liveplay` projects open and play identically. Operators can then open the new Routing UI to split out source channels or send to additional devices.

---

## Repository Layout

```
liveplay/
├── client/                          Vue 3 / Nuxt 3 / Electron desktop UI
│   ├── components/                  Vue SFCs
│   │   ├── WaveformCanvas.vue       (NEW) canvas-rendered waveform
│   │   ├── ServerFileBrowser.vue    (NEW) /api/fs/list browser
│   │   ├── RoutingMatrixPanel.vue   (NEW) 3-tier routing UI
│   │   ├── LiveMeterBar.vue         (NEW) live WS meter widget
│   │   ├── ServerSettingsModal.vue  (NEW) server URL config
│   │   └── …existing 1.x components…
│   ├── composables/
│   │   ├── useLiveplayServer.ts     (NEW) REST + WS client (singleton)
│   │   ├── useLiveMeters.ts         (NEW) meter-subscription helpers
│   │   └── …existing composables…
│   ├── plugins/
│   │   └── liveplay-server.client.ts  Nuxt plugin: auto-connect on boot
│   ├── types/
│   │   ├── server.ts                (NEW) server DTOs
│   │   └── project.ts               existing project model
│   ├── electron/                    Electron main process + preload
│   ├── nuxt.config.ts
│   └── package.json
│
├── server/                          C++ audio engine + control server
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── vcpkg.json
│   ├── include/liveplay/
│   │   ├── logger.hpp
│   │   ├── audio/  (types, meter, limiter, ltc_generator, mixer_channel, playback_item, engine)
│   │   ├── core/   (project_state)
│   │   ├── meta/   (metadata, waveform)
│   │   └── net/    (control_server)
│   └── src/
│       ├── main.cpp                 banner, CLI, signal handling
│       ├── logger.cpp
│       ├── audio/                   miniaudio_impl.c + all .cpp files
│       ├── core/project_state.cpp
│       ├── meta/                    metadata.cpp, waveform.cpp
│       └── net/control_server.cpp
│
├── .github/workflows/
│   ├── build-server.yml             C++ matrix (Win MSVC, macOS Clang, Linux GCC)
│   └── build-release.yml            Client (electron-builder) — paths updated to /client
│
├── package.json                     workspaces root (orchestrator scripts)
├── README.md                        this file
├── LICENCE.txt                      AGPL-3.0
├── docs-site/, guides/, openspec/   unchanged
└── .gitignore                       client + server build outputs
```

---

## Local Development

### Prerequisites

| Tool | Minimum | Notes |
|------|---------|-------|
| Git  | any     | with submodule support |
| Node.js | 20 LTS | for the client |
| CMake | 3.21   | for the server |
| C++ toolchain | C++20 | MSVC 2022 / Clang 15+ / GCC 12+ |
| vcpkg | any recent | `git clone https://github.com/microsoft/vcpkg && cd vcpkg && ./bootstrap-vcpkg.{sh,bat}` |
| Ninja | latest | strongly recommended (`brew install ninja`, `choco install ninja`, `apt install ninja-build`) |

Export `VCPKG_ROOT` to point at your vcpkg checkout — the server CMake reads it:

```sh
# Linux / macOS
export VCPKG_ROOT="$HOME/dev/vcpkg"
echo 'export VCPKG_ROOT="$HOME/dev/vcpkg"' >> ~/.zshrc

# Windows (PowerShell)
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\dev\vcpkg", "User")
```

Linux only — also install the system audio dev headers miniaudio links against:

```sh
sudo apt install libasound2-dev libpulse-dev libjack-jackd2-dev pkg-config
```

### Server (C++)

```sh
cd server
cmake --preset default          # downloads deps via vcpkg; ~5 min first time
cmake --build build --config Release -j
./build/liveplay-server          # binary at build/liveplay-server[.exe]
```

Debug build (with assertions + symbols):

```sh
cd server
cmake --preset debug
cmake --build build-debug -j
./build-debug/liveplay-server --verbose   # enables DBUG-level logging
```

Live-rebuild loop (uses `entr` on Linux/macOS or PowerShell `fswatch` on Windows):

```sh
# macOS / Linux
find server/src server/include -name '*.cpp' -o -name '*.hpp' -o -name '*.h' | entr -c sh -c 'cmake --build server/build -j && ./server/build/liveplay-server'
```

CLI flags:

```
liveplay-server [options]
  -p, --port <port>     Port to listen on (default 4480)
  -b, --bind <addr>     Interface to bind (default 0.0.0.0)
  -v, --verbose         Enable debug-level logging
  -h, --help            Show this help and exit

Environment:
  LIVEPLAY_PORT         Same as --port
  NO_COLOR=1            Disable ANSI colour in logs (e.g. for log aggregators)
  FORCE_COLOR=1         Force colour even when stdout is not a tty
```

The server prints a startup banner showing the LAN address to point the client at:

```
  Listening
    REST       http://0.0.0.0:4480
    WebSocket  ws://0.0.0.0:4480/ws
    LAN reach  http://192.168.1.42:4480
```

### Client (Vue / Nuxt / Electron)

```sh
cd client
npm install
npm run dev         # nuxt dev + electron-on-localhost:3000 (via concurrently)
```

The client reads the server URL from `localStorage`'s `liveplay.serverUrl` key (default `http://127.0.0.1:4480`). Use the in-app **Server Settings** modal ([`ServerSettingsModal.vue`](client/components/ServerSettingsModal.vue)) to switch to a remote server.

The `useLiveplayServer` composable is a **singleton** — every component that calls it receives the same WebSocket and the same reactive state.

### Running both concurrently

From the **repository root**, the orchestrator `package.json` provides:

```sh
npm install                      # installs client deps via npm workspaces

# server lifecycle (requires VCPKG_ROOT to be set)
npm run server:configure         # one-time CMake configure
npm run server:build             # rebuild C++ after edits
npm run server:run               # launch the compiled binary

# client lifecycle
npm run dev:client               # nuxt + electron
npm run build:client             # production nuxt generate
npm run build:client:electron    # full electron-builder packaging

# run both at once (server must already be built)
npm run dev:all
```

---

## Production Build & Packaging

LivePlay produces three artefacts per OS:

1. **`liveplay-server`** — the headless C++ binary (`liveplay-server.exe` / `liveplay-server`). Cross-compiled by `build-server.yml` in CI; downloaded from the Actions artefact list.
2. **Electron installer** — built by `build-release.yml` from `client/package.json`'s `build` block. Produces `.exe` (Windows), `.dmg`/`.zip` (macOS), `.AppImage`/`.deb`/`.rpm` (Linux).
3. **Bundle** — the two combined. The client's electron-builder config copies the server binary into the installer's `resources/` folder; on first launch, the Electron main process spawns it as a child process.

### Building the server binary for distribution

```sh
cd server
cmake -S . -B build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release -j
cmake --install build --prefix dist
# dist/bin/liveplay-server[.exe] is now ready to ship.
```

Strip the binary on Linux/macOS (optional, halves the size):

```sh
strip dist/bin/liveplay-server
```

### Bundling the server inside the Electron installer

The recipe (proposed for v2.1 — currently the server ships as a separate artefact during the migration window):

1. Build `liveplay-server` for each target OS using `build-server.yml`.
2. Download the per-OS binary into `client/electron/resources/server/`.
3. Reference it from `client/package.json`'s `build.extraResources`:

```json
"build": {
  "extraResources": [
    { "from": "electron/resources/server", "to": "server" }
  ]
}
```

4. In `client/electron/main.js`, spawn it at app start:

```js
const { spawn } = require('node:child_process');
const path      = require('node:path');
const serverBin = path.join(process.resourcesPath, 'server',
  process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server');
const child = spawn(serverBin, ['--port', '4480'], { stdio: 'inherit' });
app.on('before-quit', () => child.kill('SIGTERM'));
```

5. **Code signing** — sign the server binary alongside the Electron app: Windows via `signtool` (electron-builder does this automatically if the binary is in `extraResources`), macOS via the hardened runtime + the `entitlements` in the `build.mac` block (requires `com.apple.security.device.audio-input` for ASIO-like backends).

### Cross-platform notes

| Platform | Backend | Build notes |
|----------|---------|-------------|
| Windows  | WASAPI (default), DirectSound, WinMM | MSVC 2022. `ws2_32` + `ole32` + `winmm` linked automatically by the CMake target. |
| macOS    | Core Audio | Frameworks linked via CMake: CoreAudio, AudioToolbox, AudioUnit, CoreFoundation. Set `CMAKE_OSX_DEPLOYMENT_TARGET=12.0` for older macOS support. |
| Linux    | ALSA + PulseAudio (both compiled in; runtime fallback) | `libasound2-dev`, `libpulse-dev`, `libjack-jackd2-dev` headers needed. JACK is optional. |

ASIO support on Windows requires the **Steinberg ASIO SDK**, which has a redistribution licence incompatible with AGPL bundling. For now LivePlay uses WASAPI Exclusive Mode for low-latency Windows output. If you need ASIO, build miniaudio with `MA_ENABLE_ASIO` after locally placing the SDK at the expected path.

---

## CI / GitHub Actions

| Workflow | Triggers | What it does |
|----------|----------|--------------|
| [`build-server.yml`](.github/workflows/build-server.yml) | push/PR to `server/**`, manual | Matrix build (Win MSVC / macOS Clang / Linux GCC), smoke-tests `--help`, uploads `liveplay-server-{windows-x64,macos-arm64,linux-x64}` artefacts |
| [`build-release.yml`](.github/workflows/build-release.yml) | push to `main` touching `client/package.json` with a bumped `version` | electron-builder matrix, produces installers, creates a GitHub Release with auto-generated changelog |
| `deploy-docs.yml`   | docs-site/ changes | publishes the docs site |

GitHub's pre-installed vcpkg on the hosted runners is used (`VCPKG_INSTALLATION_ROOT` → `VCPKG_ROOT`) with the `x-gha,readwrite` binary cache backend so subsequent runs reuse compiled libraries.

---

## Contributing

1. Fork & clone.
2. Create a branch off `main` (`git checkout -b feat/something`).
3. Server changes: build + run with `cmake --build server/build && ./server/build/liveplay-server --verbose`. Client changes: `npm run dev --workspace=client`.
4. Open a PR. CI must pass.

Style:

- Server: C++20, `clang-format` (Google-ish), no exceptions in audio-callback code, atomics for hot params, RAII everywhere.
- Client: Vue 3 Composition API + TypeScript. New components consume `useLiveplayServer()` rather than `useAudioEngine()` where practical — the legacy in-process engine is being retired component-by-component.

The migration of the remaining 1.x components (`CartPlayer`, `PlaylistItem`, `WaveformTrimmer`, etc.) onto the server pipeline is tracked in [openspec/](openspec/). PRs welcome.

---

## License

[AGPL-3.0-only](LICENCE.txt). See the file header for the full text. Third-party dependencies retain their own licences (miniaudio: public domain / MIT-0; Crow: BSD-3; TagLib: LGPL-2.1+; nlohmann/json: MIT).
