<template>
  <!--
    Stereo meter with EBU R128-inspired colour scheme and dB scale.
    Used for both per-cue source metering (pass cueId) and per-output
    master-bus metering (pass leftIndex + rightIndex).

    Bug fix: composables are always called unconditionally so that a cueId
    which resolves after mount (server decorates items asynchronously) still
    activates the subscription on the next meter frame.
  -->
  <div class="stereo-meter" :class="{ 'stereo-meter--bare': bare }">
    <div v-if="label" class="stereo-meter__label">{{ label }}</div>

    <div class="stereo-meter__body">
      <!-- dB scale -->
      <div v-if="showScale" class="stereo-meter__scale">
        <div
          v-for="m in scaleMarks"
          :key="m.db"
          class="stereo-meter__mark"
          :style="{ bottom: m.pct + '%' }"
        >
          <span class="stereo-meter__mark-text" :style="{ color: m.color }">{{ m.label }}</span>
          <span class="stereo-meter__mark-tick" :style="{ background: m.color }" />
        </div>
      </div>

      <!-- L + R bars -->
      <div class="stereo-meter__bars">
        <div class="stereo-meter__chan">
          <!-- Clip latch — lights on any raw sample ≥ clip threshold since
               the last click (click either indicator to reset both). -->
          <div
            class="stereo-meter__clip"
            :class="{ 'is-clipped': holdL.clipped.value || holdR.clipped.value }"
            :title="'Clip (click to reset)'"
            @click="resetClips"
          />
          <div class="stereo-meter__bar-group">
            <div class="stereo-meter__track">
              <div class="stereo-meter__fill" :style="rmsStyleL" />
              <div class="stereo-meter__fill" :style="peakStyleL" />
              <div v-if="holdVisibleL" class="stereo-meter__hold" :style="holdStyleL" />
            </div>
            <!-- GR track: same rounded-rect shape, accent fill from top -->
            <div v-if="grVisible" class="stereo-meter__gr-track">
              <div class="stereo-meter__gr-fill" :style="grStyleL" />
            </div>
          </div>
          <div class="stereo-meter__chan-label">{{ mono ? 'M' : 'L' }}</div>
        </div>
        <div v-if="!mono" class="stereo-meter__chan">
          <div
            class="stereo-meter__clip"
            :class="{ 'is-clipped': holdL.clipped.value || holdR.clipped.value }"
            :title="'Clip (click to reset)'"
            @click="resetClips"
          />
          <div class="stereo-meter__bar-group">
            <div class="stereo-meter__track">
              <div class="stereo-meter__fill" :style="rmsStyleR" />
              <div class="stereo-meter__fill" :style="peakStyleR" />
              <div v-if="holdVisibleR" class="stereo-meter__hold" :style="holdStyleR" />
            </div>
            <div v-if="grVisible" class="stereo-meter__gr-track">
              <div class="stereo-meter__gr-fill" :style="grStyleR" />
            </div>
          </div>
          <div class="stereo-meter__chan-label">R</div>
        </div>
      </div>
    </div>

    <div v-if="showPeakValue" class="stereo-meter__peak-text">
      {{ peakLabel }}
    </div>
    <!-- Short-term loudness readout (LUFS mode only): momentary drives the
         bars + main label; this second line shows the 3 s EBU "S" value. -->
    <div v-if="showPeakValue && meterMode === 'LUFS'" class="stereo-meter__peak-text">
      {{ shortTermLabel }}
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useMasterMeter, useCueMeters, useMixerMeter, usePeakHold, lufsFromKwMs } from '~/composables/useLiveMeters';
import { useOutputTarget, METER_COLORS } from '~/composables/useOutputTarget';
import { useProject } from '~/composables/useProject';
import { formatMeterLabel, formatShortTermLabel } from '~/utils/meterScale';

