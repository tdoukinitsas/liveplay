// Base item interface that all items extend from
export interface BaseItem {
  uuid: string;
  index: number[];
  displayName: string;
  color: string;
  type: 'audio' | 'group' | 'action'; // Extensible for future item types
  /**
   * Which bus this item plays through. Absent means *inherit* — from the
   * nearest ancestor group, or the master-role bus if nothing along the
   * chain assigns one. It is the whole of an item's routing.
   *
   * Declared on BaseItem because a group carries it too and passes it down.
   * It was missing from these types entirely, so every site that touched it
   * cast to `any` and nothing type-checked that it was being carried through
   * the save and sync paths.
   */
  busId?: string;
}

// Audio-specific properties
export interface AudioItem extends BaseItem {
  type: 'audio';
  mediaFileName: string;
  mediaPath: string; // Relative path from project folder (e.g., "media/audio.mp3")
  // Optional absolute path on the server's filesystem. When set, the audio
  // engine uses this directly and ignores mediaPath/folder construction.
  // Populated when a cue is imported via the server file browser or by
  // uploading a file to the server's media_root (rather than the old local
  // copy-into-project-folder path).
  mediaServerPath?: string;
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
  fadeOutDuration: number; // legacy stop fade, seconds (default: 1); see manualStopFade
  playFade: number; // fade in duration when playing (default: 0)
  stopFade: number; // fade out duration before end (default: 0)
  crossFade: number; // cross-fade duration to next track (default: 0)
  // Seconds the Stop button fades this cue over, 0 = cut (#56). Absent on
  // items saved before 2.5.0, which keep the old rule: the larger of stopFade
  // and fadeOutDuration. manualStopFadeOf() reads either.
  manualStopFade?: number;
  // Seconds a looping cue's end blends back into its start (#56); 0 or absent
  // = a plain loop. Only meaningful with endBehavior.action 'loop'. The
  // server caps it at half the trimmed length.
  loopCrossfade?: number;
  // "Wait before next" (#8): seconds between this cue ending and its end
  // behaviour starting the next / goto target. 0 or absent = straight away.
  // Ignored while a crossfade or Start Next is set (they overlap on purpose).
  advanceDelay?: number;
  // "Start Next" segue marker (radio-style transition): when the playhead
  // crosses startNextTime, the next item starts at its own volume/fades
  // while this one keeps playing. Independent of the fade-out markers.
  startNextEnabled?: boolean;
  startNextTime?: number;     // absolute seconds within the file
  startNextFadeOut?: boolean; // also begin this item's fade-out at the marker
  // LTC (SMPTE Linear Timecode) output for this cue.
  ltcEnabled?: boolean;         // output LTC on the project's ltcOutput when playing
  ltcStartTimecode?: string;    // starting timecode "HH:MM:SS:FF" (default "00:00:00:00")
  ltcFrameRate?: number;        // 0=24, 1=25, 2=29.97NDF, 3=29.97DF, 4=30 (default 4)
}

