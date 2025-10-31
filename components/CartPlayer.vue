<template>
  <div class="cart-player">
    <div class="cart-header">
      <h2>Cart Player</h2>
    </div>
    
    <div class="cart-grid">
      <CartSlot
        v-for="slot in 16"
        :key="slot"
        :slot="slot - 1"
        :item="getCartItem(slot - 1)"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import type { AudioItem } from '~/types/project';

const { currentProject } = useProject();

const getCartItem = (slot: number): AudioItem | null => {
  if (!currentProject.value) return null;
  
  const cartItem = currentProject.value.cartItems.find(ci => ci.slot === slot);
  if (!cartItem) return null;
  
  const { findItemByUuid } = useProject();
  const item = findItemByUuid(cartItem.itemUuid);
  
  // Only return audio items for cart slots
  return item && item.type === 'audio' ? item as AudioItem : null;
};
</script>

<style scoped>
.cart-player {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  background-color: var(--color-background);
}

.cart-header {
  padding: var(--spacing-md) var(--spacing-lg);
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
}

.cart-header h2 {
  font-size: 18px;
  font-weight: 600;
}

.cart-grid {
  flex: 1;
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  grid-auto-rows: minmax(100px, 1fr);
  gap: var(--spacing-sm);
  padding: var(--spacing-md);
  overflow-y: auto;
  align-content: start;
}
</style>
