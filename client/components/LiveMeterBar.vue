<template>
  <div class="live-meter" :class="{ vertical }">
    <div class="live-meter__track">
      <div class="live-meter__rms"  :style="rmsStyle"  />
      <div class="live-meter__peak" :style="peakStyle" />
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
  LiveMeterBar.vue
  -----------------------------------------------------------------------
  A simple peak/RMS meter widget that consumes the WebSocket meter stream
  from the C++ server (via useLiveMeters). Replaces the 1.x "cheat"
  meters that drew the static waveform's per-frame amplitude.

  Usage:
    <LiveMeterBar
      source="cue" :cue-id="cueId" :channel="0"  :vertical="true" />
    <LiveMeterBar source="mixer" :mixer-id="mid" />
    <LiveMeterBar source="master" :index="0"     :show-label="true" />
-->
<script setup lang="ts">
import { computed } from 'vue';
import {
  useCueMeters,
  useMixerMeter,
  useMasterMeter,
} from '~/composables/useLiveMeters';

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

const cueStream    = props.source === 'cue'    ? useCueMeters(() => props.cueId)        : null;
const mixerStream  = props.source === 'mixer'  ? useMixerMeter(() => props.mixerId)     : null;
const masterStream = props.source === 'master' ? useMasterMeter(() => props.index)      : null;

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
const gainReduction = computed<number | null>(() => {
  return masterStream ? masterStream.gainReduction.value : null;
});

function dbToPercent(db: number): number {
  const min = props.minDb, max = props.maxDb;
  const clamped = Math.min(max, Math.max(min, db));
  return ((clamped - min) / (max - min)) * 100;
}

const peakLabel = computed(() => peakDb.value <= -119
  ? '−∞ dB' : peakDb.value.toFixed(1) + ' dB');

const rmsStyle = computed(() => {
  const pct = dbToPercent(rmsDb.value);
  return props.vertical
    ? { height: `${pct}%` }
    : { width:  `${pct}%` };
});
const peakStyle = computed(() => {
  const pct = dbToPercent(peakDb.value);
  return props.vertical
    ? { height: `${pct}%`, background: pct > 95 ? '#ff5151' : '#5fb5ff' }
    : { width:  `${pct}%`, background: pct > 95 ? '#ff5151' : '#5fb5ff' };
});
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
    height: 8px;
    background: #1a1a1a;
    border: 1px solid #2a2a2a;
    border-radius: 3px;
    position: relative;
    overflow: hidden;
  }

  &__rms,
  &__peak {
    position: absolute;
    left: 0; top: 0; bottom: 0;
    transition: width 30ms linear;
  }
  &__rms  { background: rgba(95, 181, 255, 0.35); }
  &__peak { background: #5fb5ff; }

  &__label {
    font-family: monospace;
    font-size: 10px;
    color: #bbb;
    display: flex;
    gap: 6px;
    .gr { color: #ffb050; }
  }

  &.vertical {
    flex-direction: column-reverse;
    width: auto;
    height: 100%;
    .live-meter__track {
      width: 8px;
      height: 100%;
    }
    .live-meter__rms,
    .live-meter__peak {
      left: 0; right: 0; bottom: 0; top: auto;
      width: 100%;
      transition: height 30ms linear;
    }
  }
}
</style>
