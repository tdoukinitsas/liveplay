<template>
  <!-- Always left to right: time on a waveform doesn't flip with the language. -->
  <div class="wave" :class="{ 'wave--live': live }" dir="ltr" aria-hidden="true">
    <div class="wave__layer">
      <i v-for="(h, i) in heights" :key="i" :style="{ height: `${h}%` }" />
    </div>
    <div v-if="live" class="wave__layer wave__layer--on">
      <i v-for="(h, i) in heights" :key="i" :style="{ height: `${h}%` }" />
    </div>
    <div v-if="live" class="wave__head" />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';

const props = withDefaults(defineProps<{
  bars?: number;
  seed?: number;
  live?: boolean;
}>(), { bars: 132, seed: 7, live: false });

// A made-up but musical shape: phrases that swell, with a beat on top.
const heights = computed(() => Array.from({ length: props.bars }, (_, i) => {
  const x = i / props.bars;
  const phrase = 0.5 + 0.34 * Math.sin(x * 15 + props.seed) + 0.16 * Math.sin(x * 41 + props.seed * 2);
  const beat = 0.62 + 0.38 * Math.abs(Math.sin(i * 0.72 + props.seed));
  const grain = 0.82 + 0.18 * Math.sin(i * 7.3 + props.seed * 3);
  return Math.max(5, Math.round(phrase * beat * grain * 100));
}));
</script>
