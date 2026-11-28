# Ownership and persistence

LivePlay has server, user and project state, plus machine-local storage and transient sessions. This guide describes implemented placement; it does not prescribe unimplemented migrations.

## Persistence map

| Owner | Examples | Storage / scope |
|---|---|---|
| Server | Boot settings and policy | `liveplay.json`, selected by launch configuration |
| Server | Hardware output mappings | `outputs.json` in the server data directory |
| Server | Accounts, authentication posture, API-token records | `users.json` |
| Server | The key that seals credentials in show files | `secret.key` in the server data directory |
| User | Theme, meter display, playback keyboard bindings and workflow preferences | `prefs/<id>.json` on the server |
| Project | Playlist, groups, carts, bus graph, DSP, fades, target levels and LTC output name | `.liveplay` document |
| Project | Playlist tabs, cart pages, cart keys (position and pinned), item events, volume automation, event connections and presets (credentials sealed) | `.liveplay` document |
| Machine | Server connection selection, recent paths, window geometry, workspace layout | Electron `userData` and renderer `localStorage` |
| Machine | MIDI configuration and local input device | `userData/midi-config.json` |
| Session | Shared selection, armed next item, live PFL, preview and transport, and the order cues were fired in (GO, Up Next and Stop Current follow it) | Server memory |
| Session | What events changed (bus fields, the Stop All fade mode) and each connection's sign-in state | Server memory; saved as the operator left them |
| Connection | Meter cadence, analyser selection, locale, handshake identity, and the tab and page a window shows | WebSocket session |

Configuration and profile schemas are defined in the [API reference](https://tdoukinitsas.github.io/liveplay/api/#schemas).

## Server state

- Boot configuration resolves defaults → file → environment → CLI. Stored settings may differ from the running values until restart.
- The configuration lock prevents API edits to the boot configuration. It is not a general lock on project edits or hardware output mapping.
- Logical output names travel with projects; physical device/channel mappings remain on the server.
- Authentication is disabled by default. Creating the first account forces its role to administrator and enables authentication.
- User accounts and API-token secrets are managed by the server. A token is not project content.
- Event connections travel with the show, so a show opened elsewhere already knows its devices; addresses, ports and MIDI port names are show content to adjust at the next venue. Credentials in them (HTTP header values such as Authorization or an API key) are sealed on disk with `secret.key`, which never leaves the machine, and each sealed value is bound to its connection, that connection's address and its header name: copied anywhere else, it doesn't open. No API response or frame carries one: every response and frame carries a mask in its place, and a mask sent back means "as stored", only for the same connection at the same address. Sealed values are only opened from a show file read by this server; one sent in a request is treated as a mask. A credential goes only where it was given for: pointing a connection at another address (or removing it and making another with the same id) leaves it, and the cues' events on it, asking for their credentials again. A show opened on another server can't open them either, so its connections ask for a sign-in, as do connections a device refused (401/403). Without a usable `secret.key` a credential is left out of the saved file, never written in plain text.
- Credentials the header rule doesn't recognise (a header named outside it, a key in a URL's query or in a body) are show content like any other field: masked from no one and saved as typed.
- API tokens can read connections (every header value masked) and fire what the operator set up, but can't change connections, change or add a cue's events, or replace the open show with another (by document or load-from-document); they may open a show file.

The server data directory and the selected boot-config path are distinct concepts: naming another `liveplay.json` does not relocate every other persistence file.

## User profiles

Profiles are sparse: omitted keys use client defaults. Top-level null removes an override; nested theme keys merge, while the playback-key map is replaced as a unit.

A signed-in user's profile notifications target that user's sockets. Installation-local preferences remain available for an anonymous client. Locale also has a connection-specific value; display choices should not be treated as project state.

The implementation still stores MIDI bindings and device configuration locally. Do not assume they synchronize with a signed-in profile.

## Project state

The server owns the active document. Any permitted show-control client may edit it.