// Waveform data format (from the server's decoder, or legacy ffmpeg files)
export interface WaveformData {
  length: number;
  duration: number;
  // Combined trace: the per-bucket maximum across every source channel.
  // Normalized 0..1. This is what analysis (auto-trim, perceived loudness) and
  // the compact row/cart renderers use — taking channel 0 instead meant a
  // stereo file was measured and drawn from its LEFT channel only (#47).
  peaks: number[];
  // Per-channel traces, in source channel order (stereo = [L, R]). Present for
  // waveforms produced by the server; absent for legacy single-array data.
  // Renderers with room for it (the trimmer) draw one lane per channel.
  channelPeaks?: number[][];
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

// Cart slot key binding
export interface CartSlotKeyBinding {
  key: string;
  ctrlKey: boolean;
  shiftKey: boolean;
  altKey: boolean;
}

// Configurable playback keyboard actions
export type PlaybackKeyAction =
  | 'pause-resume'
  | 'toggle-loop'
  | 'cue-to-continue'
  | 'jump-cue'
  | 'stop-all'
  | 'select-up'
  | 'select-down'
  | 'play-selected'
  | 'play-next';

// Cart player item
export interface CartItem {
  slot: number; // 0-15
  itemUuid: string;
  index: number[]; // [-1, slot] for API triggering
}

/**
 * Where a bus sends its audio.
 *
 * `target` is a bus id for `bus`, or an output name for `output`: a *logical*
 * name ("FOH", "Comms", the built-in "Main Out" / "Preview Out") that the
 * server's outputs.json binds to hardware, or — since D29 — a device name
 * picked straight from the strip, which the server treats as bound while a
 * device of that name is present. There is no `master` kind any more (D25):
 * the master-role bus is an ordinary Output-kind bus on "Main Out", and a
 * sub-mix reaches the house by targeting it as a `bus`.
 */
export interface BusOutput {
  type: 'bus' | 'output';
  target: string;
}

/**
 * One filter block on a strip. There is no separate in/out switch: a high-pass
 * parked at the bottom of its range and a low-pass parked at the top are out
 * of circuit, which is what the knob's origin already means on the surface.
 */
export interface BusFilter {
  freq: number;
  q: number;
}

/**
 * One EQ band. All four are bells; a band at 0 dB gain is out of circuit,
 * because a peaking section at unity is an identity whatever its Q.
 */
export interface BusEqBand {
  freq: number;
  gain: number;
  q: number;
  /**
   * Switch this band from a bell to a shelf. Honoured only on the outer bands
   * — LF shelves the bottom, HF the top; which end follows from the band's
   * position and is not stored.
   */
  shelf: boolean;
  /**
   * A shelf's steepness, 0.1..2. Separate from `q` rather than sharing it,
   * because they are different quantities with barely overlapping ranges, and
   * one field would mean switching to shelf and back silently changed the
   * bell's width.
   */
  slope: number;
}

/** Which bands may be shelved. The middle two are always bells. */
export const EQ_SHELVABLE_BANDS = [0, 3] as const;

/**
 * The expander / gate.
 *
 * `attack` opens the gate and `release` closes it — the opposite of what those
 * words mean on a compressor. `range` is the deepest attenuation, so the gate
 * ducks rather than slamming to silence.
 */
export interface BusGate {
  on: boolean;
  threshold: number;
  ratio: number;
  range: number;
  attack: number;
  hold: number;
  release: number;
}

/**
 * The compressor / limiter.
 *
 * `attack` clamps down and `release` recovers — the opposite sense to BusGate's
 * pair of the same name. `knee` is the total width of the soft knee, centred on
 * the threshold; at 0 it is a hard knee.
 */
export interface BusComp {
  on: boolean;
  threshold: number;
  ratio: number;
  makeup: number;
  attack: number;
  knee: number;
  release: number;
}

/**
 * Stereo image width, on a stereo bus only.
 *
 * `width` is 0 mono, 1 untouched, 2 double. `bassMonoHz` high-passes the SIDE
 * signal alone, so the low end collapses to the centre and stays mono-safe
 * however hard the rest is widened; parked at 20 Hz it is out of circuit, the
 * same convention the strip's HPF and LPF use.
 *
 * There is no level compensation, deliberately: widening only raises material
 * that has side content, so any static correction would attenuate mono-ish
 * material it never touched.
 */
export interface BusWidth {
  width: number;
  bassMonoHz: number;
  bassMonoQ: number;
}

/** The strip's tone controls. */
export interface BusDsp {
  /**
   * Section bypass. Distinct from every band being flat: it takes the whole
   * section out in one press and puts it back exactly as it was, which is why
   * a desk has an in/out button per section rather than expecting you to
   * zero the controls and remember what they were.
   */
  eqEnabled: boolean;
  dynEnabled: boolean;
  hpf: BusFilter;
  lpf: BusFilter;
  eq: BusEqBand[];
  gate: BusGate;
  comp: BusComp;
  width: BusWidth;
}

/** Band names, in order. Fixed layout, so the surface can label the columns. */
export const EQ_BAND_NAMES = ['LF', 'LMF', 'HMF', 'HF'] as const;

/** Where the filters sit when out of circuit. Must match the server's. */
export const HPF_PARKED_HZ = 20;
export const LPF_PARKED_HZ = 20000;
/** Bass-mono parks at the bottom of its travel, the same way the HPF does. */
export const BASS_MONO_PARKED_HZ = 20;

/**
 * A bus is the user-facing mixer strip. Items and groups are assigned to one
 * via `busId` and carry no other routing; the bus alone decides where the
 * audio goes, and it is edited from the mixer rather than per item.
 */
export interface Bus {
  id: string;
  name: string;
  color: string;
  order: number;
  /** 1 = mono, 2 = stereo. Wider buses are a separate design conversation. */
  width: number;
  gainDb: number;
  mute: boolean;
  /**
   * The strip's position control, -1 hard left .. 0 centre .. +1 hard right.
   * One field, two meanings, decided by `width`:
   *
   * - mono bus: PAN, the position of its single lane between the destination's
   *   two. Constant-power, so centre sits at -3 dB.
   * - stereo bus: BALANCE, a trim between its own two lanes. Only ever
   *   attenuates, so centre is unity and correcting a lopsided mix cannot push
   *   the loud side up.
   */
  pan: number;
  /**
   * Pre-fade listen: this bus is being tapped into the preview-role bus,
   * pre-fader and pre-mute. Live state rather than part of the show — it is read from the
   * engine, never saved, and a reload clears it.
   */
  pfl: boolean;
  /**
   * Preview-role bus only: the mono-sum audition is folding the phones to
   * mono. Live state like `pfl` — read from the engine, never saved. Always
   * false on every other bus.
   */
  monoCheck: boolean;
  /** The strip's tone controls: filters, EQ and dynamics. */
  dsp: BusDsp;
  /**
   * Whether this bus actually reaches hardware. Server-computed (D26): true
   * for a mapped logical output, for "Main Out" even unmapped (it falls back
   * to the default device), and for a target naming a device that is present.
   * Not derivable from the output name list, so the strip renders it rather
   * than inferring it.
   */
  bound: boolean;
  // Output-kind buses: the real device names this machine resolves the target
  // to ("" = the default device), from the server — which matches a
  // renumbered Windows device ("(2- …)") to the name the show saved.
  outputDevices?: string[];
  /**
   * Roles (D24). Exactly one bus in a project holds each; never both on one
   * bus. The master-role bus is the inheritance fallback and the house
   * (masters 0/1); the preview-role bus is where PFL and cue pre-listen land.
   * Moved with PATCH {master:true} / {preview:true}; the holder cannot be
   * deleted until the role has moved.
   */
  master: boolean;
  preview: boolean;
  /**
   * The engine master pair this bus's hardware output occupies — [0,1] for
   * the master role, the reserved pair for preview, a pool pair for any other
   * hardware-bound bus — or null when it is not bound to hardware (D32).
   */
  masters: [number, number] | null;
  output: BusOutput;
  /** Engine strip backing this bus; empty only while the engine is rebuilding. */
  mixerId: string;
  /** Items resolving to this bus, including ones inheriting it from a group. */
  itemUuids: string[];
}

export interface ProjectSettings {
  defaultOutputDevice?: string | null;
  previewDevice?: string | null;
  // Where this show's timecode goes, as a LOGICAL output name — the same
  // vocabulary a bus target uses, bound to hardware by the machine's output
  // map (D38). `ltcDevice` was its predecessor and named a sound card; the
  // server migrates it on load and erases it, so it never appears here.
  ltcOutput?: string | null;
  outputTarget?: string;
  outputTargetLevels?: Record<string, unknown>;
  /** LEGACY as of U4 — see usePreferences().meterMode. Read as a seed only. */
  meterMode?: string;
  /**
   * Meter ballistics preset id, or "custom" to use `meterBallisticsCustom`.
   * The server's settings registry is the authority on the accepted ids:
   * digital-ppm | ppm-i | ppm-ii | vu | instant | custom.
   */
  meterBallistics?: string;
  meterBallisticsCustom?: {
    attackMs?: number;
    releaseMs?: number;
    rmsWindowMs?: number;
  };
  defaultTransitionMode?: TransitionMode;
  autoCueNextWithoutEndBehavior?: boolean;
  stopAllFadeMs?: number;
  // Absent = true: Stop All silences the preview audition too (#60).
  stopAllStopsPreview?: boolean;
  /** LEGACY as of U4 — see usePreferences().scrollToPlaying. Seed only. */
  uiScrollToPlaying?: boolean;
  disableAutoVolumeAndTrim?: boolean;
  disableLimiter?: boolean;
  disableSilenceWarning?: boolean;
  autoSave?: boolean;

