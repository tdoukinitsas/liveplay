<template>
  <ClientOnly>
  <Teleport to="body">
    <div
      v-if="current"
      class="lightbox"
      role="dialog"
      aria-modal="true"
      :aria-label="current.alt"
      @click.self="close"
    >
      <button ref="closeButton" type="button" class="lightbox__close" :aria-label="t('site.close')" @click="close">
        <span class="material-symbols-rounded" aria-hidden="true" data-icon="close"></span>
      </button>
      <img :src="current.src" :alt="current.alt" />
    </div>
  </Teleport>
  </ClientOnly>
</template>

<script setup lang="ts">
import { nextTick, onMounted, onUnmounted, ref, watch } from 'vue';
import { useI18n } from '../composables/useI18n';
import { useLightbox } from '../composables/useLightbox';

const { t } = useI18n();
const { current, close } = useLightbox();
const closeButton = ref<HTMLButtonElement | null>(null);
let returnFocus: HTMLElement | null = null;

watch(current, async (value) => {
  document.documentElement.style.overflow = value ? 'hidden' : '';
  if (value) {
    returnFocus = document.activeElement as HTMLElement | null;
    await nextTick();
    closeButton.value?.focus();
  } else {
    returnFocus?.focus();
    returnFocus = null;
  }
});

const onKey = (event: KeyboardEvent) => {
  if (event.key === 'Escape' && current.value) close();
};
onMounted(() => document.addEventListener('keydown', onKey));
onUnmounted(() => document.removeEventListener('keydown', onKey));
</script>
