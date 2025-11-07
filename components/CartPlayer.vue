<template>
  <div class="cart-player" ref="cartPlayerRef">
    <div class="cart-header">
      <h2>Cart Player</h2>
    </div>
    
    <div class="cart-grid" :class="gridClass">
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
const { getCartItem } = useCartItems();

const cartPlayerRef = ref<HTMLElement | null>(null);
const gridClass = ref('grid-cols-2');

// Watch for resize and adjust grid columns
const updateGridColumns = () => {
  if (!cartPlayerRef.value) return;
  
  const width = cartPlayerRef.value.offsetWidth;
  
  // Adjust grid columns based on width
  if (width < 500) {
    gridClass.value = 'grid-cols-2';
  } else if (width < 800) {
    gridClass.value = 'grid-cols-2';
  } else if (width < 1100) {
    gridClass.value = 'grid-cols-3';
  } else {
    gridClass.value = 'grid-cols-4';
  }
};

onMounted(() => {
  if (import.meta.client) {
    // Initial setup
    updateGridColumns();
    
    // Watch for resize
    const resizeObserver = new ResizeObserver(() => {
      updateGridColumns();
    });
    
    if (cartPlayerRef.value) {
      resizeObserver.observe(cartPlayerRef.value);
    }
    
    onUnmounted(() => {
      resizeObserver.disconnect();
    });
  }
});
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
  grid-auto-rows: minmax(100px, 1fr);
  gap: var(--spacing-sm);
  padding: var(--spacing-md);
  overflow-y: auto;
  align-content: start;
  
  &.grid-cols-2 {
    grid-template-columns: repeat(2, 1fr);
  }
  
  &.grid-cols-3 {
    grid-template-columns: repeat(3, 1fr);
  }
  
  &.grid-cols-4 {
    grid-template-columns: repeat(4, 1fr);
  }
}
</style>
