<template>
  <div class="companion-links">
    <a :href="MODULE_URL" target="_blank" rel="noopener noreferrer" class="btn companion-links__main">
      {{ t('companion.get') }}
    </a>
    <p class="companion-links__meta">
      <span>{{ credit[0] }}<a :href="SOURCE_URL" target="_blank" rel="noopener noreferrer">DI Aspinwall</a>{{ credit[1] }}</span>
      <a :href="COMPANION_URL" target="_blank" rel="noopener noreferrer">{{ t('companion.getCompanion') }}</a>
    </p>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from '../composables/useI18n';
import { COMPANION_URL, MODULE_URL, SOURCE_URL } from '../data/companion';

const { t } = useI18n();

// "Module by {name}": the name becomes a link to the module's own repository.
const credit = computed(() => {
  const [before, after = ''] = t('companion.credit').split('{name}');
  return [before, after];
});
</script>
