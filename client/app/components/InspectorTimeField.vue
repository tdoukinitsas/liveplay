<template>
  <!-- A time in a timeline inspector: label (with its description behind an
       ⓘ), then [−][m:ss.mmm][+]. Typing accepts m:ss.mmm, h:mm:ss or plain
       seconds; anything else puts the old value back rather than jumping to 0.
       Read-only fields (a duration) drop the steppers. -->
  <div class="insp-field">
    <!-- The label slot replaces the text (a checkbox that switches the field
         on, say); it is then a label for that, not for the time. -->
    <label class="insp-label" :for="$slots.label ? undefined : id">
      <slot name="label"><span class="insp-label-text">{{ label }}</span></slot>
      <FieldHelp v-if="help" :text="help" />
    </label>
    <!-- Disabled dims the time, not the label: its description, and any
         switch in the label, stay readable and in reach. -->
    <div class="insp-stepper" :class="{ 'insp-disabled': disabled }">
      <button
        v-if="!readonly"
        type="button"
        class="insp-step"
        :disabled="disabled"
        :title="t('waveform.decreaseBy', { seconds: String(step) })"
        :aria-label="t('waveform.decreaseBy', { seconds: String(step) })"
        @click="emit('nudge', -step)"
      >
        <span class="material-symbols-rounded" aria-hidden="true">remove</span>
      </button>
      <input
        :id="id"
        type="text"
        class="insp-input insp-time"
        :value="mixed ? '' : formatEventTime(value)"
        :placeholder="mixed ? t('properties.multipleValues') : ''"
        :readonly="readonly"
        :disabled="disabled"
        @focus="($event.target as HTMLInputElement).select()"
        @keydown.enter="($event.target as HTMLInputElement).blur()"
        @change="onChange"
      />
      <button
        v-if="!readonly"
        type="button"
        class="insp-step"
        :disabled="disabled"
        :title="t('waveform.increaseBy', { seconds: String(step) })"
        :aria-label="t('waveform.increaseBy', { seconds: String(step) })"
        @click="emit('nudge', step)"
      >
        <span class="material-symbols-rounded" aria-hidden="true">add</span>
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { formatEventTime, parseEventTime } from '~/utils/events';
import FieldHelp from './FieldHelp.vue';

const props = withDefaults(defineProps<{
  label?: string;
  value: number;
  /** The selection disagrees: show "Multiple values" until it is set. */
  mixed?: boolean;
  help?: string;
  disabled?: boolean;
  readonly?: boolean;
  step?: number;
}>(), { label: '', mixed: false, help: '', disabled: false, readonly: false, step: 0.5 });

const emit = defineEmits<{
  /** A typed time, in seconds. The parent clamps it. */
  set: [seconds: number];
  /** A stepper press: add this many seconds. */
  nudge: [delta: number];
}>();

const { t } = useLocalization();
const id = `insp-time-${Math.random().toString(36).slice(2, 9)}`;

function onChange(e: Event) {
  const input = e.target as HTMLInputElement;
  const parsed = parseEventTime(input.value);
  if (parsed === null) {
    input.value = props.mixed ? '' : formatEventTime(props.value);
    return;
  }
  emit('set', parsed);
  // The parent clamps: show what it kept, even when that is the value the
  // field already had (no re-render would correct the typed text then).
  void nextTick(() => { input.value = props.mixed ? '' : formatEventTime(props.value); });
}
</script>

<style scoped>
.insp-label-text {
  overflow: hidden;
  text-overflow: ellipsis;
}
</style>
