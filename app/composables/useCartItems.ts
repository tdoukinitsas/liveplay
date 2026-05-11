import type { AudioItem } from '~/types/project';

// Store cart-only items separately (not in the playlist)
const cartOnlyItems = ref<Map<string, AudioItem>>(new Map());

export const useCartItems = () => {
  const { currentProject } = useProject();

  // Add an item that exists ONLY in the cart (not in playlist)
  const addCartOnlyItem = (item: AudioItem) => {
    cartOnlyItems.value.set(item.uuid, item);
  };

  // Get cart item - checks both cart-only items and project items
  const getCartItem = (slot: number): AudioItem | null => {
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
    return item && item.type === 'audio' ? item as AudioItem : null;
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
