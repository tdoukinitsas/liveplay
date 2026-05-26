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
          <div class="stereo-meter__track">
            <div class="stereo-meter__fill" :style="rmsStyleL" />
            <div class="stereo-meter__fill" :style="peakStyleL" />
          </div>
          <div class="stereo-meter__chan-label">L</div>
        </div>
        <div class="stereo-meter__chan">
          <div class="stereo-meter__track">
            <div class="stereo-meter__fill" :style="rmsStyleR" />
            <div class="stereo-meter__fill" :style="peakStyleR" />
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
import { useMasterMeter, useCueMeters } from '~/composables/useLiveMeters';

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

const peakL = computed(() => props.cueId != null
  ? (cueStream.sources.value[0]?.peak_db ?? -120)
  : leftStream.peak.value);
const rmsL = computed(() => props.cueId != null
  ? (cueStream.sources.value[0]?.rms_db ?? -120)
  : leftStream.rms.value);
const peakR = computed(() => props.cueId != null
  ? (cueStream.sources.value[1]?.peak_db ?? cueStream.sources.value[0]?.peak_db ?? -120)
  : rightStream.peak.value);
const rmsR = computed(() => props.cueId != null
  ? (cueStream.sources.value[1]?.rms_db ?? cueStream.sources.value[0]?.rms_db ?? -120)
  : rightStream.rms.value);

// EBU R128-inspired band colours. The meter now renders as a SOLID block of
// the colour matching the current level, rather than a gradient — quieter
// reads stay cyan/green; only loud signals turn amber or red. Scale tick
// labels are coloured to match the band they sit in so the same palette
// reads the same way against the bar.
//   Cyan  #00b8d4 : undermodulated  (below -40 dBFS)
//   Green #00e676 : normal range    (-40 to -18 dBFS, around EBU target)
//   Amber #ffc400 : tolerance zone  (-18 to -9 dBFS)
//   Red   #ff1744 : out-of-bounds   (above -9 dBFS / True Peak warning)
function colorForDb(db: number): string {
  if (db >= -9)  return '#ff1744';
  if (db >= -18) return '#ffc400';
  if (db >= -40) return '#00e676';
  return '#00b8d4';
}

function fillStyle(db: number, opacity: number): Record<string, string> {
  const pct = Math.min(100, Math.max(0,
    ((db - props.minDb) / (props.maxDb - props.minDb)) * 100));
  return {
    height: '100%',
    background: colorForDb(db),
    clipPath: `inset(${(100 - pct).toFixed(2)}% 0 0 0)`,
    opacity: String(opacity),
  };
}

const peakStyleL = computed(() => fillStyle(peakL.value, 1));
const rmsStyleL  = computed(() => fillStyle(rmsL.value,  0.4));
const peakStyleR = computed(() => fillStyle(peakR.value, 1));
const rmsStyleR  = computed(() => fillStyle(rmsR.value,  0.4));

// Scale tick marks at key EBU reference levels. Each tick carries the band
// colour for its dB value so the legend visually agrees with the bar fill.
const scaleMarks = computed(() => {
  const { minDb, maxDb } = props;
  const range = maxDb - minDb;
  return [0, -9, -18, -30, -40]
    .filter(db => db >= minDb && db <= maxDb)
    .map(db => ({
      db,
      pct: ((db - minDb) / range) * 100,
      label: db === 0 ? '0' : String(db),
      color: colorForDb(db),
    }));
});

const peakLabel = computed(() => {
  const m = Math.max(peakL.value, peakR.value);
  return m <= -119 ? '−∞' : m.toFixed(1);
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
  min-width: 54px;
  gap: 3px;

  &__label {
    font-family: var(--font-mono, monospace);
    font-size: 9px;
    color: var(--color-text-secondary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    text-align: center;
    flex-shrink: 0;
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
    // Center the mark text on the dB line
    transform: translateY(50%);
  }

  &__mark-text {
    font-family: var(--font-mono, monospace);
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

  // Stereo bar pair
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
    flex: 1;
    min-width: 8px;
  }

  &__track {
    flex: 1;
    width: 8px;
    min-width: 8px;
    background: var(--color-background);
    border: 1px solid var(--color-border);
    border-radius: 2px;
    position: relative;
    overflow: hidden;
  }

  &__fill {
    position: absolute;
    inset: 0;
    // clip-path animation is set inline; transition smooths between frames
    transition: clip-path 110ms linear;
  }

  &__chan-label {
    font-family: var(--font-mono, monospace);
    font-size: 7px;
    color: var(--color-text-secondary);
    flex-shrink: 0;
  }

  &__peak-text {
    font-family: var(--font-mono, monospace);
    font-size: 9px;
    color: var(--color-text-secondary);
    text-align: center;
    flex-shrink: 0;
  }
}
</style>
