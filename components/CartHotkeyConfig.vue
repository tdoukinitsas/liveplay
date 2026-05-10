<template>
  <div class="hotkey-config-overlay" @click.self="$emit('close')">
    <div class="hotkey-config-panel">
      <div class="config-header">
        <h3>{{ t('cart.hotkeyConfig') }}</h3>
        <button class="close-btn" @click="$emit('close')">&times;</button>
      </div>

      <div class="config-body">
        <div
          v-for="slot in 16"
          :key="slot"
          class="slot-row"
          :class="{ capturing: capturingSlot === slot - 1, conflict: conflictSlot === slot - 1 }"
          @click="startCapture(slot - 1)"
        >
          <span class="slot-index">{{ slot }}</span>
          <span class="slot-binding">
            <template v-if="capturingSlot === slot - 1">
              {{ t('cart.pressAnyKey') }}
            </template>
            <template v-else>
              {{ getLabel(slot - 1) }}
            </template>
          </span>
          <span v-if="errorMessage && errorSlot === slot - 1" class="error-msg">{{ errorMessage }}</span>
        </div>
      </div>

      <div class="config-footer">
        <button class="reset-btn" @click="handleReset">{{ t('cart.resetDefaults') }}</button>
        <button class="done-btn" @click="$emit('close')">{{ t('cart.close') }}</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { formatKeyLabel, eventToBinding, isReservedCombo } from '~/composables/useCartHotkeys';

const emit = defineEmits<{
  close: [];
}>();

const { t } = useLocalization();
const { keyMappings, updateBinding, resetToDefaults } = useCartHotkeys();
const { saveProject } = useProject();

const capturingSlot = ref<number | null>(null);
const errorMessage = ref<string | null>(null);
const errorSlot = ref<number | null>(null);
const conflictSlot = ref<number | null>(null);

const getLabel = (slotIndex: number): string => {
  const binding = keyMappings.value[slotIndex];
  return binding ? formatKeyLabel(binding) : '—';
};

const startCapture = (slotIndex: number) => {
  capturingSlot.value = slotIndex;
  errorMessage.value = null;
  errorSlot.value = null;
  conflictSlot.value = null;
};

const handleKeydown = (e: KeyboardEvent) => {
  if (capturingSlot.value === null) return;

  // Ignore modifier-only key presses
  if (['Control', 'Shift', 'Alt', 'Meta'].includes(e.key)) return;

  e.preventDefault();
  e.stopPropagation();

  const binding = eventToBinding(e);

  // Check for Escape — cancel capture
  if (e.key === 'Escape') {
    capturingSlot.value = null;
    errorMessage.value = null;
    errorSlot.value = null;
    return;
  }

  // Check reserved combos
  if (isReservedCombo(binding)) {
    errorMessage.value = t('cart.reserved');
    errorSlot.value = capturingSlot.value;
    return;
  }

  // Try to update binding
  const result = updateBinding(capturingSlot.value, binding);
  if (result.conflict >= 0) {
    errorMessage.value = `Already assigned to Slot ${result.conflict + 1}`;
    errorSlot.value = capturingSlot.value;
    conflictSlot.value = result.conflict;
    return;
  }

  // Success
  errorMessage.value = null;
  errorSlot.value = null;
  conflictSlot.value = null;
  capturingSlot.value = null;
  saveProject();
};

const handleReset = () => {
  resetToDefaults();
  saveProject();
};

onMounted(() => {
  window.addEventListener('keydown', handleKeydown, true);
});

onUnmounted(() => {
  window.removeEventListener('keydown', handleKeydown, true);
});
</script>

<style scoped>
.hotkey-config-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.hotkey-config-panel {
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
  width: 400px;
  max-height: 80vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
}

.config-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 16px 20px;
  border-bottom: 1px solid var(--color-border);
}

.config-header h3 {
  margin: 0;
  font-size: 16px;
  font-weight: 600;
  color: var(--color-text-primary);
}

.close-btn {
  background: none;
  border: none;
  font-size: 24px;
  cursor: pointer;
  color: var(--color-text-secondary);
  padding: 0 4px;
  line-height: 1;
}

.close-btn:hover {
  color: var(--color-text-primary);
}

.config-body {
  flex: 1;
  overflow-y: auto;
  padding: 8px 0;
}

.slot-row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 8px 20px;
  cursor: pointer;
  transition: background-color 0.1s;
}

.slot-row:hover {
  background: var(--color-surface-hover);
}

.slot-row.capturing {
  background: var(--color-accent-bg, rgba(59, 130, 246, 0.1));
}

.slot-row.conflict {
  background: rgba(239, 68, 68, 0.1);
}

.slot-index {
  font-weight: 700;
  font-size: 14px;
  min-width: 24px;
  color: var(--color-text-secondary);
}

.slot-binding {
  font-family: monospace;
  font-size: 13px;
  font-weight: 600;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  padding: 2px 8px;
  min-width: 60px;
  text-align: center;
}

.error-msg {
  font-size: 12px;
  color: #ef4444;
  margin-left: auto;
}

.config-footer {
  display: flex;
  justify-content: space-between;
  padding: 12px 20px;
  border-top: 1px solid var(--color-border);
}

.reset-btn,
.done-btn {
  padding: 6px 16px;
  border-radius: 4px;
  font-size: 13px;
  cursor: pointer;
  border: 1px solid var(--color-border);
  background: var(--color-background);
  color: var(--color-text-primary);
  transition: background-color 0.15s;
}

.reset-btn:hover,
.done-btn:hover {
  background: var(--color-surface-hover);
}

.done-btn {
  background: var(--color-accent, #3b82f6);
  color: white;
  border-color: transparent;
}

.done-btn:hover {
  opacity: 0.9;
}
</style>
