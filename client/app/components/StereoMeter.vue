<template>
  <!--
    Stereo meter with EBU R128-inspired colour scheme and dB scale.
    Used for both per-cue source metering (pass cueId) and per-output
    master-bus metering (pass leftIndex + rightIndex).

    Bug fix: composables are always called unconditionally so that a cueId
    which resolves after mount (server decorates items asynchronously) still
    activates the subscription on the next meter frame.
  -->
  <div class="stereo-meter">
    <div v-if="label" class="stereo-meter__label">{{ label }}</div>

    <div class="stereo-meter__body">
      <!-- dB scale -->
      <div class="stereo-meter__scale">
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
            <div v-if="props.leftIndex != null" class="stereo-meter__gr-track">
              <div class="stereo-meter__gr-fill" :style="grStyleL" />
            </div>
          </div>
          <div class="stereo-meter__chan-label">L</div>
        </div>
        <div class="stereo-meter__chan">
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
            <div v-if="props.rightIndex != null" class="stereo-meter__gr-track">
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
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useMasterMeter, useCueMeters, usePeakHold } from '~/composables/useLiveMeters';
import { useOutputTarget, METER_COLORS } from '~/composables/useOutputTarget';
import { useProject } from '~/composables/useProject';

const props = withDefaults(defineProps<{
  leftIndex?: number | null;
  rightIndex?: number | null;
  cueId?: string | null;
  label?: string;
  showPeakValue?: boolean;
  minDb?: number;
  maxDb?: number;
}>(), {
  leftIndex: null,
  rightIndex: null,
  cueId: null,
  label: '',
  showPeakValue: false,
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

// Server-reported output-target levels and meter mode.
const { levels, meterMode, colorForLevel } = useOutputTarget();

const { currentProject } = useProject();
const accentColor = computed(() => currentProject.value?.theme?.accentColor ?? '#DA1E28');

// Raw signal values from the server (always peak_db and rms_db).
const rawPeakL = computed(() => props.cueId != null
  ? (cueStream.sources.value[0]?.peak_db ?? -120)
  : leftStream.peak.value);
const rawRmsL = computed(() => props.cueId != null
  ? (cueStream.sources.value[0]?.rms_db ?? -120)
  : leftStream.rms.value);
const rawPeakR = computed(() => props.cueId != null
  ? (cueStream.sources.value[1]?.peak_db ?? cueStream.sources.value[0]?.peak_db ?? -120)
  : rightStream.peak.value);
const rawRmsR = computed(() => props.cueId != null
  ? (cueStream.sources.value[1]?.rms_db ?? cueStream.sources.value[0]?.rms_db ?? -120)
  : rightStream.rms.value);

// Lossless raw max since the previous frame — drives peak hold + clip latch.
// In dBTP mode the true-peak max is used, so intersample overs latch clip.
const rawMaxL = computed(() => {
  if (props.cueId != null) {
    const s = cueStream.sources.value[0];
    return (meterMode.value === 'dBTP' ? s?.true_peak_max_db : s?.peak_max_db) ?? -120;
  }
  return meterMode.value === 'dBTP' ? leftStream.truePeakMax.value : leftStream.peakMax.value;
});
const rawMaxR = computed(() => {
  if (props.cueId != null) {
    const s = cueStream.sources.value[1] ?? cueStream.sources.value[0];
    return (meterMode.value === 'dBTP' ? s?.true_peak_max_db : s?.peak_max_db) ?? -120;
  }
  return meterMode.value === 'dBTP' ? rightStream.truePeakMax.value : rightStream.peakMax.value;
});

const holdL = usePeakHold(() => rawMaxL.value);
const holdR = usePeakHold(() => rawMaxR.value);
const resetClips = () => { holdL.resetClip(); holdR.resetClip(); };

// True-peak stream (4× oversampled server-side when the project's meter
// mode is dBTP; mirrors sample peak otherwise).
const rawTpL = computed(() => props.cueId != null
  ? (cueStream.sources.value[0]?.true_peak_db ?? -120)
  : leftStream.truePeak.value);
const rawTpR = computed(() => props.cueId != null
  ? (cueStream.sources.value[1]?.true_peak_db ?? cueStream.sources.value[0]?.true_peak_db ?? -120)
  : rightStream.truePeak.value);

// Display value selected by the active meter mode.
// dBTP → oversampled true peak; dBFS → sample peak; RMS → rms.
// LUFS ≈ rms_db until the server's K-weighted loudness lands.
const displayL = computed(() => {
  switch (meterMode.value) {
    case 'RMS':  return rawRmsL.value;
    case 'LUFS': return rawRmsL.value;
    case 'dBTP': return rawTpL.value;
    default:     return rawPeakL.value; // dBFS
  }
});
const displayR = computed(() => {
  switch (meterMode.value) {
    case 'RMS':  return rawRmsR.value;
    case 'LUFS': return rawRmsR.value;
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

const modeLabel = computed(() => meterMode.value);

const peakLabel = computed(() => {
  const m = Math.max(displayL.value, displayR.value);
  return m <= -119 ? '−∞' : `${Math.round(m)} ${modeLabel.value}`;
});
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
