# LivePlay Feature Enhancements Implementation Plan

## Overview

This plan covers 9 major features to enhance LivePlay's audio playback capabilities, UI flexibility, and workflow efficiency. Implementation is organized in 5 phases over approximately 10-12 weeks.

## User Clarifications

- **Multi-output routing**: Software bus mixing (single output device) - simpler implementation, hardware routing can be added later
- **EOD point**: User-adjustable marker (not auto-calculated) - provides creative flexibility for timing
- **Cart overwrite**: Replace with confirmation dialog - safest approach
- **Cart slot maximum**: 64 slots - allows power users flexibility

---

## Phase 1: Foundation & Cleanup (Week 1-2)

### Feature 1: Remove YouTube Import Functionality

**Priority**: HIGH | **Effort**: 4-6 hours | **Risk**: LOW

#### Objective
Complete removal of all YouTube download/import functionality, including UI components, IPC handlers, dependencies, and documentation.

#### Files to Delete
- `components/YouTubeImportModal.vue` - Complete YouTube import modal component
- `public/screenshots/liveplay_screenshot_youtube*.jpg` - YouTube feature screenshots

#### Files to Modify

**1. components/PlaylistView.vue**
- Remove YouTube import button from toolbar
- Remove YouTubeImportModal component import and usage
- Remove showYouTubeModal ref

**2. electron/main.js** (~250 lines)
- Remove YouTube package imports (lines 7-8): `youtube-search-api`, `yt-dlp-wrap`
- Remove `initializeYtDlp()` function and related variables
- Remove IPC handlers: `search-youtube`, `download-youtube-audio`
- Remove yt-dlp initialization calls

**3. electron/preload.js** (~78 lines)
- Remove `searchYouTube()` API export
- Remove `downloadYouTubeAudio()` API export
- Remove YouTube progress event listeners

**4. types/global.d.ts** (~20 lines)
- Remove YouTube-related TypeScript interface definitions

**5. package.json** (both root and docs-site)
- Remove dependencies:
  - `youtube-mp3-downloader: ^0.7.11`
  - `youtube-search-api: ^2.0.1`
  - `yt-dlp-wrap: ^2.3.12`
  - `play-dl: ^1.9.7`
- Run `npm install` after changes

