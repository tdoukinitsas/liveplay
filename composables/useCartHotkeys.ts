import type { CartSlotKeyBinding } from '~/types/project';
import type { AudioItem } from '~/types/project';
import { DEFAULT_CART_SLOT_KEYS } from '~/types/project';

// Reserved combos that cannot be assigned to cart slots
const RESERVED_COMBOS: CartSlotKeyBinding[] = [
  { key: 's', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'q', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'w', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'z', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'n', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'o', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'a', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'c', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'v', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'x', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'y', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'r', ctrlKey: true, shiftKey: false, altKey: false },
  { key: 'F1', ctrlKey: false, shiftKey: false, altKey: false },
  { key: ' ', ctrlKey: false, shiftKey: false, altKey: false },
  { key: 'Shift', ctrlKey: false, shiftKey: false, altKey: false },
];

/**
 * Check if two key bindings match.
 */
const bindingsMatch = (a: CartSlotKeyBinding, b: CartSlotKeyBinding): boolean => {
  return a.key.toLowerCase() === b.key.toLowerCase()
    && a.ctrlKey === b.ctrlKey
    && a.shiftKey === b.shiftKey
    && a.altKey === b.altKey;
};

/**
 * Check if a binding is a reserved combo.
 */
export const isReservedCombo = (binding: CartSlotKeyBinding): boolean => {
  return RESERVED_COMBOS.some(r => bindingsMatch(r, binding));
};

/**
 * Format a key binding for display (e.g., "Ctrl+1", "Q", "0").
 */
export const formatKeyLabel = (binding: CartSlotKeyBinding): string => {
  const parts: string[] = [];
  if (binding.ctrlKey) parts.push('Ctrl');
  if (binding.shiftKey) parts.push('Shift');
  if (binding.altKey) parts.push('Alt');
  // Capitalize single-letter keys
  const keyLabel = binding.key.length === 1 ? binding.key.toUpperCase() : binding.key;
  parts.push(keyLabel);
  return parts.join('+');
};

/**
 * Convert a KeyboardEvent to a CartSlotKeyBinding.
 */
export const eventToBinding = (e: KeyboardEvent): CartSlotKeyBinding => {
  return {
    key: e.key,
    ctrlKey: e.ctrlKey || e.metaKey,
    shiftKey: e.shiftKey,
    altKey: e.altKey,
  };
};

export const useCartHotkeys = () => {
  const { currentProject, selectedItem, saveProject } = useProject();
  const { getCartItem } = useCartItems();
  const { playCue, stopCue, activeCues } = useAudioEngine();

  /**
   * Get current key mappings, falling back to defaults.
   */
  const keyMappings = computed(() => {
    return currentProject.value?.cartSlotKeys ?? { ...DEFAULT_CART_SLOT_KEYS };
  });

  /**
   * Trigger a cart slot by index — mirrors click behavior.
   */
  const triggerSlot = (slotIndex: number) => {
    const item = getCartItem(slotIndex);
    if (!item) return;

    if (activeCues.value.has(item.uuid)) {
      // Already playing — stop it (toggle behavior)
      stopCue(item.uuid);
    } else {
      playCue(item);
    }
  };

  /**
   * Find which slot a key event maps to. Returns slot index or -1.
   */
  const findSlotForEvent = (e: KeyboardEvent): number => {
    const mappings = keyMappings.value;
    for (const [slotStr, binding] of Object.entries(mappings)) {
      if (
        e.key.toLowerCase() === binding.key.toLowerCase()
        && (e.ctrlKey || e.metaKey) === binding.ctrlKey
        && e.shiftKey === binding.shiftKey
        && e.altKey === binding.altKey
      ) {
        return parseInt(slotStr, 10);
      }
    }
    return -1;
  };

  /**
   * Check if focus is on a text input element.
   */
  const isTextInputFocused = (): boolean => {
    const el = document.activeElement;
    if (!el) return false;
    const tag = el.tagName.toLowerCase();
    if (tag === 'input' || tag === 'textarea') return true;
    if ((el as HTMLElement).isContentEditable) return true;
    return false;
  };

  /**
   * Toggle play/stop for the selected playlist item.
   */
  const togglePlayStop = () => {
    if (!selectedItem.value || selectedItem.value.type !== 'audio') return;
    const item = selectedItem.value as AudioItem;
    if (activeCues.value.has(item.uuid)) {
      stopCue(item.uuid);
    } else {
      playCue(item);
    }
  };

  /**
   * Toggle loop on the selected item's endBehavior.
   */
  const toggleLoop = () => {
    if (!selectedItem.value || selectedItem.value.type !== 'audio') return;
    const item = selectedItem.value as AudioItem;
    if (item.endBehavior.action === 'loop') {
      item.endBehavior = { action: 'nothing' };
    } else {
      item.endBehavior = { action: 'loop' };
    }
    saveProject();
  };

  /**
   * Global keydown handler.
   */
  const handleKeydown = (e: KeyboardEvent) => {
    if (isTextInputFocused()) return;
    if (!currentProject.value) return;

    // Space = toggle play/stop of selected item
    if (e.key === ' ' && !e.ctrlKey && !e.altKey && !e.shiftKey) {
      e.preventDefault();
      e.stopPropagation();
      togglePlayStop();
      return;
    }

    // Right Shift = toggle loop on selected item
    if (e.key === 'Shift' && e.location === KeyboardEvent.DOM_KEY_LOCATION_RIGHT) {
      e.preventDefault();
      e.stopPropagation();
      toggleLoop();
      return;
    }

    // Cart slot hotkeys
    const slotIndex = findSlotForEvent(e);
    if (slotIndex >= 0) {
      e.preventDefault();
      e.stopPropagation();
      triggerSlot(slotIndex);
    }
  };

  /**
   * Update a slot's key binding. Returns conflict slot index or -1.
   */
  const updateBinding = (slotIndex: number, binding: CartSlotKeyBinding): { conflict: number } => {
    if (!currentProject.value) return { conflict: -1 };

    // Check for conflicts with other slots
    const mappings = currentProject.value.cartSlotKeys ?? {};
    for (const [slotStr, existing] of Object.entries(mappings)) {
      const existingSlot = parseInt(slotStr, 10);
      if (existingSlot !== slotIndex && bindingsMatch(existing, binding)) {
        return { conflict: existingSlot };
      }
    }

    // Apply the binding
    if (!currentProject.value.cartSlotKeys) {
      currentProject.value.cartSlotKeys = { ...DEFAULT_CART_SLOT_KEYS };
    }
    currentProject.value.cartSlotKeys[slotIndex] = binding;
    return { conflict: -1 };
  };

  /**
   * Reset all bindings to defaults.
   */
  const resetToDefaults = () => {
    if (!currentProject.value) return;
    currentProject.value.cartSlotKeys = { ...DEFAULT_CART_SLOT_KEYS };
  };

  // Lifecycle: register/unregister global listener
  let mounted = false;

  const mount = () => {
    if (mounted) return;
    window.addEventListener('keydown', handleKeydown);
    mounted = true;
  };

  const unmount = () => {
    if (!mounted) return;
    window.removeEventListener('keydown', handleKeydown);
    mounted = false;
  };

  return {
    keyMappings,
    triggerSlot,
    mount,
    unmount,
    updateBinding,
    resetToDefaults,
    findSlotForEvent,
  };
};
