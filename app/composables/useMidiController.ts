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
  | 'pause-resume' | 'toggle-loop' | 'stop-all' | 'master-volume';

// Config stored in midi-config.json
export interface MidiConfig {
  bindings: Record<string, MidiBinding>; // actionId → binding
}

// All available actions with display metadata
export const MIDI_ACTIONS: { id: MidiActionId; label: string; category: string; continuous: boolean }[] = [
  // Cart slots
  ...Array.from({ length: 16 }, (_, i) => ({
    id: `trigger-slot-${i}` as MidiActionId,
    label: `Cart Slot ${i + 1}`,
    category: 'Cart Slots',
    continuous: false,
  })),
  // Playback
  { id: 'pause-resume', label: 'Pause / Resume', category: 'Playback', continuous: false },
  { id: 'toggle-loop', label: 'Toggle Loop', category: 'Playback', continuous: false },
  { id: 'stop-all', label: 'Stop All', category: 'Playback', continuous: false },
  // Volume
  { id: 'master-volume', label: 'Master Volume', category: 'Volume', continuous: true },
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

export const useMidiController = () => {
  const { getCartItem } = useCartItems();
  const { playCue, stopCue, pauseCue, resumeCue, stopAllCues, activeCues, setMasterGain } = useAudioEngine();
  const { selectedItem, saveProject } = useProject();

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
      if (data && data.bindings) {
        config.value = data as MidiConfig;
      }
    }
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
    learning,
    lastMidiMessage,
    mount,
    unmount,
    startLearn,
    stopLearn,
    updateBinding,
    clearBinding,
    clearAllBindings,
  };
};