**6. Localization files** (48 files: locales/*.json and docs-site/public/locales/*.json)
- Remove `youtube.*` translation keys from each JSON file

**7. Documentation**
- README.md - Remove YouTube feature from features list
- guides/DEVELOP.md - Remove YouTube component documentation
- guides/INTERNATIONALIZATION.md - Remove YouTube locale references
- docs-site/app.vue - Remove YouTube screenshot and description

**8. .gitignore**
- Remove `yt-dlp.exe` pattern

#### Testing
- Verify app builds successfully with `npm run build`
- Test audio import still works
- Ensure no console errors about missing YouTube modules
- Validate all locale JSON files

---

### Feature 2: Vertical Properties Panel Resize

**Priority**: MEDIUM | **Effort**: 6-8 hours | **Risk**: LOW

#### Objective
Add vertical resize capability to properties panel, following the existing horizontal resize pattern used for Playlist/Cart split.

#### Implementation Approach
Mirror the existing horizontal resize logic (MainWorkspace.vue lines 52-98) but for vertical axis.

#### Files to Modify

**1. components/MainWorkspace.vue**

Add state:
```typescript
const propertiesPanelHeight = ref(300); // Default from CSS variable
const isResizingVertical = ref(false);
const propertiesPanelCollapsed = ref(false);
```

Add resize handle between workspace-content and PropertiesPanel:
```vue
<div
  v-if="selectedItem && !propertiesPanelCollapsed"
  class="properties-resize-handle"
  @mousedown="startVerticalResize"
></div>
```

Add resize handler:
```typescript
const startVerticalResize = (e: MouseEvent) => {
  isResizingVertical.value = true;
  e.preventDefault();

  const handleMouseMove = (e: MouseEvent) => {
    if (!isResizingVertical.value) return;

    const container = document.querySelector('.main-workspace');
    if (!container) return;

    const rect = container.getBoundingClientRect();
    const newHeight = rect.bottom - e.clientY;

    const snapThreshold = 50;
    const minHeight = 150;
    const maxHeight = rect.height * 0.8;

    // Snap closed
    if (newHeight < snapThreshold) {
      propertiesPanelCollapsed.value = true;
      return;
    }

    // Snap maximize
    if (newHeight > maxHeight) {
      propertiesPanelHeight.value = maxHeight;
      return;
    }

    propertiesPanelCollapsed.value = false;
    propertiesPanelHeight.value = Math.max(minHeight, Math.min(maxHeight, newHeight));
  };

  const handleMouseUp = () => {
    isResizingVertical.value = false;
    document.removeEventListener('mousemove', handleMouseMove);
    document.removeEventListener('mouseup', handleMouseUp);
  };

  document.addEventListener('mousemove', handleMouseMove);
  document.addEventListener('mouseup', handleMouseUp);
};
```

Persist height in localStorage:
```typescript
watch(propertiesPanelHeight, (height) => {
  if (import.meta.client) {
    localStorage.setItem('liveplay-properties-panel-height', height.toString());
  }
});

onMounted(() => {
  if (import.meta.client) {
    const stored = localStorage.getItem('liveplay-properties-panel-height');
    if (stored) {
      propertiesPanelHeight.value = parseInt(stored);
    }
  }
});
```

**2. components/PropertiesPanel.vue**
- Remove fixed height from CSS (line 650)
- Make height dynamic via parent style binding
- Ensure content scrolls properly at different heights

#### Testing
- Test resize drag functionality
- Verify snap zones (close at bottom, maximize at top)
- Test at different viewport sizes
- Ensure WaveformTrimmer adapts to height changes
- Verify persistence across app restarts

---

## Phase 2: Audio Engine Core (Week 3-5)

### Feature 3: Multi Audio Output Routing

**Priority**: HIGH | **Effort**: 16-24 hours | **Risk**: MEDIUM

#### Objective
Implement software bus mixing system allowing independent volume/mute control for Playlist, Cart, Cue/Editor, and Master buses.

#### Architecture

```
Audio Sources (Howl instances)
        ↓
    Router (determines bus)
        ↓
    ┌────┴────┬──────────┬──────────┐
    ↓         ↓          ↓          ↓
Playlist   Cart    Cue/Editor   Master
  Bus      Bus        Bus         Bus
 (Gain)   (Gain)     (Gain)     (Gain)
    └────────┴──────────┴──────────┘
                ↓
        AudioContext.destination
```

#### Files to Create

**1. composables/useAudioBuses.ts** (NEW)

```typescript
import { Howl } from 'howler';

export type AudioBusType = 'playlist' | 'cart' | 'cue-editor' | 'master';

interface AudioBus {
  name: string;
  type: AudioBusType;
  gainNode: GainNode;
  volume: number; // 0-1 linear
  muted: boolean;
}

export const useAudioBuses = () => {
  const audioContext = Howl.ctx; // Access Howler's AudioContext
  const buses = useState<Map<AudioBusType, AudioBus>>('audioBuses', () => new Map());

  const initializeBuses = () => {
    if (buses.value.size > 0) return;

    const busTypes: AudioBusType[] = ['playlist', 'cart', 'cue-editor', 'master'];

    busTypes.forEach(type => {
      const gainNode = audioContext.createGain();

      // Connect to destination (or master bus)
      if (type === 'master') {
        gainNode.connect(audioContext.destination);
      } else {
        // Non-master buses connect to master
        const masterBus = buses.value.get('master');
        if (masterBus) {
          gainNode.connect(masterBus.gainNode);
        }
      }

      buses.value.set(type, {
        name: type,
        type,
        gainNode,
        volume: 1.0,
        muted: false
      });
    });
  };

  const routeToAudioBus = (howl: Howl, busType: AudioBusType) => {
    const bus = buses.value.get(busType);
    if (!bus) return;

    // Access Howler's internal Web Audio nodes
    const sounds = (howl as any)._sounds;
    if (sounds && sounds.length > 0) {
      const sound = sounds[0];
      if (sound._node) {
        sound._node.disconnect();
        sound._node.connect(bus.gainNode);
      }
    }
  };

  const setBusVolume = (busType: AudioBusType, volume: number) => {
    const bus = buses.value.get(busType);
    if (bus) {
      bus.volume = volume;
      bus.gainNode.gain.setValueAtTime(volume, audioContext.currentTime);
    }
  };

  const muteBus = (busType: AudioBusType, muted: boolean) => {
    const bus = buses.value.get(busType);
    if (bus) {
      bus.muted = muted;
      const targetVolume = muted ? 0 : bus.volume;
      bus.gainNode.gain.setValueAtTime(targetVolume, audioContext.currentTime);
    }
  };

  return {
    buses,
    initializeBuses,
    routeToAudioBus,
    setBusVolume,
    muteBus
  };
};
```

#### Files to Modify

**1. composables/useAudioEngine.ts**

Import and initialize buses:
```typescript
const { initializeBuses, routeToAudioBus } = useAudioBuses();

// In playCue() function, after Howl creation (around line 225):
initializeBuses(); // Ensure buses exist

// Determine bus based on item type
const busType: AudioBusType = item.index[0] === -1 ? 'cart' : 'playlist';

// Route to appropriate bus
routeToAudioBus(howl, busType);
```

**2. components/PlaybackControls.vue**

Add bus controls UI:
- Master volume fader
- Optional per-bus volume controls (in expandable section or modal)
- Mute buttons for each bus

**3. electron/main.js** (API endpoints)

Add HTTP API endpoints:
- `GET /api/buses` - Get all audio buses and states
- `POST /api/buses/:busType/volume` - Set bus volume
- `POST /api/buses/:busType/mute` - Mute/unmute bus

#### Settings

Store bus preferences in localStorage:
```typescript
interface BusSettings {
  playlistVolume: number;
  cartVolume: number;
  cueEditorVolume: number;
  masterVolume: number;
  playlistMuted: boolean;
  cartMuted: boolean;
  cueEditorMuted: boolean;
}
```

#### Testing
- Verify audio routes correctly based on item type (playlist vs cart)
- Test bus volume controls affect only routed items
- Ensure no audio glitches or dropouts
- Test multiple simultaneous items on different buses
- Performance test with many active cues

---

### Feature 4: Audio Play Mode Changes (EOD vs Crossfade)

**Priority**: HIGH | **Effort**: 12-16 hours | **Risk**: MEDIUM

#### Objective
- Add global setting for crossfade vs EOD (End-Of-Deck) mode
- Implement user-adjustable EOD marker per track
- Rename `playFade` → `fadeIn`, `stopFade` → `fadeOut`
- Change `fadeOut` behavior: actual stop time = `eodPoint` + `fadeOut`

#### EOD Mode Behavior
When a track reaches its EOD point:
1. Next track starts at full volume immediately
2. Current track fades out over `fadeOut` duration
3. Both tracks play simultaneously during fade
4. Current track stops at `eodPoint + fadeOut`

#### Files to Create

**1. composables/useAppSettings.ts** (NEW)

```typescript
export type PlayMode = 'crossfade' | 'eod';

export interface AppSettings {
  playMode: PlayMode;
  defaultFadeIn: number;
  defaultFadeOut: number;
  // ... other global settings
}

export const useAppSettings = () => {
  const settings = useState<AppSettings>('appSettings', () => ({
    playMode: 'crossfade',
    defaultFadeIn: 0,
    defaultFadeOut: 0
  }));

  const loadSettings = () => {
    if (import.meta.client) {
      const stored = localStorage.getItem('liveplay-app-settings');
      if (stored) {
        Object.assign(settings.value, JSON.parse(stored));
      }
    }
  };

  const saveSettings = () => {
    if (import.meta.client) {
      localStorage.setItem('liveplay-app-settings', JSON.stringify(settings.value));
    }
  };

  return { settings, loadSettings, saveSettings };
};
```

**2. components/SettingsModal.vue** (NEW)

Modal for global app settings:
- Play mode toggle (crossfade / EOD)
- Default fade durations
- Accessible from menu or keyboard shortcut

#### Files to Modify

**1. types/project.ts**

Rename fields:
```typescript
export interface AudioItem extends BaseItem {
  // ... existing fields
  fadeIn: number;      // was: playFade
  fadeOut: number;     // was: stopFade
  eodPoint?: number;   // NEW: user-adjustable EOD marker (seconds)
  // ... rest of fields
}
```

Update defaults:
```typescript
export const DEFAULT_AUDIO_ITEM: Partial<AudioItem> = {
  // ...
  fadeIn: 0,      // was: playFade
  fadeOut: 0,     // was: stopFade
  // ...
};
```

**2. composables/useAudioEngine.ts**

Update all references (search and replace):
- `playFade` → `fadeIn`
- `stopFade` → `fadeOut`

Modify fade-out logic (around line 347):
```typescript
const { settings } = useAppSettings();

if (settings.value.playMode === 'eod' && item.eodPoint !== undefined && item.fadeOut > 0 && !cue.stopFadeTriggered) {
  const timeToEod = item.eodPoint - cue.currentTime;

  if (timeToEod <= 0.1) { // Reached EOD point
    cue.stopFadeTriggered = true;

    // Start next track at FULL volume (no fade)
    const nextItem = getNextItem(item);
    if (nextItem) {
      playCue(nextItem, { skipFadeIn: true }); // New option to skip fade-in
    }

    // Fade out current track over fadeOut duration
    const currentVol = howl.volume();
    howl.fade(currentVol, 0, item.fadeOut * 1000);

    // Schedule stop at eodPoint + fadeOut
    setTimeout(() => {
      stopCue(item.uuid);
    }, item.fadeOut * 1000);
  }
}
```

**3. components/PropertiesPanel.vue**

Update method names:
- `handlePlayFadeUpdate` → `handleFadeInUpdate`
- `handleStopFadeUpdate` → `handleFadeOutUpdate`

Update labels:
- "Play Fade" → "Fade In"
- "Stop Fade" → "Fade Out"

Add EOD Point control:
```vue
<div v-if="settings.playMode === 'eod'" class="property-field">
  <label>EOD Point (s)</label>
  <input
    type="number"
    :value="audioItem.eodPoint || (audioItem.outPoint - audioItem.fadeOut)"
    @input="handleEodPointUpdate"
    :min="audioItem.inPoint"
    :max="audioItem.outPoint"
    step="0.1"
  />
</div>
```

**4. components/WaveformTrimmer.vue**

Rename emits:
- `update:play-fade` → `update:fade-in`
- `update:stop-fade` → `update:fade-out`

Add EOD marker visualization:
- Blue vertical line at eodPoint position (draggable)
- Show "EOD" label
- Only visible when playMode === 'eod'

#### Migration Strategy

On project load, check for old field names and rename:
```typescript
const migrateAudioItem = (item: AudioItem) => {
  if ('playFade' in item) {
    item.fadeIn = (item as any).playFade;
    delete (item as any).playFade;
  }
  if ('stopFade' in item) {
    item.fadeOut = (item as any).stopFade;
    delete (item as any).stopFade;
  }
  // Set default eodPoint if not present
  if (item.eodPoint === undefined) {
    item.eodPoint = (item.outPoint || item.duration) - item.fadeOut;
  }
};
```

#### Testing
- Test crossfade mode behaves as before
- Test EOD mode: next track at full volume, current fades out
- Verify timing accuracy (eodPoint + fadeOut = actual stop)
- Test draggable EOD marker in waveform
- Test with different fade durations
- Ensure cart items work correctly

---

## Phase 3: Editor & Preview (Week 6-7)

### Feature 5: Audio Editor/Preview in Properties Panel

**Priority**: HIGH | **Effort**: 16-20 hours | **Risk**: MEDIUM

#### Objective
Add separate "editor" audio instance in properties panel for previewing in/out point changes. Routes to 'cue-editor' bus (from Feature 3).

#### User Workflow
1. Select audio item in playlist
2. Properties panel opens
3. Click "Preview" in WaveformTrimmer
4. Editor instance plays (routes to cue-editor bus)
5. Adjust in/out points while previewing
6. Editor updates in real-time

#### Files to Modify

**1. composables/useAudioEngine.ts**

Add editor state and functions:
```typescript
const editorCue = useState<ActiveCueState | null>('editorCue', () => null);

const playEditor = async (item: AudioItem, options?: { startTime?: number }): Promise<boolean> => {
  try {
    // Stop existing editor if playing
    if (editorCue.value) {
      stopEditor();
    }

    const audioPath = `${currentProject.value.folderPath}/media/${item.mediaFileName}`;
    const fileUrl = 'file:///' + audioPath.replace(/\\/g, '/');

    const startTime = options?.startTime || item.inPoint || 0;

    const howl = new Howl({
      src: [fileUrl],
      html5: true,
      volume: 1.0,
      sprite: {
        main: [
          (item.inPoint || 0) * 1000,
          ((item.outPoint || item.duration) - (item.inPoint || 0)) * 1000
        ]
      },
      onload: () => {
        editorCue.value = {
          uuid: item.uuid,
          displayName: item.displayName + ' (Preview)',
          currentTime: 0,
          duration: (item.outPoint || item.duration) - (item.inPoint || 0),
          volume: 1.0,
          isDucked: false,
          isPaused: false,
          originalVolume: 1.0,
          duckedBy: new Set(),
          howl
        };

        // Set up progress tracking
        editorCue.value.progressInterval = setInterval(() => {
          if (editorCue.value && editorCue.value.howl.playing()) {
            editorCue.value.currentTime = editorCue.value.howl.seek();
          }
        }, 100);
      },
      onend: () => {
        stopEditor();
      }
    });

    // Route to cue-editor bus
    const { routeToAudioBus } = useAudioBuses();
    routeToAudioBus(howl, 'cue-editor');

    howl.play('main');
    return true;

  } catch (error) {
    console.error('Error playing editor:', error);
    return false;
  }
};

const stopEditor = () => {
  if (editorCue.value) {
    if (editorCue.value.progressInterval) {
      clearInterval(editorCue.value.progressInterval);
    }
    editorCue.value.howl.stop();
    editorCue.value.howl.unload();
    editorCue.value = null;
  }
};

const pauseEditor = () => {
  if (editorCue.value) {
    editorCue.value.howl.pause();
    editorCue.value.isPaused = true;
  }
};

const resumeEditor = () => {
  if (editorCue.value && editorCue.value.isPaused) {
    editorCue.value.howl.play();
    editorCue.value.isPaused = false;
  }
};

const seekEditor = (time: number) => {
  if (editorCue.value) {
    editorCue.value.howl.seek(time);
  }
};

// Export new functions
return {
  // ... existing exports
  editorCue,
  playEditor,
  stopEditor,
  pauseEditor,
  resumeEditor,
  seekEditor
};
```

**2. components/WaveformTrimmer.vue**

Add editor controls:
```vue
<div class="editor-controls">
  <button @click="toggleEditorPlayback" class="editor-play-btn" :class="{ playing: editorPlaying }">
    <span class="material-symbols-rounded">
      {{ editorPlaying ? (editorPaused ? 'play_arrow' : 'pause') : 'play_arrow' }}
    </span>
  </button>
  <button @click="stopEditorPlayback" class="editor-stop-btn">
    <span class="material-symbols-rounded">stop</span>
  </button>
  <div class="editor-scrubber">
    <input
      type="range"
      :min="audioItem.inPoint || 0"
      :max="audioItem.outPoint || audioItem.duration"
      v-model="editorPosition"
      @input="handleEditorSeek"
      step="0.01"
    />
  </div>
  <span class="editor-time">{{ formatTime(editorPosition) }}</span>
  <span class="editor-label">Preview</span>
</div>
```

Add script logic:
```typescript
const { playEditor, stopEditor, pauseEditor, resumeEditor, seekEditor, editorCue } = useAudioEngine();

const editorPlaying = computed(() => editorCue.value !== null);
const editorPaused = computed(() => editorCue.value?.isPaused || false);
const editorPosition = ref(props.audioItem.inPoint || 0);

const toggleEditorPlayback = () => {
  if (!editorPlaying.value) {
    playEditor(props.audioItem);
  } else if (editorPaused.value) {
    resumeEditor();
  } else {
    pauseEditor();
  }
};

const stopEditorPlayback = () => {
  stopEditor();
  editorPosition.value = props.audioItem.inPoint || 0;
};

const handleEditorSeek = () => {
  seekEditor(editorPosition.value);
};

// Update position while playing
watch(editorCue, () => {
  if (editorCue.value) {
    const interval = setInterval(() => {
      if (editorCue.value && !editorCue.value.isPaused) {
        editorPosition.value = editorCue.value.currentTime + (props.audioItem.inPoint || 0);
      }
    }, 100);

    return () => clearInterval(interval);
  }
}, { deep: true });

// Auto-stop editor when properties panel closes
onUnmounted(() => {
  stopEditor();
});
```

Add visual indicator for editor playback:
- Second playhead in waveform canvas (different color, e.g., blue)
- "PREVIEW" badge when editor is active

#### Settings

User preferences:
- **Auto-preview**: Automatically start editor when in/out points change (checkbox)
- **Loop editor**: Loop trimmed region continuously (checkbox)
- **Editor volume**: Independent volume for cue-editor bus

#### Testing
- Verify editor routes to cue-editor bus
- Test simultaneous editor + main playback (different buses)
- Ensure editor stops when expected (panel close, item deselect)
- Test in/out point changes during preview
- Verify no interference with main playback state
- Test seek/scrub functionality

---

## Phase 4: Workflow Modes (Week 8-9)

### Feature 6: Show Mode / Edit Mode

**Priority**: MEDIUM | **Effort**: 8-12 hours | **Risk**: LOW-MEDIUM

#### Objective
Add global mode toggle that changes interaction behavior: Edit mode (current) vs Show mode (performance/live).

#### Mode Behaviors

| Feature | Edit Mode | Show Mode |
|---------|-----------|-----------|
| Playlist double-click | Open properties | Play item |
| Cart click | Select item | Play/Stop toggle |
| Properties panel | Editable | Hidden |
| Drag & drop | Enabled | Disabled |
| Context menu | Full | Limited |

#### Files to Modify

**1. composables/useAppSettings.ts**

Add mode to settings:
```typescript
export interface AppSettings {
  appMode: 'edit' | 'show';
  // ... other settings
}
```

**2. components/PlaylistItem.vue**

Modify double-click handler:
```typescript
const handleDoubleClick = () => {
  const { settings } = useAppSettings();
  if (settings.value.appMode === 'show') {
    // Show mode: play the item
    if (props.item.type === 'audio') {
      const { playCue } = useAudioEngine();
      playCue(props.item as AudioItem);
    } else if (props.item.type === 'group') {
      // Play first child or all children based on startBehavior
      const { triggerGroup } = useAudioEngine();
      triggerGroup(props.item as GroupItem);
    }
  } else {
    // Edit mode: select for properties (existing behavior)
    const { selectedItem } = useProject();
    selectedItem.value = props.item;
  }
};
```

Disable drag in show mode:
```vue
<div
  class="playlist-item"
  :draggable="appMode === 'edit'"
  @dragstart="appMode === 'edit' ? handleDragStart : null"
>
```

**3. components/CartSlot.vue**

Modify click handler:
```typescript
const handleClick = () => {
  const { settings } = useAppSettings();
  const item = props.item;

  if (!item) return;

  if (settings.value.appMode === 'show') {
    // Show mode: toggle play/stop
    const { activeCues, playCue, stopCue } = useAudioEngine();
    const isPlaying = activeCues.value.has(item.uuid);

    if (isPlaying) {
      stopCue(item.uuid);
    } else {
      playCue(item);
    }
  } else {
    // Edit mode: select for properties
    const { selectedItem } = useProject();
    selectedItem.value = item;
  }
};
```

**4. components/MainWorkspace.vue**

Hide properties panel in show mode:
```vue
<PropertiesPanel v-if="selectedItem && settings.appMode === 'edit'" />
```

**5. components/ProjectHeader.vue**

Add mode toggle button:
```vue
<ModeToggle />
```

#### Files to Create

**1. components/ModeToggle.vue** (NEW)

```vue
<template>
  <div class="mode-toggle">
    <label class="mode-label">Mode:</label>
    <div class="toggle-switch" @click="toggleMode" :class="{ 'show-mode': isShowMode }">
      <div class="toggle-track">
        <div class="toggle-thumb"></div>
      </div>
      <span class="mode-text">{{ isShowMode ? 'Show' : 'Edit' }}</span>
    </div>
    <span class="mode-hint">(F2)</span>
  </div>
</template>

<script setup lang="ts">
const { settings, saveSettings } = useAppSettings();

const isShowMode = computed(() => settings.value.appMode === 'show');

const toggleMode = () => {
  settings.value.appMode = isShowMode.value ? 'edit' : 'show';
  saveSettings();
};

// Keyboard shortcut: F2
onMounted(() => {
  const handleKey = (e: KeyboardEvent) => {
    if (e.key === 'F2') {
      e.preventDefault();
      toggleMode();
    }
  };
  window.addEventListener('keydown', handleKey);
  onUnmounted(() => window.removeEventListener('keydown', handleKey));
});
</script>

<style scoped lang="scss">
.mode-toggle {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
}

.toggle-switch {
  display: flex;
  align-items: center;
  gap: var(--spacing-xs);
  cursor: pointer;
  padding: var(--spacing-xs) var(--spacing-sm);
  border-radius: var(--border-radius-lg);
  background-color: var(--color-surface);
  transition: background-color var(--transition-fast);

  &:hover {
    background-color: var(--color-surface-hover);
  }

  &.show-mode .toggle-track {
    background-color: var(--color-accent);
  }
}

.toggle-track {
  width: 40px;
  height: 20px;
  border-radius: 10px;
  background-color: var(--color-border);
  position: relative;
  transition: background-color var(--transition-fast);
}

.toggle-thumb {
  width: 16px;
  height: 16px;
  border-radius: 50%;
  background-color: white;
  position: absolute;
  top: 2px;
  left: 2px;
  transition: left var(--transition-fast);

  .show-mode & {
    left: 22px;
  }
}

.mode-text {
  font-weight: 500;
  min-width: 40px;
}

.mode-hint {
  color: var(--color-text-secondary);
  font-size: 0.875rem;
}
</style>
```

#### Visual Feedback
- Change app background tint slightly in show mode
- Show "SHOW MODE" badge in title bar
- Disable/hide menu items that are edit-only

#### Testing
- Test all interaction points in both modes
- Verify properties panel behavior
- Test mode toggle keyboard shortcut (F2)
- Ensure mode persists across app restarts
- Test drag-and-drop disabled in show mode

---

### Feature 7: Key Bindings (Editable Keyboard Shortcuts)

**Priority**: MEDIUM | **Effort**: 12-16 hours | **Risk**: MEDIUM

#### Objective
Centralized keyboard shortcut system with user-configurable mappings.

#### Default Key Bindings

| Action | Default Key | Scope |
|--------|-------------|-------|
| Play/Stop Selected | F1 | Global |
| Toggle Mode | F2 | Global |
| Play Next | F3 | Global |
| Stop All | Escape | Global |
| Focus Playlist | Ctrl+1 | Global |
| Focus Cart | Ctrl+2 | Global |
| Cart Slot 1-16 | F5-F8, Ctrl+F5-F8, etc. | Global |

#### Files to Create

**1. composables/useKeyBindings.ts** (NEW)

```typescript
export type KeyAction =
  | 'play-stop-selected'
  | 'toggle-mode'
  | 'play-next'
  | 'stop-all'
  | 'focus-playlist'
  | 'focus-cart'
  | 'cart-slot-1' | 'cart-slot-2' /* ... */ | 'cart-slot-16';

const DEFAULT_BINDINGS: Record<KeyAction, string> = {
  'play-stop-selected': 'F1',
  'toggle-mode': 'F2',
  'play-next': 'F3',
  'stop-all': 'Escape',
  'focus-playlist': 'Ctrl+1',
  'focus-cart': 'Ctrl+2',
  'cart-slot-1': 'F5',
  'cart-slot-2': 'F6',
  'cart-slot-3': 'F7',
  'cart-slot-4': 'F8',
  // ... etc for slots 5-16
};

export const useKeyBindings = () => {
  const bindings = useState<Record<KeyAction, string>>('keyBindings',
    () => ({ ...DEFAULT_BINDINGS })
  );

  const loadBindings = () => {
    if (import.meta.client) {
      const stored = localStorage.getItem('liveplay-key-bindings');
      if (stored) {
        bindings.value = { ...DEFAULT_BINDINGS, ...JSON.parse(stored) };
      }
    }
  };

  const saveBindings = () => {
    if (import.meta.client) {
      localStorage.setItem('liveplay-key-bindings', JSON.stringify(bindings.value));
    }
  };

  const setBinding = (action: KeyAction, key: string) => {
    bindings.value[action] = key;
    saveBindings();
  };

  const resetToDefaults = () => {
    bindings.value = { ...DEFAULT_BINDINGS };
    saveBindings();
  };

  const formatKeyCombo = (e: KeyboardEvent): string => {
    const parts: string[] = [];
    if (e.ctrlKey) parts.push('Ctrl');
    if (e.altKey) parts.push('Alt');
    if (e.shiftKey) parts.push('Shift');
    if (e.metaKey) parts.push('Meta');

    const key = e.key === ' ' ? 'Space' : e.key;
    parts.push(key);

    return parts.join('+');
  };

  const registerKeyListener = () => {
    if (!import.meta.client) return;

    const handleKeyDown = (e: KeyboardEvent) => {
      const key = formatKeyCombo(e);

      // Find action for this key
      const action = Object.entries(bindings.value).find(
        ([_, boundKey]) => boundKey === key
      )?.[0] as KeyAction | undefined;

      if (action) {
        e.preventDefault();
        executeAction(action);
      }
    };

    window.addEventListener('keydown', handleKeyDown);

    return () => window.removeEventListener('keydown', handleKeyDown);
  };

  const executeAction = (action: KeyAction) => {
    const { selectedItem } = useProject();
    const { playCue, stopCue, activeCues, panicStop } = useAudioEngine();
    const { settings, saveSettings } = useAppSettings();

    switch (action) {
      case 'play-stop-selected':
        if (selectedItem.value && selectedItem.value.type === 'audio') {
          const isPlaying = activeCues.value.has(selectedItem.value.uuid);
          if (isPlaying) {
            stopCue(selectedItem.value.uuid);
          } else {
            playCue(selectedItem.value as AudioItem);
          }
        }
        break;

      case 'toggle-mode':
        settings.value.appMode = settings.value.appMode === 'edit' ? 'show' : 'edit';
        saveSettings();
        break;

      case 'play-next':
        // Implement play-next logic
        break;

      case 'stop-all':
        panicStop();
        break;

      case 'focus-playlist':
        // Focus playlist section
        break;

      case 'focus-cart':
        // Focus cart section
        break;

      default:
        // Handle cart slots
        if (action.startsWith('cart-slot-')) {
          const slotNum = parseInt(action.replace('cart-slot-', ''));
          // Trigger cart slot
        }
    }
  };

  return {
    bindings,
    loadBindings,
    saveBindings,
    setBinding,
    resetToDefaults,
    registerKeyListener,
    formatKeyCombo
  };
};
```

**2. components/KeyBindingsModal.vue** (NEW)

Modal for editing key bindings:
- Table with columns: Action | Current Key | Record New
- Key recorder button (press to record new key combo)
- Conflict detection and warnings
- Reset to defaults button

#### Files to Modify

**1. components/MainWorkspace.vue**

Replace hardcoded F1 handler (lines 257-265):
```typescript
const { registerKeyListener, loadBindings } = useKeyBindings();

onMounted(() => {
  if (import.meta.client) {
    loadBindings();
    const cleanup = registerKeyListener();
    onUnmounted(cleanup);
  }
});
```

**2. electron/main.js**

Add menu item: "Edit Key Bindings..." under Preferences

#### Testing
- Test all default key bindings work
- Test changing bindings persists
- Test conflict detection
- Verify no conflicts with browser/OS shortcuts
- Test in both Edit and Show modes

---

## Phase 5: Advanced Features (Week 10-12)

### Feature 8: Variable Number of Carts / Popout Cart Wall

**Priority**: LOW | **Effort**: 12-16 hours | **Risk**: MEDIUM

#### Objective
- Allow user to configure cart slot count (4-64 slots)
- Optional popout cart window

#### Files to Modify

**1. composables/useAppSettings.ts**

Add cart configuration:
```typescript
export interface AppSettings {
  // ... existing
  cartSlotCount: number; // 4-64, default 16
  cartGridColumns: number; // 2-6, default auto
}
```

**2. components/CartPlayer.vue**

Dynamic slot rendering:
```vue
<CartSlot
  v-for="slot in settings.cartSlotCount"
  :key="slot"
  :slot="slot - 1"
  :item="getCartItem(slot - 1)"
/>
```

Update responsive grid logic:
```typescript
const getGridColumns = computed(() => {
  if (settings.cartGridColumns !== 'auto') {
    return settings.cartGridColumns;
  }

  // Auto-calculate based on container width and slot count
  if (containerWidth.value < 500) return 2;
  if (containerWidth.value < 800) return Math.min(3, Math.ceil(Math.sqrt(settings.cartSlotCount)));
  if (containerWidth.value < 1100) return Math.min(4, Math.ceil(Math.sqrt(settings.cartSlotCount)));
  return Math.min(6, Math.ceil(Math.sqrt(settings.cartSlotCount)));
});
```

**3. types/project.ts**

Update CartItem to support larger slot numbers:
```typescript
export interface CartItem {
  slot: number; // 0-63 (was 0-15)
  itemUuid: string;
  index: number[]; // [-1, slot]
}
```

**4. electron/main.js**

Popout cart window:
```javascript
let cartWindow = null;

function createCartWindow() {
  cartWindow = new BrowserWindow({
    width: 800,
    height: 600,
    parent: mainWindow,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      nodeIntegration: false,
      contextIsolation: true
    }
  });

  cartWindow.loadURL(`http://localhost:3000/cart`);

  cartWindow.on('closed', () => {
    cartWindow = null;
  });
}

