<template>
  <div class="site" :dir="direction">
    <SiteHeader />
    <main id="main">
      <NuxtPage />
    </main>
    <SiteFooter />
    <SiteLightbox />
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, watch } from 'vue';
import { useI18n } from './composables/useI18n';
import { useSiteInfo } from './composables/useSiteInfo';
import { useDownloads } from './composables/useDownloads';

const { direction, initLocale, locale } = useI18n();
const { load } = useSiteInfo();
const { detect } = useDownloads();

// Lets headings pick line heights that suit the script, and screen readers the
// right pronunciation.
useHead({
  htmlAttrs: {
    lang: computed(() => locale.value),
    dir: computed(() => direction.value),
  },
});

onMounted(async () => {
  detect();
  try {
    await initLocale();
  } finally {
    // Set by the inline script in nuxt.config.ts, so a translated visit doesn't
    // flash the prerendered English first.
    document.documentElement.classList.remove('i18n-pending');
  }
  await load();
});

watch(locale, () => {
  if (import.meta.client) document.documentElement.lang = locale.value;
});
</script>
