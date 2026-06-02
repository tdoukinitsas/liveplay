<template>
  <div class="volume-slider" :title="title">
    <div class="volume-slider__label-wrap">
      <span
        v-if="!isEditing"
        class="volume-slider__label"
        :title="t('actions.clickToEdit')"
        @click="startEdit"
      >{{ formatLabel(db) }}</span>
      <input
        v-else
        ref="inputRef"
        type="number"
        class="volume-slider__input"
        v-model.number="editValue"
        step="0.1"
        @blur="commitEdit"
        @keyup.enter="commitEdit"
        @keyup.escape="cancelEdit"
      />
    </div>
    <CanvasFader
      :db="db"
      :min-db="minDb"
      :max-db="maxDb"
      @input="$emit('input', $event)"
      @reset="$emit('reset')"
    />
  </div>
</template>

<script setup lang="ts">
import CanvasFader from './CanvasFader.vue';

const props = withDefaults(defineProps<{
  db: number;
  minDb?: number;
  maxDb?: number;
  title?: string;
}>(), {
  minDb: -60,
  maxDb: 6,
});

const emit = defineEmits<{
  (e: 'input', db: number): void;
  (e: 'reset'): void;
}>();

const { t } = useLocalization();

const isEditing = ref(false);
const editValue = ref(0);
const inputRef = ref<HTMLInputElement | null>(null);

function formatLabel(db: number): string {
  if (db <= -60) return '−∞';
  if (db === 0) return '0';
  return (db > 0 ? '+' : '') + db.toFixed(db % 1 === 0 ? 0 : 1);
}

function startEdit() {
  editValue.value = Number(props.db.toFixed(1));
  isEditing.value = true;
  nextTick(() => {
    if (inputRef.value) {
      inputRef.value.focus();
      inputRef.value.select();
    }
  });
}

function commitEdit() {
  if (!isEditing.value) return;
  let val = Number(editValue.value);
  if (isNaN(val)) val = 0;
  val = Math.max(props.minDb, Math.min(props.maxDb, val));
  emit('input', val);
  isEditing.value = false;
}

function cancelEdit() {
  isEditing.value = false;
}
</script>

<style scoped>
.volume-slider {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
  width: 20px;
  padding: 2px 0;
}

.volume-slider__label-wrap {
  position: relative;
  height: 18px;
  width: 100%;
  display: flex;
  justify-content: center;
  align-items: center;
}

.volume-slider__label {
  font-family: var(--font-mono, monospace);
  font-size: 10px;
  font-weight: 600;
  color: var(--color-text-primary);
  text-align: center;
  line-height: 1;
  cursor: text;
  padding: 2px 4px;
  background: rgba(128, 128, 128, 0.15);
  border-radius: 4px;
  white-space: nowrap;
  min-width: 28px;
  user-select: none;
}

.volume-slider__input {
  position: absolute;
  bottom: 0;
  left: 50%;
  transform: translateX(-50%);
  z-index: var(--z-index-dropdown, 1000);
  width: 40px;
  height: 20px;
  font-family: var(--font-mono, monospace);
  font-size: 10px;
  text-align: center;
  background: var(--color-surface);
  color: var(--color-text-primary);
  border: 1px solid var(--color-accent);
  border-radius: 2px;
  outline: none;
  padding: 0;
}
.volume-slider__input::-webkit-inner-spin-button,
.volume-slider__input::-webkit-outer-spin-button {
  -webkit-appearance: none;
  margin: 0;
}
</style>
