# LivePlay - Project Architecture & Implementation Summary

## Project Overview

LivePlay is a professional audio cue playback application designed for live events, theatre productions, and broadcast operations. It combines the power of Electron for cross-platform desktop delivery, Node.js for backend operations, and Vue 3/Nuxt 3 for a modern, reactive user interface.

## Architecture

### Technology Stack

```
┌─────────────────────────────────────────────────┐
│              Electron (Desktop App)              │
├─────────────────────────────────────────────────┤
│  Main Process          │   Renderer Process     │
│  ├─ Express Server     │   ├─ Nuxt 3           │
│  ├─ File System        │   ├─ Vue 3            │
│  ├─ IPC Handlers       │   ├─ Web Audio API    │
│  └─ REST API           │   └─ Carbon Design    │
└─────────────────────────────────────────────────┘
```

### Directory Structure

```
liveplay/
│
├── electron/                    # Electron Main Process
│   ├── main.js                 # Entry point, window management, menu
│   └── preload.js              # IPC bridge (contextBridge)
│
├── components/                  # Vue 3 Components
│   ├── WelcomeScreen.vue       # Initial project selection screen
│   ├── MainWorkspace.vue       # Main application layout
│   ├── PlaybackControls.vue    # Top section: play/pause/stop
│   ├── ActiveCueItem.vue       # Individual playing cue display
│   ├── PlaylistView.vue        # Middle-left: cue management
│   ├── PlaylistItem.vue        # Individual playlist item (recursive)
│   ├── CartPlayer.vue          # Right section: 16-slot cart
│   ├── CartSlot.vue            # Individual cart slot
│   └── PropertiesPanel.vue     # Bottom: item properties editor
│
├── composables/                 # Vue Composables (Business Logic)
│   ├── useProject.ts           # Project CRUD, item management
│   └── useAudioEngine.ts       # Audio playback, ducking, triggers
│
├── types/                       # TypeScript Definitions
│   ├── project.ts              # Data models, interfaces
│   └── global.d.ts             # Global type augmentations
│
├── assets/styles/               # Global Styles
│   ├── main.scss               # Main stylesheet with CSS variables
│   └── variables.scss          # SCSS variables for components
│
├── app.vue                      # Root Vue component
├── nuxt.config.ts              # Nuxt 3 configuration
├── tsconfig.json               # TypeScript configuration
├── package.json                # Dependencies & scripts
├── README.md                   # Comprehensive documentation
├── GETTING_STARTED.md          # Quick start guide
└── .gitignore                  # Git ignore rules
```

## Core Systems

### 1. Project Management (`composables/useProject.ts`)

**Responsibilities:**
- Create new projects with folder structure
- Open existing `.liveplay` files
- Save project data as JSON
- Manage project state (items, cart, theme)
- Item CRUD operations (add, remove, move)
- UUID-based and index-based item lookup
- Automatic index recalculation on structure changes

**Key Functions:**
```typescript
createNewProject(name, folderPath)  // Initialize new project
openProject(projectFilePath)         // Load existing project
saveProject()                        // Serialize to JSON
addItem(item, parentIndex?)          // Add audio/group item
removeItem(uuid)                     // Delete item
findItemByUuid(uuid)                // Lookup by UUID
findItemByIndex(index)              // Lookup by index array
moveItem(fromIndex, toIndex)        // Reorder items
```

### 2. Audio Engine (`composables/useAudioEngine.ts`)

**Responsibilities:**
- Web Audio API management
- Audio buffer loading and caching
- Playback control (play, stop, pause)
- Multiple simultaneous cue playback
- Ducking implementation (stop-all, duck-others, no-ducking)
- Start/end behavior execution
- Custom action scheduling
- HTTP request execution

**Key Functions:**
```typescript
initAudioContext()                   // Initialize Web Audio API
playCue(item)                       // Start audio playback
stopCue(uuid)                       // Stop specific cue
stopAllCues()                       // Emergency stop
triggerByUuid(uuid)                 // External trigger by UUID
triggerByIndex(index)               // External trigger by index
triggerGroup(group)                 // Trigger group behavior
applyDucking(cue, behavior)         // Apply ducking to active cues
```

### 3. Electron Main Process (`electron/main.js`)

**Responsibilities:**
- Window lifecycle management
- Native file dialogs (open folder, open file, select audio)
- File system operations (read, write, copy, ensure directory)
- Application menu (File, View)
- REST API server (Express)
- IPC communication with renderer

