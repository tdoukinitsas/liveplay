# API documentation audit

Scope: the repository's 2.5.0 implementation. Source is authoritative; these findings describe code paths, not a live penetration test or a guarantee about every malformed input.

## Coverage

| Surface | Discovered | Documented |
|---|---:|---:|
| Application HTTP method/path combinations | 118 | 118 |
| Crow implicit static-file GET route | 1 | 1 |
| Total HTTP method/path combinations | **119** | **119** |
| WebSocket client command types | **20** | **20** |
| WebSocket server frame types | **8** | **8** |
| Operations within server `doc_patch` frames | **29** | **29** |

Application routes comprise 111 registrations across 88 distinct paths. Including Crow's static route gives 112 registrations and 89 paths. Compatible GET/POST aliases are grouped into one catalog entry. There are 28 distinct directional WebSocket frame types; the 29 patch operations are subtypes of one of the eight server frames.

Crow's automatic HEAD/OPTIONS handling and the `/ws` upgrade are documented separately from the 119 count. No discovered route is intentionally omitted. No duplicate application method/path registration was found.

## HTTP categories

| Category | Method/path combinations |
|---|---:|
| Server and authentication | 8 |
| Users and API tokens | 8 |
| Configuration and preferences | 8 |
| Transport and selection | 15 |
| Projects | 10 |
| Playlist items and carts | 14 |
| Audio devices and engine cues | 12 |
| Master and preview | 12 |
| Buses, DSP and outputs | 11 |
| Low-level mixer and routing | 9 |
| Filesystem and media | 8 |
| Project archives | 3 |
| Framework HTTP behavior | 1 |

## Source evidence

| Area | Reviewed implementation |
|---|---|
| HTTP registration / permissions | [control_server.cpp](../server/src/net/control_server.cpp): AuthGuard, access_for, register_routes |
| WS receive/send/broadcast | Same file: handle_ws_message, broadcast_loop, broadcast_doc_patch, broadcast_to_user and connection callbacks |
| Project-generated notifications | [project_state.cpp](../server/src/core/project_state.cpp) callback invocations |
| Account/token behavior | [user_store.cpp](../server/src/core/user_store.cpp) |
| Configuration / profiles | [server_config.cpp](../server/src/core/server_config.cpp), [user_prefs.cpp](../server/src/core/user_prefs.cpp) |
| Hardware outputs / engine behavior | [output_map.cpp](../server/src/core/output_map.cpp), [engine.cpp](../server/src/audio/engine.cpp) |
| Client consumption / custom HTTP actions | [useAudioEngine.ts](../client/app/composables/useAudioEngine.ts), [useProject.ts](../client/app/composables/useProject.ts), [useLiveplayServer.ts](../client/app/composables/useLiveplayServer.ts) |
| Discovery (UDP, outside WS count) | [discovery.cpp](../server/src/net/discovery.cpp) |
| Framework behavior | Installed Crow headers: `app.h`, `settings.h`, `routing.h`, `http_response.h` |

The full server source/header tree was searched for route registration, dispatch, serialization, sends and broadcasts. Literal catalog coverage is reproducible with:

```sh
node docs-site/scripts/check-api-reference.mjs
```

That script is a pattern-based consistency check, not a C++ parser. It needs review if route registration or message construction changes. Crow is a dependency: its implicit route/HEAD/OPTIONS behavior was checked against the installed headers, with no application `CROW_DISABLE_STATIC_DIR` definition found.

## Findings requiring review

