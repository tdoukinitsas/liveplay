<template>
  <div class="control-config-overlay" @click.self="$emit('close')">
    <div class="control-config-panel">
      <div class="config-header">
        <div class="header-left">
          <h3>{{ t('controls.title') }}</h3>
        </div>
        <button class="close-btn" @click="$emit('close')">&times;</button>
      </div>

      <div class="tab-bar">
        <button
          class="tab-btn"
          :class="{ active: activeTab === 'keyboard' }"
          @click="activeTab = 'keyboard'"
        >
          <span class="tab-icon">&#x2328;</span> {{ t('controls.tabKeyboard') }}
        </button>
        <button
          class="tab-btn"
          :class="{ active: activeTab === 'midi' }"
          @click="activeTab = 'midi'"
        >
          <span class="tab-icon">&#x1F3B9;</span> {{ t('controls.tabMidi') }}
        </button>
      </div>

      <!-- Keyboard tab -->
      <div v-if="activeTab === 'keyboard'" class="config-body">
        <!-- Playback section -->
        <div class="category-header">{{ t('controls.sectionPlayback') }}</div>
        <div
          v-for="action in PLAYBACK_ACTIONS"
          :key="action.id"
          class="action-row keyboard-action-row"
          :class="{
            capturing: capturingKeyAction === action.id,
            conflict: keyConflictAction === action.id || keyConflictSlotForAction === action.id
          }"
          @click="startCaptureAction(action.id)"
        >
          <span class="action-label">{{ t(action.labelKey) }}</span>
          <span class="action-binding">
            <template v-if="capturingKeyAction === action.id">
              {{ t('controls.pressAnyKey') }}
            </template>
            <template v-else-if="getPlaybackKeyLabel(action.id)">
              {{ getPlaybackKeyLabel(action.id) }}
            </template>
            <template v-else>—</template>
          </span>
          <button
            v-if="getPlaybackBinding(action.id)"
            class="clear-key-btn"
            @click.stop="clearPlaybackBinding(action.id)"
          >{{ t('controls.clear') }}</button>
          <span v-if="keyErrorActionId === action.id && keyErrorMessage" class="error-msg">{{ keyErrorMessage }}</span>
        </div>

        <!-- Cart Slots section -->
        <div class="category-header">{{ t('controls.sectionCartSlots') }}</div>
        <div
          v-for="slot in 16"
          :key="slot"
          class="action-row keyboard-action-row"
          :class="{
            capturing: capturingSlot === slot - 1,
            conflict: conflictSlot === slot - 1
          }"
          @click="startCaptureSlot(slot - 1)"
        >
          <span class="action-label">{{ t('controls.cartSlot', { n: slot }) }}</span>
          <span class="action-binding">
            <template v-if="capturingSlot === slot - 1">
              {{ t('controls.pressAnyKey') }}
            </template>
            <template v-else-if="getSlotKeyLabel(slot - 1)">
              {{ getSlotKeyLabel(slot - 1) }}
            </template>
            <template v-else>—</template>
          </span>
          <button
            v-if="keyMappings[slot - 1]"
            class="clear-key-btn"
            @click.stop="clearSlotBinding(slot - 1)"
          >{{ t('controls.clear') }}</button>
          <span v-if="keyErrorMessage && keyErrorSlot === slot - 1" class="error-msg">{{ keyErrorMessage }}</span>
        </div>
      </div>

      <!-- MIDI tab -->
      <div v-if="activeTab === 'midi'" class="config-body">
        <!-- Device selector -->
        <div class="device-row">
          <label class="device-label">{{ t('controls.midiDevice') }}</label>
          <select
            class="device-select"
            :value="preferredDevice ?? ''"
            @change="handleDeviceChange"
          >
            <option value="">{{ t('controls.allDevices') }}</option>
            <option v-for="dev in connectedDevices" :key="dev" :value="dev">{{ dev }}</option>
          </select>
          <span v-if="connectedDevices.length === 0" class="no-device-hint">{{ t('controls.noDevices') }}</span>
        </div>

        <template v-for="category in midiCategories" :key="category">
          <div class="category-header">{{ t(categoryLabelKey(category)) }}</div>
          <div
            v-for="action in midiActionsByCategory(category)"
            :key="action.id"
            class="action-row"
            :class="{ learning: learning === action.id }"
          >
            <span class="action-label">{{ action.n !== undefined ? t(action.labelKey, { n: action.n }) : t(action.labelKey) }}</span>
            <span class="action-binding">
              <template v-if="learning === action.id">
                {{ t('controls.waitingForInput') }}
              </template>
              <template v-else-if="getMidiBinding(action.id)">
                {{ formatMidiBindingLabel(action.id) }}
              </template>
              <template v-else>—</template>
            </span>
            <div class="action-buttons">
              <button
                class="learn-btn"
                :class="{ active: learning === action.id }"
                @click="toggleLearn(action.id)"
              >
                {{ learning === action.id ? t('controls.cancel') : t('controls.learn') }}
              </button>
              <button
                v-if="getMidiBinding(action.id)"
                class="clear-btn"
                @click="handleMidiClear(action.id)"
              >
                {{ t('controls.clear') }}
              </button>
            </div>
          </div>

          <!-- Master-volume step multiplier (incremental control) -->
          <template v-if="category === 'Volume'">
            <div class="action-row">
              <span class="action-label">{{ t('controls.volumeMultiplier') }}</span>
              <input
                class="multiplier-input"
                type="number"
                min="0.1"
                max="60"
                step="0.1"
                :value="masterVolumeMultiplier"
                @change="onMultiplierChange"
              />
            </div>
            <p class="multiplier-hint">{{ t('controls.volumeMultiplierHint') }}</p>
          </template>
        </template>
      </div>

      <div class="config-footer">
        <button class="reset-btn" @click="handleReset">
          {{ activeTab === 'keyboard' ? t('controls.resetKeyboard') : t('controls.resetMidi') }}
        </button>
        <button class="done-btn" @click="$emit('close')">{{ t('controls.close') }}</button>
      </div>

      <!-- MIDI Conflict dialog -->
      <div v-if="midiConflictInfo" class="conflict-overlay" @click.self="midiConflictInfo = null">
        <div class="conflict-dialog">
          <p>{{ midiConflictMessage }}</p>
          <div class="conflict-buttons">
            <button class="cancel-btn" @click="midiConflictInfo = null">{{ t('controls.cancel') }}</button>
            <button class="confirm-btn" @click="resolveMidiConflict">{{ t('controls.reassign') }}</button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { formatKeyLabel, eventToBinding, isReservedCombo } from '~/composables/useCartHotkeys';
