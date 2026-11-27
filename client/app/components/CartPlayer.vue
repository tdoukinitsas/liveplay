<template>
  <div class="cart-player" ref="cartPlayerRef" :class="{ 'show-mode': showMode }">
    <div class="cart-header">
      <div class="cart-title">
        <h2>{{ t('cart.title') }}</h2>
        <TabStrip v-if="cartPages.length <= 1" kind="cart" />
      </div>
      <!-- Window controls only: the cart's pads are its content. The same
           component every pane uses, so the moves are the same everywhere.
           Not in a cart page's own window: that is not the cart pane, and
           "Dock to main window" would close the detached cart window instead
           (as PlaylistView leaves them out of a playlist tab's window). -->
      <PaneControls v-if="!isTabWindow" pane="cart" :in-window="isDetachedWindow" :divided="false" />
    </div>

    <TabStrip v-if="cartPages.length > 1" kind="cart" />

    <div class="cart-grid" :style="{ gridTemplateColumns: `repeat(${columns}, 1fr)` }">
      <CartSlot
        v-for="position in activeCartPage.size"
        :key="activeCartPage.slotBase + position - 1"
        :slot="activeCartPage.slotBase + position - 1"
        :position="position - 1"
        :max-slot="activeCartPage.slotBase + activeCartPage.size - 1"
        :item="getCartItem(activeCartPage.slotBase + position - 1)"
        :keyLabel="keyLabelFor(activeCartPage.slotBase + position - 1, position - 1)"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import type { AudioItem } from '~/types/project';
import { formatKeyLabel } from '~/composables/useCartHotkeys';
import { shouldIgnoreHotkey } from '~/utils/hotkeyGuard';
import PaneControls from './PaneControls.vue';
import TabStrip from './TabStrip.vue';

const props = defineProps<{
  isDetachedWindow?: boolean;
}>();

// A cart page popped out on its own (#74) loads as ?cartWindow=1&cartPage=<id>;
// the detached cart pane is ?cartWindow=1 alone.
const isTabWindow = import.meta.client
  && new URLSearchParams(window.location.search).has('cartPage');

const { currentProject, requestDeleteFromKeyboard } = useProject();
const { getCartItem } = useCartItems();
const { keyMappings, pinnedKeys, mount: mountHotkeys, unmount: unmountHotkeys } = useCartHotkeys();
const { cartPages, activeCartPage } = useShowTabs();
const { mount: mountMidi, unmount: unmountMidi } = useMidiController();
const { t } = useLocalization();
const { uiMode } = useUiMode();
const showMode = computed(() => uiMode.value === 'playback');

const cartPlayerRef = ref<HTMLElement | null>(null);
const columns = ref(2);

// Watch for resize and adjust grid columns
const updateGridColumns = () => {
  if (!cartPlayerRef.value) return;

  const width = cartPlayerRef.value.offsetWidth;
  const size = activeCartPage.value.size;
  const byWidth = width < 800 ? 2 : width < 1100 ? 3 : 4;
  // Never wider than a square grid of this many carts. Pages over 16 can use a
  // wide window's extra room, so 64 carts isn't a very long scroll.
  const square = Math.ceil(Math.sqrt(size));
  columns.value = size <= 16
    ? Math.min(byWidth, square)
    : Math.min(square, Math.max(byWidth, Math.floor(width / 170)));
};
watch(() => activeCartPage.value.size, updateGridColumns);

// A cart's own pinned key if it has one, else its position's key on this page.
const keyLabelFor = (slot: number, position: number): string => {
  const uuid = currentProject.value?.cartItems.find(c => c.slot === slot)?.itemUuid;
  const binding = (uuid && pinnedKeys.value[uuid]) || keyMappings.value[position];
  return binding ? formatKeyLabel(binding) : '';
};

// In the detached cart window there's no MainWorkspace to own the global
// DEL key, so handle it here. (In the attached layout MainWorkspace already
// does — adding it there too would double-fire.) Same rule as there for
// which keys belong to the control with focus.
const handleCartKeydown = (e: KeyboardEvent) => {
  if (e.key !== 'Delete' && e.key !== 'Backspace') return;
  if (shouldIgnoreHotkey(e) || !currentProject.value) return;
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
}

.cart-title {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  min-width: 0;
}

/* Show Mode — taller cart tiles so the enlarged play/stop controls and
   3-line names have room to breathe on a touch screen. */
.cart-player.show-mode .cart-grid {
  grid-auto-rows: minmax(150px, 1fr);
}
</style>