const props = withDefaults(defineProps<{
  leftIndex?: number | null;
  rightIndex?: number | null;
  cueId?: string | null;
  /** Mixer strip source — lane 0 and lane 1 of one bus. */
  mixerId?: string | null;
  /** One bar instead of two, for a mono bus. Lane 0 only. */
  mono?: boolean;
  label?: string;
  showPeakValue?: boolean;
  /**
   * The tick column. Off inside a channel strip, which has its own shared
   * scale sitting between the meter and the fader — rendering both put two
   * scales side by side.
   */
  showScale?: boolean;
  /**
   * Drop the frame (border, background, padding, fixed width) so the meter
   * can sit inside a strip that already provides them.
   */
  bare?: boolean;
  /**
   * Force the gain-reduction sub-track on or off. Left unset it appears only
   * for master channels, which are the only source that reports GR today.
   *
   * The mixer sets it on every strip so the rail is one uniform grid: with it
   * appearing on the master alone, that strip's meter block was wider than the
   * others and its fader sat further across. A bus reads zero reduction, which
   * is not a placeholder (nothing is reducing it) and is where the bus
   * compressor's reading will land.
   */
  showGr?: boolean;
  minDb?: number;
  maxDb?: number;
}>(), {
  leftIndex: null,
  rightIndex: null,
  cueId: null,
  mixerId: null,
  mono: false,
  label: '',
  showPeakValue: false,
  showScale: true,
  bare: false,
  showGr: undefined,
  minDb: -60,
  maxDb: 0,
});

// Always subscribe unconditionally — composables handle null IDs by returning
// silence. This ensures a cueId that resolves after mount (the server
// decorates items asynchronously after mirroring audio into the engine)
// still activates the meter subscription on the next broadcast frame.
const cueStream   = useCueMeters(() => props.cueId);
const leftStream  = useMasterMeter(() => props.leftIndex);
const rightStream = useMasterMeter(() => props.rightIndex);
// A mono bus has only lane 0, so both sides read it and only one bar is drawn.
const mixerL = useMixerMeter(() => props.mixerId, () => 0);
const mixerR = useMixerMeter(() => props.mixerId, () => (props.mono ? 0 : 1));

// One place decides which stream feeds each side. Everything below reads these
// rather than repeating the source branch — with three sources the ternary
// chains were about to appear a dozen times over.
type Reading = {
  peak: number; rms: number; max: number;
  truePeak: number; trueMax: number;
  kwMs: number; kwMsS: number;
};
const SILENT_READING: Reading = {
  peak: -120, rms: -120, max: -120, truePeak: -120, trueMax: -120, kwMs: 0, kwMsS: 0,
};

function readCue(i: number): Reading {
  // A mono cue has one source; its lane 0 feeds both sides.
  const s = cueStream.sources.value[i] ?? cueStream.sources.value[0];
  if (!s) return SILENT_READING;
  return {
    peak: s.peak_db ?? -120, rms: s.rms_db ?? -120, max: s.peak_max_db ?? -120,
    truePeak: s.true_peak_db ?? -120, trueMax: s.true_peak_max_db ?? -120,
    kwMs: s.kw_ms ?? 0, kwMsS: s.kw_ms_s ?? 0,
  };
}
function readStream(s: {
  peak: { value: number }; rms: { value: number }; peakMax: { value: number };
  truePeak: { value: number }; truePeakMax: { value: number };
  kwMs: { value: number }; kwMsS: { value: number };
}): Reading {
  return {
    peak: s.peak.value, rms: s.rms.value, max: s.peakMax.value,
    truePeak: s.truePeak.value, trueMax: s.truePeakMax.value,
    kwMs: s.kwMs.value, kwMsS: s.kwMsS.value,
  };
}

const srcL = computed<Reading>(() =>
  props.cueId   != null ? readCue(0)
  : props.mixerId != null ? readStream(mixerL)
  : readStream(leftStream));
const srcR = computed<Reading>(() =>
  props.cueId   != null ? readCue(1)
  : props.mixerId != null ? readStream(mixerR)
  : readStream(rightStream));

// Gain reduction is reported by master channels today; a bus has no dynamics
// of its own until Stage 5, so it reads zero. The caller can still ask for the
// track, and the mixer does, so every strip in the rail is the same width.
const grVisible = computed(() =>
  props.showGr ?? (props.leftIndex != null || props.rightIndex != null));

// Server-reported output-target levels and meter mode.
const { levels, meterMode, colorForLevel } = useOutputTarget();

const { currentProject } = useProject();
const accentColor = computed(() => currentProject.value?.theme?.accentColor ?? '#DA1E28');

// Raw signal values from the server (always peak_db and rms_db).
const rawPeakL = computed(() => srcL.value.peak);
const rawRmsL  = computed(() => srcL.value.rms);
const rawPeakR = computed(() => srcR.value.peak);
const rawRmsR  = computed(() => srcR.value.rms);

// Lossless raw max since the previous frame — drives peak hold + clip latch.
// In dBTP mode the true-peak max is used, so intersample overs latch clip.
const rawMaxL = computed(() =>
  meterMode.value === 'dBTP' ? srcL.value.trueMax : srcL.value.max);