import {
  MIDI_ACTIONS,
  formatMidiBinding,
  DEFAULT_MASTER_VOLUME_MULTIPLIER,
  type MidiBinding,
  type MidiActionId,
} from '~/composables/useMidiController';
import type { PlaybackKeyAction, CartSlotKeyBinding } from '~/types/project';

const emit = defineEmits<{ close: [] }>();

const { t } = useLocalization();

// Playback actions metadata (mirrors MIDI_ACTIONS order for the keyboard tab)
const PLAYBACK_ACTIONS: { id: PlaybackKeyAction; labelKey: string }[] = [
  { id: 'play-next',     labelKey: 'controls.playNext'     },
  { id: 'pause-resume',  labelKey: 'controls.pauseResume'  },
  { id: 'toggle-loop',   labelKey: 'controls.toggleLoop'   },
  { id: 'stop-all',      labelKey: 'controls.stopAll'      },
  { id: 'select-up',     labelKey: 'controls.selectUp'     },
  { id: 'select-down',   labelKey: 'controls.selectDown'   },
  { id: 'play-selected', labelKey: 'controls.playSelected' },
];

// Keyboard state
const {
  keyMappings,
  playbackMappings,
  updateBinding: updateKeyBinding,
  updatePlaybackBinding,
  resetToDefaults,
  resetPlaybackToDefaults,
} = useCartHotkeys();
const { saveProject, currentProject } = useProject();

const capturingSlot = ref<number | null>(null);
const capturingKeyAction = ref<PlaybackKeyAction | null>(null);
const keyErrorMessage = ref<string | null>(null);
const keyErrorSlot = ref<number | null>(null);
const keyErrorActionId = ref<PlaybackKeyAction | null>(null);
const conflictSlot = ref<number | null>(null);
const keyConflictAction = ref<PlaybackKeyAction | null>(null);
const keyConflictSlotForAction = ref<PlaybackKeyAction | null>(null);

