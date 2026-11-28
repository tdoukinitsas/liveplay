<template>
  <!-- The right-hand end of every timeline toolbar in the Properties panel:
       where the edit cursor is (type a time to move it) and auditioning from
       there on the preview bus. Clicking the waveform places the cursor. -->
  <div class="tl-cursor-tools">
    <label class="tl-cursor" :title="t('events.cursorHint')">
      <span class="material-symbols-rounded" aria-hidden="true">vertical_align_center</span>
      <span>{{ t('events.cursor') }}</span>
      <input
        class="tl-time-input"
        type="text"
        :value="cursor === null ? '' : formatEventTime(cursor)"
        :placeholder="formatEventTime(fallback)"
        @focus="($event.target as HTMLInputElement).select()"
        @keydown.enter="($event.target as HTMLInputElement).blur()"
        @change="onInput"
      />
    </label>
    <Btn
      :icon="previewingThis ? 'stop_circle' : 'headphones'"
      :text="previewingThis ? t('automation.stopPreview') : t('automation.previewFromCursor')"
      :title="previewTitle"
      :class="{ active: previewingThis }"
      :disabled="!previewingThis && !previewAvailable"
      @click="togglePreview"
    />
  </div>
</template>

<script setup lang="ts">
import { formatEventTime, parseEventTime } from '~/utils/events';
import Btn from './Btn.vue';

const props = defineProps<{
  itemUuid: string;
  /** The edit cursor, a file time; null when none is placed. */
  cursor: number | null;
  /** Where the cue starts playing: what an unplaced cursor means. */
  fallback: number;
  /** The file's length, to keep a typed time on it. */
  duration: number;
}>();
const emit = defineEmits<{ 'update:cursor': [value: number | null] }>();

const { t } = useLocalization();

function onInput(e: Event) {
  const input = e.target as HTMLInputElement;
  if (!input.value.trim()) { emit('update:cursor', null); return; }
  const parsed = parseEventTime(input.value);
  if (parsed === null) {
    input.value = props.cursor === null ? '' : formatEventTime(props.cursor);
    return;
  }
  const s = Math.round(Math.min(Math.max(0, parsed), props.duration) * 1000) / 1000;
  emit('update:cursor', s);
  input.value = formatEventTime(s);
}

// The audition goes to the preview-role bus. Without one that reaches
// hardware there is nothing to hear, so the button stays off and says why.
const { previewItemUuid, startPreview, stopPreview } = useProject();
const server = useLiveplayServer();
const previewingThis = computed(() => !!props.itemUuid && previewItemUuid.value === props.itemUuid);
const previewAvailable = computed(() => {
  if (!server.connected) return false;
  const bus = server.buses.find(b => b.preview);
  return !!bus && bus.bound !== false;
});
const previewTitle = computed(() => {
  if (previewingThis.value) return t('automation.stopPreview');
  return previewAvailable.value ? t('automation.previewFromCursorHint') : t('automation.previewUnavailable');
});
function togglePreview() {
  if (previewingThis.value) { stopPreview(); return; }
  if (!previewAvailable.value) return;
  startPreview(props.itemUuid, props.cursor ?? props.fallback);
}
</script>

<style scoped>
.tl-cursor-tools {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  flex: 0 0 auto;
}

.tl-cursor {
  display: flex;
  align-items: center;
  gap: var(--spacing-xs);
  font-size: 12px;
  color: var(--color-text-secondary);

  .material-symbols-rounded { font-size: 16px; }
}

.tl-time-input {
  width: 92px;
  min-height: 30px;
  padding: var(--spacing-xs) var(--spacing-sm);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  color: var(--color-text-primary);
  font-family: var(--font-mono);
  font-size: 12px;
  font-variant-numeric: tabular-nums;

  &:focus {
    outline: none;
    border-color: var(--color-accent);
  }
}

.btn.active {
  border-color: var(--color-accent);
  color: var(--color-accent);
}
</style>
