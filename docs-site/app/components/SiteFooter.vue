<template>
  <footer class="foot">
    <WaveBand class="foot__wave" :seed="23" :bars="96" />
    <div class="container foot__grid">
      <div>
        <NuxtLink to="/" class="brand">
          <img :src="asset('assets/logo.svg')" alt="" width="38" height="29" />
          <span>LivePlay</span>
        </NuxtLink>
        <p class="foot__line">
          {{ t('footer.developedBy') }}
          <a href="https://github.com/tdoukinitsas" target="_blank" rel="noopener noreferrer">{{ t('footer.developerName') }}</a>
        </p>
        <p class="foot__line">
          LivePlay {{ t('footer.license') }}
          <a href="https://www.gnu.org/licenses/agpl-3.0.en.html" target="_blank" rel="noopener noreferrer">AGPL-3.0</a>
        </p>
      </div>

      <nav class="foot__links" :aria-label="t('site.footerNav')">
        <NuxtLink :to="{ path: '/', hash: '#features' }">{{ t('site.features') }}</NuxtLink>
        <NuxtLink to="/whats-new/">{{ t('site.whatsNew') }}</NuxtLink>
        <NuxtLink :to="{ path: '/', hash: '#download' }">{{ t('site.download') }}</NuxtLink>
        <a :href="asset('api/')">{{ t('api.referenceLabel') }}</a>
        <a href="https://github.com/tdoukinitsas/liveplay" target="_blank" rel="noopener noreferrer">{{ t('footer.viewOnGitHub') }}</a>
      </nav>

      <div v-if="contributors.length">
        <h2 class="foot__title">{{ t('footer.contributors') }}</h2>
        <ul class="foot__people">
          <li v-for="person in contributors" :key="person.name">
            <a :href="person.link" target="_blank" rel="noopener noreferrer">{{ person.name }}</a>
          </li>
        </ul>
      </div>
    </div>
  </footer>
</template>

<script setup lang="ts">
import { useI18n } from '../composables/useI18n';
import { useSiteInfo } from '../composables/useSiteInfo';

const { t } = useI18n();
const { asset, contributors } = useSiteInfo();
</script>
