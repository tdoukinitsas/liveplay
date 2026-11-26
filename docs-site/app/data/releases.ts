// What's New. Words live in the locale files under whatsNew.<key>. New features
// get a designed section; bug fixes are one short paragraph at the end.
import type { Tone } from './tour';

export interface ReleaseSection {
  id: string;
  /** Screenshot name. Not needed when `companion` is set. */
  shot?: string;
  /** Shows the Companion keypad and module links instead of a screenshot. */
  companion?: boolean;
  tone: Tone;
  /** panel: full-bleed colour, image below (the keypad sits beside the words).
   *  split: on black, beside the image. */
  layout: 'panel' | 'split';
}

export interface ReleaseMore {
  id: string;
  icon: string;
}

export interface Release {
  slug: string;
  key: string;
  version: string;
  tag: string;
  tone: Tone;
  shot: string;
  sections: ReleaseSection[];
  more: ReleaseMore[];
}

export const releases: Release[] = [
  {
    slug: '2-6',
    key: 'v26',
    version: '2.6',
    tag: 'v2.6.0',
    tone: 'room',
    shot: 'events',
    sections: [
      { id: 'events', shot: 'events', tone: 'room', layout: 'panel' },
      { id: 'presets', shot: 'event_presets', tone: 'room', layout: 'split' },
      { id: 'tabs', shot: 'tabs', tone: 'run', layout: 'panel' },
      { id: 'pages', shot: 'cart_pages', tone: 'go', layout: 'split' },
      { id: 'automation', shot: 'automation', tone: 'rely', layout: 'panel' },
      { id: 'relink', shot: 'missing_media', tone: 'rose', layout: 'split' },
      { id: 'companion', companion: true, tone: 'go', layout: 'panel' },
    ],
    more: [
      { id: 'wholePlaylist', icon: 'queue_music' },
      { id: 'duckTimes', icon: 'trending_down' },
      { id: 'stopFades', icon: 'motion_photos_paused' },
      { id: 'stopKey', icon: 'keyboard' },
      { id: 'openFile', icon: 'folder_open' },
      { id: 'waveformPan', icon: 'pan_tool' },
    ],
  },
  {
    slug: '2-5',
    key: 'v25',
    version: '2.5',
    tag: 'v2.5.0',
    tone: 'mix',
    shot: 'mixer',
    sections: [
      { id: 'mixer', shot: 'mixer', tone: 'mix', layout: 'panel' },
      { id: 'eq', shot: 'mixer_channel', tone: 'mix', layout: 'split' },
      { id: 'outputs', shot: 'hardware_outputs', tone: 'run', layout: 'panel' },
      { id: 'workspace', shot: 'workspace', tone: 'go', layout: 'split' },
      { id: 'accounts', shot: 'users', tone: 'rely', layout: 'panel' },
      { id: 'fades', shot: 'fades', tone: 'rose', layout: 'split' },
    ],
    more: [
      { id: 'settings', icon: 'settings' },
      { id: 'multiEdit', icon: 'edit_note' },
      { id: 'groupTime', icon: 'timer' },
      { id: 'sends', icon: 'call_split' },
      { id: 'tray', icon: 'dock_to_bottom' },
      { id: 'youtubeFormat', icon: 'audio_file' },
    ],
  },
];

// Earlier releases, as a compact timeline. `tag` is the first release of the line.
export const timeline = [
  { key: 'v24', version: '2.4', tag: 'v2.4.1' },
  { key: 'v23', version: '2.3', tag: 'v2.3.0' },
  { key: 'v22', version: '2.2', tag: 'v2.2.0' },
  { key: 'v21', version: '2.1', tag: 'v2.1.0' },
  { key: 'v20', version: '2.0', tag: 'v2.0.5' },
];
