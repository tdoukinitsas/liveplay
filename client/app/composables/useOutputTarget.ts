// =====================================================================
// useOutputTarget.ts
// ---------------------------------------------------------------------
// Exposes the output-target loudness levels and meter mode that the
// server computes and embeds in settings.outputTargetLevels. The numbers
// are platform-specific (EBU R128, Streaming, Radio, Netflix, Live) and
// are never hardcoded in the UI — always read from what the server reports.
// =====================================================================
import type { OutputTargetLevels, MeterMode } from '~/types/server';

// Fallback levels used before a project is loaded (EBU R128 defaults).
export const DEFAULT_OUTPUT_TARGET_LEVELS: OutputTargetLevels = {
  blueBelow:          -28,
  greenMin:           -28,
  greenMax:           -20,
  yellowMin:          -20,
  yellowMax:          -1,
  redAbove:           -1,
  limiterCeilingDb:   -1,
  autoVolumeTargetDb: -23,
  meterUnit:          'LUFS',
  waveformColor:      '#00e676',
};

// The 4 meter zone colours (applied consistently across meter, waveform, etc.)
export const METER_COLORS = {
  blue:   '#00b8d4',
  green:  '#00e676',
  yellow: '#ffc400',
  red:    '#ff1744',
} as const;

export function useOutputTarget() {
  const { currentProject } = useProject();

  const levels = computed<OutputTargetLevels>(() => {
    const s = (currentProject.value as any)?.settings;
    if (s?.outputTargetLevels && typeof s.outputTargetLevels === 'object') {
      return s.outputTargetLevels as OutputTargetLevels;
    }
    return DEFAULT_OUTPUT_TARGET_LEVELS;
  });

  // Active meter display mode. This operator's own choice wins; with none, the
  // unit the show's output target recommends. It moved out of project settings
  // in U4 — two people watching the same meters can want different units, and
  // one of them changing it should not have dirtied the show file.
  //
  // The server reads this too, but only to gate the true-peak / loudness DSP,
  // and it takes the union across everyone connected: the meter frame is
  // computed once and broadcast, so there is no per-connection DSP to gate.
  const { meterMode: userMeterMode } = usePreferences();
  const meterMode = computed<MeterMode>(() => {
    if (userMeterMode.value) return userMeterMode.value as MeterMode;
    return levels.value.meterUnit;
  });

  // Return the meter zone colour for a given dB reading.
  function colorForLevel(db: number): string {
    const lv = levels.value;
    if (db >= lv.redAbove)    return METER_COLORS.red;
    if (db >= lv.yellowMin)   return METER_COLORS.yellow;
    if (db >= lv.greenMin)    return METER_COLORS.green;
    return METER_COLORS.blue;
  }

  return { levels, meterMode, colorForLevel };
}
