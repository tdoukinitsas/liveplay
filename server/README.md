# LivePlay Server

Headless C++ audio engine and control server for the LivePlay project. This
binary is the master state holder: it owns the audio graph, the routing
matrix, the project file, and exposes a REST + WebSocket control surface that
the Vue/Electron desktop client (in [`../client/`](../client/)) consumes.

> This `README.md` covers the **server only**. The full architectural
> document — Client/Server topology, mixer tree, multi-device routing, LTC,
> network lifecycle, packaging — lives at the repository root (delivered in
> Milestone 6).

## Stack

| Layer        | Library                                           |
|--------------|---------------------------------------------------|
| Audio I/O    | [miniaudio](https://miniaud.io/) (header-only)    |
| HTTP / WS    | [Crow](https://crowcpp.org/)                      |
| Metadata     | [TagLib](https://taglib.org/)                     |
| JSON         | [nlohmann/json](https://github.com/nlohmann/json) |
| Build system | CMake + vcpkg (manifest mode)                     |

## Building

### Prerequisites

- **CMake** &ge; 3.21
- A C++20 toolchain — MSVC 2022, Clang 15+, or GCC 12+
- **vcpkg** with `VCPKG_ROOT` exported

Linux extras for miniaudio's runtime: `libasound2-dev` and `libpulse-dev`
(install via `apt`/`dnf`/`pacman`).

### Configure & build

```bash
cd server
cmake --preset default       # or: cmake -S . -B build
cmake --build build --config Release -j
```

The binary lands at `build/liveplay-server` (or `build/Release/liveplay-server.exe`
on Windows).

### Run

```bash
./build/liveplay-server                # default: 0.0.0.0:4480
./build/liveplay-server --port 5005    # custom port
./build/liveplay-server --verbose      # enable debug-level logs
```

The startup banner shows the LAN address to point the client at. `Ctrl+C`
shuts the server down cleanly.

## Layout

```
server/
├── CMakeLists.txt
├── vcpkg.json
├── include/
│   └── liveplay/
│       └── logger.hpp        # public logger interface
└── src/
    ├── main.cpp              # entrypoint, banner, signal handling
    ├── logger.cpp            # ANSI-color logger implementation
    └── audio/
        └── miniaudio_impl.c  # the single TU that compiles miniaudio
```

Milestones 2 and 3 will introduce `src/audio/` (engine, mixer, routing,
LTC), `src/net/` (Crow app, WebSocket broadcaster), `src/meta/` (TagLib +
waveform downsampler), and `src/core/` (project state).
