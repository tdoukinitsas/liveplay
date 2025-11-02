// Base item interface that all items extend from
export interface BaseItem {
  uuid: string;
  index: number[];
  displayName: string;
  color: string;
  type: 'audio' | 'group' | 'action'; // Extensible for future item types
}

// Audio-specific properties
export interface AudioItem extends BaseItem {
  type: 'audio';
  mediaFileName: string;
  mediaPath: string; // Relative path from project folder (e.g., "media/audio.mp3")
  waveformPath: string;
  waveform?: WaveformData; // Optional: waveform data for visualization
  inPoint: number; // in seconds
  outPoint: number; // in seconds
  volume: number; // 0-2 (1 is normal, >1 is louder, <1 is quieter)
  endBehavior: EndBehavior;
  startBehavior: StartBehavior;
  customActions: CustomAction[];
  duckingBehavior: DuckingBehavior;
  duration: number; // total duration in seconds
  fadeOutDuration: number; // fade out duration in seconds when stopping (default: 1)
}

// Waveform data format (from ffmpeg/audiowaveform)
export interface WaveformData {
  length: number;
  duration: number;
  peaks: number[]; // Normalized values between 0 and 1
}

// Group item properties
export interface GroupItem extends BaseItem {
  type: 'group';
  children: (AudioItem | GroupItem)[];
  startBehavior: GroupStartBehavior;
  endBehavior: EndBehavior;
  isExpanded: boolean; // UI state
}

// End behavior options
export interface EndBehavior {
  action: 'nothing' | 'next' | 'goto-item' | 'goto-index' | 'loop';
  targetUuid?: string; // for goto-item
  targetIndex?: number[]; // for goto-index
}

// Start behavior options
export interface StartBehavior {
  action: 'nothing' | 'play-next' | 'play-item' | 'play-index';
  targetUuid?: string;
  targetIndex?: number[];
}

// Group start behavior
export interface GroupStartBehavior {
  action: 'play-first' | 'play-all';
}

// Custom action at specific time
export interface CustomAction {
  timePoint: number; // in seconds
  action: CustomActionType;
}

export type CustomActionType = 
  | { type: 'play-item'; uuid: string }
  | { type: 'play-index'; index: number[] }
  | { type: 'stop-all' }
  | { type: 'http-request'; request: HttpRequest };

export interface HttpRequest {
  method: 'GET' | 'POST' | 'PUT' | 'DELETE';
  url: string;
  contentType: 'form' | 'json';
  body?: Record<string, any>;
}

// Ducking behavior
export interface DuckingBehavior {
  mode: 'stop-all' | 'no-ducking' | 'duck-others';
  duckLevel?: number; // 0-1, volume multiplier for other cues
  duckFadeIn?: number; // fade in duration in seconds when ducking (default: 0.25)
  duckFadeOut?: number; // fade out duration in seconds when restoring (default: 1)
}

// Cart player item
export interface CartItem {
  slot: number; // 0-15
  itemUuid: string;
}

// Project structure
export interface Project {
  name: string;
  version: string;
  folderPath: string;
  items: (AudioItem | GroupItem)[];
  cartItems: CartItem[];
  cartOnlyItems: AudioItem[]; // Items that exist only in cart (not in playlist)
  theme: Theme;
  createdAt: string;
  lastModified: string;
}

// Theme configuration
export interface Theme {
  mode: 'light' | 'dark';
  accentColor: string;
}

// Active playback state
export interface ActiveCue {
  uuid: string;
  displayName: string;
  startTime: number;
  currentTime: number;
  duration: number;
  volume: number;
  isDucked: boolean;
  originalVolume: number;
  audioContext?: AudioContext;
  audioSource?: AudioBufferSourceNode;
  gainNode?: GainNode;
}

// Predefined colors for items
export const PRESET_COLORS = [
  '#FF0000', // Red
  '#FF6600', // Orange
  '#FFCC00', // Yellow
  '#99CC00', // Lime
  '#00CC00', // Green
  '#00CC99', // Teal
  '#00CCFF', // Cyan
  '#0066FF', // Blue
  '#3300FF', // Indigo
  '#9900FF', // Purple
  '#FF00CC', // Magenta
  '#FF0066', // Pink
  '#CC0000', // Dark Red
  '#996600', // Brown
  '#666666', // Gray
  '#333333'  // Dark Gray
];

// Default values
export const DEFAULT_THEME: Theme = {
  mode: 'dark',
  accentColor: '#0066FF'
};

export const DEFAULT_AUDIO_ITEM: Partial<AudioItem> = {
  color: PRESET_COLORS[0],
  inPoint: 0,
  volume: 1.0,
  endBehavior: { action: 'next' }, // Default: play next item
  startBehavior: { action: 'nothing' },
  customActions: [],
  duckingBehavior: { 
    mode: 'stop-all', // Default for playlist items: stop all other cues
    duckFadeIn: 0.25,
    duckFadeOut: 1.0
  },
  fadeOutDuration: 1.0
};

// Default for cart items (different from playlist)
export const DEFAULT_CART_AUDIO_ITEM: Partial<AudioItem> = {
  color: PRESET_COLORS[0],
  inPoint: 0,
  volume: 1.0,
  endBehavior: { action: 'nothing' },
  startBehavior: { action: 'nothing' },
  customActions: [],
  duckingBehavior: { 
    mode: 'duck-others', // Default for cart items: duck to 0.2
    duckLevel: 0.2,
    duckFadeIn: 0.25,
    duckFadeOut: 1.0
  },
  fadeOutDuration: 1.0
};

export const DEFAULT_GROUP_ITEM: Partial<GroupItem> = {
  color: PRESET_COLORS[8],
  startBehavior: { action: 'play-first' },
  endBehavior: { action: 'nothing' },
  isExpanded: true,
  children: []
};
