import type { CartSlotKeyBinding, PlaybackKeyAction, AudioItem } from '~/types/project';
import { shouldIgnoreHotkey } from '~/utils/hotkeyGuard';
import { DEFAULT_CART_SLOT_KEYS, DEFAULT_PLAYBACK_KEYS } from '~/types/project';

const RESERVED_COMBOS: CartSlotKeyBinding[] = [
  { key: 's', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'q', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'w', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'z', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'n', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'o', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'a', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'c', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'v', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'x', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'y', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'r', ctrlKey: true,  shiftKey: false, altKey: false },
  { key: 'F1', ctrlKey: false, shiftKey: false, altKey: false },
];

// A binding's key, lower case; '' for a malformed one (a show file can hold
// anything), which then matches nothing instead of throwing (N-C16).
const keyOf = (b: Partial<CartSlotKeyBinding> | null | undefined): string =>
  typeof b?.key === 'string' ? b.key.toLowerCase() : '';

const bindingsMatch = (a: CartSlotKeyBinding, b: CartSlotKeyBinding): boolean =>
  keyOf(a) !== '' && keyOf(a) === keyOf(b)
  && a.ctrlKey  === b.ctrlKey
  && a.shiftKey === b.shiftKey
  && a.altKey   === b.altKey;

export const isReservedCombo = (binding: CartSlotKeyBinding): boolean =>
  RESERVED_COMBOS.some(r => bindingsMatch(r, binding));

export const formatKeyLabel = (binding: CartSlotKeyBinding): string => {
  const parts: string[] = [];
  if (binding.ctrlKey)  parts.push('Ctrl');
  if (binding.shiftKey) parts.push('Shift');
  if (binding.altKey)   parts.push('Alt');
  const keyLabel = binding.key === ' ' ? 'Space'
    : binding.key.length === 1 ? binding.key.toUpperCase()
    : binding.key;
  parts.push(keyLabel);
  return parts.join('+');
};

export const eventToBinding = (e: KeyboardEvent): CartSlotKeyBinding => ({
  key: e.key,
  ctrlKey: e.ctrlKey || e.metaKey,
  shiftKey: e.shiftKey,
  altKey: e.altKey,
});

