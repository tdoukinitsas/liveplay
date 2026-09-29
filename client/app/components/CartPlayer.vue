<template>
  <div class="cart-player" ref="cartPlayerRef" :class="{ 'show-mode': showMode }">
    <div class="cart-header">
      <h2>{{ t('cart.title') }}</h2>
      <!-- Window controls only: the cart's pads are its content. The same
           component every pane uses, so the moves are the same everywhere. -->
      <PaneControls pane="cart" :in-window="isDetachedWindow" :divided="false" />
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
import PaneControls from './PaneControls.vue';

const props = defineProps<{
  isDetachedWindow?: boolean;
}>();

const { currentProject, requestDeleteFromKeyboard } = useProject();
const { getCartItem } = useCartItems();
const { keyMappings, mount: mountHotkeys, unmount: unmountHotkeys } = useCartHotkeys();
const { mount: mountMidi, unmount: unmountMidi } = useMidiController();
const { t } = useLocalization();
const { uiMode } = useUiMode();
const showMode = computed(() => uiMode.value === 'playback');

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

// In the detached cart window there's no MainWorkspace to own the global
// DEL key, so handle it here. (In the attached layout MainWorkspace already
// does — adding it there too would double-fire.)
const isTextInputFocused = (): boolean => {
  const el = document.activeElement as HTMLElement | null;
  if (!el) return false;
  const tag = el.tagName.toLowerCase();
  return tag === 'input' || tag === 'textarea' || el.isContentEditable;
};
const handleCartKeydown = (e: KeyboardEvent) => {
  if (e.key !== 'Delete' && e.key !== 'Backspace') return;
  if (isTextInputFocused() || !currentProject.value) return;
  if (requestDeleteFromKeyboard()) e.preventDefault();
};

onMounted(() => {
  if (import.meta.client) {
    // Only in the detached window, which has no MainWorkspace to own them.
    // In the attached layout the workspace mounts these, so the transport keys
    // and MIDI survive this pane being closed, collapsed or popped out.
    if (props.isDetachedWindow) {
      mountHotkeys();
      mountMidi();
    }
    // Initial setup
    updateGridColumns();
    if (props.isDetachedWindow) window.addEventListener('keydown', handleCartKeydown);

    // Watch for resize
    const resizeObserver = new ResizeObserver(() => {
      updateGridColumns();
    });

    if (cartPlayerRef.value) {
      resizeObserver.observe(cartPlayerRef.value);
    }

    onUnmounted(() => {
      if (props.isDetachedWindow) {
        unmountHotkeys();
        unmountMidi();
        window.removeEventListener('keydown', handleCartKeydown);
      }
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
  gap: var(--spacing-sm);
  padding: 0 var(--spacing-lg);
  /* One fixed height for all three pane headers, so they line up side by
     side whatever buttons each carries (or hides in Show Mode). */
  height: 60px;
  flex: 0 0 auto;
  box-sizing: border-box;
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
}

.cart-header h2 {
  font-size: 18px;
  font-weight: 600;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  min-width: 0;
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

/* Show Mode — taller cart tiles so the enlarged play/stop controls and
   3-line names have room to breathe on a touch screen. */
.cart-player.show-mode .cart-grid {
  grid-auto-rows: minmax(150px, 1fr);
}
</style>
