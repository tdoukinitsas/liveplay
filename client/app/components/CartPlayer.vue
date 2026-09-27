<template>
  <div class="cart-player" ref="cartPlayerRef" :class="{ 'show-mode': showMode }">
    <div class="cart-header">
      <h2>{{ t('cart.title') }}</h2>
      <!-- The three views asked for, in the order the mixer's own action bar
           uses: undock, then expand/dock. Neither is shown in the detached
           window — there is no docked pane to resize there, so the only move
           that means anything is coming home.

           NO CLOSE BUTTON, deliberately, even though `cartClosed` exists and
           the mixer has one. The mixer's Close is safe because the header has a
           Mixer toggle to bring it back; the cart has no such toggle, so a
           Close here would leave a thin splitter handle as the only way back
           and read as having lost the cart wall. Closing stays what it already
           was — dragging the separator off the edge — and that is unchanged. -->
      <div class="cart-header-actions">
        <template v-if="!isDetachedWindow">
          <Btn
            icon="open_in_new"
            :text="t('cart.detach')"
            :disabled="!currentProject"
            @click="handleDetach"
          />
          <!-- One button, two directions, like the mixer's: the state under the
               pointer decides which. A pair of buttons where only one can ever
               apply is the thing this replaces. -->
          <Btn
            :icon="cartFullscreen ? 'picture_in_picture_alt' : 'open_in_full'"
            :text="cartFullscreen ? t('cart.dock') : t('cart.expand')"
            @click="toggleFullscreen"
          />
        </template>
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

const handleDetach = () => {
  if (!currentProject.value || !import.meta.client || !window.electronAPI) return;
  window.electronAPI.openCartPlayerWindow(currentProject.value.folderPath);
};

const handleAttach = () => {
  if (!import.meta.client || !window.electronAPI) return;
  window.electronAPI.attachCartPlayerWindow();
};

// The docked view, shared with MainWorkspace, which is what actually renders
// from these. Asked for 2026-09-27: the cart should get the mixer's treatment —
// dock to the side, fill the workspace, or go to its own window. The flags and
// the splitter's snap logic were all already here; only a way to say so from
// the panel was missing.
//
// Deliberately NOT persisted, unlike the mixer's mode: the mixer's view became
// a machine-store setting because it has a Settings pane that has to report it.
// Nothing asks the cart's view what it is between launches, and cart WIDTH is
// not persisted either — both belong to P4's layout work, together, rather than
// half of it arriving here.
const cartFullscreen = useState<boolean>('liveplay:cartFullscreen', () => false);
const cartClosed = useState<boolean>('liveplay:cartClosed', () => false);

function toggleFullscreen() {
  cartFullscreen.value = !cartFullscreen.value;
  // Filling the workspace and being closed are mutually exclusive — the
  // splitter's own snap handlers already clear each when setting the other, and
  // the same has to hold from here. Unreachable from this button today (a closed
  // cart is not rendered, so it has no header to press), and kept because the
  // invariant belongs with the write rather than with who happens to call it.
  cartClosed.value = false;
}

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

/* Show Mode — taller cart tiles so the enlarged play/stop controls and
   3-line names have room to breathe on a touch screen. */
.cart-player.show-mode .cart-grid {
  grid-auto-rows: minmax(150px, 1fr);
}
</style>