**IPC Handlers:**
```javascript
select-project-folder               // Choose directory for new project
select-project-file                 // Open .liveplay file
select-audio-files                  // Choose audio files to import
read-file                           // Read file content
write-file                          // Write file content
copy-file                           // Copy audio to media folder
ensure-directory                    // Create directories
open-folder                         // Open in Explorer/Finder
set-current-project                 // Update current project path
```

**Menu Events:**
```javascript
menu-new-project
menu-open-project
menu-save-project
menu-close-project
menu-open-project-folder
menu-toggle-dark-mode
menu-change-accent-color
```

### 4. REST API Server (`electron/main.js`)

Running on `http://localhost:8080`

**Endpoints:**
```
GET  /api/trigger/uuid/:uuid        # Trigger cue by UUID
GET  /api/trigger/index/:index      # Trigger cue by index (comma-separated)
GET  /api/stop/uuid/:uuid           # Stop cue by UUID
GET  /api/project/info              # Get current project information
```

## Data Models

### BaseItem
```typescript
interface BaseItem {
  uuid: string;              // Unique identifier
  index: number[];           // Position in hierarchy [0,1,2]
  displayName: string;       // User-visible name
  color: string;             // Hex color for UI
  type: 'audio' | 'group' | 'action';
}
```

### AudioItem extends BaseItem
```typescript
interface AudioItem extends BaseItem {
  type: 'audio';
  mediaFileName: string;        // Original filename
  mediaPath: string;            // Absolute path to media
  waveformPath: string;         // Path to waveform JSON
  inPoint: number;              // Start time in seconds
  outPoint: number;             // End time in seconds
  volume: number;               // 0-2 (1 = normal)
  duration: number;             // Total duration
  endBehavior: EndBehavior;     // What happens when cue ends
  startBehavior: StartBehavior; // What happens when cue starts
  customActions: CustomAction[]; // Timed actions
  duckingBehavior: DuckingBehavior;
}
```

### GroupItem extends BaseItem
```typescript
interface GroupItem extends BaseItem {
  type: 'group';
  children: (AudioItem | GroupItem)[];
  startBehavior: GroupStartBehavior;
  endBehavior: EndBehavior;
  isExpanded: boolean;
}
```

### Project
```typescript
interface Project {
  name: string;
  version: string;
  folderPath: string;
  items: (AudioItem | GroupItem)[];
  cartItems: CartItem[];
  theme: Theme;
  createdAt: string;
  lastModified: string;
}
```

## User Interface Layout

```
┌─────────────────────────────────────────────────────────────┐
│  Menu Bar (File, View)                                       │
├─────────────────────────────────────────────────────────────┤
│  Playback Controls (120px height)                            │
│  [PLAY] [Pause] [Stop]  |  Active Cues Display              │
├─────────────────────────────────────────┬───────────────────┤
│  Playlist View (66% width)              │  Cart Player      │
│  ┌───────────────────────────────────┐  │  (33% width)      │
│  │ [Import Audio] [Add Group]        │  │  ┌───┬───┐       │
│  ├───────────────────────────────────┤  │  │ 1 │ 2 │       │
│  │ □ 0   Opening Music          ▶ ⏹🗑│  │  ├───┼───┤       │
│  │ ▼ 1   Act 1                  ▶  🗑│  │  │ 3 │ 4 │       │
│  │   □ 1,0 Scene 1 Music        ▶ ⏹🗑│  │  ├───┼───┤       │
│  │   □ 1,1 Scene 2 Music        ▶ ⏹🗑│  │  │ 5 │ 6 │       │
│  │ □ 2   Intermission            ▶ ⏹🗑│  │  └───┴───┘       │
│  │ □ 3   Finale                  ▶ ⏹🗑│  │  ...up to 16     │
│  └───────────────────────────────────┘  │                   │
│                                          │                   │
├──────────────────────────────────────────┴───────────────────┤
│  Properties Panel (25% height) - Shows when item selected    │
│  [Basic] [Media] [Playback] [Ducking] [End] [Start] [Actions]│
│  Display Name: [____________]  Color: [■■■■■■■■■■■■■■■■]     │
│  UUID: [abc-123...]  [Copy]   Index: [0,1,2]                 │
└─────────────────────────────────────────────────────────────┘
```

