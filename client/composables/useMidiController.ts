import type { AudioItem } from '~/types/project';

// MIDI binding: identifies a specific control on a MIDI device
export interface MidiBinding {
  channel: number;   // 0-15
  type: 'note' | 'cc' | 'pitchbend';
  number: number;    // note number, CC number (0-127), or 0 for pitchbend
}

// Available action IDs
export type MidiActionId =
  | 'trigger-slot-0' | 'trigger-slot-1' | 'trigger-slot-2' | 'trigger-slot-3'
  | 'trigger-slot-4' | 'trigger-slot-5' | 'trigger-slot-6' | 'trigger-slot-7'
  | 'trigger-slot-8' | 'trigger-slot-9' | 'trigger-slot-10' | 'trigger-slot-11'
  | 'trigger-slot-12' | 'trigger-slot-13' | 'trigger-slot-14' | 'trigger-slot-15'
  | 'pause-resume' | 'toggle-loop' | 'stop-all' | 'master-volume'
  | 'select-up' | 'select-down' | 'play-selected' | 'play-next';

// Config stored in midi-config.json
export interface MidiConfig {
  bindings: Record<string, MidiBinding>; // actionId → binding
  preferredDevice?: string;              // preferred MIDI input device name
}

// All available actions with display metadata
export const MIDI_ACTIONS: { id: MidiActionId; labelKey: string; category: string; continuous: boolean; n?: number }[] = [
  // Cart slots
  ...Array.from({ length: 16 }, (_, i) => ({
    id: `trigger-slot-${i}` as MidiActionId,
    labelKey: 'controls.cartSlot',
    category: 'Cart Slots',
    continuous: false,
    n: i + 1,
  })),
  // Playback
  { id: 'pause-resume',  labelKey: 'controls.pauseResume',  category: 'Playback', continuous: false },
  { id: 'toggle-loop',   labelKey: 'controls.toggleLoop',   category: 'Playback', continuous: false },
  { id: 'stop-all',      labelKey: 'controls.stopAll',      category: 'Playback', continuous: false },
  { id: 'select-up',     labelKey: 'controls.selectUp',     category: 'Playback', continuous: false },
  { id: 'select-down',   labelKey: 'controls.selectDown',   category: 'Playback', continuous: false },
  { id: 'play-selected', labelKey: 'controls.playSelected', category: 'Playback', continuous: false },
  { id: 'play-next',     labelKey: 'controls.playNext',     category: 'Playback', continuous: false },
  // Volume
  { id: 'master-volume', labelKey: 'controls.masterVolume', category: 'Volume',   continuous: true  },
];

/**
 * Format a MIDI binding for display (e.g., "Ch1 Note 36", "Ch1 CC 9").
 */
export const formatMidiBinding = (binding: MidiBinding): string => {
  const ch = `Ch${binding.channel + 1}`;
  if (binding.type === 'pitchbend') return `${ch} Fader`;
  const type = binding.type === 'note' ? 'Note' : 'CC';
  return `${ch} ${type} ${binding.number}`;
};

/**
 * Check if two MIDI bindings match.
 */
const bindingsMatch = (a: MidiBinding, b: MidiBinding): boolean => {
  return a.channel === b.channel && a.type === b.type && a.number === b.number;
};

/**
 * Parse a raw MIDI message into channel, type, number, value.
 */
const parseMidiMessage = (data: Uint8Array): { channel: number; type: 'note' | 'cc' | 'pitchbend' | 'other'; number: number; value: number } | null => {
  if (data.length < 2) return null;
  const status = data[0];
  const channel = status & 0x0F;
  const messageType = status & 0xF0;

  if (data.length >= 3 && messageType === 0x90 && data[2] > 0) {
    // Note On (velocity > 0)
    return { channel, type: 'note', number: data[1], value: data[2] };
  }
  if (data.length >= 3 && (messageType === 0x80 || (messageType === 0x90 && data[2] === 0))) {
    // Note Off
    return { channel, type: 'note', number: data[1], value: 0 };
  }
  if (data.length >= 3 && messageType === 0xB0) {
    // Control Change
    return { channel, type: 'cc', number: data[1], value: data[2] };
  }
  if (data.length >= 3 && messageType === 0xE0) {
    // Pitchbend: 14-bit value from two 7-bit bytes, scaled to 0-127
    const raw14 = data[1] | (data[2] << 7); // 0-16383
    const value = Math.round(raw14 / 128); // scale to 0-127
    return { channel, type: 'pitchbend', number: 0, value };
  }
  return null;
};

