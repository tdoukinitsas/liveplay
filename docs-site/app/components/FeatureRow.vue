<template>
  <article v-reveal class="row" :class="{ 'row--flip': flip, 'row--lead': lead }">
    <div class="row__copy">
      <NuxtLink v-if="feature.isNew" to="/whats-new/" class="new-badge">{{ t('site.newIn', { version: shortVersion }) }}</NuxtLink>
      <h3 class="display display--md">{{ t(`features.${feature.key}.title`) }}</h3>
      <p class="lede">{{ t(`features.${feature.key}.description`) }}</p>
      <ul v-if="feature.points" class="ticks">
        <li v-for="point in tl(`features.${feature.key}.points`)" :key="point">{{ point }}</li>
      </ul>
      <CompanionLinks v-if="feature.companion" />
    </div>
    <CompanionPad v-if="feature.companion" />
    <ShotFrame v-else :src="shot(feature.shot)" :alt="`LivePlay: ${t(`features.${feature.key}.title`)}`" />
  </article>
</template>

<script setup lang="ts">
import { useI18n } from '../composables/useI18n';
import { useSiteInfo } from '../composables/useSiteInfo';
import type { TourFeature } from '../data/tour';

defineProps<{ feature: TourFeature; flip?: boolean; lead?: boolean }>();

const { t, tl } = useI18n();
const { shot, shortVersion } = useSiteInfo();
</script>
