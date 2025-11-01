# LivePlay

![Main liveplay user interface, with playlist editor, cue cart and properties panel](/public/liveplay_screenshot.jpg)

A professional open source audio cue playback application built with Electron, Node.js, and Nuxt 3/Vue for live events and productions.
Currently under active development.

Built with help from Github Copilot and Claude Sonnet 4.5

## Features

- **Project Management**: Create and manage audio cue projects with all data stored as JSON
- **Playlist System**: Organize audio cues in a hierarchical structure with groups
- **Advanced Playback Controls**: 
  - Play, pause, stop controls
  - Multiple simultaneous audio playback
  - Real-time progress tracking with seek capability
  - Audio ducking (reduce volume of other cues or stop them entirely)
- **Cart Player**: 16-slot cart for quick-access audio triggers
- **Properties Inspector**: 
  - Configure audio in/out points
  - Set volume levels (0-2x)
  - Define start/end behaviors
  - Custom actions at specific timepoints
  - HTTP request triggers
- **Waveform Display**: Visual waveform representation of audio files
- **REST API**: Trigger cues remotely via HTTP requests
- **Theming**: Light/dark mode with customizable accent colors

## Project Structure

```
liveplay/
├── electron/              # Electron main process files
│   ├── main.js           # Main process entry point
│   └── preload.js        # Preload script for IPC
├── components/           # Vue components
│   ├── WelcomeScreen.vue
│   ├── MainWorkspace.vue
│   ├── PlaybackControls.vue
│   ├── PlaylistView.vue
│   ├── PlaylistItem.vue
│   ├── CartPlayer.vue
│   ├── CartSlot.vue
│   └── PropertiesPanel.vue
├── composables/          # Vue composables
│   ├── useProject.ts     # Project management logic
│   └── useAudioEngine.ts # Audio playback engine
├── types/                # TypeScript type definitions
│   ├── project.ts        # Data models
│   └── global.d.ts       # Global type augmentations
├── assets/styles/        # Global styles
│   ├── main.scss         # Main stylesheet
│   └── variables.scss    # SCSS variables
├── app.vue               # Root component
├── nuxt.config.ts        # Nuxt configuration
├── package.json          # Dependencies
└── tsconfig.json         # TypeScript configuration
```

## Installation

1. **Clone the repository**:
   ```powershell
   cd m:\Github\liveplay
   ```

2. **Install dependencies**:
   ```powershell
   npm install
   ```

## Development

Run the application in development mode:

```powershell
npm run dev
```

This will:
1. Start the Nuxt dev server on `http://localhost:3000`
2. Launch the Electron application
3. Enable hot-reload for both frontend and backend changes

## Project File Format

LivePlay projects use the `.liveplay` (LivePlay JSON) format. A project folder contains:

```
MyProject/
├── MyProject.liveplay         # Project configuration and cue data
├── media/                # Audio files
│   ├── audio1.mp3
│   └── audio2.wav
└── waveforms/            # Generated waveform data
    ├── uuid1.json
    └── uuid2.json
```

### Project JSON Structure

```json
{
  "name": "My Show",
  "version": "1.0.0",
  "folderPath": "C:/Projects/MyShow",
  "items": [
    {
      "uuid": "...",
      "index": [0],
      "displayName": "Opening Music",
      "color": "#FF0000",
      "type": "audio",
      "mediaFileName": "opening.mp3",
      "volume": 1.0,
      "inPoint": 0,
      "outPoint": 120,
      "duckingBehavior": { "mode": "stop-all" },
      "endBehavior": { "action": "next" },
      "startBehavior": { "action": "nothing" },
      "customActions": []
    }
  ],
  "cartItems": [],
  "theme": {
    "mode": "dark",
    "accentColor": "#0066FF"
  }
}
```

## REST API

The application runs a REST API server on `http://localhost:8080`:

### Trigger Cue by UUID
```
GET /api/trigger/uuid/:uuid
```

### Trigger Cue by Index
```
GET /api/trigger/index/:index
```
Example: `/api/trigger/index/0,1,2` triggers item at index [0, 1, 2]

### Stop Cue by UUID
```
GET /api/stop/uuid/:uuid
```

### Get Project Info
```
GET /api/project/info
```

## Keyboard Shortcuts

- **F1**: Play/trigger the currently selected cue
- **Ctrl+N**: New Project
- **Ctrl+O**: Open Project
- **Ctrl+S**: Save Project
- **Ctrl+W**: Close Project
- **Ctrl+Q**: Quit Application

## Audio Item Features

### Ducking Behavior
- **Stop All**: Stops all other playing cues when this cue starts
- **No Ducking**: Allows multiple cues to play simultaneously
- **Duck Others**: Lowers the volume of other cues while this one plays

### End Behaviors
- **Nothing**: Cue ends and nothing happens
- **Next**: Automatically plays the next cue in the playlist
- **Go to Item**: Jumps to a specific cue by UUID
- **Go to Index**: Jumps to a cue by its index
- **Loop**: Repeats the current cue

### Start Behaviors
- **Nothing**: Only this cue plays
- **Play Next**: Simultaneously starts the next cue
- **Play Item**: Simultaneously starts a specific cue by UUID
- **Play Index**: Simultaneously starts a cue by index

### Custom Actions
Schedule actions at specific timepoints during playback:
- Play another cue (by UUID or index)
- Stop all other cues
- Send HTTP requests (GET, POST, PUT, DELETE)

## Building for Production

Build the Nuxt application:
```powershell
npm run build
```

Package the Electron application:
```powershell
npm run build:electron
```

This will create distributable packages in the `dist-electron` folder for your platform.

## Technology Stack

- **Electron 28**: Desktop application framework
- **Nuxt 3**: Vue.js framework for the frontend
- **Vue 3**: Reactive UI components
- **TypeScript**: Type-safe development
- **Express**: REST API server
- **Web Audio API**: Audio playback and processing
- **WaveSurfer.js**: Waveform visualization (ready for integration)
- **Carbon Design System**: Design principles and patterns (custom implementation)
- **SCSS**: Styling with CSS variables for theming

## Customization

### CSS Variables

The application uses CSS variables for easy theming. Edit `assets/styles/main.scss` to customize:

```scss
:root {
  --spacing-xs: 4px;
  --spacing-sm: 8px;
  --spacing-md: 16px;
  // ... more variables
}

[data-theme='dark'] {
  --color-background: #161616;
  --color-surface: #262626;
  --color-text-primary: #f4f4f4;
  // ... more colors
}
```

### Adding New Item Types

The architecture supports extensible item types. To add a new type:

1. Extend `BaseItem` in `types/project.ts`
2. Create a new component in `components/`
3. Add handling in `useAudioEngine.ts`
4. Update the properties panel

## Notes for Development

- `import.meta.client` is used instead of `process.client` in Nuxt 3
- `prompt()` is not supported in Electron's renderer process - use custom dialogs
- Audio files are copied to the project's media folder on import
- Waveforms are generated asynchronously and cached
- All project data is stored as JSON for easy serialization

## License

MIT

## Support

For issues, questions, or contributions, please visit the project repository.