| Finding | Current behavior / integration consequence |
|---|---|
| Action GETs also execute on HEAD | Crow maps HEAD to GET and runs the handler. Use POST for actions; a HEAD probe can trigger playback. |
| Authentication disabled permits administration | Middleware bypasses role checks when auth is off. The first account can bootstrap administration; individual password/profile checks still apply. |
| Established socket authorization | While login is required, credentials are rechecked before commands and broadcasts. Invalid credentials and anonymous sessions are closed with code 1008; the client rechecks login before reconnecting. |
| API tokens are not a filesystem sandbox | Tokens deny selected file/archive/preferences paths, but other permitted project/cue operations can access disk. |
| Filesystem-root checks are selective | Direct cue loading and media references embedded in project documents do not all pass through the same path guard. |
| Item creation can report success without insertion | Missing/duplicate UUID or failed media loading is not reliably an HTTP error; `item_added` echoes submitted content. Re-read state. |
| Out-of-range cart slot can report success | The route accepts a nonnegative slot, state rejects slots 64+, and the route still sends 200 plus `cart_slot_set`. UUID existence is not checked. |
| Cue deletion can retain a project-owned cue | The low-level DELETE handler can return 200 even when ProjectState keeps the cue because an item owns it. |
| Output-map persistence failure can be hidden | PUT applies the map but does not use the save return value to choose its HTTP result. A 200 does not guarantee persistence. |
| DSP writes require HTTP | No WebSocket EQ/filter/gate/compressor/stereo-image edit command exists. Use HTTP reads/PATCH and WebSocket buses_patched/meter/analyser feedback. |
| Live DSP requests do not accumulate as a stored edit | Each request merges against persisted bus DSP. Live pan/DSP changes do not save or broadcast a document patch. |
| Bus creation ignores some recognized patch fields | Creation reads a subset of bus properties; apply DSP, sends and role changes with PATCH. |
| Archive upload limit differs from ordinary upload | Multipart project import does not enforce the ordinary upload-size limit. Temporary archive filenames can collide; avoid exporting an archive inside its own source tree. |
| Custom HTTP actions may execute more than once | The server broadcasts `custom_action_http`; connected desktop consumers execute the HTTP request. There is no single designated executor or acknowledgment. |
| WS success/error delivery is uneven | Most commands have no success reply; some invalid targets are silent. Invalid top-level JSON type can escape to logging without a reply. |
| Analyser acknowledgment is not proof of delivery | Subscription acknowledgment does not validate bus existence or the four-distinct-bus engine capacity. |
| Login error text can misdescribe disabled authentication | Login returns the no-accounts wording when auth is disabled even if accounts exist. |
| Token rename persistence errors use 400 | This handler differs from creation/deletion paths that map persistence failures to 500. |
| No replay or edit versioning | WS events have no replay/revision/request ID; project writes have no ETag/conflict protocol. Fetch current state on reconnect. |
| Framework static route remains exposed | Crow registers `GET /static/<path>` against its static directory. AuthGuard still applies. |

Reachable low-level cue/mixer/routing and live pan/DSP routes are included even where comments describe them as client-internal. That description does not make them inaccessible. No route or message is labeled unused solely because the desktop does not visibly call it; external consumers cannot be ruled out from this repository.

## Documentation/source conflicts resolved

- Meter broadcast default is **30 Hz**; individual connections can request less.
- Stop All defaults to the project's **1000 ms** fade and stops preview by default.
- Current packages use **Nuxt 4**.
- Buses support auxiliary sends, output-to-none and up to **32 EQ bands**. Fresh buses have four enabled flat bell bands; the separate HPF/LPF defaults are 20/20000 Hz. EQ edits merge by slot, and the compatibility shelf flag can override type.
- Master/Preview are bus roles; unbound Preview is silent.
- MIDI configuration is still machine-local in Electron `midi-config.json`; moving it to server ownership is not implemented.
- Save As changes the project file location without copying media.
- The plugin rack is a UI placeholder; documentation does not advertise plugin hosting.
- The docs homepage does not render the copied README, and a static preview must preserve `/liveplay/`.

## Documentation layout

- Root README: installation, setup, operating settings, media transfer, remote control and troubleshooting.
- Repository guides: current build, process, bus and persistence information.
- Removed `IMPROVEMENTS_PLAN.md` and `MIXER_BUSES_PLAN.md`; retained relevant implemented behavior in the current guides.
- Dedicated API page at `/liveplay/api/`, with one contract catalog, shared schemas, category links, search and collapsible entries. DSP reads, writes and processor examples are part of the existing HTTP bus entries. DSP notifications, metering and analyser behavior are in the corresponding WebSocket entries; 31 parameter definitions remain in the shared BusDsp schema.
- Homepage changes are confined to its API section and unused API constants. Only the English locale's API object changed. Other product content, download links, styling, locales and navigation structure are preserved.
- No application source, comments, tests, dependency files, runtime configuration or build scripts were changed. The new documentation checker is explicitly invoked and is not wired into the build.

## Validation

- Existing `npm run generate`: passed. Nuxt reports a module-preload sourcemap warning, an unresolved cache-driver import treated as external, and the expected `ssr: false` notice; static output is generated.
- API inventory and catalog checks: passed, with the counts above and 28 shared schemas. Endpoint detail-section schema references and request examples also passed.
- Homepage comparison after excluding the API section/constants: identical. Non-API English locale values: identical.
- Headless Edge: 197 catalog entries rendered, including HTTP DSP details and WebSocket notifications; search, expand/collapse, internal anchors, direct/filtered deep links, mobile overflow and French-homepage English API fallback passed. No browser exceptions or local asset 404s occurred. Desktop/mobile screenshots were visually inspected.
- 67 local Markdown links/fragments, API-page anchors and generated assets: checked. External release/source URLs were not crawled. The CI-copied public README is checked against its canonical root source.
- Application integration/audio tests were not run: application code and tests are unchanged.
- Historical-language search retained only descriptions of current replacement operations; no planning narrative remains.
- No unresolved documentation blocker found; the runtime findings above remain for maintainer review.
