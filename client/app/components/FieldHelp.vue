<template>
  <!-- An option's description, behind an ⓘ beside its label. This is how the
       Properties panel explains a field: it is an inspector used mid-show and
       short on room, so descriptions don't sit there as paragraphs (Settings,
       where there is room, shows them under each setting instead). Warnings
       and errors are not this: those always show inline.

       Opens on hover, on keyboard focus and on a tap or click (which pins it
       until the next click outside); Escape closes it and still goes on to
       Stop All, which a description must never swallow. -->
  <span class="field-help">
    <button
      ref="btn"
      type="button"
      class="field-help-btn"
      :class="{ open }"
      :aria-label="t('common.moreInfo')"
      :aria-describedby="open ? bubbleId : undefined"
      :aria-expanded="open"
      @pointerenter="onEnter"
      @pointerleave="onLeave"
      @focus="show"
      @blur="onBlur"
      @click.stop.prevent="onClick"
      @keydown.esc="hide"
    >
      <span class="material-symbols-rounded" aria-hidden="true">info</span>
    </button>
    <!-- On the body, so a scrolling panel can't clip it. Styled by the
         unscoped block below for the same reason. -->
    <Teleport to="body">
      <div
        v-if="open"
        :id="bubbleId"
        ref="bubble"
        role="tooltip"
        class="field-help-bubble"
        :style="bubbleStyle"
      >{{ text }}</div>
    </Teleport>
  </span>
</template>

<script setup lang="ts">
defineProps<{ text: string }>();
const { t } = useLocalization();

let seq = 0;
const bubbleId = `field-help-${Math.random().toString(36).slice(2, 8)}-${++seq}`;

const btn = ref<HTMLButtonElement | null>(null);
const bubble = ref<HTMLDivElement | null>(null);
const open = ref(false);
const pinned = ref(false);
const pos = ref({ left: 0, top: 0 });
const bubbleStyle = computed(() => ({ left: `${pos.value.left}px`, top: `${pos.value.top}px` }));

const GAP = 6;
const MARGIN = 8;

// Above the icon when it fits, so it doesn't cover the field it describes
// (labels sit over their fields); below it otherwise. Never off the window.
function place() {
  const b = btn.value?.getBoundingClientRect();
  const el = bubble.value;
  if (!b || !el) return;
  const w = el.offsetWidth;
  const h = el.offsetHeight;
  const vw = window.innerWidth;
  const vh = window.innerHeight;
  let left = b.left + b.width / 2 - w / 2;
  left = Math.max(MARGIN, Math.min(left, vw - w - MARGIN));
  let top = b.top - GAP - h;
  if (top < MARGIN) top = Math.min(b.bottom + GAP, vh - h - MARGIN);
  pos.value = { left, top };
}

function show() {
  if (open.value) return;
  open.value = true;
  void nextTick(place);
}
function hide() {
  open.value = false;
  pinned.value = false;
}

// A mouse hovering shows it; a touch "hover" is the start of a tap, which
// onClick handles, so it is left alone here.
function onEnter(e: PointerEvent) { if (e.pointerType === 'mouse') show(); }
function onLeave(e: PointerEvent) { if (e.pointerType === 'mouse' && !pinned.value) hide(); }
function onBlur() { if (!pinned.value) hide(); }
function onClick() {
  if (pinned.value) { hide(); return; }
  pinned.value = true;
  show();
}

// A pinned bubble closes on the next press anywhere else, and on scroll or
// resize, where it would otherwise float away from its icon.
function onOutside(e: PointerEvent) {
  if (btn.value?.contains(e.target as Node)) return;
  hide();
}
watch(open, (isOpen) => {
  if (!import.meta.client) return;
  if (isOpen) {
    document.addEventListener('pointerdown', onOutside, true);
    window.addEventListener('scroll', hide, true);
    window.addEventListener('resize', hide);
  } else {
    document.removeEventListener('pointerdown', onOutside, true);
    window.removeEventListener('scroll', hide, true);
    window.removeEventListener('resize', hide);
  }
});
onBeforeUnmount(() => { if (open.value) hide(); });
</script>

<style scoped>
.field-help {
  display: inline-flex;
  align-items: center;
  vertical-align: middle;
}

.field-help-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 18px;
  height: 18px;
  padding: 0;
  border: none;
  border-radius: 50%;
  background: none;
  color: var(--color-text-secondary);
  cursor: help;
  opacity: 0.75;

  .material-symbols-rounded {
    font-size: 15px;
  }

  &:hover,
  &:focus-visible,
  &.open {
    opacity: 1;
    color: var(--color-accent);
  }

  &:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: 1px;
  }
}
</style>

<style>
.field-help-bubble {
  position: fixed;
  z-index: 3000;
  max-width: 280px;
  padding: 8px 10px;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-surface);
  color: var(--color-text-primary);
  box-shadow: 0 6px 18px rgba(0, 0, 0, 0.35);
  font-size: 12px;
  font-weight: 400;
  line-height: 1.45;
  text-transform: none;
  letter-spacing: normal;
  white-space: normal;
  pointer-events: none;
}
</style>