## UI Components Breakdown

### WelcomeScreen.vue
- **Purpose**: Initial landing page when no project is open
- **Actions**: 
  - Create New Project (prompts for folder + name)
  - Open Existing Project (file picker for .liveplay files)
- **Note**: Uses custom inline dialog instead of `prompt()` (not supported in Electron renderer)

### MainWorkspace.vue
- **Purpose**: Main application shell
- **Layout**: Organizes PlaybackControls, PlaylistView, CartPlayer, PropertiesPanel
- **Listeners**: 
  - Menu events (save, close, open folder)
  - API trigger events from REST API
  - F1 key for quick play

### PlaybackControls.vue
- **Purpose**: Global playback controls and active cues monitoring
- **Features**:
  - Large PLAY button (disabled when nothing selected)
  - Pause/Stop buttons (disabled when no active cues)
  - Real-time list of playing cues with progress bars
  - Each active cue shows: name, time remaining, progress, stop button

### ActiveCueItem.vue
- **Purpose**: Individual playing cue display within PlaybackControls
- **Features**:
  - Displays cue name, current time, remaining time
  - Interactive progress bar (seeking simplified in this version)
  - Stop button per cue
  - Animated progress using requestAnimationFrame

### PlaylistView.vue
- **Purpose**: Main cue list management
- **Features**:
  - Import Audio button (file picker)
  - Add Group button
  - Drag-and-drop zone for audio files
  - Recursive display of items with PlaylistItem
  - Empty state with helpful hint

### PlaylistItem.vue
- **Purpose**: Individual playlist item (audio or group)
- **Features**:
  - Visual indicator of item type
  - Color-coded left border
  - Index display (e.g., "0,1,2")
  - Expand/collapse button for groups
  - Inline play/stop/delete buttons (appear on hover)
  - Click to select and open properties
  - Draggable for reordering (simplified in this version)
  - Recursive rendering for nested groups

### CartPlayer.vue
- **Purpose**: Quick-access cart with 16 slots
- **Features**:
  - 2-column grid layout (8 rows)
  - Displays CartSlot components
  - Maps cart item slots to actual audio items

### CartSlot.vue
- **Purpose**: Individual cart button
- **Features**:
  - Shows slot number (1-16)
  - Displays assigned cue name
  - Click anywhere to trigger (no arming needed)
  - Edit button (⚙) opens properties panel
  - Drop zone for assigning cues from playlist
  - Visual feedback when playing (pulsing animation)
  - Empty state with "Drag item here" hint

### PropertiesPanel.vue
- **Purpose**: Comprehensive item property editor
- **Layout**: Horizontal scrolling sections
- **Sections**:
  1. **Basic Info**: Name, color picker (16 presets), UUID (copyable), index, API URL
  2. **Media** (audio only): File name, replace media button, duration
  3. **Playback** (audio only): Volume slider (0-2), in/out point inputs
  4. **Ducking Behavior** (audio only): Mode dropdown, duck level slider
  5. **End Behavior**: Action dropdown, target UUID/index inputs
  6. **Start Behavior**: Action dropdown, target UUID/index inputs
  7. **Group Behavior** (groups only): Start action (play-first/play-all)

## State Management

Using Vue 3 Composition API with `useState` (Nuxt 3 composable):

```typescript
// Global reactive state
currentProject: Ref<Project | null>
selectedItem: Ref<BaseItem | null>
activeCues: Ref<Map<string, ActiveCue>>
audioContext: Ref<AudioContext | null>
theme: Ref<'light' | 'dark'>
```

## Audio Playback Flow

```
1. User triggers cue (click, F1, API, cart)
   ↓
2. useAudioEngine.playCue(item)
   ↓
3. Load audio buffer from media folder
   ↓
4. Create AudioBufferSourceNode + GainNode
   ↓
5. Apply ducking to other active cues
   ↓
6. Schedule custom actions at timepoints
   ↓
7. Start playback with in/out points
   ↓
8. Add to activeCues map
   ↓
9. Execute start behavior (if any)
   ↓
10. Monitor playback via requestAnimationFrame
   ↓
11. On end: execute end behavior, remove from activeCues
   ↓
12. Restore ducked volumes if applicable
```

## Ducking Modes

1. **Stop All** (default):
   - Stops all other active cues immediately
   - Ensures only one cue plays at a time

