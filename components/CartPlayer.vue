<template>
  <div class="cart-player" ref="cartPlayerRef">
    <div class="cart-header">
      <h2>Cart Player</h2>
      <button class="hotkey-config-btn" @click="showHotkeyConfig = true" :title="t('cart.configureHotkeys')">
        <span class="config-icon">⌨</span>
      </button>
    </div>
    
    <div class="cart-grid" :class="gridClass">
      <CartSlot
        v-for="slot in 16"
        :key="slot"
        :slot="slot - 1"
        :item="getCartItem(slot - 1)"
        :keyLabel="getKeyLabel(slot - 1)"
      />
    </div>

    <CartHotkeyConfig
      v-if="showHotkeyConfig"
      @close="showHotkeyConfig = false"
    />
  </div>
</template>

<script setup lang="ts">
import type { AudioItem } from '~/types/project';
import { formatKeyLabel } from '~/composables/useCartHotkeys';

const { currentProject } = useProject();
const { getCartItem } = useCartItems();
const { keyMappings, mount: mountHotkeys, unmount: unmountHotkeys } = useCartHotkeys();
const { t } = useLocalization();

const showHotkeyConfig = ref(false);
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

const getKeyLabel = (slotIndex: number): string => {
  const binding = keyMappings.value[slotIndex];
  return binding ? formatKeyLabel(binding) : '';
};

onMounted(() => {
  if (import.meta.client) {
    mountHotkeys();
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
      unmountHotkeys();
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
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-md) var(--spacing-lg);
  min-height: 56px;
  box-sizing: border-box;
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
}

.cart-header h2 {
  font-size: 18px;
  font-weight: 600;
}

.hotkey-config-btn {
  background: none;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  padding: 4px 8px;
  cursor: pointer;
  color: var(--color-text-secondary);
  font-size: 16px;
  display: flex;
  align-items: center;
  transition: background-color 0.15s, color 0.15s;
}

.hotkey-config-btn:hover {
  background-color: var(--color-hover);
  color: var(--color-text);
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
