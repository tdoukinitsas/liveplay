<template>
  <div class="cart-player" ref="cartPlayerRef">
    <div class="cart-header">
      <h2>{{ t('cart.title') }}</h2>
      <div class="cart-header-actions">
        <Btn
          v-if="!isDetachedWindow"
          icon="open_in_new"
          :text="t('cart.detach')"
          :disabled="!currentProject"
          @click="handleDetach"
        />
        <Btn
          v-else
          icon="picture_in_picture_alt"
          :text="t('cart.attach')"
          @click="handleAttach"
        />
      </div>
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
  </div>
</template>

<script setup lang="ts">
import type { AudioItem } from '~/types/project';
import { formatKeyLabel } from '~/composables/useCartHotkeys';
import Btn from './Btn.vue';

defineProps<{
  isDetachedWindow?: boolean;
}>();

const { currentProject } = useProject();
const { getCartItem } = useCartItems();
const { keyMappings, mount: mountHotkeys, unmount: unmountHotkeys } = useCartHotkeys();
const { mount: mountMidi, unmount: unmountMidi } = useMidiController();
const { t } = useLocalization();

const handleDetach = () => {
  if (!currentProject.value || !import.meta.client || !window.electronAPI) return;
  window.electronAPI.openCartPlayerWindow(currentProject.value.folderPath);
};

const handleAttach = () => {
  if (!import.meta.client || !window.electronAPI) return;
  window.electronAPI.attachCartPlayerWindow();
};

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
    mountMidi();
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
      unmountMidi();
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
  min-height: 68px;
  box-sizing: border-box;
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
}

.cart-header h2 {
  font-size: 18px;
  font-weight: 600;
}

.cart-header-actions {
  display: flex;
  gap: var(--spacing-sm);
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
