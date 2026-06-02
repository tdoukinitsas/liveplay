<template>
  <div class="live-meter" :class="{ vertical }">
    <div class="live-meter__track">
      <div class="live-meter__fill" :style="rmsStyle"  />
      <div class="live-meter__fill" :style="peakStyle" />
    </div>
    <div v-if="showLabel" class="live-meter__label">
      <span class="peak">{{ peakLabel }}</span>
      <span v-if="gainReduction != null && gainReduction < -0.1" class="gr">
        GR {{ gainReduction.toFixed(1) }} dB
      </span>
    </div>
  </div>
</template>

<!--
  LiveMeterBar.vue — single-channel peak/RMS meter with EBU R128 colour scheme.
  For stereo pairs use StereoMeter.vue (which includes a dB scale).
-->
<script setup lang="ts">
import { computed } from 'vue';
import { useCueMeters, useMixerMeter, useMasterMeter } from '~/composables/useLiveMeters';

type Source = 'cue' | 'mixer' | 'master';

const props = withDefaults(defineProps<{
  source: Source;
  cueId?: string | null;
  channel?: number;
  mixerId?: string | null;
  index?: number | null;
  vertical?: boolean;
  showLabel?: boolean;
  minDb?: number;
  maxDb?: number;
}>(), {
  channel: 0,
  cueId: null,
  mixerId: null,
  index: null,
  vertical: false,
  showLabel: false,
  minDb: -60,
  maxDb: 0,
});

const cueStream    = props.source === 'cue'    ? useCueMeters(() => props.cueId)    : null;
const mixerStream  = props.source === 'mixer'  ? useMixerMeter(() => props.mixerId) : null;
const masterStream = props.source === 'master' ? useMasterMeter(() => props.index)  : null;

const peakDb = computed<number>(() => {
  if (cueStream)    return cueStream.sources.value[props.channel]?.peak_db ?? -120;
  if (mixerStream)  return mixerStream.peak.value;
  if (masterStream) return masterStream.peak.value;
  return -120;
});
const rmsDb = computed<number>(() => {
  if (cueStream)    return cueStream.sources.value[props.channel]?.rms_db  ?? -120;
  if (mixerStream)  return mixerStream.rms.value;
  if (masterStream) return masterStream.rms.value;
  return -120;
});
const gainReduction = computed<number | null>(() =>
  masterStream ? masterStream.gainReduction.value : null);

// EBU R128-inspired gradient matching StereoMeter.vue
const ebuGradient = computed(() => {
  const { minDb, maxDb } = props;
  const range = maxDb - minDb;
  const p = (db: number) =>
    Math.min(100, Math.max(0, ((db - minDb) / range) * 100)).toFixed(2);
  // Horizontal meter: gradient runs left→right
  const dir = props.vertical ? 'to top' : 'to right';
  return (
    `linear-gradient(${dir},` +
    `#00b8d4 0%,#00b8d4 ${p(-40)}%,` +
    `#00e676 ${p(-40)}%,#00e676 ${p(-18)}%,` +
    `#ffc400 ${p(-18)}%,#ffc400 ${p(-9)}%,` +
    `#ff1744 ${p(-9)}%,#ff1744 100%)`
  );
});

function fillStyle(db: number, opacity: number) {
  const pct = Math.min(100, Math.max(0,
    ((db - props.minDb) / (props.maxDb - props.minDb)) * 100));
  const clip = (100 - pct).toFixed(2);
  const clipPath = props.vertical
    ? `inset(${clip}% 0 0 0)`
    : `inset(0 ${clip}% 0 0)`;
  return { background: ebuGradient.value, clipPath, opacity: String(opacity) };
}

const peakLabel = computed(() => peakDb.value <= -119
  ? '−∞ dB' : peakDb.value.toFixed(1) + ' dB');

const peakStyle = computed(() => fillStyle(peakDb.value, 1));
const rmsStyle  = computed(() => fillStyle(rmsDb.value,  0.4));
</script>

<style lang="scss" scoped>
.live-meter {
  display: flex;
  flex-direction: row;
  align-items: center;
  gap: 6px;
  width: 100%;

  &__track {
    flex: 1;
    height: 6px;
    background: var(--color-surface);
    border: 1px solid var(--color-border);
    border-radius: 3px;
    position: relative;
    overflow: hidden;
  }

  &__fill {
    position: absolute;
    inset: 0;
    transition: clip-path 110ms linear;
  }

  &__label {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-text-secondary);
    display: flex;
    gap: 6px;
    .gr { color: #ffb050; }
  }

  &.vertical {
    flex-direction: column-reverse;
    width: auto;
    height: 100%;

    .live-meter__track {
      width: 6px;
      height: 100%;
    }
  }
}
</style>
