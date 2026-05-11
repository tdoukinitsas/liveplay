<template>
  <div class="midi-config-overlay" @click.self="$emit('close')">
    <div class="midi-config-panel">
      <div class="config-header">
        <div class="header-left">
          <h3>{{ t('midi.configTitle') }}</h3>
          <span v-if="connectedDevices.length > 0" class="device-info">
            {{ connectedDevices.join(', ') }}
          </span>
          <span v-else class="device-info no-device">{{ t('midi.noDevices') }}</span>
        </div>
        <button class="close-btn" @click="$emit('close')">&times;</button>
      </div>

      <div class="config-body">
        <template v-for="category in categories" :key="category">
          <div class="category-header">{{ category }}</div>
          <div
            v-for="action in actionsByCategory(category)"
            :key="action.id"
            class="action-row"
            :class="{ learning: learning === action.id }"
          >
            <span class="action-label">{{ action.label }}</span>
            <span class="action-binding">
              <template v-if="learning === action.id">
                {{ t('midi.waitingForInput') }}
              </template>
              <template v-else-if="getBinding(action.id)">
                {{ formatBinding(action.id) }}
              </template>
              <template v-else>
                —
              </template>
            </span>
            <div class="action-buttons">
              <button
                class="learn-btn"
                :class="{ active: learning === action.id }"
                @click="toggleLearn(action.id)"
              >
                {{ learning === action.id ? t('midi.cancel') : t('midi.learn') }}
              </button>
              <button
                v-if="getBinding(action.id)"
                class="clear-btn"
                @click="handleClear(action.id)"
              >
                {{ t('midi.clear') }}
              </button>
            </div>
          </div>
        </template>
      </div>

      <div class="config-footer">
        <button class="reset-btn" @click="handleResetAll">{{ t('midi.resetAll') }}</button>
        <button class="done-btn" @click="$emit('close')">{{ t('midi.close') }}</button>
      </div>

      <!-- Conflict dialog -->
      <div v-if="conflictInfo" class="conflict-overlay" @click.self="conflictInfo = null">
        <div class="conflict-dialog">
          <p>{{ conflictMessage }}</p>
          <div class="conflict-buttons">
            <button class="cancel-btn" @click="conflictInfo = null">{{ t('midi.cancel') }}</button>
            <button class="confirm-btn" @click="resolveConflict">{{ t('midi.reassign') }}</button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import {
  MIDI_ACTIONS,
  formatMidiBinding,
  type MidiBinding,
  type MidiActionId,
} from '~/composables/useMidiController';

const emit = defineEmits<{
  close: [];
}>();

const { t } = useLocalization();
const {
  config,
  connectedDevices,
  learning,
  startLearn,
  stopLearn,
  updateBinding,
  clearBinding,
  clearAllBindings,
} = useMidiController();

const conflictInfo = ref<{ actionId: MidiActionId; binding: MidiBinding; conflictAction: string } | null>(null);

const categories = computed(() => {
  const cats: string[] = [];
  for (const action of MIDI_ACTIONS) {
    if (!cats.includes(action.category)) cats.push(action.category);
  }
  return cats;
});

const actionsByCategory = (category: string) => {
  return MIDI_ACTIONS.filter(a => a.category === category);
};

const getBinding = (actionId: string): MidiBinding | undefined => {
  return config.value.bindings[actionId];
};

const formatBinding = (actionId: string): string => {
  const binding = config.value.bindings[actionId];
  return binding ? formatMidiBinding(binding) : '';
};

const conflictMessage = computed(() => {
  if (!conflictInfo.value) return '';
  const conflictAction = MIDI_ACTIONS.find(a => a.id === conflictInfo.value!.conflictAction);
  const label = conflictAction?.label ?? conflictInfo.value.conflictAction;
  return `This control is already assigned to "${label}". Reassign it?`;
});

