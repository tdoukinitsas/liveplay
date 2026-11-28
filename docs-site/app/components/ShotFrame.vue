<template>
  <button
    type="button"
    class="shot"
    :style="ratio ? { '--ratio': ratio } : undefined"
    :aria-label="t('site.enlarge', { name: alt })"
    @click="open(src, alt)"
  >
    <img :src="src" :alt="alt" :loading="eager ? 'eager' : 'lazy'" :fetchpriority="eager ? 'high' : undefined" decoding="async" />
    <span class="shot__zoom material-symbols-rounded" aria-hidden="true" data-icon="open_in_full"></span>
  </button>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from '../composables/useI18n';
import { useLightbox } from '../composables/useLightbox';

const props = defineProps<{ src: string; alt: string; eager?: boolean }>();

// Screenshots are 1920x1032 unless listed here.
const otherRatios: Record<string, string> = {
  mixer: '1500 / 1032',
  event_presets: '1120 / 1032',
};
const ratio = computed(() => {
  const name = props.src.match(/liveplay_screenshot_(\w+)\.jpg$/)?.[1];
  return name ? otherRatios[name] : undefined;
});

const { t } = useI18n();
const { open } = useLightbox();
</script>