// Shared singleton state (module-level so all useMidiController() calls share it)
const config = ref<MidiConfig>({ bindings: {} });
const midiAccess = ref<MIDIAccess | null>(null);
const connectedDevices = ref<string[]>([]);
const learning = ref<MidiActionId | null>(null);
const lastMidiMessage = ref<MidiBinding | null>(null);
let onLearnCapture: ((binding: MidiBinding) => void) | null = null;
let mounted = false;

const preferredDevice = computed(() => config.value.preferredDevice ?? null);

export const useMidiController = () => {
  const { getCartItem } = useCartItems();
  const { playCue, stopCue, pauseCue, resumeCue, stopAllCues, activeCues, setMasterGain, nextItemOverrideUuid, autoNextItemUuid, setNextItem, triggerGroup } = useAudioEngine();
  const { selectedItem, selectedItems, saveProject, currentProject, getAllItemsFlat, toggleItemSelection, findItemByUuid: findProjectItem } = useProject();

  /**
   * Dispatch a discrete action.
   */
  const dispatchDiscrete = (actionId: MidiActionId) => {
    if (actionId.startsWith('trigger-slot-')) {
      const slot = parseInt(actionId.replace('trigger-slot-', ''), 10);
      const item = getCartItem(slot);
      if (!item) return;
      if (activeCues.value.has(item.uuid)) {
        const cue = activeCues.value.get(item.uuid);
        if (cue && cue.isPaused) {
          resumeCue(item.uuid);
        } else {
          stopCue(item.uuid);
        }
      } else {
        playCue(item);
      }
      return;
    }

    if (actionId === 'pause-resume') {
      // Same logic as keyboard Space — prefer active cue over selectedItem
      let targetItem: AudioItem | null = null;
      if (activeCues.value.size > 0) {
        const firstUuid = activeCues.value.keys().next().value;
        if (firstUuid) {
          const { findItemByUuid } = useProject();
          const found = findItemByUuid(firstUuid);
          if (found && found.type === 'audio') targetItem = found as AudioItem;
          if (!targetItem) {
            const { getCartOnlyItem } = useCartItems();
            targetItem = getCartOnlyItem(firstUuid);
          }
        }
      }
      if (!targetItem && selectedItem.value && selectedItem.value.type === 'audio') {
        targetItem = selectedItem.value as AudioItem;
      }
      if (!targetItem) return;
      if (activeCues.value.has(targetItem.uuid)) {
        const cue = activeCues.value.get(targetItem.uuid);
        if (cue && cue.isPaused) {
          resumeCue(targetItem.uuid);
        } else {
          pauseCue(targetItem.uuid);
        }
      } else {
        playCue(targetItem);
      }
      return;
    }

    if (actionId === 'toggle-loop') {
      let targetItem: AudioItem | null = null;
      if (activeCues.value.size > 0) {
        const firstUuid = activeCues.value.keys().next().value;
        if (firstUuid) {
          const { findItemByUuid } = useProject();
          const found = findItemByUuid(firstUuid);
          if (found && found.type === 'audio') targetItem = found as AudioItem;
          if (!targetItem) {
            const { getCartOnlyItem } = useCartItems();
            targetItem = getCartOnlyItem(firstUuid);
          }
        }
      }
      if (!targetItem && selectedItem.value && selectedItem.value.type === 'audio') {
        targetItem = selectedItem.value as AudioItem;
      }
      if (!targetItem) return;
      if (targetItem.endBehavior.action === 'loop') {
        targetItem.endBehavior = { action: 'nothing' };
      } else {
        targetItem.endBehavior = { action: 'loop' };
      }
      saveProject();
      return;
    }

    if (actionId === 'stop-all') {
      stopAllCues();
      return;
    }

    if (actionId === 'select-up' || actionId === 'select-down') {
      const project = currentProject.value;
      if (!project) return;
      const all = getAllItemsFlat(project.items);
      if (all.length === 0) return;
      if (!selectedItem.value) {
        toggleItemSelection(all[0].uuid, false, false);
        return;
      }
      const idx = all.findIndex(i => i.uuid === selectedItem.value!.uuid);
      if (actionId === 'select-up' && idx > 0) {
        toggleItemSelection(all[idx - 1].uuid, false, false);
      } else if (actionId === 'select-down' && idx < all.length - 1) {
        toggleItemSelection(all[idx + 1].uuid, false, false);
      } else if (idx === -1) {
        toggleItemSelection(all[0].uuid, false, false);
      }
      return;
    }

    if (actionId === 'play-selected') {
      const uuid = Array.from(selectedItems.value).pop();
      if (!uuid) return;
      const item = findProjectItem(uuid);
      if (!item || item.type !== 'audio') return;
      playCue(item as import('~/types/project').AudioItem);
      return;
    }

    if (actionId === 'play-next') {
      const effectiveUuid = nextItemOverrideUuid.value ?? autoNextItemUuid.value;
      if (!effectiveUuid) return;
      const item = findProjectItem(effectiveUuid);
      if (!item) return;
      if (nextItemOverrideUuid.value) setNextItem(null);
      if (item.type === 'audio') playCue(item as import('~/types/project').AudioItem);
      else if (item.type === 'group') triggerGroup(item);
      return;
    }
  };

  /**
   * Dispatch a continuous action (CC value 0-127).
   */
  const dispatchContinuous = (actionId: MidiActionId, value: number) => {
    if (actionId === 'master-volume') {
      // Map CC 0-127 to -60dB to 0dB (linear in dB)
      const db = -60 + (value / 127) * 60;
      setMasterGain(db);
    }
  };

  /**
   * Handle an incoming MIDI message.
   */
  const handleMidiMessage = (event: MIDIMessageEvent) => {
    const data = event.data;
    if (!data) return;

    // Filter to preferred device if one is selected
    if (config.value.preferredDevice) {
      const inputName = (event.target as any)?.name as string | undefined;
      if (inputName !== config.value.preferredDevice) return;
    }

    const parsed = parseMidiMessage(data);
    if (!parsed || parsed.type === 'other') return;

    const binding: MidiBinding = { channel: parsed.channel, type: parsed.type as MidiBinding['type'], number: parsed.number };
    lastMidiMessage.value = binding;

    // MIDI Learn mode: capture this message
    if (learning.value !== null) {
      if (onLearnCapture) {
        onLearnCapture(binding);
      }
      return;
    }

    // Look up binding in config
    for (const [actionId, configured] of Object.entries(config.value.bindings)) {
      if (bindingsMatch(configured, binding)) {
        const action = MIDI_ACTIONS.find(a => a.id === actionId);
        if (!action) continue;

        if (action.continuous) {
          dispatchContinuous(actionId as MidiActionId, parsed.value);
        } else if (parsed.type === 'pitchbend') {
          // Pitchbend as discrete: trigger at top of range
          if (parsed.value > 63) {
            dispatchDiscrete(actionId as MidiActionId);
          }
        } else {
          // Only trigger on note-on or CC > 63 (button press)
          if (parsed.type === 'note' && parsed.value > 0) {
            dispatchDiscrete(actionId as MidiActionId);
          } else if (parsed.type === 'cc' && parsed.value > 63) {
            dispatchDiscrete(actionId as MidiActionId);
          }
        }
        break;
      }
    }
  };

  /**
   * Update connected device list.
   */
  const updateDeviceList = () => {
    if (!midiAccess.value) {
      connectedDevices.value = [];
      return;
    }
    const names: string[] = [];
    midiAccess.value.inputs.forEach((input) => {
      if (input.name) names.push(input.name);
    });
    connectedDevices.value = names;
  };

  /**
   * Attach listeners to all MIDI input ports.
   */
  const attachListeners = () => {
    if (!midiAccess.value) return;
    midiAccess.value.inputs.forEach((input) => {
      input.onmidimessage = handleMidiMessage;
    });
  };

  /**
   * Request MIDI access and set up listeners.
   */
  const initMidi = async () => {
    if (!navigator.requestMIDIAccess) {
      console.warn('[MIDI] Web MIDI API not available');
      return;
    }
    try {
      const access = await navigator.requestMIDIAccess({ sysex: false });
      midiAccess.value = access;

      attachListeners();
      updateDeviceList();

      // Handle hot-plug
      access.onstatechange = () => {
        attachListeners();
        updateDeviceList();
      };
    } catch (err) {
      console.error('[MIDI] Failed to request MIDI access:', err);
    }
  };

  /**
   * Load config from main process.
   */
  const loadConfig = async () => {
    if (import.meta.client && window.electronAPI) {
      const data = await window.electronAPI.readMidiConfig();
      if (data) {
        config.value = { bindings: {}, ...data } as MidiConfig;
      }
    }
  };

  /**
   * Set (or clear) the preferred MIDI input device.
   */
  const setPreferredDevice = (deviceName: string | null) => {
    if (deviceName) {
      config.value.preferredDevice = deviceName;
    } else {
      delete config.value.preferredDevice;
    }
    saveConfig();
  };

  /**
   * Save config to main process.
   */
  const saveConfig = async () => {
    if (import.meta.client && window.electronAPI) {
      await window.electronAPI.writeMidiConfig(config.value);
    }
  };

  /**
   * Start MIDI Learn mode for an action.
   */
  const startLearn = (actionId: MidiActionId, callback: (binding: MidiBinding) => void) => {
    learning.value = actionId;
    onLearnCapture = callback;
  };

  /**
   * Stop MIDI Learn mode.
   */
  const stopLearn = () => {
    learning.value = null;
    onLearnCapture = null;
  };

  /**
   * Update a binding. Returns conflict action ID or null.
   */
  const updateBinding = (actionId: MidiActionId, binding: MidiBinding): { conflict: string | null } => {
    // Check for conflicts
    for (const [existingAction, existingBinding] of Object.entries(config.value.bindings)) {
      if (existingAction !== actionId && bindingsMatch(existingBinding, binding)) {
        return { conflict: existingAction };
      }
    }
    config.value.bindings[actionId] = binding;
    saveConfig();
    return { conflict: null };
  };

  /**
   * Clear a binding.
   */
  const clearBinding = (actionId: MidiActionId) => {
    delete config.value.bindings[actionId];
    saveConfig();
  };

  /**
   * Clear all bindings.
   */
  const clearAllBindings = () => {
    config.value.bindings = {};
    saveConfig();
  };

  /**
   * Mount: init MIDI and load config.
   */
  const mount = async () => {
    if (mounted) return;
    mounted = true;
    await loadConfig();
    await initMidi();
  };

  /**
   * Unmount: detach listeners.
   */
  const unmount = () => {
    if (!mounted) return;
    mounted = false;
    if (midiAccess.value) {
      midiAccess.value.inputs.forEach((input) => {
        input.onmidimessage = null;
      });
    }
  };

  return {
    config,
    connectedDevices,
    preferredDevice,
    learning,
    lastMidiMessage,
    mount,
    unmount,
    startLearn,
    stopLearn,
    updateBinding,
    clearBinding,
    clearAllBindings,
    setPreferredDevice,
  };
};
