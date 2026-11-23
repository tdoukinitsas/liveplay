<template>
  <div class="credits">
    <h2>{{ t('footer.contributors') }}</h2>
    <ul class="closing__people">
      <li v-for="person in people" :key="person.link">
        <a :href="person.link" target="_blank" rel="noopener noreferrer">{{ person.name }}</a>
      </li>
    </ul>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from '../composables/useI18n';
import { useSiteInfo } from '../composables/useSiteInfo';

const { t } = useI18n();
const { contributors } = useSiteInfo();

// Everyone who has contributed to LivePlay, on every What's New page.
const people = computed(() => [
  { name: t('footer.developerName'), link: 'https://github.com/tdoukinitsas' },
  ...contributors.value,
]);
</script>