  /**
   * Number shown for the first playlist item.
   *
   * This affects UI display and user-entered index paths only.
   * Internal item.index values, stored targetIndex values, and REST by-index
   * paths remain zero-based for backwards compatibility.
   */
  indexDisplayStart?: number;
}


// The bus-schema version this build speaks, matching the server's
// kBusSchemaVersion. A whole-document save declares it so the server can tell a
// round trip of the loaded project (buses omitted because they are edited
// through their own endpoints) from a pre-bus project being pushed over the top
// of one (buses omitted because it has never heard of them). See D11.
export const BUS_SCHEMA_VERSION = 2;

// Project structure
export interface Project {
  name: string;
  version: string;
  // Where the project folder is ON THIS MACHINE. Derived by the server from the
  // location of the .liveplay file, not stored in it — a saved project names no
  // directory, so one that has been moved, mailed or unzipped elsewhere still
  // finds its media. The client sets this exactly once, for a new project that
  // has a chosen folder but no file yet; after that it is read-only, and every
  // load and save-as replaces it.
  folderPath: string;
  // Mirrored from the server's header and handed straight back on save, so the
  // client never claims a version it wasn't given.
  busSchema?: number;
  items: (AudioItem | GroupItem)[];
  cartItems: CartItem[];
  cartSlotKeys?: Record<number, CartSlotKeyBinding>;
  /**
   * LEGACY as of U4 — the transport keymap belongs to the person, not the
   * show, and lives in their profile (usePreferences). Still typed because a
   * 2.4 document carries it and it is read once as a seed for an operator who
   * has never set one; the server drops it on the next save.
   */
  playbackKeys?: Record<string, CartSlotKeyBinding | null>;
  cartOnlyItems: AudioItem[]; // Items that exist only in cart (not in playlist)
  /**
   * LEGACY as of U4, same as playbackKeys. Read usePreferences().theme for the
   * colour scheme actually in force — a document's copy is a seed, and mailing
   * a project no longer repaints the recipient's desk.
   */
  theme?: Theme;
  settings?: ProjectSettings;
  createdAt: string;
  lastModified: string;
}

// Theme configuration
export interface Theme {
  mode: 'light' | 'dark';
  accentColor: string;
}

// Which track-to-track transition newly imported tracks default to. Stored in
// project settings as `defaultTransitionMode`; every track can still be
// switched individually in its properties.
export type TransitionMode = 'crossfade' | 'start-next';

// Transition defaults for a freshly imported audio item, derived from the
// project's `settings.defaultTransitionMode`. Spread these over
// DEFAULT_AUDIO_ITEM when creating the item.
export function transitionDefaultsForImport(
  mode: TransitionMode | string | undefined,
  duration: number
): Partial<AudioItem> {
  if (mode === 'start-next') {
    return {
      startNextEnabled: true,
      // Same default as the per-track toggle: 5s before the end. The engine
      // ignores the marker while it is <= 0 (e.g. duration still unknown).
      startNextTime: Math.max(0, duration - 5),
      startNextFadeOut: false,
    };
  }
  return {};
}

// Re-anchor a freshly imported item's default start-next marker once its real
// trim window is known — waveform arrival and auto-trim can both move the out
// point after import. Only call this for items imported this session, before
// the user has had a chance to edit the marker.
export function anchorStartNextMarker(item: AudioItem): void {
  if (!item.startNextEnabled) return;
  item.startNextTime = Math.max(item.inPoint, item.outPoint - 5);
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
  accentColor: '#DA1E28'
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
  fadeOutDuration: 1.0,
  manualStopFade: 1.0,
  playFade: 0,
  stopFade: 0,
  crossFade: 0,
  startNextEnabled: false,
  startNextTime: 0,
  startNextFadeOut: false,
  ltcEnabled: false,
  ltcStartTimecode: '00:00:00:00',
  ltcFrameRate: 4,
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
    mode: 'duck-others', // Default for cart items: duck to -20dB
    duckLevel: 0.1,
    duckFadeIn: 0.25,
    duckFadeOut: 1.0
  },
  fadeOutDuration: 1.0,
  manualStopFade: 1.0,
  playFade: 0,
  stopFade: 0,
  crossFade: 0,
  startNextEnabled: false,
  startNextTime: 0,
  startNextFadeOut: false
};

