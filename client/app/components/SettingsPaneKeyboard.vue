<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionKeyboard') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionKeyboardHelp') }}</p>

    <div class="binding-group">
      <div class="category-header">{{ t('controls.sectionPlayback') }}</div>
      <div
        v-for="action in PLAYBACK_ACTIONS"
        :key="action.id"
        class="action-row"
        :class="{
          capturing: capturingKeyAction === action.id,
          conflict: keyConflictAction === action.id || keyConflictSlotForAction === action.id
        }"
        @click="startCaptureAction(action.id)"
      >
        <span class="action-label">{{ t(action.labelKey) }}</span>
        <span class="action-binding">
          <template v-if="capturingKeyAction === action.id">{{ t('controls.pressAnyKey') }}</template>
          <template v-else-if="getPlaybackKeyLabel(action.id)">{{ getPlaybackKeyLabel(action.id) }}</template>
          <template v-else>—</template>
        </span>
        <button
          v-if="getPlaybackBinding(action.id)"
          class="clear-key-btn"
          @click.stop="clearPlaybackBinding(action.id)"
        >{{ t('controls.clear') }}</button>
        <span v-if="keyErrorActionId === action.id && keyErrorMessage" class="error-msg">{{ keyErrorMessage }}</span>
      </div>
    </div>

    <div class="binding-group">
      <div class="category-header">{{ t('controls.sectionCartSlots') }}</div>
      <div
        v-for="slot in 16"
        :key="slot"
        class="action-row"
        :class="{ capturing: capturingSlot === slot - 1, conflict: conflictSlot === slot - 1 }"
        @click="startCaptureSlot(slot - 1)"
      >
        <span class="action-label">{{ t('controls.cartSlot', { n: slot }) }}</span>
        <span class="action-binding">
          <template v-if="capturingSlot === slot - 1">{{ t('controls.pressAnyKey') }}</template>
          <template v-else-if="getSlotKeyLabel(slot - 1)">{{ getSlotKeyLabel(slot - 1) }}</template>
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

    <div class="settings-actions">
      <button class="reset-btn" @click="handleReset">{{ t('controls.resetKeyboard') }}</button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { formatKeyLabel, eventToBinding, isReservedCombo } from '~/composables/useCartHotkeys';
import type { PlaybackKeyAction, CartSlotKeyBinding } from '~/types/project';

const { t } = useLocalization();

// Mirrors MIDI_ACTIONS' order, so the same action sits in the same place on
// this pane and on Control surfaces.
const PLAYBACK_ACTIONS: { id: PlaybackKeyAction; labelKey: string }[] = [
  { id: 'play-next',       labelKey: 'controls.playNext'       },
  { id: 'pause-resume',    labelKey: 'controls.pauseResume'    },
  { id: 'toggle-loop',     labelKey: 'controls.toggleLoop'     },
  { id: 'cue-to-continue', labelKey: 'controls.cueToContinue'  },
  { id: 'jump-cue',        labelKey: 'controls.jumpCue'        },
  { id: 'stop-all',        labelKey: 'controls.stopAll'        },
  { id: 'select-up',       labelKey: 'controls.selectUp'       },
  { id: 'select-down',     labelKey: 'controls.selectDown'     },
  { id: 'play-selected',   labelKey: 'controls.playSelected'   },
];

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

const getSlotKeyLabel = (slotIndex: number): string => {
  const binding = keyMappings.value[slotIndex];
  return binding ? formatKeyLabel(binding) : '';
};
const getPlaybackBinding = (action: PlaybackKeyAction): CartSlotKeyBinding | null =>
  playbackMappings.value[action] ?? null;
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

const resetCaptureState = () => {
  keyErrorMessage.value = null;
  keyErrorSlot.value = null;
  keyErrorActionId.value = null;
  conflictSlot.value = null;
  keyConflictAction.value = null;
  keyConflictSlotForAction.value = null;
};
const startCaptureSlot = (slotIndex: number) => {
  capturingSlot.value = slotIndex;
  capturingKeyAction.value = null;
  resetCaptureState();
};
const startCaptureAction = (action: PlaybackKeyAction) => {
  capturingKeyAction.value = action;
  capturingSlot.value = null;
  resetCaptureState();
};

const handleKeydown = (e: KeyboardEvent) => {
  // Escape cancels a capture in progress and stops there — the page's own
  // Escape handler would otherwise close Settings out from under someone who
  // only meant to abandon one binding. When nothing is being captured this
  // falls through untouched and the page closes, which is what you want.
  //
  // This listener is registered before the page's because a child mounts
  // first, and capture-phase listeners on the same target fire in
  // registration order.
  if (e.key === 'Escape') {
    if (capturingSlot.value !== null || capturingKeyAction.value !== null) {
      capturingSlot.value = null;
      capturingKeyAction.value = null;
      resetCaptureState();
      e.preventDefault();
      e.stopPropagation();
    }
    return;
  }

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
    // updateBinding only checks slot-vs-slot, so the cart-vs-playback
    // direction is checked here — the same split the modal had.
    for (const [action, pb] of Object.entries(playbackMappings.value) as [PlaybackKeyAction, CartSlotKeyBinding | null][]) {
      if (pb && pb.key.toLowerCase() === binding.key.toLowerCase()
        && pb.ctrlKey === binding.ctrlKey && pb.shiftKey === binding.shiftKey && pb.altKey === binding.altKey) {
        keyErrorMessage.value = t('controls.conflictAction', {
          action: t(PLAYBACK_ACTIONS.find(a => a.id === action)?.labelKey ?? ''),
        });
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
    resetCaptureState();
    capturingSlot.value = null;
    saveProject();
  } else if (capturingKeyAction.value !== null) {
    const action = capturingKeyAction.value;
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
      keyErrorMessage.value = t('controls.conflictAction', {
        action: t(PLAYBACK_ACTIONS.find(a => a.id === result.conflictAction)?.labelKey ?? ''),
      });
      keyErrorActionId.value = action;
      keyConflictAction.value = result.conflictAction;
      return;
    }
    if (result.conflictSlot >= 0) {
      keyErrorMessage.value = t('controls.conflictSlot', { n: result.conflictSlot + 1 });
      keyErrorActionId.value = action;
      return;
    }
    resetCaptureState();
    capturingKeyAction.value = null;
    saveProject();
  }
};

const handleReset = () => {
  resetToDefaults();
  resetPlaybackToDefaults();
  saveProject();
};

onMounted(() => window.addEventListener('keydown', handleKeydown, true));
onUnmounted(() => window.removeEventListener('keydown', handleKeydown, true));
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
  cursor: pointer;
  font-size: 13px;
}
.action-row:hover {
  background: var(--color-surface-hover);
}
.action-row.capturing {
  border-color: var(--color-accent);
}
.action-row.conflict {
  border-color: #e5484d;
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
.clear-key-btn {
  flex: 0 0 auto;
  background: none;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  color: var(--color-text-secondary);
  font-size: 11px;
  padding: 3px 8px;
  cursor: pointer;
}
.clear-key-btn:hover {
  color: var(--color-text-primary);
  border-color: var(--color-text-secondary);
}
.error-msg {
  flex: 1 0 100%;
  font-size: 11px;
  color: #e5484d;
}
.reset-btn {
  background: none;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  color: var(--color-text-secondary);
  font-size: 13px;
  padding: 8px 14px;
  cursor: pointer;
}
.reset-btn:hover {
  color: var(--color-text-primary);
  border-color: var(--color-text-secondary);
}
</style>