// MIDI state
const {
  config: midiConfig,
  connectedDevices,
  preferredDevice,
  learning,
  startLearn,
  stopLearn,
  updateBinding: updateMidiBinding,
  clearBinding: clearMidiBinding,
  clearAllBindings,
  setPreferredDevice,
  setMasterVolumeMultiplier,
} = useMidiController();

const masterVolumeMultiplier = computed(
  () => midiConfig.value.masterVolumeMultiplier ?? DEFAULT_MASTER_VOLUME_MULTIPLIER,
);

const onMultiplierChange = (e: Event) => {
  const raw = parseFloat((e.target as HTMLInputElement).value);
  if (Number.isNaN(raw)) return;
  setMasterVolumeMultiplier(raw);
};

const midiConflictInfo = ref<{ actionId: MidiActionId; binding: MidiBinding; conflictAction: string } | null>(null);
const activeTab = ref<'keyboard' | 'midi'>('keyboard');

// --- Keyboard helpers ---

const getSlotKeyLabel = (slotIndex: number): string => {
  const binding = keyMappings.value[slotIndex];
  return binding ? formatKeyLabel(binding) : '';
};

const getPlaybackBinding = (action: PlaybackKeyAction): CartSlotKeyBinding | null => {
  return playbackMappings.value[action] ?? null;
};

const getPlaybackKeyLabel = (action: PlaybackKeyAction): string => {
  const binding = getPlaybackBinding(action);
  return binding ? formatKeyLabel(binding) : '';
};

const clearSlotBinding = (slotIndex: number) => {
  if (!currentProject.value?.cartSlotKeys) return;
  delete currentProject.value.cartSlotKeys[slotIndex];
  saveProject();
};

const clearPlaybackBinding = (action: PlaybackKeyAction) => {
  updatePlaybackBinding(action, null);
  saveProject();
};

const startCaptureSlot = (slotIndex: number) => {
  capturingSlot.value = slotIndex;
  capturingKeyAction.value = null;
  keyErrorMessage.value = null;
  keyErrorSlot.value = null;
  keyErrorActionId.value = null;
  conflictSlot.value = null;
  keyConflictAction.value = null;
  keyConflictSlotForAction.value = null;
};

const startCaptureAction = (action: PlaybackKeyAction) => {
  capturingKeyAction.value = action;
  capturingSlot.value = null;
  keyErrorMessage.value = null;
  keyErrorSlot.value = null;
  keyErrorActionId.value = null;
  conflictSlot.value = null;
  keyConflictAction.value = null;
  keyConflictSlotForAction.value = null;
};

