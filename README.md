# LivePlay

![LivePlay main interface — playlist editor, cart grid and properties panel](client/public/screenshots/liveplay_screenshot.jpg)

**LivePlay is a free, open-source app for playing back audio in live shows.** If you run sound for theatre, conferences, houses of worship, AV installs, or any live event, LivePlay lets you line up your music, sound effects and stings ahead of time and fire them off reliably on the night — from a laptop, a touchscreen, or even a separate stage-side machine you control over the network.

You build a **show** as a list of cues plus a grid of one-touch buttons, then trigger them with a click, a tap, a keyboard shortcut, or a MIDI controller. LivePlay handles the fades, the transitions between tracks, and keeps a close eye on your levels so nothing clips or distorts.

It runs on **Windows, macOS and Linux**, and it's completely free.

> 📥 **[Download the latest version →](https://github.com/tdoukinitsas/liveplay/releases)** &nbsp;·&nbsp; 🌐 **[Website & docs](https://tdoukinitsas.github.io/liveplay/)**

---

## What you can do with it

- **🎵 Build a cue list** — arrange audio into a playlist with nested groups. Set a volume, trim the start and end, add fade-ins and fade-outs, and choose what happens when a track finishes: stop, play the next cue, loop, or jump to another cue.
- **🎛 Fire off carts** — a grid of one-touch buttons for stings, SFX, walk-on music and beds. Great for touchscreens; the cart grid can even pop out into its own window.
- **📺 Show Mode** — switch to a simplified, touch-friendly playback view for the actual performance, so operators can trigger cues without the editing clutter. Each device remembers its own preference.
- **⏭ Smooth transitions** — automatic advance, crossfades, and radio-style "Start Next" segue markers with an on-screen countdown for seamless back-to-back playback.
- **🔊 Sounds great, stays safe** — pick an **Output Target** for your show (Broadcast / EBU R128, Streaming, Radio, Netflix / OTT, or Live console) and LivePlay sets an appropriate loudness target, a brick-wall limiter to stop clipping, and matching meter and waveform colours automatically.
- **📊 See your levels** — real-time metering at every stage (per-cue, per-channel and master), shown in LUFS, dBFS, true-peak or RMS.
- **🎚 Route anywhere** — send audio to multiple outputs at once (front-of-house, monitors, comms, a record bus…) across one or more sound cards.
- **🎬 Timecode** — send SMPTE LTC timecode from a cue to keep lighting, video or other systems in sync.
- **📥 Bring in audio easily** — drag and drop files (or import several at once), or pull audio straight from YouTube.
- **🎹 Trigger it your way** — click, tap, keyboard hotkeys, MIDI controllers, or automation over the network (HTTP / WebSocket).
- **🌍 Speak your language** — available in **20+ languages**, including full right-to-left support.
- **🖥 Run it remotely** — operate a stage-side machine wired to your sound gear from a separate show laptop over the local network, with automatic discovery so you don't have to type in IP addresses.

---

## Download and install

Pre-built installers for **Windows, macOS and Linux** are on the **[GitHub Releases page](https://github.com/tdoukinitsas/liveplay/releases)** and the **[docs site](https://tdoukinitsas.github.io/liveplay/)**.

| Platform | What to download |
|----------|------------------|
| **Windows** | `LivePlay-Setup-x.y.z.exe` (installer, 64-bit) |
| **macOS — Apple Silicon** (M1/M2/M3 and newer) | `LivePlay-x.y.z-arm64.dmg` |
| **macOS — Intel** (older Macs) | `LivePlay-x.y.z.dmg` |
| **Linux** | `LivePlay-x.y.z.AppImage`, `liveplay_x.y.z_amd64.deb`, or `liveplay-x.y.z.x86_64.rpm` |

On macOS, pick the build that matches your Mac — Apple Silicon for M1/M2/M3 (and newer), Intel for older machines.

You only need the one installer. Everything is bundled inside it, so a normal single-machine install works with **no setup or configuration** — install, launch, and start building a show. LivePlay also checks for updates on launch and can update itself.

### First launch on macOS ("LivePlay is damaged and can't be opened")

LivePlay's macOS builds are **not yet signed with an Apple Developer ID certificate or notarized** (Apple charges for this — it's on the roadmap). Because of that, macOS quarantines the app on download and refuses to open it, usually with *"LivePlay is damaged and can't be opened."* The app is not actually damaged — macOS just won't run an unsigned, quarantined binary.

After dragging **LivePlay.app** into `/Applications`, remove the quarantine flag once from Terminal:

```sh
sudo xattr -rd com.apple.quarantine "/Applications/LivePlay.app"
```

Enter your password when prompted, then launch LivePlay normally. You only need to do this once per install (repeat it after each update). This applies to both the Apple Silicon and Intel builds.

### First launch on Windows ("Windows protected your PC")

LivePlay's Windows installer isn't yet signed with a certificate that Microsoft SmartScreen recognises (code signing via [SignPath](SIGNING.md) is in progress). Until then, Windows may show a blue **"Windows protected your PC"** dialog the first time you run the installer. The app is safe — it just hasn't accumulated SmartScreen download reputation yet.

To run it:

1. Click **More info** on the warning dialog.
2. Click the **Run anyway** button that appears, then continue the installation normally.

If your browser blocked the download instead, choose **Keep** to save the installer first.

---

## Getting started

1. Install LivePlay and launch it.
2. Choose **New Project** and pick a folder — LivePlay creates the project file and a `media/` sub-folder there.
3. Drop audio files onto the playlist, or use **Import audio** to copy them in (you can also import from YouTube).
4. Click a cue to open its properties, then set in/out points, fade times, volume and what happens when it ends.
5. Assign your most-used cues to cart buttons for one-touch playback.
6. Fire cues by clicking, tapping a cart, or pressing a keyboard shortcut. Live meters show your signal at every stage.
7. Heading into a show? Switch on **Show Mode** for a clean, touch-friendly playback view.

**Running on a separate machine?** LivePlay can run the audio engine on a stage-side computer wired to your sound gear and be controlled from a different laptop over your local network. Open **Server Settings** and either pick a discovered server or point the client at `http://<server-host>:4480`. See [Network ports](#network-ports) below for the firewall details.

---

## For developers

Everything below is for people who want to build LivePlay from source, contribute, or understand how it works under the hood. If you just want to use the app, you're all set — grab a [release](https://github.com/tdoukinitsas/liveplay/releases) above.

### How it's built

LivePlay uses a **decoupled client/server architecture**: a headless C++ audio engine (`liveplay-server`) handles all sound, while a cross-platform Electron desktop app drives it as a remote control. No audio ever plays in the desktop UI — it sends commands to the server and receives meters and state back.

Made with some help from Claude Sonnet 4.5, Claude Sonnet 4.6 and Claude Opus 4.8.

### Architecture in one diagram

```
+--------------------------------+   WebSocket (ws://host:4480/ws)   +-----------------------------------+
|  client/                       | <----- meters @ ~60 Hz ---------> |  server/  (liveplay-server)       |
|  Electron + Nuxt 3 + Vue 3     | <----- transport / route cmds --- |  C++20, miniaudio, Crow, TagLib   |
|                                |        REST  (http://host:4480)   |                                   |
|  - Playlist / cart / routing UI| <----- list / load / waveform --> |  - AudioEngine (mixer + limiter)  |
|  - WaveformCanvas              |                                   |  - ProjectState (.liveplay I/O)   |
|  - LiveMeterBar                |                                   |  - ControlServer (REST + WS)      |
|                                |                                   |  - Metadata + waveform services   |
|  No audio plays in the         |                                   |                                   |
|  renderer process.             |                                   |  Win → WASAPI · Mac → CoreAudio   |
|                                |                                   |  Linux → ALSA / PulseAudio        |
+--------------------------------+                                   +-----------------------------------+
```

Client and server can run on **the same machine** (the desktop installer bundles both, and the client spawns the server as a child process on `127.0.0.1:4480`) or on **different machines** on a LAN — e.g. the show laptop driving a stage-side mini-PC that's wired to the actual sound interfaces.

For the deep architectural docs (mixer tiers, routing matrix, LTC, limiter, metering, network event lifecycle, project-file backwards compatibility), see [`server/README.md`](server/README.md).

### Network ports

A single-machine install talks to itself over `127.0.0.1` and needs nothing opened. When the client and server run on **different machines** on a LAN, make sure these ports are reachable through any firewalls in between:

| Port | Protocol | Used for |
|------|----------|----------|
| `4480` | TCP | Control surface — REST API + WebSocket (transport, project data, routing, live meters). |
| `4481` | UDP | LAN auto-discovery beacon (broadcast + multicast group `239.255.69.80`). Lets clients find servers without typing an IP. |

On Windows the NSIS installer adds the necessary inbound firewall rules at install time; the app also makes a best-effort runtime pass if run elevated. On macOS/Linux, allow the `liveplay-server` binary through your firewall if you operate it remotely.

## Repository layout

```
liveplay/
├── client/         Electron + Nuxt 3 + Vue 3 desktop UI — see client/README.md
├── server/         C++20 audio engine + REST/WS control server — see server/README.md
├── docs-site/      Public-facing Nuxt 3 site (GitHub Pages) — see docs-site/README.md
├── scripts/        Cross-platform build orchestrator scripts — see scripts/README.md
├── build/          Collected installer artefacts after `npm run build`
├── .github/workflows/
│   ├── build-release.yml   Cuts releases on version bumps to package.json
│   ├── build-server.yml    Standalone server matrix build (Win / macOS / Linux)
│   └── deploy-docs.yml     Publishes the docs site to GitHub Pages
├── package.json    Monorepo root — orchestrator scripts only
├── LICENCE.txt     AGPL-3.0-only
└── README.md       This file
```

Each sub-package has its own README with developer documentation tailored to that area.

---

## Building from source

### Prerequisites

All platforms need:

| Tool | Minimum | Notes |
|------|---------|-------|
| Git  | any     | |
| Node.js | 20 LTS | for the client + orchestrator scripts |
| CMake | 3.21   | for the server |
| C++20 toolchain | — | MSVC 2022 / Clang 15+ / GCC 12+ |
| [vcpkg](https://github.com/microsoft/vcpkg) | recent | `VCPKG_ROOT` env var must point at your checkout |
| Ninja | latest | strongly recommended (`brew install ninja`, `choco install ninja`, `apt install ninja-build`) |

Set the `VCPKG_ROOT` environment variable:

```pwsh
# Windows (PowerShell, persistent)
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\dev\vcpkg", "User")
```

```sh
# macOS / Linux
export VCPKG_ROOT="$HOME/dev/vcpkg"
echo 'export VCPKG_ROOT="$HOME/dev/vcpkg"' >> ~/.zshrc
```

Then from a clean checkout:

```sh
git clone https://github.com/tdoukinitsas/liveplay.git
cd liveplay
npm install                # installs client deps via npm workspaces
npm run build              # builds server + client and collects installers into /build
```

`npm run build` runs the unified pipeline in [scripts/build-all.js](scripts/build-all.js):

1. Configures and builds the C++ server through CMake/vcpkg.
2. On macOS, wraps the server binary into a `LivePlay Server.app` for DMG inclusion.
3. Runs `nuxt generate` and `electron-builder` in `client/`.
4. Copies the installer artefacts (`.exe`, `.dmg`, `.AppImage`, `.deb`, `.rpm`) into `build/`.

Use `npm run build:clean` to wipe previous build outputs first (it preserves `vcpkg_installed/` so C++ deps don't get re-downloaded).

#### Platform-specific notes

##### Windows

- Install **Visual Studio 2022** with the *Desktop development with C++* workload (includes MSVC + Windows SDK).
- Install Node.js 20 LTS, CMake (≥ 3.21) and Ninja (e.g. `choco install nodejs cmake ninja`).
- Clone and bootstrap vcpkg:
  ```pwsh
  git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
  C:\dev\vcpkg\bootstrap-vcpkg.bat
  ```
- Set `VCPKG_ROOT` (see above), open a fresh PowerShell, `npm install`, then `npm run build`.
- Output: `dist-electron/LivePlay-Setup-<version>.exe` (NSIS installer, x64). The `artifactName` uses hyphens (no spaces) so the local file, the GitHub release asset and the `latest.yml` auto-update manifest all reference the same name.

##### macOS

- Install Xcode Command Line Tools (`xcode-select --install`).
- Install Homebrew deps: `brew install node cmake ninja pkg-config`.
- Bootstrap vcpkg:
  ```sh
  git clone https://github.com/microsoft/vcpkg "$HOME/dev/vcpkg"
  "$HOME/dev/vcpkg"/bootstrap-vcpkg.sh
  ```
- Set `VCPKG_ROOT`, then `npm install && npm run build`.
- Output: `build/LivePlay-<version>.dmg` on Intel, or `build/LivePlay-<version>-arm64.dmg` on Apple Silicon (each with a matching `.zip`). CI builds both x64 and arm64 **on Apple Silicon runners** — the Intel slice is cross-compiled with `-DCMAKE_OSX_ARCHITECTURES=x86_64` (Apple clang cross-compiles, vcpkg builds the `x64-osx` triplet, and `electron-builder --x64` fetches the prebuilt x64 Electron). To cross-build the Intel slice locally on an Apple Silicon Mac, configure the server with `-DCMAKE_OSX_ARCHITECTURES=x86_64` and run `electron-builder --mac --x64`.
- Code signing is skipped by default. Users will see a Gatekeeper warning on first launch — see [First launch on macOS](#first-launch-on-macos-liveplay-is-damaged-and-cant-be-opened).

##### Linux

- Install build tools and audio dev headers:
  ```sh
  sudo apt update
  sudo apt install -y build-essential cmake ninja-build pkg-config \
                      libasound2-dev libpulse-dev libjack-jackd2-dev libx11-dev
  ```
  (use the equivalent `dnf` / `pacman` packages on Fedora / Arch).
- Install Node.js 20 LTS via your distro or [nvm](https://github.com/nvm-sh/nvm).
- Bootstrap vcpkg as on macOS, set `VCPKG_ROOT`, then `npm install && npm run build`.
- Output: `build/LivePlay-<version>.AppImage`, `liveplay_<version>_amd64.deb`, `liveplay-<version>.x86_64.rpm`.

---

## Development workflow

From the monorepo root:

```sh
# One-time
npm install                      # installs client deps via npm workspaces
npm run server:configure         # CMake configure for the server (idempotent)

# Iterating on the server only
npm run server:build             # rebuild the C++ server
npm run server:run               # launch the compiled binary (forwards CLI args)

# Iterating on the client only — ensures the server is built first, then runs
# Nuxt + Electron in dev mode against it
npm run dev

# Running both in side-by-side terminals (the server in one pane, client dev in the other)
npm run dev:all
```

The default `npm run dev` calls [scripts/ensure-server.js](scripts/ensure-server.js), which is a no-op if the server binary already exists and otherwise configures + builds it. After that it launches `nuxt dev` + Electron in the `client/` workspace.

Bumping versions across the monorepo:

```sh
npm run bump -- patch        # 2.0.0 → 2.0.1
npm run bump -- minor        # 2.0.0 → 2.1.0
npm run bump -- major        # 2.0.0 → 3.0.0
npm run version -- 2.1.4     # set an explicit version
```

For deeper development notes:

- **Server internals** (mixer tiers, routing, LTC, project-file format, REST/WS surface): [`server/README.md`](server/README.md)
- **Client internals** (composables, IPC, Electron main process, localisation, MIDI/hotkeys): [`client/README.md`](client/README.md)
- **Build/utility scripts**: [`scripts/README.md`](scripts/README.md)
- **Public docs site**: [`docs-site/README.md`](docs-site/README.md)

---

## Releases & GitHub Actions

Releases are fully automated. The release pipeline lives in [`.github/workflows/build-release.yml`](.github/workflows/build-release.yml).

### Triggering a release

1. Bump the version in the root `package.json` (use `npm run bump -- patch|minor|major`, which propagates to `client/package.json`).
2. Commit and push to `main`.
3. The `build-release` workflow detects the version change and runs the platform matrix:
   - **Windows x64** (MSVC, WASAPI)
   - **macOS Intel x64** (Clang, CoreAudio, deployment target 11.0 — cross-compiled `x86_64` on an Apple Silicon runner)
   - **macOS Apple Silicon arm64** (Clang, CoreAudio, deployment target 12.0)
   - **Linux x64** (GCC, ALSA + PulseAudio + JACK)
4. Each job builds the C++ server through CMake/vcpkg, then runs the client `electron-builder` step with `extraResources` picking up the freshly compiled server binary.
5. All artefacts are uploaded, then a final `release` job downloads them, auto-generates a changelog from git commits since the last tag, and creates a GitHub Release tagged `v<version>` with every installer attached.

The vcpkg binary cache (`x-gha,readwrite` backend) is reused across runs so compiled C++ dependencies don't have to be re-built from scratch every time.

### Other workflows

- **[`build-server.yml`](.github/workflows/build-server.yml)** — builds the server alone on PRs and pushes that touch `server/**`. Cross-platform matrix; uploads `liveplay-server-<platform>` artefacts for download from the Actions UI. Useful for vetting server-only PRs without running the full release pipeline.
- **[`deploy-docs.yml`](.github/workflows/deploy-docs.yml)** — rebuilds [the docs site](https://tdoukinitsas.github.io/liveplay/) when `docs-site/`, the root `README.md`, or `package.json` changes.

---

## Contributing

Contributions of all sizes are welcome — bug fixes, new features, translations, documentation, screenshots, you name it.

1. **Fork** the repo and `git checkout -b feat/something` off `main`.
2. **Build it locally** following the steps above. For server changes, run `npm run server:build && npm run server:run --verbose`. For client changes, `npm run dev`.
3. **Test your change**. There's no automated test suite yet — please verify the path you touched works end-to-end in the running app. Mention any platform you couldn't test on in the PR description so reviewers can cover it.
4. **Open a PR** to `main`. CI must pass (server matrix build on the relevant platforms).

### Style

- **Server** (C++20): atomics for hot params on the audio thread, no exceptions inside the audio callback, RAII everywhere, header-per-class.
- **Client** (TypeScript): Vue 3 Composition API with `<script setup>`. All audio + project state goes through `useLiveplayServer()` — components don't talk to the server directly.
- **Commits**: short, prefer present-tense imperatives ("fix routing-matrix off-by-one"). Changelogs are generated from commit messages, so make them readable.

### Translations

LivePlay ships with 20 locale files at [`client/locales/`](client/locales/). To add a new language or fix existing translations:

1. Copy `en.json` to `<lang-code>.json` (e.g. `nl.json`).
2. Update the `_metadata` block (`code`, `name`, `nativeName`, `direction`).
3. Translate the values. Don't change keys; missing keys auto-fall-back to English at runtime.
4. Run `node scripts/sync-locale-keys.js` to ensure your new file has every key `en.json` has.
5. The locale is picked up automatically — no code changes needed.

For right-to-left languages, set `"direction": "rtl"` in `_metadata` and verify the layout in-app.

### Reporting bugs

File issues at [github.com/tdoukinitsas/liveplay/issues](https://github.com/tdoukinitsas/liveplay/issues). Include OS, LivePlay version (visible in the About dialog), and a minimal repro.

---

## License

[**AGPL-3.0-only**](LICENCE.txt). Third-party dependencies retain their own licences (miniaudio: public domain / MIT-0; Crow: BSD-3; TagLib: LGPL-2.1+; nlohmann/json: MIT).