2. **No Ducking**:
   - Allows multiple cues to play simultaneously
   - No volume adjustments

3. **Duck Others**:
   - Reduces volume of other active cues
   - Stores original volume
   - Restores volume when ducking cue ends
   - Duck level: 0-1 (multiplier for other cues' volumes)

## Project File Structure

When you create a project, LivePlay creates:

```
MyProject/
├── MyProject.liveplay              # JSON file with all data
├── media/                     # Imported audio files
│   ├── opening.mp3
│   ├── scene1.wav
│   └── finale.flac
└── waveforms/                 # Generated waveform data
    ├── uuid-1.json
    ├── uuid-2.json
    └── uuid-3.json
```

### .liveplay File Example

```json
{
  "name": "My Show",
  "version": "1.0.0",
  "folderPath": "C:/Projects/MyShow",
  "items": [
    {
      "uuid": "abc-123-def-456",
      "index": [0],
      "displayName": "Opening Music",
      "color": "#FF0000",
      "type": "audio",
      "mediaFileName": "opening.mp3",
      "mediaPath": "C:/Projects/MyShow/media/opening.mp3",
      "waveformPath": "C:/Projects/MyShow/waveforms/abc-123.json",
      "inPoint": 0,
      "outPoint": 120,
      "volume": 1.0,
      "duration": 120,
      "endBehavior": { "action": "next" },
      "startBehavior": { "action": "nothing" },
      "customActions": [],
      "duckingBehavior": { "mode": "stop-all" }
    }
  ],
  "cartItems": [
    { "slot": 0, "itemUuid": "abc-123-def-456" }
  ],
  "theme": {
    "mode": "dark",
    "accentColor": "#0066FF"
  },
  "createdAt": "2025-10-31T12:00:00.000Z",
  "lastModified": "2025-10-31T12:30:00.000Z"
}
```

## Theming System

### CSS Variables Architecture

All colors and spacing use CSS variables defined in `assets/styles/main.scss`:

```scss
:root {
  --spacing-xs: 4px;
  --spacing-sm: 8px;
  --spacing-md: 16px;
  // ... more spacing

  --border-radius-sm: 2px;
  --border-radius-md: 4px;
  // ... more sizing

  --transition-fast: 150ms ease;
  // ... more timings
}

[data-theme='light'] {
  --color-background: #ffffff;
  --color-surface: #f4f4f4;
  --color-text-primary: #161616;
  --color-accent: var(--color-accent-custom, #0f62fe);
  // ... more colors
}

[data-theme='dark'] {
  --color-background: #161616;
  --color-surface: #262626;
  --color-text-primary: #f4f4f4;
  --color-accent: var(--color-accent-custom, #0f62fe);
  // ... more colors
}
```

### Theme Switching

```typescript
// In app.vue
const theme = useState('theme', () => 'dark');

// Toggle via menu
window.electronAPI.onMenuToggleDarkMode(() => {
  theme.value = theme.value === 'dark' ? 'light' : 'dark';
  if (currentProject.value) {
    currentProject.value.theme.mode = theme.value;
  }
});

// Applied to root element
<div id="app" :data-theme="theme">
```

### Accent Color Customization

```typescript
// Set accent color
document.documentElement.style.setProperty(
  '--color-accent-custom', 
  project.theme.accentColor
);
```

## Extensibility

### Adding New Item Types

The architecture is designed for easy extension:

1. **Add Type Definition** (`types/project.ts`):
```typescript
export interface HttpRequestItem extends BaseItem {
  type: 'http-request';
  method: 'GET' | 'POST' | 'PUT' | 'DELETE';
  url: string;
  body?: Record<string, any>;
}
```

2. **Create Component** (`components/HttpRequestItem.vue`)

3. **Add Trigger Logic** (`composables/useAudioEngine.ts`):
```typescript
const triggerHttpRequest = (item: HttpRequestItem) => {
  // Implementation
};
```

4. **Update Properties Panel** to handle new type

5. **Add to Playlist** rendering logic

## Known Limitations & TODOs

### Current Limitations

1. **Audio Seeking**: 
   - Seeking during playback requires stopping and restarting
   - Web Audio API doesn't support native seeking on playing sources

2. **Pause/Resume**:
   - Currently implemented as stop
   - True pause/resume would require tracking playback position and resuming from there

3. **Waveform Generation**:
   - Simplified implementation using random data
   - Production should use proper audio analysis (AnalyserNode, FFT)

4. **Drag and Drop Reordering**:
   - Structure is in place but full implementation simplified
   - Would need drop zone indicators, visual feedback, and complex state updates

5. **Custom Actions UI**:
   - Data model supports custom actions at timepoints
   - UI for adding/editing them is not yet implemented

### Future Enhancements

1. **WaveSurfer.js Integration**: Real, interactive waveforms
2. **Custom Actions Editor**: Visual timeline for adding actions
3. **MIDI Support**: Trigger cues with MIDI controllers
4. **OSC Support**: Open Sound Control for theatre/broadcast integration
5. **Timecode Sync**: SMPTE/MTC synchronization
6. **Multi-output Routing**: Route cues to different audio outputs
7. **Effects Chain**: Built-in audio effects (EQ, compression, etc.)
8. **Fade In/Out**: Automatic fades with configurable curves
9. **Crossfades**: Automatic crossfading between cues
10. **Cue Macros**: Group multiple actions into reusable macros
11. **Show Mode**: Simplified, distraction-free operation mode
12. **Cue History**: Track what's been played and when
13. **Export**: Generate reports, cue sheets, etc.

## Development Workflow

### Running the App

```powershell
# Install dependencies (first time only)
npm install

# Start development server
npm run dev
```

### Building for Production

```powershell
# Build Nuxt app
npm run build

# Package Electron app
npm run build:electron
```

### Adding a New Component

1. Create `.vue` file in `components/`
2. Use `<script setup>` syntax
3. Import types from `~/types/project`
4. Use composables: `useProject()`, `useAudioEngine()`
5. Apply CSS variables for theming
6. Register globally (auto-imported by Nuxt 3)

### Modifying Data Models

1. Update interfaces in `types/project.ts`
2. Update default values if needed
3. Update components that display/edit that data
4. Update serialization/deserialization if structure changed
5. Consider migration strategy for existing projects

## Performance Considerations

1. **Audio Buffer Caching**: 
   - Buffers are loaded once per file
   - Reused for multiple playback instances
   - Consider LRU cache for large projects

2. **Waveform Caching**:
   - Generated once, stored as JSON
   - Loaded on-demand
   - Consider lazy loading for large playlists

3. **Vue Reactivity**:
   - Use `shallowRef` for large data structures
   - Avoid deep reactivity on audio buffers
   - Use `v-once` for static content

4. **RequestAnimationFrame**:
   - Used for smooth progress updates
   - Properly cleaned up on unmount
   - One RAF per active cue

## Security Considerations

1. **File System Access**: Limited to user-selected folders
2. **HTTP Requests**: Custom actions can make arbitrary HTTP requests
   - Consider whitelist/blacklist in production
3. **API Server**: Runs locally, no authentication
   - Consider adding authentication for production
4. **Content Security Policy**: Relaxed for Electron
   - Tighten for production if loading external content

## Testing Strategy

### Recommended Test Coverage

1. **Unit Tests**:
   - Project management functions
   - Audio engine logic
   - Utility functions

2. **Component Tests**:
   - Vue components in isolation
   - Props, events, slots
   - User interactions

3. **Integration Tests**:
   - Electron IPC communication
   - File system operations
   - Audio playback end-to-end

4. **E2E Tests**:
   - Full user workflows
   - Project creation, save, load
   - Audio import and playback

## Deployment

### Windows
```powershell
npm run build:electron
# Creates: dist-electron/LivePlay Setup x.x.x.exe
```

### macOS
```bash
npm run build:electron
# Creates: dist-electron/LivePlay-x.x.x.dmg
```

### Linux
```bash
npm run build:electron
# Creates: dist-electron/LivePlay-x.x.x.AppImage
```

## Conclusion

LivePlay is a comprehensive audio cue playback system with a solid foundation for professional use. The modular architecture, extensible data models, and modern tech stack provide a robust platform for future enhancements.

Key strengths:
- ✅ Clean separation of concerns
- ✅ Type-safe with TypeScript
- ✅ Reactive UI with Vue 3
- ✅ Cross-platform with Electron
- ✅ REST API for external control
- ✅ Extensible item type system
- ✅ Professional theming system
- ✅ Comprehensive documentation

The application is ready for further development and customization to meet specific production needs.
