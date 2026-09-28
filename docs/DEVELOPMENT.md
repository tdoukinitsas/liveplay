# Development

## Prerequisites

| Tool | Use |
|---|---|
| Node.js and npm | Client, scripts and documentation site; use a Node version accepted by the installed Nuxt package |
| CMake 3.21+ | Server configuration and build |
| C++20 compiler | MSVC/Visual Studio 2022 on Windows; Clang or GCC on macOS/Linux |
| vcpkg | Server dependencies; set `VCPKG_ROOT` to its checkout |
| Ninja | The default and debug presets on macOS/Linux |
| Platform audio development packages | Linux ALSA/PulseAudio and other packages installed by the [build workflow](../.github/workflows/build-release.yml) |

The root npm workspace includes `client`. The documentation site has its own package directory. Dependency versions and engine requirements are defined by the package manifests/lockfiles, not this guide.

## Build and run

From the repository root:

```sh
npm install
npm run server:build
npm run dev
```

`npm run dev` ensures a server binary exists, then starts Nuxt and Electron. It does not rebuild an existing server binary after C++ changes; run `npm run server:build` for those changes.

| Command | Result |
|---|---|
| `npm run dev:client` | Start the client without the server build check |
| `npm run server:run -- --port 4500` | Run the built server with arguments |
| `npm run dev:all` | Start a standalone server and client development processes |
| `npm run build:client` | Generate the client renderer |
| `npm run build:client:electron` | Generate renderer and package desktop app; expects server resources |
| `npm run build` | Build server, package desktop app and collect installers in root `build/` |
| `npm run build:clean` | Remove build outputs and rebuild; retains compiled vcpkg dependencies |

Server-only CMake commands, from `server/`:

```sh
# Windows
cmake --preset vs2022
cmake --build --preset vs2022

# macOS / Linux
cmake --preset default
cmake --build --preset default
```

The `debug` preset writes to `server/build-debug`. The root `server:configure` command selects the Windows preset; use the appropriate CMake preset directly on other systems.

## Checks

| Area | Command / guide |
|---|---|
| Client generation | `npm run build:client` |
| Window geometry | `node client/tests/window-bounds-tests.js` |
| Settings placement | `node client/tests/settings-form-placement-tests.js` |
| Mixer regressions | `node client/tests/mixer-regression-tests.js` |
| Server unit suites | Configure with `-DLIVEPLAY_BUILD_TESTS=ON`, build, then run CTest |
| Audio integration | [E2E guide](../server/tests/e2e/README.md); many suites require an audio device |
| Documentation site | `npm run generate` from `docs-site/` |
| API inventory | `node docs-site/scripts/check-api-reference.mjs` |

Example Windows unit test run from `server/`:

```sh
cmake --preset vs2022 -DLIVEPLAY_BUILD_TESTS=ON
cmake --build --preset vs2022
ctest --test-dir build -C Release --output-on-failure
```

Use `default` instead of `vs2022` for the Ninja build. There are nine registered CTest suites: meter, device_name, mixer, biquad, dynamics, stereo, drift, topo and waveform.

## Repository map

| Directory | Responsibility |
|---|---|
| `client/app/` | Nuxt 4 / Vue 3 renderer, components, composables and utilities |
| `client/electron/` | Desktop main process and preload bridges |
| `client/locales/` | Application translations |
| `server/src/audio/` | Playback, mixing, metering, limiting and LTC |
| `server/src/core/` | Project, configuration, output map, users and preferences |
| `server/src/net/` | Crow HTTP/WebSocket and discovery |
| `server/src/meta/` | Metadata and waveform generation |
| `server/include/liveplay/` | Server headers and DSP helpers |
| `scripts/` | Build/version/localization automation |
| `docs-site/` | Public product website and API reference |

## Contributor boundaries

- Keep audio work in the server; the renderer sends control requests and displays state.
- Choose a persistence owner using the [ownership guide](OWNERSHIP_MODEL.md).
- Keep DSP display math consistent with the corresponding server implementation.
- Update the API catalog when externally observable protocol behavior changes.
- Use targeted unit checks for deterministic logic and audio E2Es for routing/timing claims.
- Release automation watches root package version changes. Review [build scripts](../scripts/README.md) before bumping versions.