ipcMain.handle('popout-cart', () => {
  if (!cartWindow) {
    createCartWindow();
  } else {
    cartWindow.focus();
  }
});
```

#### Files to Create

**1. pages/cart.vue** (NEW)

Standalone cart page for popout window:
```vue
<template>
  <div class="cart-only-page">
    <CartPlayer />
  </div>
</template>

<script setup lang="ts">
// Minimal page - just cart player
</script>

<style scoped lang="scss">
.cart-only-page {
  width: 100%;
  height: 100vh;
  padding: var(--spacing-md);
  background-color: var(--color-background);
}
</style>
```

#### Settings UI

Add to SettingsModal:
- Cart slot count slider (4, 8, 16, 32, 64)
- Grid columns dropdown (Auto, 2, 3, 4, 6)
- "Popout Cart Wall" button

#### Data Synchronization

Cart window syncs with main via IPC:
- `cart-item-updated`
- `active-cues-changed`
- `project-changed`

#### Testing
- Test with different slot counts (4, 16, 32, 64)
- Verify grid layouts adjust properly
- Test popout window state sync
- Test closing/reopening popout
- Ensure audio plays from both windows

---

### Feature 9: Drag Playlist Items into Carts

**Priority**: LOW | **Effort**: 8-12 hours | **Risk**: LOW

#### Objective
Drag-and-drop from playlist to cart slots. Creates new cart-only instance with inherited settings.

#### Implementation

**1. components/PlaylistItem.vue**

Add drag support:
```typescript
const handleDragStart = (e: DragEvent) => {
  const { settings } = useAppSettings();
  if (settings.value.appMode !== 'edit') return; // Only in edit mode

  if (!e.dataTransfer) return;
  e.dataTransfer.effectAllowed = 'copy';
  e.dataTransfer.setData('application/json', JSON.stringify({
    type: 'playlist-item',
    uuid: props.item.uuid
  }));
};
```

**2. components/CartSlot.vue**

Add drop handlers:
```typescript
const isDragOver = ref(false);

