<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionSurfaces') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionSurfacesHelp') }}</p>

    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">piano</span>
        {{ t('controls.midiDevice') }}
      </label>
      <select class="settings-select" :value="preferredDevice ?? ''" @change="handleDeviceChange">
        <option value="">{{ t('controls.allDevices') }}</option>
        <option v-for="dev in connectedDevices" :key="dev" :value="dev">{{ dev }}</option>
      </select>
      <p v-if="connectedDevices.length === 0" class="settings-help">{{ t('controls.noDevices') }}</p>
    </section>

    <template v-for="category in midiCategories" :key="category">
      <div class="binding-group">
        <div class="category-header">{{ t(categoryLabelKey(category)) }}</div>
        <div
          v-for="action in midiActionsByCategory(category)"
          :key="action.id"
          class="action-row"
          :class="{ learning: learning === action.id }"
        >
          <span class="action-label">
            {{ action.n !== undefined ? t(action.labelKey, { n: action.n }) : t(action.labelKey) }}
          </span>
          <span class="action-binding">
            <template v-if="learning === action.id">{{ t('controls.waitingForInput') }}</template>
            <template v-else-if="getMidiBinding(action.id)">{{ formatMidiBindingLabel(action.id) }}</template>
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
              class="clear-key-btn"
              @click="handleMidiClear(action.id)"
            >{{ t('controls.clear') }}</button>
          </div>
        </div>

        <!-- Master volume is the one continuous binding, so it carries a step
             size the discrete ones do not need. -->
        <template v-if="category === 'Volume'">
          <div class="action-row action-row--static">
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
          <p class="settings-help">{{ t('controls.volumeMultiplierHint') }}</p>
        </template>
      </div>
    </template>

    <div class="settings-actions">
      <button class="reset-btn" @click="clearAllBindings">{{ t('controls.resetMidi') }}</button>
    </div>

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
</template>

<script setup lang="ts">
import {
  MIDI_ACTIONS,
  formatMidiBinding,
  DEFAULT_MASTER_VOLUME_MULTIPLIER,
  type MidiBinding,
  type MidiActionId,
} from '~/composables/useMidiController';

const { t } = useLocalization();

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

const midiConflictInfo = ref<{ actionId: MidiActionId; binding: MidiBinding; conflictAction: string } | null>(null);

const masterVolumeMultiplier = computed(
  () => midiConfig.value.masterVolumeMultiplier ?? DEFAULT_MASTER_VOLUME_MULTIPLIER,
);

const onMultiplierChange = (e: Event) => {
  const raw = parseFloat((e.target as HTMLInputElement).value);
  if (Number.isNaN(raw)) return;
  setMasterVolumeMultiplier(raw);
};

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
  setPreferredDevice((e.target as HTMLSelectElement).value || null);
};

// Escape cancels an armed Learn and stops there, so abandoning one binding
// does not also close Settings. With nothing armed it falls through to the
// page's handler, which closes. Registered before the page's because a child
// mounts first and capture-phase listeners fire in registration order.
const handleKeydown = (e: KeyboardEvent) => {
  if (e.key !== 'Escape') return;
  if (midiConflictInfo.value) {
    midiConflictInfo.value = null;
    e.preventDefault();
    e.stopPropagation();
    return;
  }
  if (learning.value) {
    stopLearn();
    e.preventDefault();
    e.stopPropagation();
  }
};

onMounted(() => window.addEventListener('keydown', handleKeydown, true));
onUnmounted(() => {
  window.removeEventListener('keydown', handleKeydown, true);
  // Leaving the pane with Learn still armed would swallow the next inbound
  // MIDI message somewhere the operator cannot see it.
  if (learning.value) stopLearn();
});
</script>

<style scoped>
.binding-group {
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.category-header {
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
  margin: 8px 0 6px;
}
.action-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 10px;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-surface);
  font-size: 13px;
}
.action-row.learning {
  border-color: var(--color-accent);
}
.action-label {
  flex: 1 1 auto;
  color: var(--color-text-primary);
}
.action-binding {
  flex: 0 0 auto;
  font-family: 'IBM Plex Mono', monospace;
  font-size: 12px;
  color: var(--color-text-secondary);
  padding: 2px 8px;
  border-radius: 4px;
  background: var(--color-background);
}
.action-buttons {
  display: flex;
  gap: 6px;
  flex: 0 0 auto;
}
.learn-btn,
.clear-key-btn,
.reset-btn {
  background: none;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  color: var(--color-text-secondary);
  font-size: 11px;
  padding: 3px 8px;
  cursor: pointer;
}
.learn-btn:hover,
.clear-key-btn:hover,
.reset-btn:hover {
  color: var(--color-text-primary);
  border-color: var(--color-text-secondary);
}
.learn-btn.active {
  color: var(--color-accent);
  border-color: var(--color-accent);
}
.reset-btn {
  font-size: 13px;
  padding: 8px 14px;
  border-radius: 6px;
}
.multiplier-input {
  flex: 0 0 90px;
  padding: 6px 8px;
  background: var(--color-background);
  color: var(--color-text-primary);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  font-size: 13px;
}
.multiplier-input:focus {
  outline: none;
  border-color: var(--color-accent);
}

.conflict-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.55);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1100;
}
.conflict-dialog {
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  padding: 20px;
  max-width: 420px;
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.35);
}
.conflict-dialog p {
  margin: 0 0 16px;
  font-size: 14px;
  color: var(--color-text-primary);
}
.conflict-buttons {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}
.cancel-btn,
.confirm-btn {
  border-radius: 6px;
  font-size: 13px;
  padding: 8px 14px;
  cursor: pointer;
  border: 1px solid var(--color-border);
  background: none;
  color: var(--color-text-secondary);
}
.confirm-btn {
  border-color: var(--color-accent);
  color: var(--color-accent);
}
</style>
