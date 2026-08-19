<template>
  <!--
    A knob with the number beside it, typeable.

    Every processing parameter wants both: the knob for the gesture, the box
    for the value you were given. An EQ set by ear needs one; an EQ set from a
    spec sheet needs the other, and a console that only offers the knob makes
    the second job miserable.
  -->
  <div class="kf" :class="{ 'kf--disabled': disabled }">
    <Knob
      :value="value"
      :min="min"
      :max="max"
      :origin="origin"
      :step="step"
      :fine-step="fineStep"
      :taper="taper"
      :size="size"
      :disabled="disabled"
      :title="label"
      @input="onKnob"
      @reset="$emit('input', origin)"
    />
    <label class="kf__val">
      <input
        class="kf__input"
        :value="display"
        :disabled="disabled"
        inputmode="decimal"
        @change="onEntry"
        @keyup.enter="onEntry"
      />
      <span v-if="unit" class="kf__unit">{{ unit }}</span>
    </label>
    <span v-if="label && showLabel" class="kf__label">{{ label }}</span>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import Knob from './Knob.vue';

const props = withDefaults(defineProps<{
  value: number;
  min: number;
  max: number;
  origin?: number;
  step?: number;
  fineStep?: number;
  /** See Knob: 'log' for anything in Hertz or milliseconds. */
  taper?: 'linear' | 'log';
  size?: number;
  decimals?: number;
  unit?: string;
  label?: string;
  showLabel?: boolean;
  disabled?: boolean;
}>(), {
  origin: 0,
  step: 0.1,
  fineStep: 0.01,
  taper: 'linear',
  size: 34,
  decimals: 1,
  unit: '',
  label: '',
  showLabel: true,
  disabled: false,
});

const emit = defineEmits<{ (e: 'input', value: number): void }>();

const display = computed(() => props.value.toFixed(props.decimals));

// Round to the precision actually shown before passing it on.
//
// A log taper does not snap to a step — a fixed grid would quantise the bottom
// of the range into a few positions and do nothing at the top — so without
// this the box would read 1023 Hz while the value was 1023.4567, and typing
// the number back would be a change. What is displayed is what is stored.
function onKnob(v: number) {
  const p = Math.pow(10, props.decimals);
  emit('input', Math.round(v * p) / p);
}

// Typed values are clamped rather than rejected: an operator entering 20000 on
// a control that stops at 18000 means "as high as it goes", not "ignore me".
function onEntry(e: Event) {
  const el = e.target as HTMLInputElement;
  const n = Number(el.value.replace(/[^0-9.+-]/g, ''));
  if (!Number.isFinite(n)) { el.value = display.value; return; }
  const clamped = Math.max(props.min, Math.min(props.max, n));
  el.value = clamped.toFixed(props.decimals);
  emit('input', clamped);
}
</script>

<style scoped>
.kf {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
}
.kf--disabled { opacity: 0.55; }

.kf__val {
  display: flex;
  align-items: baseline;
  gap: 2px;
}
.kf__input {
  width: 44px;
  font-family: var(--font-mono);
  font-size: 10px;
  text-align: center;
  padding: 1px 2px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
}
.kf__input:disabled { cursor: not-allowed; }
.kf__unit { font-size: 8px; color: var(--color-text-disabled); }
.kf__label {
  font-size: 9px;
  letter-spacing: 0.02em;
  color: var(--color-text-secondary);
  white-space: nowrap;
}
</style>
