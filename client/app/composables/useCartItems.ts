import type { AudioItem, GroupItem } from '~/types/project';

export const useCartItems = () => {
  // Store cart-only items separately (not in the playlist). Held via
  // useState so the shared map is SSR-safe and — critically — its
  // initializer doesn't run at module-top-level, which combined with
  // the bidirectional auto-import between useCartItems and useProject
  // could trip Vite's ESM-cycle TDZ ("Cannot access '_$__useProject'
  // before initialization").
  const cartOnlyItems = useState<Map<string, AudioItem>>(
    'useCartItems.cartOnlyItems',
    () => new Map(),
  );
  const { currentProject } = useProject();

  // Add an item that exists ONLY in the cart (not in playlist)
  const addCartOnlyItem = (item: AudioItem) => {
    cartOnlyItems.value.set(item.uuid, item);
  };

  // Get cart item - checks both cart-only items and project items. A slot may
  // also hold a playlist GROUP, bound by reference rather than cloned (a copy
  // would duplicate every cue inside it) — discussion #61.
  const getCartItem = (slot: number): AudioItem | GroupItem | null => {
    if (!currentProject.value) return null;

    const cartItem = currentProject.value.cartItems.find(ci => ci.slot === slot);
    if (!cartItem) return null;

    // First check cart-only items
    const cartOnlyItem = cartOnlyItems.value.get(cartItem.itemUuid);
    if (cartOnlyItem) {
      return cartOnlyItem;
    }

    // Fallback to project items (for items dragged from playlist)
    const { findItemByUuid } = useProject();
    const item = findItemByUuid(cartItem.itemUuid);
    if (!item) return null;
    return item.type === 'group' ? item as GroupItem : item as AudioItem;
  };

  // Update a cart-only item
  const updateCartOnlyItem = (uuid: string, item: AudioItem) => {
    if (cartOnlyItems.value.has(uuid)) {
      cartOnlyItems.value.set(uuid, item);
    }
  };

  // Get a cart-only item by UUID
  const getCartOnlyItem = (uuid: string): AudioItem | null => {
    return cartOnlyItems.value.get(uuid) || null;
  };

  // Remove a cart-only item
  const removeCartOnlyItem = (uuid: string) => {
    cartOnlyItems.value.delete(uuid);
  };

  // Clear all cart-only items
  const clearCartOnlyItems = () => {
    cartOnlyItems.value.clear();
  };

  return {
    cartOnlyItems,
    addCartOnlyItem,
    getCartItem,
    updateCartOnlyItem,
    getCartOnlyItem,
    removeCartOnlyItem,
    clearCartOnlyItems
  };
};