const rawMaxR = computed(() =>
  meterMode.value === 'dBTP' ? srcR.value.trueMax : srcR.value.max);

const holdL = usePeakHold(() => rawMaxL.value);
const holdR = usePeakHold(() => rawMaxR.value);
const resetClips = () => { holdL.resetClip(); holdR.resetClip(); };

// True-peak stream (4× oversampled server-side when the project's meter
// mode is dBTP; mirrors sample peak otherwise).
const rawTpL = computed(() => srcL.value.truePeak);
const rawTpR = computed(() => srcR.value.truePeak);

// Loudness (BS.1770) of the metered pair: sum of the channels' K-weighted
// mean squares. One value for the pair — loudness has no L/R.
// Momentary (400 ms, EBU "M") drives the bars; short-term (3 s, EBU "S")
// is shown as a second readout. A mono source is summed once, not twice.
const lufsMomentary = computed(() => props.mono
  ? lufsFromKwMs([srcL.value.kwMs])
  : lufsFromKwMs([srcL.value.kwMs, srcR.value.kwMs]));
const lufsShortTerm = computed(() => props.mono
  ? lufsFromKwMs([srcL.value.kwMsS])
  : lufsFromKwMs([srcL.value.kwMsS, srcR.value.kwMsS]));

// Display value selected by the active meter mode.
// dBTP → oversampled true peak; dBFS → sample peak; RMS → rms;
// LUFS → K-weighted momentary loudness (same value on both bars — loudness
// is a property of the pair, not of a channel).
const displayL = computed(() => {
  switch (meterMode.value) {
    case 'RMS':  return rawRmsL.value;
    case 'LUFS': return lufsMomentary.value;
    case 'dBTP': return rawTpL.value;
    default:     return rawPeakL.value; // dBFS
  }
});
const displayR = computed(() => {
  switch (meterMode.value) {
    case 'RMS':  return rawRmsR.value;
    case 'LUFS': return lufsMomentary.value;
    case 'dBTP': return rawTpR.value;
    default:     return rawPeakR.value;
  }
});

function fillStyle(db: number, opacity: number): Record<string, string> {
  const pct = Math.min(100, Math.max(0,
    ((db - props.minDb) / (props.maxDb - props.minDb)) * 100));
  return {
    height: '100%',
    background: colorForLevel(db),
    clipPath: `inset(${(100 - pct).toFixed(2)}% 0 0 0)`,
    opacity: String(opacity),
  };
}

const peakStyleL = computed(() => fillStyle(displayL.value, 1));
const rmsStyleL  = computed(() => fillStyle(rawRmsL.value,  0.4));
const peakStyleR = computed(() => fillStyle(displayR.value, 1));
const rmsStyleR  = computed(() => fillStyle(rawRmsR.value,  0.4));

// Gain-reduction fill: grows downward from the top of the GR track,
// sized by how much the brickwall limiter is currently reducing the signal.
function grStyle(grDb: number): Record<string, string> {
  const range = props.maxDb - props.minDb;
  const pct = range > 0 ? Math.min(100, (Math.abs(grDb) / range) * 100) : 0;
  return {
    background: accentColor.value,
    clipPath: `inset(0 0 ${(100 - pct).toFixed(2)}% 0)`,
  };
}

const grStyleL = computed(() => grStyle(leftStream.gainReduction.value));
const grStyleR = computed(() => grStyle(rightStream.gainReduction.value));

// Peak-hold line: thin marker at the held level, coloured by zone.
function holdStyle(db: number): Record<string, string> {
  const pct = Math.min(100, Math.max(0,
    ((db - props.minDb) / (props.maxDb - props.minDb)) * 100));
  return { bottom: `${pct.toFixed(2)}%`, background: colorForLevel(db) };
}
const holdVisibleL = computed(() => holdL.held.value > props.minDb);
const holdVisibleR = computed(() => holdR.held.value > props.minDb);
const holdStyleL = computed(() => holdStyle(holdL.held.value));
const holdStyleR = computed(() => holdStyle(holdR.held.value));

// Scale tick marks at key zone boundary levels from the server-reported
// output target. Ticks use the zone colour for their position.
const scaleMarks = computed(() => {
  const { minDb, maxDb } = props;
  const range = maxDb - minDb;
  const lv = levels.value;
  const candidates = [
    lv.redAbove, lv.yellowMin, lv.greenMin, lv.blueBelow,
    // Always include 0 at the top as a ceiling reference.
    0,
  ];
  return [...new Set(candidates)]
    .filter(db => db >= minDb && db <= maxDb)
    .sort((a, b) => b - a)
    .map(db => ({
      db,
      pct: ((db - minDb) / range) * 100,
      label: String(Math.round(db)),
      color: colorForLevel(db),
    }));
});