const toggleLearn = (actionId: MidiActionId) => {
  if (learning.value === actionId) {
    stopLearn();
    return;
  }
  // Start learning
  startLearn(actionId, (binding: MidiBinding) => {
    stopLearn();
    const result = updateBinding(actionId, binding);
    if (result.conflict) {
      conflictInfo.value = { actionId, binding, conflictAction: result.conflict };
    }
  });
};

const resolveConflict = () => {
  if (!conflictInfo.value) return;
  const { actionId, binding, conflictAction } = conflictInfo.value;
  // Clear the conflicting binding, then assign
  clearBinding(conflictAction as MidiActionId);
  updateBinding(actionId, binding);
  conflictInfo.value = null;
};

const handleClear = (actionId: MidiActionId) => {
  clearBinding(actionId);
};

const handleResetAll = () => {
  clearAllBindings();
};

// Cancel learn on Escape
const handleKeydown = (e: KeyboardEvent) => {
  if (e.key === 'Escape') {
    if (learning.value) {
      stopLearn();
      e.preventDefault();
    } else {
      emit('close');
    }
  }
};

onMounted(() => {
  window.addEventListener('keydown', handleKeydown, true);
});

onUnmounted(() => {
  window.removeEventListener('keydown', handleKeydown, true);
  if (learning.value) stopLearn();
});
</script>

<style scoped>
.midi-config-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.midi-config-panel {
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
  width: 500px;
  max-height: 80vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
  position: relative;
}

.config-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  padding: 16px 20px;
  border-bottom: 1px solid var(--color-border);
}

.header-left {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.config-header h3 {
  margin: 0;
  font-size: 16px;
  font-weight: 600;
  color: var(--color-text-primary);
}

.device-info {
  font-size: 12px;
  color: var(--color-success);
}

.device-info.no-device {
  color: var(--color-text-disabled);
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

.category-header {
  font-size: 11px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  color: var(--color-text-secondary);
  padding: 12px 20px 4px;
}

.action-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 20px;
  transition: background-color 0.1s;
}

.action-row:hover {
  background: var(--color-surface-hover);
}

.action-row.learning {
  background: rgba(59, 130, 246, 0.1);
}

.action-label {
  font-size: 13px;
  color: var(--color-text-primary);
  min-width: 120px;
  flex-shrink: 0;
}

.action-binding {
  font-family: monospace;
  font-size: 12px;
  font-weight: 600;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  padding: 2px 8px;
  min-width: 90px;
  text-align: center;
  flex: 1;
}

.action-buttons {
  display: flex;
  gap: 4px;
  flex-shrink: 0;
}

.learn-btn,
.clear-btn {
  font-size: 11px;
  padding: 3px 8px;
  border-radius: 3px;
  border: 1px solid var(--color-border);
  background: var(--color-background);
  color: var(--color-text-primary);
  cursor: pointer;
  transition: background-color 0.15s;
}

.learn-btn:hover,
.clear-btn:hover {
  background: var(--color-surface-hover);
}

.learn-btn.active {
  background: var(--color-accent, #3b82f6);
  color: white;
  border-color: transparent;
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

/* Conflict dialog */
.conflict-overlay {
  position: absolute;
  inset: 0;
  background: rgba(0, 0, 0, 0.4);
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 8px;
}

.conflict-dialog {
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
  padding: 20px;
  max-width: 300px;
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.3);
}

.conflict-dialog p {
  font-size: 13px;
  color: var(--color-text-primary);
  margin-bottom: 16px;
}

.conflict-buttons {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}

.cancel-btn,
.confirm-btn {
  padding: 6px 14px;
  border-radius: 4px;
  font-size: 13px;
  cursor: pointer;
  border: 1px solid var(--color-border);
  background: var(--color-background);
  color: var(--color-text-primary);
}

.confirm-btn {
  background: var(--color-accent, #3b82f6);
  color: white;
  border-color: transparent;
}
</style>
