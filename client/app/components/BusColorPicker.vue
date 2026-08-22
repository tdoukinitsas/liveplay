<template>
  <!--
    The bus colour chip, and the swatch popover it opens. One component so the
    rail strip's 6px dot, the channel view's header chip and the ⋮ menu's
    "Colour" row all pick from the same PRESET_COLORS grid with the same
    behaviour — picking patches the bus straight away (there is no drag
    gesture to debounce, unlike gain or pan) and the server's buses_patched
    broadcast is what every window renders from.
  -->
  <span class="bcp" @click.stop>
    <button
      class="bcp__chip"
      :class="{ 'bcp__chip--lg': size === 'lg' }"
      :style="{ background: color || 'var(--color-accent)' }"
      :title="t('mixer.busColor')"
      @click.stop="toggle"
      @mousedown.stop
    ></button>
    <BusPopover :open="open" :x="pos.x" :y="pos.y" @close="open = false">
      <div class="bcp__grid">
        <button
          v-for="c in PRESET_COLORS"
          :key="c"
          class="bcp__swatch"
          :class="{ 'bcp__swatch--active': color === c }"
          :style="{ background: c }"
          :title="c"
          @click="pick(c)"
        ></button>
      </div>
    </BusPopover>
  </span>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { PRESET_COLORS } from '~/types/project';
import BusPopover from './BusPopover.vue';

withDefaults(defineProps<{
  color: string;
  /** 'sm' is the strip's scribble-strip dot; 'lg' the channel view's header chip. */
  size?: 'sm' | 'lg';
}>(), { size: 'sm' });

const emit = defineEmits<{ (e: 'pick', color: string): void }>();
const { t } = useLocalization();

const open = ref(false);
const pos  = ref({ x: 0, y: 0 });

function toggle(e: MouseEvent) {
  if (open.value) { open.value = false; return; }
  // Anchor under the chip rather than at the pointer, so the grid lines up
  // with the thing you clicked.
  const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
  pos.value = { x: r.left, y: r.bottom + 4 };
  open.value = true;
}
function pick(c: string) {
  emit('pick', c);
  open.value = false;
}
</script>

<style scoped>
.bcp { display: inline-flex; flex: 0 0 auto; position: relative; }
.bcp__chip {
  width: 6px;
  height: 6px;
  padding: 0;
  border: none;
  border-radius: 50%;
  cursor: pointer;
  flex: 0 0 auto;
}
.bcp__chip--lg { width: 10px; height: 10px; }
.bcp__grid {
  display: grid;
  grid-template-columns: repeat(8, 1fr);
  gap: 4px;
}
.bcp__swatch {
  width: 16px;
  height: 16px;
  padding: 0;
  border-radius: var(--border-radius-sm);
  border: 2px solid transparent;
  cursor: pointer;
}
.bcp__swatch:hover { transform: scale(1.1); }
.bcp__swatch--active {
  border-color: var(--color-text-primary);
  box-shadow: 0 0 0 2px var(--color-background);
}
</style>
