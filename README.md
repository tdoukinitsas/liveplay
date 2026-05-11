# LivePlay

![Main liveplay user interface, with playlist editor, cue cart and properties panel](/public/screenshots/liveplay_screenshot.jpg)

LivePlay is a free, open-source audio playback system designed for live sound operators who need reliable, flexible cue management. Built with modern web technologies (Electron, Vue 3, Nuxt 3), it runs on Windows, macOS, and Linux.

Built with help from Github Copilot and Claude Sonnet 4.5

**Available in 20 languages** with full RTL support

---

## 📥 Download

Get the latest release for your platform:

[![Download Latest Release](https://img.shields.io/github/v/release/tdoukinitsas/liveplay?label=Download&style=for-the-badge)](https://github.com/tdoukinitsas/liveplay/releases/latest)

- **Windows**: `.exe` installer (x64)
- **Linux**: `.AppImage`, `.deb`, or `.rpm` (x64)
- **macOS**: `.dmg` or `.zip` (Universal: Intel + Apple Silicon)

---

## ✨ Features

- **Project Management**: Create and manage audio cue projects with all data stored as JSON. All media files are automatically collected.

![Youtube downloader interface, showing a search window, a preview and download buttons](/public/screenshots/liveplay_screenshot_youtube.jpg)

- **Integrated YouTube downloader**: Download directly from YouTube as mp3 files and auto-import them in to your playlist.

### 🎵 Audio Playback- **Playlist System**: Organize audio cues in a hierarchical structure with groups

- **Multi-track playback**: Play multiple audio cues simultaneously

- **Precise trimming**: Set in/out points to play only the parts you need

- **Volume control**: Individual volume adjustment per cue (-60 to +10db)

- **Real-time progress**: Visual progress bars with time remaining

- **Instant stopping**: Emergency stop all cues with one clic

- **Cart Player**: 16-slot cart for quick-access audio triggers

### 📋 Playlist Management- **Properties Inspector**: 

- **Hierarchical organization**: Group cues into folders for complex shows

- **Drag & drop**: Import audio files by dragging them into the app

- **Color coding**: Assign colors to cues for quick visual identification

- **Index-based addressing**: Access cues by position (e.g., "0,1,2")

- **UUID tracking**: Each cue has a unique identifier for reliable triggering

- **Waveform Display**: Visual waveform representation of audio files

### 🎛️ Cart Player

- **16 quick-access slots**: Instant playback buttons for your most-used cues

- **Drag to assign**: Simply drag playlist items to cart slots

- **Visual feedback**: See which cues are playing directly on the cart

- **One-click triggering**: No arming required—just click to play

### 🎚️ Advanced Controls

#### Ducking Behavior  

- **Stop All**: Automatically stop other cues when this one plays

- **Duck Others**: Lower the volume of other cues while this plays

- **No Ducking**: Allow multiple cues to play at full volume

#### Start Behavior

- **Play Next**: Automatically trigger the next cue when this one starts

- **Play Specific**: Trigger any other cue by UUID or index

- **Play First/All**: For groups—play just the first item or all items

#### End Behavior

- **Play Next**: Continue to the next cue when this one finishes

- **Go To**: Jump to any specific cue

- **Loop**: Repeat the current cue

- **Nothing**: Just stop when done

### 🌐 Remote Control

- **REST API**: Trigger cues from external applications

- **HTTP endpoints**: Simple GET requests to play/stop cues

- **Local server**: Runs on `http://localhost:8080`

- **Integration ready**: Works with control systems, scripts, or web pages

### 🎨 Customization

- **Dark/Light themes**: Choose your preferred interface style

- **Accent colors**: Customize with 18 preset colors

- **Per-project settings**: Each show can have its own theme

- **Professional design**: Clean, modern interface based on IBM's Carbon Design

### 🌍 Internationalization

- **20 Languages**: English, Greek (Ελληνικά), French (Français), Spanish (Español), Italian (Italiano), Portuguese (Português), Arabic (العربية), Farsi (فارسی), German (Deutsch), Swedish (Svenska), Norwegian (Norsk), Russian (Русский), Japanese (日本語), Chinese (中文), Hindi (हिन्दी), Bengali (বাংলা), Turkish (Türkçe), Korean (한국어), Albanian (Shqip), Urdu (اردو)

- **RTL Support**: Full right-to-left layout support for Arabic, Farsi, and Urdu

- **Native Display**: Languages shown in their native scripts

- **Easy Switching**: Change language from `View > Language` menu



### 💾 Project Management

- **Single-file projects**: Everything stored in one `.liveplay` JSON file

- **Media management**: Audio files organized in project folders

- **Auto-save**: Changes saved instantly (or manually with Ctrl+S)

- **Portable**: Move entire projects between computers easily

### 🔄 Auto-Updates

- **Automatic checking**: App checks for updates on launch

- **One-click updates**: Download and install new versions in seconds

- **Release notes**: See what's new before updating

- **GitHub releases**: Updates delivered via GitHub releases

---

## 🚀 Getting Started


#### Windows

1. Download `LivePlay Setup x.x.x.exe` from [Releases](https://github.com/tdoukinitsas/liveplay/releases)

2. Run the installer

3. Launch LivePlay from the Start Menu



#### macOS

1. Download `LivePlay-x.x.x.dmg` from [Releases](https://github.com/tdoukinitsas/liveplay/releases)

2. Open the DMG and drag LivePlay to Applications

3. Launch from Applications folder



### First Steps

1. **Create a Project** 

   - Click "New Project" on the welcome screen

   - Choose a folder location

   - Enter a project name



2. **Import Audio**

   - Click "Import Audio" button

   - Select your audio files (MP3, WAV, OGG, FLAC, M4A, AAC)

   - Or drag files directly into the playlist 

3. **Configure Cues**

   - Click on a cue to select it

   - Adjust settings in the Properties Panel:

     - Display name

     - Color

     - Volume

     - In/Out points (trimming)

     - Behaviors

4. **Play Audio**

   - Select a cue and press F1

   - Or click the play button next to the cue

   - Or drag it to the cart player for quick access

## 📖 User Guide

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| **F1** | Play selected cue |
| **Ctrl+N** | New project |
| **Ctrl+O** | Open project |
| **Ctrl+S** | Save project |
| **Ctrl+W** | Close project |
| **Ctrl+Q** | Quit application |
| **F11** | Toggle fullscreen |

### Working with Groups

Groups help organize cues into logical sections (e.g., "Act 1", "Scene 2"):

1. Click "Add Group"

2. Give it a name

3. Drag audio cues into the group

4. Configure group behavior:

   - **Play First**: Only play the first item when triggered

   - **Play All**: Play all items in sequence

### Using the Cart Player

The cart provides 16 slots for instant playback:

1. Drag a cue from the playlist to a cart slot

2. Click the slot number to play

3. Click the gear icon (⚙) to edit the cue's properties

4. The slot glows when its cue is playing



### Remote Control API

LivePlay exposes an HTTP API on `http://localhost:8080` so external applications can inspect and control playback. All responses are JSON. Slot numbers are **0-based** (slot 1 in the UI = slot `0` in the API).

---

#### Data models

##### Cue (AudioItem)

Every cue endpoint returns or accepts an object with the following shape. Fields marked **read-only** are ignored on `PATCH`.

| Field | Type | Read-only | Description |
|---|---|:---:|---|
| `uuid` | string | ✓ | Unique identifier |
| `type` | `"audio"` | ✓ | Always `"audio"` for cues |
| `index` | number[] | ✓ | Position in playlist, managed internally |
| `displayName` | string | | Label shown in the UI |
| `color` | string | | Hex colour, e.g. `"#FF0000"` |
| `mediaFileName` | string | ✓ | Audio filename |
| `mediaPath` | string | ✓ | Relative path from the project folder |
| `waveformPath` | string | ✓ | Path to the waveform data file |
| `duration` | number | ✓ | Total file duration in seconds |
| `inPoint` | number | | Trim start in seconds |
| `outPoint` | number | | Trim end in seconds |
| `volume` | number | | `0.0`–`2.0` — `1.0` = 100%, `2.0` = 200% |
| `fadeOutDuration` | number | | Fade-out when stopped manually, in seconds |
| `playFade` | number | | Fade-in when playback starts, in seconds |
| `stopFade` | number | | Fade-out before the natural end, in seconds |
| `crossFade` | number | | Cross-fade duration to the next track, in seconds |
| `startBehavior` | StartBehavior | | Action triggered at the start of playback |
| `endBehavior` | EndBehavior | | Action triggered at the end of playback |
| `duckingBehavior` | DuckingBehavior | | How this cue interacts with others |
| `customActions` | CustomAction[] | | Time-point triggered actions |

**StartBehavior**

```json
{ "action": "nothing" }
{ "action": "play-next" }
{ "action": "play-item",  "targetUuid": "..." }
{ "action": "play-index", "targetIndex": [1, 2] }
```

**EndBehavior**

```json
{ "action": "nothing" }
{ "action": "next" }
{ "action": "loop" }
{ "action": "goto-item",  "targetUuid": "..." }
{ "action": "goto-index", "targetIndex": [1, 2] }
```

**DuckingBehavior**

```json
{ "mode": "stop-all" }
{ "mode": "no-ducking" }
{ "mode": "duck-others", "duckLevel": 0.2, "duckFadeIn": 0.25, "duckFadeOut": 1.0 }
```

`duckLevel` is a multiplier — `0.2` means other cues drop to 20% volume.

**CustomAction**

```json
{ "timePoint": 30.5, "action": { "type": "stop-all" } }
{ "timePoint": 10.0, "action": { "type": "play-item",  "uuid": "..." } }
{ "timePoint": 10.0, "action": { "type": "play-index", "index": [0] } }
{ "timePoint": 5.0,  "action": { "type": "http-request", "request": { "method": "POST", "url": "http://...", "contentType": "json", "body": {} } } }
```

---

##### Cart slot

| Field | Type | Description |
|---|---|---|
| `slot` | number | Slot number, 0-based (slot 1 in the UI = `0`) |
| `itemUuid` | string | UUID of the assigned audio item |
| `item` | Cue (AudioItem) | Full audio item details (see above) |

---

#### List all cues

```
GET /api/cues
```

Returns every audio item in the playlist (groups are flattened).

```bash
curl http://localhost:8080/api/cues
```

**Response**

```json
{
  "success": true,
  "cues": [
    {
      "uuid": "a1b2c3d4-...",
      "displayName": "Intro Music",
      "volume": 1.0,
      "inPoint": 0,
      "outPoint": 184.3,
      "duration": 184.3,
      "color": "#00CC00",
      "endBehavior": { "action": "next" },
      "startBehavior": { "action": "nothing" },
      "duckingBehavior": { "mode": "stop-all" },
      "fadeOutDuration": 1.0,
      "playFade": 0,
      "stopFade": 0,
      "crossFade": 0,
      "customActions": [],
      "type": "audio",
      "index": [0],
      "mediaFileName": "intro.mp3",
      "mediaPath": "media/intro.mp3",
      "waveformPath": "waveforms/a1b2c3d4-....json"
    }
  ]
}
```

---

#### Get a single cue

```
GET /api/cues/:uuid
```

```bash
curl http://localhost:8080/api/cues/a1b2c3d4-...
```

**Response** — same shape as a single entry from `GET /api/cues`, wrapped in `"cue"`:

```json
{
  "success": true,
  "cue": { "uuid": "a1b2c3d4-...", "displayName": "Intro Music", "..." }
}
```

**Error (not found)**

```json
{ "success": false, "message": "Cue not found" }
```

---

#### Edit a cue's properties

```
PATCH /api/cues/:uuid
Content-Type: application/json
```

Send only the fields you want to change. Read-only fields are silently ignored.

```bash
curl -X PATCH http://localhost:8080/api/cues/a1b2c3d4-... \
  -H "Content-Type: application/json" \
  -d '{"displayName": "Walk-in Music", "volume": 0.8, "endBehavior": {"action": "loop"}}'
```

**Response**

```json
{
  "success": true,
  "cue": { "uuid": "a1b2c3d4-...", "displayName": "Walk-in Music", "volume": 0.8, "..." }
}
```

---

#### List all cart slots

```
GET /api/carts
```

Returns all occupied slots (0–15) with their audio item details.

```bash
curl http://localhost:8080/api/carts
```

**Response**

```json
{
  "success": true,
  "carts": [
    {
      "slot": 0,
      "itemUuid": "a1b2c3d4-...",
      "item": { "uuid": "a1b2c3d4-...", "displayName": "Stinger 1", "..." }
    },
    {
      "slot": 2,
      "itemUuid": "e5f6a7b8-...",
      "item": { "uuid": "e5f6a7b8-...", "displayName": "Applause", "..." }
    }
  ]
}
```

---

#### Get a single cart slot

```
GET /api/carts/:slot
```

```bash
curl http://localhost:8080/api/carts/0
```

**Response**

```json
{
  "success": true,
  "cart": {
    "slot": 0,
    "itemUuid": "a1b2c3d4-...",
    "item": { "uuid": "a1b2c3d4-...", "displayName": "Stinger 1", "..." }
  }
}
```

**Error (empty slot)**

```json
{ "success": false, "message": "Cart slot is empty" }
```

---

#### Edit a cart slot's properties

```
PATCH /api/carts/:slot
Content-Type: application/json
```

Same partial-update semantics as cue editing. Send only the fields you want to change.

```bash
curl -X PATCH http://localhost:8080/api/carts/0 \
  -H "Content-Type: application/json" \
  -d '{"volume": 1.2, "duckingBehavior": {"mode": "no-ducking"}}'
```

**Response**

```json
{
  "success": true,
  "cart": {
    "slot": 0,
    "item": { "uuid": "a1b2c3d4-...", "volume": 1.2, "..." }
  }
}
```

---

#### Trigger a cue by UUID

```
GET /api/trigger/uuid/:uuid
```

```bash
curl http://localhost:8080/api/trigger/uuid/a1b2c3d4-...
```

**Response**

```json
{ "success": true, "message": "Triggered item a1b2c3d4-..." }
```

---

#### Trigger a cue by playlist index

```
GET /api/trigger/index/:index
```

Use comma-separated numbers for items nested inside groups.

```bash
curl http://localhost:8080/api/trigger/index/0       # first cue
curl http://localhost:8080/api/trigger/index/1,2     # third cue inside second group
```

**Response**

```json
{ "success": true, "message": "Triggered item at index 1,2" }
```

---

#### Trigger a cart slot

```
GET /api/trigger/cart/slot/:slot
```

```bash
curl http://localhost:8080/api/trigger/cart/slot/0   # triggers slot 1 in the UI
```

**Response**

```json
{ "success": true, "message": "Triggered cart slot 1" }
```

---

#### Stop a cue by UUID

```
GET /api/stop/uuid/:uuid
```

```bash
curl http://localhost:8080/api/stop/uuid/a1b2c3d4-...
```

**Response**

```json
{ "success": true, "message": "Stopped item a1b2c3d4-..." }
```

---

#### Stop all cues

```
GET /api/stop/all
```

```bash
curl http://localhost:8080/api/stop/all
```

**Response**

```json
{ "success": true, "message": "All cues stopped" }
```

---

#### Get project info

```
GET /api/project/info
```

Returns the full project object including all items, cart slots, and settings.

```bash
curl http://localhost:8080/api/project/info
```

**Response**

```json
{
  "success": true,
  "project": {
    "name": "My Show",
    "version": "1.0.0",
    "folderPath": "/path/to/project",
    "items": [ "..." ],
    "cartItems": [ "..." ],
    "cartOnlyItems": [ "..." ],
    "theme": { "mode": "dark", "accentColor": "#DA1E28" },
    "createdAt": "2025-01-01T00:00:00.000Z",
    "lastModified": "2025-01-01T12:00:00.000Z"
  }
}
```

---

**Tip**: Copy the UUID for any cue directly from its Properties Panel.

### Audio Ducking Explained

**Ducking** is the automatic volume adjustment of audio cues

Schedule actions at specific timepoints during playback:

- **Stop All** (default): When a new cue plays, all others stop immediately

- **Duck Others**: When a new cue plays, others reduce to a set level (e.g., 30%)

- **No Ducking**: All cues play at their normal volume simultaneously

Example use case: Background music (ducking) + voiceover (normal) = automatic volume mixing.


### Theatre Productions

- Pre-show music, sound effects, scene transitions

- Organize by act and scene with groups

- Cart player for emergency cues (phone rings, doorbells)



### Live Events

- Walk-in/walk-out music

- Intro/outro for speakers

- Award ceremony music stings



### Podcasts & Broadcasting

- Intro/outro themes

- Jingles and stings

- Background music beds


### Houses of Worship

- Service music

- Transition music between segments

- Special event audio


### DJ & Performance

- Backing tracks for live performers

- Loop playback for extended performances



## 💬 Language Support

LivePlay is available in:

- 🇬🇧 English

- 🇬🇷 Greek (Ελληνικά)


Switch languages via **View > Language** menu.


## 🤝 Contributing


LivePlay is open source! We welcome contributions:

- **Bug reports**: [Open an issue](https://github.com/tdoukinitsas/liveplay/issues)
- **Feature requests**: [Start a discussion](https://github.com/tdoukinitsas/liveplay/discussions)
- **Code contributions**: See [DEVELOP.md](/guides/DEVELOP.md) for developer documentation
- **Translations**: Help translate LivePlay to more languages - see [INTERNATIONALIZATION.md](/guides/INTERNATIONALIZATION.md)

---

## 🔄 Releases

LivePlay uses an automated CI/CD pipeline:
- ✅ Automatic builds for Windows, Linux, and macOS
- ✅ Auto-generated changelogs from commit history
- ✅ Published to [GitHub Releases](https://github.com/tdoukinitsas/liveplay/releases)

**For Contributors:** See [RELEASES.md](/guides/RELEASES.md) for the full release process documentation or [RELEASE-QUICK.md](/guides/RELEASE-QUICK.md) for a quick reference guide.

---

## 📜 License

LivePlay is licensed under the **GNU Affero General Public License v3.0** (AGPL-3.0).

This means:
- ✅ You can use it for free (personal or commercial)
- ✅ You can modify it and distribute your changes
- ✅ You must share your modifications under the same license
- ✅ You must provide source code if you run it as a network service

See [LICENSE.txt](LICENSE.txt) for full details.

---

## 👨‍💻 Developer

**Thomas Doukinitsas**
- GitHub: [@tdoukinitsas](https://github.com/tdoukinitsas)
- Project: [github.com/tdoukinitsas/liveplay](https://github.com/tdoukinitsas/liveplay)

Built with assistance from GitHub Copilot and Claude Sonnet 4.5.

For more info about what makes this project tick and how to contribute, check the "guides" folder

---

## 🙏 Acknowledgments

Built with these excellent open-source projects:
- [Electron](https://www.electronjs.org/) - Cross-platform desktop framework
- [Vue 3](https://vuejs.org/) - Progressive JavaScript framework
- [Nuxt 3](https://nuxt.com/) - Vue framework for production
- [Howler.js](https://howlerjs.com/) - Audio library
- [WaveSurfer.js](https://wavesurfer-js.org/) - Waveform visualization
- [Carbon Design System](https://carbondesignsystem.com/) - Design language
- [electron-updater](https://www.electron.build/auto-update) - Auto-update system

---


**Ready to get started? [Download LivePlay](https://github.com/tdoukinitsas/liveplay/releases) and bring your audio cues to life! 🎵**