export const useCartHotkeys = () => {
  const { currentProject, selectedItem, selectedItems, saveProject, getAllItemsFlat, toggleItemSelection, findItemByUuid } = useProject();
  const { getCartItem, getCartTarget } = useCartItems();
  const { playCue, stopCue, pauseCue, resumeCue, stopAllCues, activeCues, nextItemOverrideUuid, autoNextItemUuid, setNextItem, triggerGroup, queueLoopContinuation, jumpCue, isItemPlaying, fireItem, stopItemAny } = useAudioEngine();

  // By position on a page: the same key fires the same place on whichever
  // page this window is showing.
  const keyMappings = computed(() =>
    currentProject.value?.cartSlotKeys ?? { ...DEFAULT_CART_SLOT_KEYS }
  );
  // By cart: fires that one cart whatever page is showing. Keyed by the cart's
  // raw slot number, so the key belongs to the cart button (two carts on one
  // cue keep a key each) and goes when the cart is cleared (F-13).
  const pinnedKeys = computed<Record<string, CartSlotKeyBinding>>(() =>
    currentProject.value?.cartPinnedKeys ?? {}
  );

  // The cart keys have their own route (F-03): every client hears a change at
  // once, and another client's save can't undo it.
  const persistCartKeys = (body: { cartSlotKeys?: Record<string, CartSlotKeyBinding>; cartPinnedKeys?: Record<string, CartSlotKeyBinding> }) => {
    const server = useLiveplayServer();
    void server.saveCartKeys(body)
      .catch(async (e: unknown) => {
        console.warn('[cart keys] could not save:', e);
        // Refused (a Viewer, a server that's gone): show what the server has,
        // not a key that only this window believes in (N-C15).
        try {
          const stored = await server.fetchCartKeys();
          const project = currentProject.value;
          if (project && stored) {
            if (stored.cartSlotKeys) (project as any).cartSlotKeys = stored.cartSlotKeys;
            if (stored.cartPinnedKeys) project.cartPinnedKeys = stored.cartPinnedKeys;
          }
        } catch { /* nothing to roll back to */ }
      });
  };

  // "Page 2 · 5 · Door slam": a pinned cart, as the Keyboard pane names it.
  const cartLabel = (slot: number): string => {
    const { cartPages } = useShowTabs();
    const pages = cartPages.value;
    const index = pages.findIndex(p => slot >= p.slotBase && slot < p.slotBase + p.size);
    const page = pages[index];
    const name = getCartTarget(currentProject.value?.cartItems.find(c => c.slot === slot)?.itemUuid ?? '')?.displayName ?? '';
    if (!page) return name;
    const { t } = useLocalization();
    return [page.name || t('tabs.defaultPage', { n: index + 1 }), String(slot - page.slotBase + 1), name]
      .filter(Boolean).join(' \u00b7 ');
  };

  // The transport keymap follows the PERSON as of U4, not the show file.
  //
  // The plan called the project's copy an "override". It is not, and making it
  // one would leave the hazard in place: opening a colleague's project would
  // still reassign the keys your hands know, which is the thing U4 exists to
  // stop. So the operator's own map wins outright, and what is left in a
  // document is read only as a seed for someone who has never set one —
  // handled once, in usePreferences.adoptFromProject.
  const { playbackKeys: userPlaybackKeys, setPlaybackKeys } = usePreferences();

  const playbackMappings = computed<Partial<Record<PlaybackKeyAction, CartSlotKeyBinding | null>>>(() => {
    const saved = userPlaybackKeys.value ?? {};
    const result: Partial<Record<PlaybackKeyAction, CartSlotKeyBinding | null>> = { ...DEFAULT_PLAYBACK_KEYS };
    for (const [action, binding] of Object.entries(saved)) {
      result[action as PlaybackKeyAction] = binding;
    }
    return result;
  });

  const getTargetItem = (): AudioItem | null => {
    if (activeCues.value.size > 0) {
      const firstUuid = activeCues.value.keys().next().value;
      if (firstUuid) {
        const { findItemByUuid } = useProject();
        const item = findItemByUuid(firstUuid);
        if (item && item.type === 'audio') return item as AudioItem;
        const { getCartOnlyItem } = useCartItems();
        const cartItem = getCartOnlyItem(firstUuid);
        if (cartItem) return cartItem;
      }
    }
    if (selectedItem.value && selectedItem.value.type === 'audio') {
      return selectedItem.value as AudioItem;
    }
    return null;
  };

  const toggleCart = (item: ReturnType<typeof getCartItem>) => {
    if (!item) return;
    if (isItemPlaying(item)) stopItemAny(item);
    else fireItem(item);
  };

  // `position` on the page this window is showing.
  const triggerSlot = (position: number) => {
    const page = useShowTabs().activeCartPage.value;
    if (position >= page.size) return;
    toggleCart(getCartItem(page.slotBase + position));
  };

  // The slot of the cart pinned to this key, if that cart is still there.
  const findPinnedForEvent = (e: KeyboardEvent): number | null => {
    for (const [slotStr, binding] of Object.entries(pinnedKeys.value)) {
      if (
        binding && typeof binding.key === 'string'
        && e.key.toLowerCase() === keyOf(binding)
        && (e.ctrlKey || e.metaKey) === binding.ctrlKey
        && e.shiftKey === binding.shiftKey
        && e.altKey === binding.altKey
      ) {
        const slot = parseInt(slotStr, 10);
        return Number.isInteger(slot) && getCartItem(slot) ? slot : null;
      }
    }
    return null;
  };

  const findSlotForEvent = (e: KeyboardEvent): number => {
    const mappings = keyMappings.value;
    for (const [slotStr, binding] of Object.entries(mappings)) {
      if (
        e.key.toLowerCase() === keyOf(binding)
        && (e.ctrlKey || e.metaKey) === binding.ctrlKey
        && e.shiftKey === binding.shiftKey
        && e.altKey   === binding.altKey
      ) {
        return parseInt(slotStr, 10);
      }
    }
    return -1;
  };

  const findPlaybackActionForEvent = (e: KeyboardEvent): PlaybackKeyAction | null => {
    const mappings = playbackMappings.value;
    for (const [action, binding] of Object.entries(mappings) as [PlaybackKeyAction, CartSlotKeyBinding | null][]) {
      if (!binding) continue;
      if (
        e.key.toLowerCase() === keyOf(binding)
        && (e.ctrlKey || e.metaKey) === binding.ctrlKey
        && e.shiftKey === binding.shiftKey
        && e.altKey   === binding.altKey
      ) {
        return action;
      }
    }
    return null;
  };

  const dispatchPlaybackAction = (action: PlaybackKeyAction) => {
    if (action === 'pause-resume') {
      const item = getTargetItem();
      if (!item) return;
      if (activeCues.value.has(item.uuid)) {
        const cue = activeCues.value.get(item.uuid);
        if (cue && cue.isPaused) resumeCue(item.uuid);
        else pauseCue(item.uuid);
      } else {
        playCue(item);
      }
      return;
    }

    if (action === 'toggle-loop') {
      const item = getTargetItem();
      if (!item) return;
      if (item.endBehavior.action === 'loop') {
        item.endBehavior = { action: 'nothing' };
      } else {
        item.endBehavior = { action: 'loop' };
      }
      saveProject();
      return;
    }

    if (action === 'cue-to-continue') {
      const item = getTargetItem();
      if (!item) return;
      queueLoopContinuation(item, resolveLoopContinuationTarget(item));
      return;
    }

    if (action === 'jump-cue') {
      const item = getTargetItem();
      if (!item) return;
      jumpCue(item);
      return;
    }

    if (action === 'stop-current-cue') {
      // The server decides which cue is current (the one fired last that is
      // still on air), as it does for MIDI and POST /api/transport/stop_current.
      useLiveplayServer().stopCurrent();
      return;
    }

    if (action === 'stop-all') {
      stopAllCues();
      return;
    }

    if (action === 'select-up' || action === 'select-down') {
      const project = currentProject.value;
      if (!project) return;
      if (useShowControl().selectStepInView(action === 'select-up' ? -1 : 1)) return;
      // No server to ask: step locally through the rows on the tab this
      // window is showing, so the selection never lands out of sight.
      const all = getAllItemsFlat(useShowTabs().itemsOnActiveTab.value);
      if (all.length === 0) return;
      if (!selectedItem.value) {
        toggleItemSelection(all[0].uuid, false, false);
        return;
      }
      const idx = all.findIndex(i => i.uuid === selectedItem.value!.uuid);
      if (action === 'select-up' && idx > 0) {
        toggleItemSelection(all[idx - 1].uuid, false, false);
      } else if (action === 'select-down' && idx < all.length - 1) {
        toggleItemSelection(all[idx + 1].uuid, false, false);
      } else if (idx === -1 && all.length > 0) {
        toggleItemSelection(all[0].uuid, false, false);
      }
      return;
    }

    if (action === 'play-selected') {
      const uuid = Array.from(selectedItems.value).pop();
      if (!uuid) return;
      const item = findItemByUuid(uuid);
      if (!item || item.type !== 'audio') return;
      playCue(item as AudioItem);
      return;
    }

    if (action === 'play-next') {
      const effectiveUuid = nextItemOverrideUuid.value ?? autoNextItemUuid.value;
      if (!effectiveUuid) return;
      const item = findItemByUuid(effectiveUuid);
      if (!item) return;
      if (nextItemOverrideUuid.value) setNextItem(null);
      if (item.type === 'audio') playCue(item as AudioItem);
      else if (item.type === 'group') triggerGroup(item);
      return;
    }
  };

  const handleKeydown = (e: KeyboardEvent) => {
    // A key a control on the page already handled (Esc closing a popover,
    // Enter on a tab), or one that belongs to the control with focus (typing
    // in a field, a region with its own keys), is not a transport key (F-02,
    // F-09, F-53). Which keys a focused control owns depends on the key: Esc
    // reaches Stop All from a dropdown, a checkbox or a row (R2-06, R2-14).
    if (shouldIgnoreHotkey(e)) return;
    if (!currentProject.value) return;

    // Playback actions
    const playbackAction = findPlaybackActionForEvent(e);
    if (playbackAction) {
      e.preventDefault();
      e.stopPropagation();
      dispatchPlaybackAction(playbackAction);
      return;
    }

    // A key pinned to one cart beats the position keys.
    const pinned = findPinnedForEvent(e);
    if (pinned !== null) {
      e.preventDefault();
      e.stopPropagation();
      toggleCart(getCartItem(pinned));
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

  const updateBinding = (slotIndex: number, binding: CartSlotKeyBinding): { conflict: number } => {
    if (!currentProject.value) return { conflict: -1 };
    // A pinned key is refused here by its own check in the settings pane.
    const mappings = currentProject.value.cartSlotKeys ?? {};
    for (const [slotStr, existing] of Object.entries(mappings)) {
      const existingSlot = parseInt(slotStr, 10);
      if (existingSlot !== slotIndex && bindingsMatch(existing, binding)) {
        return { conflict: existingSlot };
      }
    }
    currentProject.value.cartSlotKeys = { ...mappings, [slotIndex]: binding };
    persistCartKeys({ cartSlotKeys: currentProject.value.cartSlotKeys as any });
    return { conflict: -1 };
  };

  const clearSlotKey = (slotIndex: number) => {
    const project = currentProject.value;
    if (!project?.cartSlotKeys) return;
    const { [slotIndex]: _gone, ...rest } = project.cartSlotKeys as Record<number, CartSlotKeyBinding>;
    project.cartSlotKeys = rest;
    persistCartKeys({ cartSlotKeys: rest as any });
  };

  // Who already has this key, for a message that names them (F-48).
  type KeyHolder = { kind: 'action'; action: PlaybackKeyAction } | { kind: 'slot'; position: number } | { kind: 'pin'; slot: number };
  const keyHolder = (binding: CartSlotKeyBinding, except?: { pinSlot?: number }): KeyHolder | null => {
    for (const [action, b] of Object.entries(playbackMappings.value) as [PlaybackKeyAction, CartSlotKeyBinding | null][]) {
      if (b && bindingsMatch(b, binding)) return { kind: 'action', action };
    }
    for (const [pos, b] of Object.entries(keyMappings.value)) {
      if (b && bindingsMatch(b, binding)) return { kind: 'slot', position: parseInt(pos, 10) };
    }
    for (const [slot, b] of Object.entries(pinnedKeys.value)) {
      const s = parseInt(slot, 10);
      if (s !== except?.pinSlot && b && bindingsMatch(b, binding)) return { kind: 'pin', slot: s };
    }
    return null;
  };

  const updatePlaybackBinding = (
    action: PlaybackKeyAction,
    binding: CartSlotKeyBinding | null
  ): { conflictSlot: number; conflictAction: PlaybackKeyAction | null; conflictPinned?: boolean } => {
    // No `if (!currentProject.value) return` guard any more: a transport
    // keymap is this operator's, so it can be edited with no show file open.
    // Conflict handling is unchanged where it can run at all — the cart-slot
    // half simply has nothing to check against when there is no cart wall,
    // and the playback-action half below never depended on a project.
    if (binding !== null) {
      // Check conflict with cart slots
      const cartMappings = currentProject.value?.cartSlotKeys ?? DEFAULT_CART_SLOT_KEYS;
      for (const [slotStr, existing] of Object.entries(cartMappings)) {
        if (bindingsMatch(existing, binding)) return { conflictSlot: parseInt(slotStr, 10), conflictAction: null };
      }
      for (const pinned of Object.values(pinnedKeys.value)) {
        if (bindingsMatch(pinned, binding)) return { conflictSlot: -1, conflictAction: null, conflictPinned: true };
      }
      // Check conflict with other playback actions
      const pbMappings = playbackMappings.value;
      for (const [existingAction, existingBinding] of Object.entries(pbMappings) as [PlaybackKeyAction, CartSlotKeyBinding | null][]) {
        if (existingAction !== action && existingBinding && bindingsMatch(existingBinding, binding)) {
          return { conflictSlot: -1, conflictAction: existingAction };
        }
      }
    }

    setPlaybackKeys({ ...(userPlaybackKeys.value ?? {}), [action]: binding });
    return { conflictSlot: -1, conflictAction: null };
  };

  /** Pin a key to the cart in `slot`, or clear it with null. Returns who already has the key, if anyone. */
  const setPinnedKey = (slot: number, binding: CartSlotKeyBinding | null): KeyHolder | null => {
    const project = currentProject.value;
    if (!project) return null;
    const pins = { ...(project.cartPinnedKeys ?? {}) };
    if (binding) {
      const holder = keyHolder(binding, { pinSlot: slot });
      if (holder) return holder;
      pins[String(slot)] = binding;
    } else {
      delete pins[String(slot)];
    }
    project.cartPinnedKeys = pins;
    persistCartKeys({ cartPinnedKeys: pins });
    return null;
  };

  /**
   * A cart moved from one slot to another: its pinned key goes with it.
   * `pin` is the key as it was before the move started, since the server
   * drops it from the old slot as soon as that slot is cleared.
   */
  const movePinnedKey = (from: number, to: number, pin = currentProject.value?.cartPinnedKeys?.[String(from)]) => {
    const project = currentProject.value;
    if (!project || !pin || from === to) return;
    const pins = { ...project.cartPinnedKeys };
    delete pins[String(from)];
    pins[String(to)] = pin;
    project.cartPinnedKeys = pins;
    persistCartKeys({ cartPinnedKeys: pins });
  };

  /**
   * After the cart bindings changed (prev and next: slot → target): a pin
   * follows its cart to a new slot, and goes with a cart that was cleared.
   * Called once the server has the new bindings, which it checks pins against.
   */
  const reconcilePinnedKeys = (prev: Map<number, string>, next: Map<number, string>,
                               before: Record<string, CartSlotKeyBinding> = currentProject.value?.cartPinnedKeys ?? {}) => {
    const project = currentProject.value;
    if (!project || Object.keys(before).length === 0) return;
    // Built afresh: pins whose cart stayed put first, then the moved ones at
    // their new slots. Moving them one by one over a copy lost a pin whenever
    // a cart was pushed into a run of pinned carts: the slot it moved to still
    // held the next cart's pin, not yet moved itself (R2-16).
    const pins: Record<string, CartSlotKeyBinding> = {};
    const moved: Array<[number, CartSlotKeyBinding]> = [];
    const claimed = new Set<number>();
    let changed = false;
    for (const [slotStr, pin] of Object.entries(before)) {
      const slot = parseInt(slotStr, 10);
      const was = prev.get(slot);
      if (!was || next.get(slot) === was) { pins[slotStr] = pin; continue; }
      changed = true;
      // Moved: the same target turned up in a slot that didn't have it.
      for (const [s, target] of next) {
        if (target === was && prev.get(s) !== was && !claimed.has(s)) {
          claimed.add(s);
          moved.push([s, pin]);
          break;
        }
      }
    }
    if (!changed) return;
    for (const [s, pin] of moved) pins[String(s)] = pin;
    project.cartPinnedKeys = pins;
    persistCartKeys({ cartPinnedKeys: pins });
  };

  const isKeyPinned = (binding: CartSlotKeyBinding) =>
    Object.values(pinnedKeys.value).some(b => bindingsMatch(b, binding));

  const resetToDefaults = () => {
    if (!currentProject.value) return;
    currentProject.value.cartSlotKeys = {};
    currentProject.value.cartPinnedKeys = {};
    persistCartKeys({ cartSlotKeys: {}, cartPinnedKeys: {} });
  };

  // No currentProject guard any more: the keymap is this operator's, and they
  // are entitled to reset it with no show file open.
  const resetPlaybackToDefaults = () => {
    setPlaybackKeys({});
  };

  let isMounted = false;

  const mount = () => {
    if (isMounted) return;
    window.addEventListener('keydown', handleKeydown);
    isMounted = true;
  };

  const unmount = () => {
    if (!isMounted) return;
    window.removeEventListener('keydown', handleKeydown);
    isMounted = false;
  };

  return {
    keyMappings,
    pinnedKeys,
    setPinnedKey,
    movePinnedKey,
    reconcilePinnedKeys,
    clearSlotKey,
    keyHolder,
    cartLabel,
    isKeyPinned,
    playbackMappings,
    triggerSlot,
    mount,
    unmount,
    updateBinding,
    updatePlaybackBinding,
    resetToDefaults,
    resetPlaybackToDefaults,
    findSlotForEvent,
  };
};