const handleDragOver = (e: DragEvent) => {
  e.preventDefault();
  if (!e.dataTransfer) return;
  e.dataTransfer.dropEffect = 'copy';
  isDragOver.value = true;
};

const handleDragLeave = () => {
  isDragOver.value = false;
};

const handleDrop = async (e: DragEvent) => {
  e.preventDefault();
  isDragOver.value = false;

  if (!e.dataTransfer) return;
  const data = JSON.parse(e.dataTransfer.getData('application/json'));

  if (data.type === 'playlist-item') {
    // Check if slot occupied
    if (props.item) {
      // Show confirmation dialog
      const confirmed = await showConfirmDialog({
        title: 'Replace Cart Item',
        message: `Replace "${props.item.displayName}" with new item?`,
        confirmText: 'Replace',
        cancelText: 'Cancel'
      });

      if (!confirmed) return;
    }

    await createCartItemFromPlaylist(data.uuid, props.slot);
  }
};
```

**3. composables/useCartItems.ts**

Create cart item from playlist:
```typescript
const createCartItemFromPlaylist = async (sourceUuid: string, slot: number) => {
  const { findItemByUuid, currentProject, saveProject } = useProject();
  const sourceItem = findItemByUuid(sourceUuid);

  if (!sourceItem || sourceItem.type !== 'audio') return;

  const sourceAudio = sourceItem as AudioItem;
  const newUuid = uuidv4();

  // Create new cart-only item (deep copy)
  const cartItem: AudioItem = {
    ...JSON.parse(JSON.stringify(sourceAudio)), // Deep clone
    uuid: newUuid,
    index: [-1, slot],
    // Override with cart defaults
    duckingBehavior: DEFAULT_CART_AUDIO_ITEM.duckingBehavior,
    endBehavior: { action: 'nothing' },
    startBehavior: { action: 'nothing' }
  };

  // Add to cartOnlyItems
  if (!currentProject.value.cartOnlyItems) {
    currentProject.value.cartOnlyItems = [];
  }
  currentProject.value.cartOnlyItems.push(cartItem);

  // Create cart mapping
  const cartMapping: CartItem = {
    slot,
    itemUuid: newUuid,
    index: [-1, slot]
  };

  currentProject.value.cartItems[slot] = cartMapping;

  await saveProject();
};
```

#### Visual Feedback
- Drag preview: Semi-transparent item thumbnail
- Drop zone highlight: Green border on valid drop target
- Occupied slot: Show confirmation icon on hover
- Success: Brief flash animation on drop

#### Testing
- Test drag from playlist to various cart slots
- Verify new item is independent (different UUID)
- Test inherited settings work correctly
- Verify cart-specific defaults applied
- Test with occupied slots (confirmation dialog)
- Test drag disabled in Show mode

---

## Critical Files Summary

### Most Modified Files

1. **composables/useAudioEngine.ts**
   - Features: 3, 4, 5
   - Impact: Multi-output routing, EOD mode, editor preview
   - ~400-500 lines modified/added

2. **types/project.ts**
   - Features: 3, 4, 8
   - Impact: Field renames, new fields, cart structure
   - ~75-100 lines modified

3. **components/MainWorkspace.vue**
   - Features: 2, 6, 7
   - Impact: Vertical resize, mode handling, key bindings
   - ~150-200 lines added

4. **components/PropertiesPanel.vue**
   - Features: 2, 4, 5, 6
   - Impact: Resize, field renames, editor controls, mode restrictions
   - ~150-200 lines modified

5. **components/WaveformTrimmer.vue**
   - Features: 4, 5
   - Impact: EOD marker, editor controls
   - ~200-250 lines added

### New Files Created

1. `composables/useAudioBuses.ts` - Multi-output routing (Feature 3)
2. `composables/useAppSettings.ts` - Global settings (Features 4, 6, 7, 8)
3. `composables/useKeyBindings.ts` - Keyboard shortcuts (Feature 7)
4. `components/SettingsModal.vue` - Settings UI (Features 4, 6, 7, 8)
5. `components/ModeToggle.vue` - Mode switcher (Feature 6)
6. `components/KeyBindingsModal.vue` - Key binding editor (Feature 7)
7. `pages/cart.vue` - Popout cart window (Feature 8)

---

## Implementation Priorities

### Must Have (Phase 1-3)
1. Remove YouTube ✓
2. Vertical resize ✓
3. Multi-output routing ✓
4. EOD/Crossfade modes ✓
5. Audio editor ✓

### Should Have (Phase 4)
6. Show/Edit mode ✓
7. Key bindings ✓

### Nice to Have (Phase 5)
8. Variable carts ✓
9. Drag to cart ✓

---

## Migration & Backward Compatibility

### Project File Format Changes

**Version bump**: `1.0.0` → `1.1.0`

**Field renames**:
- `playFade` → `fadeIn`
- `stopFade` → `fadeOut`

**New fields**:
- `eodPoint?: number`
- `audioBusRouting?: AudioBusType` (optional, defaults based on index)

**Migration function**:
```typescript
const migrateProjectTo110 = (project: Project): Project => {
  // Migrate each audio item
  const migrateAudioItem = (item: AudioItem) => {
    // Rename fields
    if ('playFade' in item) {
      item.fadeIn = (item as any).playFade;
      delete (item as any).playFade;
    }
    if ('stopFade' in item) {
      item.fadeOut = (item as any).stopFade;
      delete (item as any).stopFade;
    }

    // Set default eodPoint
    if (item.eodPoint === undefined) {
      item.eodPoint = (item.outPoint || item.duration) - (item.fadeOut || 0);
    }

    return item;
  };

  // Recursively migrate items
  const migrateItems = (items: (AudioItem | GroupItem)[]): (AudioItem | GroupItem)[] => {
    return items.map(item => {
      if (item.type === 'audio') {
        return migrateAudioItem(item);
      } else if (item.type === 'group') {
        return {
          ...item,
          children: migrateItems(item.children)
        };
      }
      return item;
    });
  };

  return {
    ...project,
    version: '1.1.0',
    items: migrateItems(project.items),
    cartOnlyItems: project.cartOnlyItems?.map(migrateAudioItem) || []
  };
};
```

---

## Testing Strategy

### Unit Tests
- `useAudioBuses`: Bus creation, routing, volume control
- `useKeyBindings`: Key binding storage, conflict detection
- `useAppSettings`: Settings persistence

### Integration Tests
- Audio routing: Verify correct bus assignment
- EOD mode: Timing accuracy
- Editor: No interference with main playback

### Manual Testing Checklist
- [ ] All YouTube functionality removed
- [ ] Vertical panel resize works smoothly
- [ ] Audio routes to correct buses
- [ ] EOD mode triggers next track correctly
- [ ] Editor preview works independently
- [ ] Mode toggle changes behavior
- [ ] Key bindings respond correctly
- [ ] Variable cart count displays properly
- [ ] Drag to cart creates independent items
- [ ] Project migration successful

---

## Risk Mitigation

### High-Risk Areas

**1. Multi-output routing (Feature 3)**
- **Risk**: Howler.js internal API may change
- **Mitigation**: Abstract routing behind interface
- **Fallback**: Single output with software mixing

**2. EOD timing (Feature 4)**
- **Risk**: Timing accuracy issues
- **Mitigation**: Extensive testing, use high-precision timers
- **Fallback**: Keep crossfade as primary mode

### Performance Considerations

- **Many active cues**: Test with 10+ simultaneous cues
- **Large cart grids**: Test 64-slot grid responsiveness
- **Audio bus overhead**: Monitor latency with multiple gain nodes

---

## Documentation Updates

### Files to Update
- `API.md` - Document new bus endpoints
- `README.md` - Update features list
- `CHANGELOG.md` - Document all changes
- `guides/USER_GUIDE.md` - New feature documentation

### New Documentation
- `guides/AUDIO_ROUTING.md` - Multi-output bus system
- `guides/KEYBOARD_SHORTCUTS.md` - Default bindings
- `guides/EOD_MODE.md` - EOD vs crossfade explanation
