# LivePlay

![Main liveplay user interface, with playlist editor, cue cart and properties panel](/public/liveplay_screenshot.jpg)

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

![Youtube downloader interface, showing a search window, a preview and download buttons](/public/liveplay_youtube_screenshot.jpg)

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

### 🎛️ Cart Player- **REST API**: Trigger cues remotely via HTTP requests

- **16 quick-access slots**: Instant playback buttons for your most-used cues

- **Drag to assign**: Simply drag playlist items to cart slots

- **Visual feedback**: See which cues are playing directly on the cart## Project Structure

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

Trigger cues from other applications using simple HTTP requests:

#### Trigger by UUID

```bash
curl http://localhost:8080/api/trigger/uuid/YOUR-UUID-HERE- 
```


#### Trigger by Index## Audio Item Features

```bash

curl http://localhost:8080/api/trigger/index/0### Ducking Behavior

curl http://localhost:8080/api/trigger/index/1,0  # Second item in first group

```

#### Stop by UUID

```bash

curl http://localhost:8080/api/stop/uuid/YOUR-UUID-HERE

```

#### Get Project Info

```bash

curl http://localhost:8080/api/project/info

```

**Tip**: Copy the API trigger URL from any cue's Properties Panel.

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
- **Code contributions**: See [DEVELOP.md](DEVELOP.md) for developer documentation
- **Translations**: Help translate LivePlay to more languages - see [INTERNATIONALIZATION.md](INTERNATIONALIZATION.md)

---

## 🔄 Releases

LivePlay uses an automated CI/CD pipeline:
- ✅ Automatic builds for Windows, Linux, and macOS
- ✅ Auto-generated changelogs from commit history
- ✅ Published to [GitHub Releases](https://github.com/tdoukinitsas/liveplay/releases)

**For Contributors:** See [RELEASES.md](RELEASES.md) for the full release process documentation or [RELEASE-QUICK.md](RELEASE-QUICK.md) for a quick reference guide.

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