const handleKeydown = (e: KeyboardEvent) => {
  if (e.key === 'Escape') {
    if (capturingSlot.value !== null || capturingKeyAction.value !== null) {
      capturingSlot.value = null;
      capturingKeyAction.value = null;
      keyErrorMessage.value = null;
      keyErrorSlot.value = null;
      keyErrorActionId.value = null;
      e.preventDefault();
      return;
    }
    if (learning.value) {
      stopLearn();
      e.preventDefault();
      return;
    }
    emit('close');
    return;
  }

  if (activeTab.value !== 'keyboard') return;
  if (capturingSlot.value === null && capturingKeyAction.value === null) return;
  if (['Control', 'Shift', 'Alt', 'Meta'].includes(e.key)) return;

  e.preventDefault();
  e.stopPropagation();

  const binding = eventToBinding(e);

  if (isReservedCombo(binding)) {
    keyErrorMessage.value = t('controls.reserved');
    if (capturingSlot.value !== null) keyErrorSlot.value = capturingSlot.value;
    if (capturingKeyAction.value !== null) keyErrorActionId.value = capturingKeyAction.value;
    return;
  }

  if (capturingSlot.value !== null) {
    // Check conflict with other playback actions
    for (const [action, pb] of Object.entries(playbackMappings.value) as [PlaybackKeyAction, CartSlotKeyBinding | null][]) {
      if (pb && pb.key.toLowerCase() === binding.key.toLowerCase()
        && pb.ctrlKey === binding.ctrlKey && pb.shiftKey === binding.shiftKey && pb.altKey === binding.altKey) {
        keyErrorMessage.value = t('controls.conflictAction', { action: t(PLAYBACK_ACTIONS.find(a => a.id === action)?.labelKey ?? '') });
        keyErrorSlot.value = capturingSlot.value;
        return;
      }
    }
    const result = updateKeyBinding(capturingSlot.value, binding);
    if (result.conflict >= 0) {
      keyErrorMessage.value = t('controls.conflictSlot', { n: result.conflict + 1 });
      keyErrorSlot.value = capturingSlot.value;
      conflictSlot.value = result.conflict;
      return;
    }
    keyErrorMessage.value = null;
    keyErrorSlot.value = null;
    conflictSlot.value = null;
    capturingSlot.value = null;
    saveProject();
  } else if (capturingKeyAction.value !== null) {
    const action = capturingKeyAction.value;
    // Check conflict with cart slots
    for (const [slotStr, slotBinding] of Object.entries(keyMappings.value)) {
      if (slotBinding.key.toLowerCase() === binding.key.toLowerCase()
        && slotBinding.ctrlKey === binding.ctrlKey && slotBinding.shiftKey === binding.shiftKey && slotBinding.altKey === binding.altKey) {
        keyErrorMessage.value = t('controls.conflictSlot', { n: parseInt(slotStr, 10) + 1 });
        keyErrorActionId.value = action;
        keyConflictSlotForAction.value = action;
        return;
      }
    }
    const result = updatePlaybackBinding(action, binding);
    if (result.conflictAction) {
      keyErrorMessage.value = t('controls.conflictAction', { action: t(PLAYBACK_ACTIONS.find(a => a.id === result.conflictAction)?.labelKey ?? '') });
      keyErrorActionId.value = action;
      keyConflictAction.value = result.conflictAction;
      return;
    }
    if (result.conflictSlot >= 0) {
      keyErrorMessage.value = t('controls.conflictSlot', { n: result.conflictSlot + 1 });
      keyErrorActionId.value = action;
      return;
    }
    keyErrorMessage.value = null;
    keyErrorActionId.value = null;
    keyConflictAction.value = null;
    keyConflictSlotForAction.value = null;
    capturingKeyAction.value = null;
    saveProject();
  }
};

// --- MIDI helpers ---

const midiCategories = computed(() => {
  const cats: string[] = [];
  for (const action of MIDI_ACTIONS) {
    if (!cats.includes(action.category)) cats.push(action.category);
  }
  return cats;
});

const categoryLabelKey = (category: string): string => {
  if (category === 'Cart Slots') return 'controls.sectionCartSlots';
  if (category === 'Playback')   return 'controls.sectionPlayback';
  if (category === 'Volume')     return 'controls.sectionVolume';
  return category;
};

const midiActionsByCategory = (category: string) =>
  MIDI_ACTIONS.filter(a => a.category === category);

const getMidiBinding = (actionId: string): MidiBinding | undefined =>
  midiConfig.value.bindings[actionId];

const formatMidiBindingLabel = (actionId: string): string => {
  const binding = midiConfig.value.bindings[actionId];
  return binding ? formatMidiBinding(binding) : '';
};

const midiConflictMessage = computed(() => {
  if (!midiConflictInfo.value) return '';
  const conflictAction = MIDI_ACTIONS.find(a => a.id === midiConflictInfo.value!.conflictAction);
  const label = conflictAction
    ? (conflictAction.n !== undefined ? t(conflictAction.labelKey, { n: conflictAction.n }) : t(conflictAction.labelKey))
    : midiConflictInfo.value.conflictAction;
  return t('controls.conflictMessage', { action: label });
});

const toggleLearn = (actionId: MidiActionId) => {
  if (learning.value === actionId) {
    stopLearn();
    return;
  }
  startLearn(actionId, (binding: MidiBinding) => {
    stopLearn();
    const result = updateMidiBinding(actionId, binding);
    if (result.conflict) {
      midiConflictInfo.value = { actionId, binding, conflictAction: result.conflict };
    }
  });
};

const resolveMidiConflict = () => {
  if (!midiConflictInfo.value) return;
  const { actionId, binding, conflictAction } = midiConflictInfo.value;
  clearMidiBinding(conflictAction as MidiActionId);
  updateMidiBinding(actionId, binding);
  midiConflictInfo.value = null;
};

const handleMidiClear = (actionId: MidiActionId) => clearMidiBinding(actionId);

const handleDeviceChange = (e: Event) => {
  const val = (e.target as HTMLSelectElement).value;
  setPreferredDevice(val || null);
};

// --- Shared ---

