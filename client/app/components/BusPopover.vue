<template>
  <!--
    A small floating panel anchored to a screen point: the strip's colour
    swatches and the ⋮ menu both render through this.

    Why it is not simply `position: absolute` inside the strip: a strip is
    96px wide with overflow hidden, so anything that opens out of it would be
    clipped to a sliver. It is not plain `position: fixed` either, because the
    mixer is a size container (MixerPanel.vue's `container: mixer`) and an
    element with layout containment becomes the containing block for fixed
    descendants — so fixed coordinates would be relative to the mixer, not the
    window, and differently so in the docked pane and the detached window.
    Rather than reason about which ancestor wins, the panel is placed at 0,0,
    measured, and shifted by the difference between where it landed and where
    it was asked to be. That is correct whatever the containing block is.
  -->
  <div
    v-if="open"
    ref="el"
    class="bpop"
    :style="{ left: pos.left + 'px', top: pos.top + 'px', visibility: placed ? 'visible' : 'hidden' }"
    @click.stop
    @mousedown.stop
    @contextmenu.prevent.stop
  >
    <slot />
  </div>
</template>

<script setup lang="ts">
import { nextTick, onBeforeUnmount, ref, watch } from 'vue';

const props = defineProps<{
  open: boolean;
  /** Where the top-left corner should land, in viewport (client) pixels. */
  x: number;
  y: number;
}>();
const emit = defineEmits<{ (e: 'close'): void }>();

const el     = ref<HTMLElement | null>(null);
const pos    = ref({ left: 0, top: 0 });
const placed = ref(false);

async function place() {
  placed.value = false;
  pos.value = { left: 0, top: 0 };
  await nextTick();
  const node = el.value;
  if (!node) return;
  // Where (0,0) actually landed tells us what the containing block is.
  const origin = node.getBoundingClientRect();
  const w = origin.width, h = origin.height;
  // Keep the whole panel on screen; flip up/left rather than spilling off
  // the bottom or right edge, which is where a strip near the end of the
  // rail would otherwise open to.
  const vw = window.innerWidth, vh = window.innerHeight;
  let cx = props.x, cy = props.y;
  if (cx + w > vw - 4) cx = Math.max(4, vw - 4 - w);
  if (cy + h > vh - 4) cy = Math.max(4, props.y - h);
  pos.value = { left: cx - origin.left, top: cy - origin.top };
  placed.value = true;
}

// Mousedown rather than click, and captured: the press that opens a
// different strip's menu, or lands on a fader, must close this one on the
// way down rather than leaving two open. Escape closes too.
function onDocDown(e: MouseEvent) {
  if (!props.open) return;
  if (el.value && el.value.contains(e.target as Node)) return;
  emit('close');
}
function onKey(e: KeyboardEvent) {
  if (props.open && e.key === 'Escape') emit('close');
}
function bind() {
  window.addEventListener('mousedown', onDocDown, true);
  window.addEventListener('keydown', onKey, true);
  window.addEventListener('resize', onResize);
}
function unbind() {
  window.removeEventListener('mousedown', onDocDown, true);
  window.removeEventListener('keydown', onKey, true);
  window.removeEventListener('resize', onResize);
}
function onResize() { if (props.open) emit('close'); }

watch(() => [props.open, props.x, props.y] as const, ([open]) => {
  if (typeof window === 'undefined') return;
  if (open) { bind(); void place(); } else { unbind(); }
}, { immediate: true });

onBeforeUnmount(() => { if (typeof window !== 'undefined') unbind(); });
</script>

<style scoped>
.bpop {
  position: fixed;
  z-index: 50;
  min-width: 120px;
  padding: var(--spacing-xs);
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
  cursor: default;
}
</style>