const peakLabel = computed(() =>
  formatMeterLabel(Math.max(displayL.value, displayR.value), meterMode.value));
const shortTermLabel = computed(() => formatShortTermLabel(lufsShortTerm.value));
</script>

<style lang="scss" scoped>
.stereo-meter {
  display: flex;
  flex-direction: column;
  height: 100%;
  padding: 4px 5px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  box-sizing: border-box;
  // Width is determined by fixed bar widths: scale(24) + gap(3) + 2×chan(14) + gap(3) + padding(10)
  width: 68px;
  flex-shrink: 0;
  gap: 3px;
  // min-height:0 lets the meter shrink with its container. Without it the bars
  // keep their content height and a short mixer pane overflows instead of the
  // meter getting shorter.
  min-height: 0;

  // Inside a channel strip the strip already draws the frame, and the width
  // has to follow the bars rather than the 68px the master meter reserves for
  // its own scale column.
  &--bare {
    padding: 0;
    background: none;
    border: none;
    border-radius: 0;
    width: auto;
  }

  &__label {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-text-secondary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    text-align: center;
    flex-shrink: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 100%;
  }

  &__body {
    display: flex;
    flex-direction: row;
    gap: 3px;
    flex: 1;
    min-height: 0;
  }

  // Scale column — tick marks at EBU reference levels
  &__scale {
    position: relative;
    width: 24px;
    flex-shrink: 0;
  }

  &__mark {
    position: absolute;
    right: 0;
    display: flex;
    align-items: center;
    gap: 2px;
    transform: translateY(50%);
  }

  &__mark-text {
    font-family: var(--font-mono);
    font-size: 7px;
    color: var(--color-text-secondary);
    opacity: 0.7;
    text-align: right;
    flex: 1;
    line-height: 1;
    white-space: nowrap;
  }

  &__mark-tick {
    display: block;
    width: 3px;
    height: 1px;
    background: var(--color-border);
    flex-shrink: 0;
  }

  // Stereo bar pair — fixed gap between L and R channels
  &__bars {
    display: flex;
    flex-direction: row;
    gap: 3px;
    flex: 1;
    min-height: 0;
  }

  &__chan {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 2px;
    flex-shrink: 0;
  }

  // Clip latch dot above each bar. Dim until a raw sample crosses the clip
  // threshold; click to acknowledge/reset.
  &__clip {
    width: 8px;
    height: 4px;
    border-radius: 1px;
    background: var(--color-border);
    cursor: pointer;
    flex-shrink: 0;

    &.is-clipped {
      background: #ff1744;
      box-shadow: 0 0 4px rgba(255, 23, 68, 0.8);
    }
  }

  // Row containing the 8px signal track + 4px GR track
  &__bar-group {
    display: flex;
    flex-direction: row;
    gap: 2px;
    flex: 1;
    min-height: 0;
    align-items: stretch;
  }

  // 8 px signal level bar — transparent bg so it adapts to light and dark
  &__track {
    width: 8px;
    flex-shrink: 0;
    background: transparent;
    border: 1px solid var(--color-border);
    border-radius: 2px;
    position: relative;
    overflow: hidden;
  }

  // 4 px gain-reduction bar — same rounded-rect shape as the signal track
  &__gr-track {
    width: 4px;
    flex-shrink: 0;
    background: transparent;
    border: 1px solid var(--color-border);
    border-radius: 2px;
    position: relative;
    overflow: hidden;
  }

  // Fill shared by both signal fills and the GR fill
  &__fill,
  &__gr-fill {
    position: absolute;
    inset: 0;
    // ~One broadcast frame (30 Hz): just enough to hide frame jitter.
    // Meter feel comes from the engine's ballistics, not CSS smoothing.
    transition: clip-path 35ms linear;
  }

  // Peak-hold line — no transition: it snaps to new peaks and drops on
  // release, like a hardware meter's hold segment.
  &__hold {
    position: absolute;
    left: 0;
    right: 0;
    height: 2px;
    pointer-events: none;
  }

  &__chan-label {
    font-family: var(--font-mono);
    font-size: 7px;
    color: var(--color-text-secondary);
    flex-shrink: 0;
  }

  &__peak-text {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-text-secondary);
    text-align: center;
    flex-shrink: 0;
  }
}
</style>
