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

These take only a port, or start a server of their own: `media-import-e2e.js <port>`, `tabs-e2e.js <port>`, `event-presets-e2e.js <port> [serverExe]`, `stability-e2e.js <port> [serverExe] [soakSeconds]`, `audit-fixes-e2e.js <port> [serverExe]` and `credentials-e2e.js <port> [serverExe]`. The last four own their server, so give them a free port; `credentials-e2e.js` also uses the next two ports: a second server that stands in for another machine, and a third whose key file is unusable. None of them needs an audible signal. For `xair-e2e.js`, `XAIR_STRIPS_ONLY=1` writes to the two named strips and nothing else on the desk.

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
| `automation-e2e.js` | Volume automation (incl. live edits and Preview from cursor), #78 duck attack/release, #85 Stop All item fades and the `stop-fade-mode` event; `<port>`, writes its own tone |
| `events-e2e.js` | Event system (#84): OSC/HTTP delivery, marker timing, pause/resume/end, test fires, connection carry-over, customActions migration; `<port>`, no audio needed |
| `audit-fixes-e2e.js` | The 2.6.0 RC audit's server fixes: raw-slot cart API and summary.cart, tab-scoped selection, Stop Current Cue, tab-cart runs, cart keys, legacy duck timing and version, customActions migration details, event-only settings, client views; and the RC2 audit's: GO and Up Next under both goContinuesFrom settings, a second Stop Current during a fade, a 2.5 show's carts past slot 16, an RC1 show's duck times; owns its server |
| `credentials-e2e.js` | Credentials in event connections: masking on every path, keeping what you were shown, sealing on disk, another machine's sign-in prompt, 401 handling, API-token limits (no replacing the show or rewiring a cue's events), no inheritance by another show or another show in the same folder, a cue's own credential never following its connection to a new address, sealed values opening nowhere else, pasted copies keeping their own credential, nothing written in plain text without a key; owns three servers |
| `liveplay-plugin-e2e.js` | LivePlay event plugin: play/pause/resume/stop/Up Next, ducking (measured on the meter), master level, bus fader/mute/sends/routing/processing with fades; `<port>`, plays a quiet tone |
| `xair-e2e.js` | X Air plugin against a real mixer: crossfades, cap, mute, sends, FX, routing, recorder refusal; `<port> <mixer ip> [strip A] [strip B]`. Restores everything; never above -20 dB on the two strips; never recalls a snapshot |
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
