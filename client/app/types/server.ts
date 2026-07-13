// =====================================================================
// types/server.ts
// ---------------------------------------------------------------------
// DTOs mirroring the C++ server's JSON payloads. Kept in this file so the
// existing project model (types/project.ts) stays unchanged.
// =====================================================================

export type CueId          = string;
export type MixerChannelId = string;
export type DeviceId       = string;
export type MasterChannelIndex = number;

export type TransportState = 0 | 1 | 2 | 3 | 4;
// 0=Stopped 1=Playing 2=FadingIn 3=FadingOut 4=Paused
export const TransportLabel: Record<TransportState, string> = {
  0: 'Stopped',
  1: 'Playing',
  2: 'Fading In',
  3: 'Fading Out',
  4: 'Paused',
};

export interface ServerDeviceInfo {
  id: DeviceId;
  display_name: string;
  channel_count: number;
  sample_rate: number;
  is_default: boolean;
}

export interface ServerCueLTC {
  enabled: boolean;
  fps: number;
  offset_ns: number;
}

export interface ServerCue {
  id: CueId;
  display_name: string;
  file_path: string;
  artist: string;
  title: string;
  duration_sec: number;
  gain_db: number;
  fade_in_ms: number;
  fade_out_ms: number;
  ltc: ServerCueLTC;
  transport?: TransportState;
  playhead_seconds?: number;
  source_channels?: number;
  file_loaded?: boolean;
}

export interface ServerMixerChannel {
  id: MixerChannelId;
  display_name: string;
  gain_db: number;
  muted: boolean;
  soloed: boolean;
}

export interface ServerFsEntry {
  name: string;
  full_path: string;
  kind: 'dir' | 'file' | 'drive' | 'home';   // drive == top-level volume (Windows C:, D:, ...); home == $HOME shortcut
  size?: number;
}

export interface ServerFsListing {
  path: string;
  parent: string;
  is_root?: boolean;     // true when path === "" — entries are drives/volumes
  entries: ServerFsEntry[];
}

export interface MeterSnapshot {
  peak_db: number;
  rms_db: number;
  // Raw sample maximum since the previous meter frame (no ballistics).
  // Lossless — transients between frames are never missed. Drives
  // peak-hold and clip detection.
  peak_max_db: number;
}

export interface ItemMeterFrame {
  cue_id: CueId;
  transport: TransportState;
  playhead_seconds: number;
  sources: MeterSnapshot[];
}

export interface MixerMeterFrame {
  mixer_id: MixerChannelId;
  peak_db: number;
  rms_db: number;
  peak_max_db: number;
}

export interface MasterMeterFrame {
  index: MasterChannelIndex;
  peak_db: number;
  rms_db: number;
  peak_max_db: number;
  gain_reduction_db: number;
}

export interface MetersBroadcast {
  type: 'meters';
  items: ItemMeterFrame[];
  mixer_channels: MixerMeterFrame[];
  master_channels: MasterMeterFrame[];
}

export interface ServerWaveformChannel {
  peak: number[];
  rms: number[];
}

// ---------------------------------------------------------------------------
// Output Target — loudness platform standard levels reported by the server.
// Never hardcoded in the client; always read from settings.outputTargetLevels.
// ---------------------------------------------------------------------------
export type OutputTargetId = 'ebu-r128' | 'streaming' | 'radio' | 'netflix' | 'live';
export type MeterMode      = 'dBTP' | 'dBFS' | 'LUFS' | 'RMS';

export interface OutputTargetLevels {
  blueBelow:          number;  // meter reads blue below this value
  greenMin:           number;  // green zone start
  greenMax:           number;  // green zone end
  yellowMin:          number;  // yellow zone start
  yellowMax:          number;  // yellow zone end
  redAbove:           number;  // same as yellowMax / hard limit
  limiterCeilingDb:   number;  // brickwall ceiling for this platform
  autoVolumeTargetDb: number;  // target for the auto-volume feature
  meterUnit:          MeterMode;   // recommended meter display unit
  waveformColor:      string;  // CSS hex color for the properties-panel waveform
}

export interface ServerWaveform {
  cue_id?: CueId;
  bucket_count: number;
  duration_ms: number;
  sample_rate: number;
  source_channels: number;
  channels: ServerWaveformChannel[];
}