| Persisted | Derived or transient |
|---|---|
| Item UUIDs, content and tree structure | Engine cue IDs |
| Playlist tabs, cart pages and their order | Which tab or page each window shows |
| Event connections, presets and item events | A connection's sign-in state (`auth`) |
| Show settings as the operator set them | A setting or bus field an event moved |
| Bus identity, roles, routes, sends and DSP | Engine mixer IDs, bindings and meter values |
| Logical output names | Device handles and physical channel assignments |
| Show settings and cue stop/fade choices | Current playback position, selection, armed-next item |
| Media references | Actual runtime project folder annotation |

Do not write API runtime annotations into a second configuration store. The server supplies those values on reads and controls which fields are saved.

Parts with their own endpoints (tabs and pages, connections, cart keys) are written through them. An ordinary save that carries a whole document leaves those parts as the server holds them for the same show, since the saving client's copy may be older than another client's change; a wholesale replace (a new show, or a client restoring one the server forgot) takes the document's.

"The same show" is the same `showId`, an id every show is given when it is made (or, for a show from before 2.6, when it is first opened) and saved with it. A document without one (from a 2.5 client) is the same show only with the same saved folder and the same name; an unsaved show is never the same as another. Two shows in one folder are two shows.

The document's `version` is the show-file format. 2.6 writes "2.6.0". An older show is upgraded once on load (customActions become events; ducks keep their 50 ms timing; a show whose API-bound carts used slots past 16 gets a first page big enough to show them). A show that already uses 2.6 features (tabs, pages, connections, events, automation) is treated as 2.6 whatever its version says. LivePlay 2.5 drops tabs, pages, connections and pinned keys when it saves a 2.6 show, so 2.6 shows are one-way.

With several cues on air and no Up Next armed, GO and Up Next continue from the cue fired first, or with the show setting `goContinuesFrom` "latest" the one fired last. The server's GO and `summary.next` and the desktop's Up Next and Space follow the same setting.

Supported compatibility fields are accepted/constrained by the current loaders and settings registry. See the reference for exact rules; there is no unrestricted merge of arbitrary project settings.

## Machine state

| Value | Store |
|---|---|
| Local/remote server mode, URL and local port | `userData/liveplay-server.json` |
| Recent servers/projects | `userData/liveplay-recent-*.json` |
| Window positions, dimensions, maximized state | `userData/liveplay-window-bounds.json` |
| Pane visibility, expanded pane (restore set) and pane sizes — including the mixer's docked/expanded view | `localStorage['liveplay-workspace-layout']` |
| Show Mode | `localStorage['liveplay-ui-mode']` |
| Locale fallback | `localStorage['liveplay-locale']` |
| MIDI configuration | `userData/midi-config.json` |
| Which operator this computer follows, by session (`{id, label}`; the id is used only while its label still matches, since session ids restart with the server) | `localStorage['liveplay.followOperator']` |
| Playlist zoom | `localStorage['liveplay.playlistZoom']` |
| The playlist tab and cart page this window shows | `sessionStorage['liveplay.playlistTab']`, `sessionStorage['liveplay.cartPage']`; a pop-out names them in its URL |
| The page and tab MIDI acts on (the ones in the window looked at last) | Main window memory, told by the other windows over IPC; MIDI is handled in the main window only |

Detached-window flags are not persisted. Geometry is restored when a window is opened, without forcing that window open at startup; a tab or page pop-out's is keyed `tab:<kind>:<id>`.

## Adding a setting

1. Determine whether it changes the show, server hardware/policy, the person's presentation, or this machine's layout/input.
2. Add validation at the owning store and make defaults explicit.
3. Keep runtime annotations separate from saved values.
4. Update the corresponding client setting and API catalog when externally visible.
5. Verify round-trip persistence and multi-client scope where relevant.

Primary implementation: [server configuration](../server/src/core/server_config.cpp), [project state](../server/src/core/project_state.cpp), [user preferences](../server/src/core/user_prefs.cpp), [client preferences](../client/app/composables/usePreferences.ts), [Electron storage](../client/electron/main.js).
