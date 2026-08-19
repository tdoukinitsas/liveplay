<template>
  <!--
    A single dB scale shared by the meter and the fader either side of it.
    That only works because both map dB to position the *same* way — linearly
    across [minDb, maxDb] — so one set of ticks reads correctly for both. If
    either ever gains a taper, this stops being honest and they need separate
    scales again.
  -->
  <div class="scale" :aria-hidden="true">
    <div
      v-for="tick in ticks"
      :key="tick"
      class="scale__tick"
      :class="{ 'scale__tick--unity': tick === 0 }"
      :style="{ bottom: pct(tick) + '%' }"
    >{{ label(tick) }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';

const props = withDefaults(defineProps<{
  minDb?: number;
  maxDb?: number;
  /** Omit for a sensible default spread across the range. */
  ticks?: number[];
}>(), {
  minDb: -60,
  maxDb: 12,
  ticks: undefined,
});

const ticks = computed(() =>
  props.ticks ?? [12, 6, 0, -6, -12, -20, -30, -40, -60].filter(
    t => t <= props.maxDb && t >= props.minDb));

const pct = (db: number) =>
  (((db - props.minDb) / (props.maxDb - props.minDb)) * 100).toFixed(2);

// -60 is the floor of the scale, not a real value, so it reads as -∞.
const label = (db: number) => (db <= props.minDb ? '-∞' : String(db));
</script>

<style scoped>
.scale {
  position: relative;
  width: 20px;
  flex: 0 0 auto;
  font-family: var(--font-mono);
  font-size: 8px;
  color: var(--color-text-disabled);
  user-select: none;
}
.scale__tick {
  position: absolute;
  right: 2px;
  transform: translateY(50%);
  line-height: 1;
  white-space: nowrap;
}
/* Unity is the reference both the fader and the meter are read against. */
.scale__tick--unity { color: var(--color-text-secondary); }
.scale__tick::before {
  content: '';
  position: absolute;
  right: -2px;
  top: 50%;
  width: 3px;
  height: 1px;
  background: currentColor;
}
</style>
