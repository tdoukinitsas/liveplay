<template>
  <!--
    The ⋮ menu for one bus — opened from the button in a strip's header row,
    from a right-click anywhere on the strip, and from the channel view's
    header. Everything here is also reachable elsewhere (double-click renames,
    the chip recolours, the EDIT button opens the channel view); the menu is
    the place where a newcomer can find all of it at once, and the only place
    the roles move from.

    Roles (D24): a project has exactly one Master and one Preview bus. "Set as
    Master bus" on a bus is how the role moves — it is hidden on the current
    holder, and on the other role's holder, since a bus never holds both.
    Delete is shown but disabled on a role holder, with the reason in its
    tooltip: move the role first. That matches the server, which 409s the
    same request.
  -->
  <BusPopover :open="open" :x="x" :y="y" @close="$emit('close')">
    <ul class="bmenu" role="menu">
      <li><button class="bmenu__item" @click="act('rename')">{{ t('mixer.menuRename') }}</button></li>
      <li>
        <button class="bmenu__item" @click="colorsOpen = !colorsOpen">
          <span>{{ t('mixer.menuColor') }}</span>
          <span class="material-symbols-rounded bmenu__chev">{{ colorsOpen ? 'expand_more' : 'chevron_right' }}</span>
        </button>
        <div v-if="colorsOpen" class="bmenu__colors">
          <button
            v-for="c in PRESET_COLORS"
            :key="c"
            class="bmenu__swatch"
            :class="{ 'bmenu__swatch--active': bus.color === c }"
            :style="{ background: c }"
            :title="c"
            @click="emit('color', c); emit('close')"
          ></button>
        </div>
      </li>
      <li><button class="bmenu__item" @click="act('open')">{{ t('mixer.menuChannelSettings') }}</button></li>
      <li v-if="!bus.master && !bus.preview" class="bmenu__sep"></li>
      <li v-if="!bus.master && !bus.preview">
        <button class="bmenu__item" :title="t('mixer.setMasterHint')" @click="act('set-master')">
          {{ t('mixer.setMaster') }}
        </button>
      </li>
      <li v-if="!bus.master && !bus.preview">
        <button class="bmenu__item" :title="t('mixer.setPreviewHint')" @click="act('set-preview')">
          {{ t('mixer.setPreview') }}
        </button>
      </li>
      <li class="bmenu__sep"></li>
      <li>
        <button
          class="bmenu__item bmenu__item--danger"
          :disabled="bus.master || bus.preview"
          :title="bus.master || bus.preview ? t('mixer.roleDeleteBlocked', { role: roleName }) : t('mixer.deleteBus')"
          @click="act('delete')"
        >{{ t('mixer.menuDelete') }}</button>
      </li>
    </ul>
  </BusPopover>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import type { Bus } from '~/types/project';
import { PRESET_COLORS } from '~/types/project';
import BusPopover from './BusPopover.vue';

const props = defineProps<{
  bus: Bus;
  open: boolean;
  x: number;
  y: number;
}>();

const emit = defineEmits<{
  (e: 'close'): void;
  (e: 'rename'): void;
  (e: 'color', color: string): void;
  (e: 'open'): void;
  (e: 'set-master'): void;
  (e: 'set-preview'): void;
  (e: 'delete'): void;
}>();

const { t } = useLocalization();

const colorsOpen = ref(false);
watch(() => props.open, () => { colorsOpen.value = false; });

const roleName = computed(() =>
  props.bus.master ? t('mixer.roleMaster') : t('mixer.rolePreview'));

function act(what: 'rename' | 'open' | 'set-master' | 'set-preview' | 'delete') {
  emit('close');
  switch (what) {
    case 'rename':      emit('rename'); break;
    case 'open':        emit('open'); break;
    case 'set-master':  emit('set-master'); break;
    case 'set-preview': emit('set-preview'); break;
    case 'delete':      emit('delete'); break;
  }
}
</script>

<style scoped>
.bmenu {
  list-style: none;
  margin: 0;
  padding: 0;
  min-width: 160px;
  font-size: 12px;
}
.bmenu__item {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacing-sm);
  width: 100%;
  padding: 5px 8px;
  text-align: left;
  color: var(--color-text-primary);
  background: none;
  border: none;
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.bmenu__item:hover:not(:disabled) { background: var(--color-background); }
.bmenu__item:disabled { color: var(--color-text-disabled); cursor: not-allowed; }
.bmenu__item--danger:not(:disabled) { color: var(--color-danger); }
.bmenu__chev { font-size: 14px; color: var(--color-text-secondary); }
.bmenu__sep { height: 1px; margin: 3px 0; background: var(--color-border); }
.bmenu__colors {
  display: grid;
  grid-template-columns: repeat(8, 1fr);
  gap: 4px;
  padding: 4px 8px 6px;
}
.bmenu__swatch {
  width: 16px;
  height: 16px;
  padding: 0;
  border-radius: var(--border-radius-sm);
  border: 2px solid transparent;
  cursor: pointer;
}
.bmenu__swatch:hover { transform: scale(1.1); }
.bmenu__swatch--active {
  border-color: var(--color-text-primary);
  box-shadow: 0 0 0 2px var(--color-background);
}
</style>
