<template>
  <div>
    <section class="news-hero">
      <div class="container">
        <p class="eyebrow">{{ t('whatsNew.index.eyebrow') }}</p>
        <h1 class="display display--xl">{{ t('whatsNew.index.title') }}</h1>
        <p class="hero__lead">{{ t('whatsNew.index.lead') }}</p>
        <NuxtLink to="/" class="btn btn--ghost">{{ t('whatsNew.index.tour') }}</NuxtLink>
      </div>
    </section>

    <section class="container news-cards">
      <NuxtLink
        v-for="(release, i) in releases"
        :key="release.slug"
        :to="`/whats-new/${release.slug}/`"
        class="news-card"
        :class="`tone-${release.tone}`"
      >
        <div class="news-card__copy">
          <span v-if="i === 0" class="new-badge">{{ t('whatsNew.index.latest') }}</span>
          <b class="news-card__version" dir="ltr">{{ release.version }}</b>
          <h2 class="display display--md">{{ t(`whatsNew.${release.key}.title`) }}</h2>
          <p>{{ t(`whatsNew.${release.key}.summary`) }}</p>
          <span class="news-card__go">{{ t('whatsNew.index.read') }}</span>
        </div>
        <img :src="shot(release.shot)" alt="" loading="lazy" />
      </NuxtLink>
    </section>

    <section class="container history">
      <h2 class="display display--md">{{ t('whatsNew.index.older') }}</h2>
      <ol>
        <li v-for="entry in timeline" :key="entry.key">
          <b dir="ltr">{{ entry.version }}</b>
          <div>
            <h3>{{ t(`whatsNew.timeline.${entry.key}.title`) }}</h3>
            <p>{{ t(`whatsNew.timeline.${entry.key}.text`) }}</p>
            <p class="history__meta">
              <a :href="`https://github.com/tdoukinitsas/liveplay/releases/tag/${entry.tag}`" target="_blank" rel="noopener noreferrer">{{ t('whatsNew.release.github') }}</a>
            </p>
          </div>
        </li>
      </ol>
    </section>

    <section class="closing">
      <div class="container">
        <ReleaseCredits />
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { useI18n } from '../../composables/useI18n';
import { usePageSeo } from '../../composables/usePageSeo';
import { useSiteInfo } from '../../composables/useSiteInfo';
import { releases, timeline } from '../../data/releases';

const { t } = useI18n();
const { shot } = useSiteInfo();

usePageSeo('whatsNew');
</script>
