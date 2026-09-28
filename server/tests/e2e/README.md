# Audio and server integration checks

These scripts drive server HTTP/WebSocket interfaces. Many measure real rendered audio and need a working playback device; a silent render path cannot validate routing or dynamics.

Use a dedicated test server and disposable projects. Suites can change the active project, routing, output maps, users, preferences or boot files. Keep their console output to diagnose failures.

## Setup

From the repository root:

```sh
npm run server:build
node server/tests/e2e/gen-signal.js test-signal.wav
node server/tests/e2e/gen-wide-signal.js test-wide.wav
npm run server:run -- --port 4500
```

In another terminal, supply the port and absolute signal path:

```sh
node server/tests/e2e/pfl-e2e.js 4500 /absolute/path/test-signal.wav
node server/tests/e2e/width-e2e.js 4500 /absolute/path/test-wide.wav
```

On Windows, use a path such as `C:/Tests/test-signal.wav`. The ordinary signal has identical stereo lanes; width checks need the separate wide signal. Do not commit generated audio.

## Suites using a running server

Unless shown otherwise, arguments are `<port> <wavPath>`.

| Script | Coverage / special arguments |
|---|---|
| `pfl-e2e.js` | Pre-fader/pre-mute listening and program isolation |
| `roles-e2e.js` | Master/Preview roles, binding and role moves |
| `preview-sends-e2e.js` | Preview promotion with incoming/outgoing sends and atomic refusals; `<port>`, no audio required |
| `filters-e2e.js` | Filter processing |
| `eq-analyser-e2e.js` | EQ and analyser delivery |
| `gate-e2e.js` | Gate behavior and timing |
| `comp-e2e.js` | Compression and gain reduction |
| `width-e2e.js` | Stereo width, balance and mono-check; use wide signal |
| `reroute-e2e.js` | Cue rerouting |
| `busbus-e2e.js` | Bus graph; `<port> <wavPath> <widePath>` |
| `sends-e2e.js` | Auxiliary send taps |
| `migration-e2e.js` | Supported project compatibility paths |
| `absent-device-e2e.js` | Missing hardware stays unbound |
| `ltc-output-e2e.js` | LTC destination binding; saves/restores output map |
| `project-folder-e2e.js` | Project folder/media resolution; creates and moves temp files |
| `session-prefs-e2e.js` | Per-connection locale/meter settings |
| `materialise-skip.js` | Bus materialization; `<port>` |
| `settings-registry-e2e.js` | Settings validation; `<port>`, no audio needed |
| `output-materialise-e2e.js` | Output materialization; `<port>`, skips without a device |
| `device-match-e2e.js` | Device identity; `<port>`, saves/restores output map |
| `transport-fixes-e2e.js` | Transport, manual fades, preview and CORS; `<port>`, creates signals |
| `loop-xfade-e2e.js` | Loop crossfade; `<port>`, creates signals |

These scripts generally assume an unauthenticated test server unless they explicitly manage authentication.

## Suites that start their own server

Each accepts an optional server executable path. Their default points at the Windows Release build; pass `server/build/liveplay-server` for a Ninja build.

| Script | Coverage / affected state |
|---|---|
| `fs-jail-e2e.js` | Configured filesystem roots |
| `boot-config-e2e.js` | Launch configuration |
| `client-session-e2e.js` | Connection tracking and handshake behavior |
| `auth-e2e.js` | Accounts, roles, tokens and restart; temporarily owns users file (`LIVEPLAY_E2E_PORT` overrides port 4573) |
| `users-e2e.js` | Profile pictures and account export/import; runs two servers (`LIVEPLAY_E2E_PORT`, default 4491, and the next port) from a temporary copy of the binary, so it never touches the real users file |
| `user-prefs-e2e.js` | Profiles and restart; temporarily owns users and prefs |
| `server-config-e2e.js` | Stored/effective configuration and lock; temporarily owns configuration |

Run these serially. Read their setup/cleanup code before using a binary directory that contains real installation data.

## Diagnostic probes

Probes print measurements for investigation; their results depend on hardware and workload.

| Script | Arguments |
|---|---|
| `control-latency-probe.js` | `<port> [eventsPerSec] [seconds]` |
| `latency-probe.js` | `<port> <wavPath> [maxItems] [soakSeconds]` |
| `ui-churn-probe.js` | `<port> <wavPath> [editsPerSec] [seconds] [items]` |
| `seam-bisect.js` | `<port> <wavPath> [repeats]` |
| `save-churn.js` | `<port> <wavPath> <projectDir> <serverLog>` |

For deterministic C++ checks, see the [CTest instructions](../../../docs/DEVELOPMENT.md#checks). Protocol contracts live in the [API reference](https://tdoukinitsas.github.io/liveplay/api/), not this test guide.
