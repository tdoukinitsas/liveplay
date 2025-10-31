# LivePlay - Getting Started

## Quick Start Guide

### 1. Install Dependencies

Open PowerShell in the project directory and run:

```powershell
npm install
```

This will install all required dependencies including:
- Electron
- Nuxt 3
- Vue 3
- Express
- TypeScript
- And all other dependencies

### 2. Run the Development Server

```powershell
npm run dev
```

This command will:
1. Start the Nuxt development server on http://localhost:3000
2. Wait for the server to be ready
3. Launch the Electron application

### 3. Create Your First Project

1. When the application opens, you'll see two options:
   - **Create New Project**: Click this to start a new project
   - **Open Existing Project**: Use this to open a previously saved project

2. For a new project:
   - Select a folder where you want to store your project
   - Enter a project name (e.g., "My Show")
   - The app will create a `.liveplay` file and `media` and `waveforms` folders

### 4. Import Audio Files

1. Click the **"Import Audio"** button in the playlist view
2. Select one or more audio files (MP3, WAV, OGG, FLAC, M4A, AAC)
3. Files will be automatically copied to your project's media folder

**Or** drag and drop audio files directly into the playlist area!

### 5. Configure Audio Cues

1. Click on an audio item in the playlist to select it
2. The Properties Panel will appear at the bottom
3. Configure:
   - Display name
   - Color (choose from 16 preset colors)
   - Volume (0-2, where 1 is normal)
   - In/Out points (trim the audio)
   - Ducking behavior
   - End behavior (what happens when the cue finishes)
   - Start behavior (what happens when the cue starts)

### 6. Play Audio Cues

There are several ways to trigger playback:

- **Method 1**: Select a cue and click the big **"PLAY"** button at the top
- **Method 2**: Select a cue and press **F1**
- **Method 3**: Click the small play button (▶) on the cue itself
- **Method 4**: Use the REST API (see below)

### 7. Use the Cart Player

The cart player provides 16 quick-access slots:

1. Drag an audio item from the playlist to any cart slot
2. Click the cart slot to instantly play that cue
3. Click the gear icon (⚙) on a cart slot to open its properties

### 8. Organize with Groups

1. Click **"Add Group"** to create a group
2. Drag audio items into the group (implementation note: full drag-drop is simplified in this version)
3. Configure the group's behavior:
   - Play first item or play all items when triggered
   - Custom end behavior

### 9. Save Your Project

- Press **Ctrl+S** or use **File > Save Project**
- All changes are saved to the `.liveplay` file

### 10. Remote Control via API

The REST API runs on `http://localhost:8080`:

#### Trigger a cue by UUID:
```powershell
Invoke-WebRequest -Uri "http://localhost:8080/api/trigger/uuid/YOUR-UUID-HERE"
```

#### Trigger a cue by index:
```powershell
Invoke-WebRequest -Uri "http://localhost:8080/api/trigger/index/0"
```

You can copy the API URL from the Properties Panel for any audio item.

## Common Tasks

### Adding Custom Actions

1. Select an audio item
2. In the Properties Panel, scroll to the "Custom Actions" section (would need to be added)
3. Set a timepoint (in seconds)
4. Choose an action:
   - Play another cue
   - Stop all cues
   - Send an HTTP request

### Changing Theme

- **File > View > Toggle Dark Mode**: Switch between light and dark themes
- **File > View > Change Accent Color**: Customize the accent color
- These settings are saved per-project

### Opening Project Folder

- **File > Open Project Folder**: Opens the project directory in Windows Explorer
- Useful for managing media files directly

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| F1 | Play selected cue |
| Ctrl+N | New Project |
| Ctrl+O | Open Project |
| Ctrl+S | Save Project |
| Ctrl+W | Close Project |
| Ctrl+Q | Quit Application |

## Troubleshooting

### Audio files won't play
- Check that the file format is supported (MP3, WAV, OGG, FLAC, M4A, AAC)
- Ensure the file is not corrupted
- Check the browser console for errors (Ctrl+Shift+I)

### Can't see the Properties Panel
- Make sure an item is selected in the playlist
- The panel appears at the bottom when an item is clicked

### API requests not working
- Confirm the server is running (should start automatically with the app)
- Check that you're using the correct port (8080)
- Look for API server logs in the terminal

### Changes not saving
- Press Ctrl+S to manually save
- Check that you have write permissions to the project folder

## Development Notes

### TypeScript Errors
The application may show some TypeScript errors during development. These are mostly related to:
- Auto-imported composables from Nuxt 3
- Vue 3 compiler macros
- Custom global types

These errors don't prevent the application from running. To resolve them:
1. Ensure `.nuxt` folder is generated (run `npm run dev` first)
2. Restart your IDE/editor
3. Check that `tsconfig.json` is properly configured

### Waveform Generation
The waveform generation in the current implementation is simplified. For production:
- Implement proper audio analysis using Web Audio API's AnalyserNode
- Generate higher resolution waveform data
- Cache waveforms efficiently

### Audio Playback Limitations
The current implementation uses Web Audio API which has some limitations:
- Seeking during playback requires restarting the audio
- Pause/resume requires more complex state management
- Consider implementing a more robust audio engine for production use

## Next Steps

### Potential Enhancements

1. **Waveform Visualization**: Implement interactive waveform displays with WaveSurfer.js
2. **Custom Actions UI**: Add a visual editor for custom actions at timepoints
3. **Undo/Redo**: Implement command pattern for action history
4. **Keyboard Shortcuts**: Add customizable keyboard shortcuts for each cue
5. **MIDI Support**: Trigger cues with MIDI controllers
6. **Multi-track Playback**: Support for multiple audio layers
7. **Effects Processing**: Add EQ, compression, reverb, etc.
8. **Automation**: Record and playback automation for volume, pan, effects
9. **Show Mode**: Simplified view for live operation
10. **Import/Export**: Export cue lists, import from other formats

## Building for Production

When you're ready to distribute your application:

```powershell
# Build the Nuxt app
npm run build

# Package the Electron app
npm run build:electron
```

Distributables will be created in `dist-electron/` folder.

## Support

If you encounter any issues or have questions:
1. Check the README.md for detailed documentation
2. Review the code comments in the source files
3. Check the browser console and terminal for error messages

Enjoy using LivePlay! 🎵🎭