// The fade the Stop button uses for this item: its manualStopFade, or for an
// item saved before 2.5.0 the rule the server still applies to it.
export const manualStopFadeOf = (item: Partial<AudioItem>): number =>
  typeof item.manualStopFade === 'number'
    ? item.manualStopFade
    : Math.max(item.stopFade || 0, item.fadeOutDuration || 0);

export const DEFAULT_GROUP_ITEM: Partial<GroupItem> = {
  color: PRESET_COLORS[8],
  startBehavior: { action: 'play-first' },
  endBehavior: { action: 'nothing' },
  isExpanded: true,
  children: []
};

// Cart slots have no default key bindings — user assigns them manually
export const DEFAULT_CART_SLOT_KEYS: Record<number, CartSlotKeyBinding> = {};

// Default keyboard bindings for playback actions
export const DEFAULT_PLAYBACK_KEYS: Partial<Record<PlaybackKeyAction, CartSlotKeyBinding>> = {
  'play-next':     { key: ' ',         ctrlKey: false, shiftKey: false, altKey: false },
  'pause-resume':  { key: 'p',         ctrlKey: false, shiftKey: false, altKey: false },
  'stop-all':      { key: 'Escape',    ctrlKey: false, shiftKey: false, altKey: false },
  'select-up':     { key: 'ArrowUp',   ctrlKey: false, shiftKey: false, altKey: false },
  'select-down':   { key: 'ArrowDown', ctrlKey: false, shiftKey: false, altKey: false },
  'play-selected': { key: 'Enter',     ctrlKey: false, shiftKey: false, altKey: false },
};