const handleReset = () => {
  if (activeTab.value === 'keyboard') {
    resetToDefaults();
    resetPlaybackToDefaults();
    saveProject();
  } else {
    clearAllBindings();
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
.control-config-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.control-config-panel {
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
  width: 560px;
  max-height: 82vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
  position: relative;
}

.config-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  padding: 16px 20px 12px;
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

/* Tabs */
.tab-bar {
  display: flex;
  gap: 0;
  padding: 0 20px;
  border-bottom: 1px solid var(--color-border);
}

.tab-btn {
  padding: 8px 16px;
  font-size: 13px;
  font-weight: 500;
  background: none;
  border: none;
  border-bottom: 2px solid transparent;
  color: var(--color-text-secondary);
  cursor: pointer;
  transition: color 0.15s, border-color 0.15s;
  display: flex;
  align-items: center;
  gap: 6px;
}

.tab-btn:hover { color: var(--color-text-primary); }
.tab-btn.active {
  color: var(--color-text-primary);
  border-bottom-color: var(--color-accent, #3b82f6);
}

.tab-icon { font-size: 15px; }

/* Body */
.config-body {
  flex: 1;
  overflow-y: auto;
  padding: 8px 0;
}

/* Category headers */
.category-header {
  font-size: 11px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  color: var(--color-text-secondary);
  padding: 12px 20px 4px;
}

/* Shared action row (both tabs) */
.action-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 20px;
  transition: background-color 0.1s;
}

.action-row:hover { background: var(--color-surface-hover); }
.action-row.capturing,
.keyboard-action-row.capturing {
  background: rgba(59, 130, 246, 0.1);
  cursor: text;
}
.action-row.conflict { background: rgba(239, 68, 68, 0.1); }
.action-row.learning { background: rgba(59, 130, 246, 0.1); }

.keyboard-action-row { cursor: pointer; }

.action-label {
  font-size: 13px;
  color: var(--color-text-primary);
  min-width: 130px;
  flex-shrink: 0;
}

.action-binding {
  font-family: var(--font-mono);
  font-size: 12px;
  font-weight: 600;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  padding: 2px 8px;
  min-width: 80px;
  text-align: center;
  flex: 1;
}

.error-msg {
  font-size: 11px;
  color: #ef4444;
  flex-shrink: 0;
  max-width: 160px;
}

/* MIDI / keyboard action buttons */
.action-buttons {
  display: flex;
  gap: 4px;
  flex-shrink: 0;
}

.learn-btn,
.clear-btn,
.clear-key-btn {
  font-size: 11px;
  padding: 3px 8px;
  border-radius: 3px;
  border: 1px solid var(--color-border);
  background: var(--color-background);
  color: var(--color-text-primary);
  cursor: pointer;
  transition: background-color 0.15s;
  flex-shrink: 0;
}

.learn-btn:hover,
.clear-btn:hover,
.clear-key-btn:hover { background: var(--color-surface-hover); }

.learn-btn.active {
  background: var(--color-accent, #3b82f6);
  color: white;
  border-color: transparent;
}

/* Master-volume multiplier input */
.multiplier-input {
  width: 80px;
  font-size: 12px;
  padding: 4px 8px;
  border-radius: 4px;
  border: 1px solid var(--color-border);
  background: var(--color-background);
  color: var(--color-text-primary);
}

.multiplier-hint {
  font-size: 11px;
  color: var(--color-text-secondary);
  padding: 0 20px 8px;
  margin: 0;
}

/* Device selector row */
.device-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 20px 8px;
  border-bottom: 1px solid var(--color-border);
  margin-bottom: 4px;
}

.device-label {
  font-size: 12px;
  font-weight: 600;
  color: var(--color-text-secondary);
  flex-shrink: 0;
}

.device-select {
  flex: 1;
  font-size: 12px;
  padding: 4px 8px;
  border-radius: 4px;
  border: 1px solid var(--color-border);
  background: var(--color-background);
  color: var(--color-text-primary);
  cursor: pointer;
}

.no-device-hint {
  font-size: 11px;
  color: var(--color-text-disabled);
  flex-shrink: 0;
}

/* Footer */
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
.done-btn:hover { background: var(--color-surface-hover); }

.done-btn {
  background: var(--color-accent, #3b82f6);
  color: white;
  border-color: transparent;
}

.done-btn:hover { opacity: 0.9; }

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
