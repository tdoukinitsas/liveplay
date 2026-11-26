// The home page's feature tour, for someone who has never used LivePlay. Words
// live in the locale files under features.<key>; this is only the running order.
export type Tone = 'run' | 'mix' | 'room' | 'rely' | 'go' | 'rose';

export interface TourFeature {
  key: string;
  /** Screenshot name. Not needed when `companion` is set. */
  shot?: string;
  /** Shows the Companion keypad and module links instead of a screenshot. */
  companion?: boolean;
  /** Added in the current release: shows a badge linking to What's New. */
  isNew?: boolean;
  /** Has a features.<key>.points list. */
  points?: boolean;
}

export interface Chapter {
  id: string;
  group: string;
  /** Gives each chapter header its own waveform shape. */
  seed: number;
  tone: Tone;
  lead: TourFeature;
  rows: TourFeature[];
  tiles: TourFeature[];
}

export const chapters: Chapter[] = [
  {
    id: 'run',
    group: 'groupShow',
    seed: 5,
    tone: 'run',
    lead: { key: 'playlist', shot: 'playlist', points: true },
    rows: [
      { key: 'tabs', shot: 'tabs', isNew: true, points: true },
      { key: 'cart', shot: 'cart', points: true },
      { key: 'cartPages', shot: 'cart_pages', isNew: true, points: true },
      { key: 'automation', shot: 'automation', isNew: true, points: true },
      { key: 'showMode', shot: 'showmode', points: true },
      { key: 'missingMedia', shot: 'missing_media', isNew: true, points: true },
    ],
    tiles: [
      { key: 'waveform', shot: 'waveformtrimmer' },
      { key: 'ducking', shot: 'ducking' },
      { key: 'interface', shot: 'workspace' },
      { key: 'properties', shot: 'properties' },
      { key: 'youtube', shot: 'youtube' },
      { key: 'firstLaunch', shot: 'nameproject' },
    ],
  },
  {
    id: 'mix',
    group: 'groupMixer',
    seed: 10,
    tone: 'mix',
    lead: { key: 'mixer', shot: 'mixer', points: true },
    rows: [
      { key: 'eqDynamics', shot: 'mixer_channel', points: true },
      { key: 'limiter', shot: 'limiter', points: true },
    ],
    tiles: [
      { key: 'meters', shot: 'meters' },
      { key: 'preview', shot: 'preview' },
      { key: 'routing', shot: 'hardware_outputs' },
      { key: 'fades', shot: 'fades' },
    ],
  },
  {
    id: 'room',
    group: 'groupEvents',
    seed: 15,
    tone: 'room',
    lead: { key: 'events', shot: 'events', isNew: true, points: true },
    rows: [
      { key: 'presets', shot: 'event_presets', isNew: true, points: true },
      { key: 'companion', companion: true, isNew: true, points: true },
    ],
    tiles: [
      { key: 'connections', shot: 'connections', isNew: true },
      { key: 'ltc', shot: 'ltc' },
    ],
  },
  {
    id: 'rely',
    group: 'groupSystem',
    seed: 20,
    tone: 'rely',
    lead: { key: 'remoteOperation', shot: 'decoupled', points: true },
    rows: [],
    tiles: [
      { key: 'discovery', shot: 'server_settings' },
      { key: 'accounts', shot: 'users' },
      { key: 'audioEngine', shot: 'audio_engine' },
      { key: 'settingsPage', shot: 'settings' },
    ],
  },
];
