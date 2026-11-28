<template>
  <div v-if="release">
    <nav class="release-nav" :aria-label="t('whatsNew.release.versions')">
      <div class="container">
        <NuxtLink to="/whats-new/">{{ t('site.whatsNew') }}</NuxtLink>
        <NuxtLink
          v-for="r in releases"
          :key="r.slug"
          :to="`/whats-new/${r.slug}/`"
          :aria-current="r.slug === release.slug ? 'page' : undefined"
          dir="ltr"
        >{{ r.version }}</NuxtLink>
      </div>
    </nav>

    <section class="release-hero" :class="`tone-${release.tone}`">
      <WaveBand class="release-hero__wave" :seed="release.version.length + Number(release.version.slice(2))" live />
      <div class="container">
        <p class="eyebrow">{{ t('whatsNew.release.label', { version: release.version }) }}</p>
        <h1 class="display display--xl">{{ t(`whatsNew.${release.key}.title`) }}</h1>
        <p class="hero__lead">{{ t(`whatsNew.${release.key}.lead`) }}</p>
        <div class="hero__cta">
          <NuxtLink :to="{ path: '/', hash: '#download' }" class="btn btn--red">
            <span class="material-symbols-rounded" aria-hidden="true" data-icon="download"></span>
            {{ t('home.hero.download') }}
          </NuxtLink>
        </div>
        <ul class="release-jump">
          <li v-for="s in release.sections" :key="s.id">
            <a :href="`#${s.id}`" @click.prevent="jump(s.id)">{{ t(`${root}.sections.${s.id}.kicker`) }}</a>
          </li>
        </ul>
      </div>
    </section>

    <section
      v-for="s in release.sections"
      :id="s.id"
      :key="s.id"
      class="rel"
      :class="[`rel--${s.layout}`, `tone-${s.tone}`, { 'rel--companion': s.companion }]"
    >
      <div v-reveal class="container rel__grid">
        <div class="rel__copy">
          <p class="kicker">{{ t(`${root}.sections.${s.id}.kicker`) }}</p>
          <h2 class="display display--lg">{{ t(`${root}.sections.${s.id}.title`) }}</h2>
          <p class="lede">{{ t(`${root}.sections.${s.id}.text`) }}</p>
          <ul class="ticks">
            <li v-for="point in tl(`${root}.sections.${s.id}.points`)" :key="point">{{ point }}</li>
          </ul>
          <CompanionLinks v-if="s.companion" />
        </div>
        <CompanionPad v-if="s.companion" />
        <ShotFrame v-else :src="shot(s.shot)" :alt="t(`${root}.sections.${s.id}.title`)" />
      </div>
    </section>

    <section class="more">
      <div class="container">
        <h2 class="display display--lg">{{ t('whatsNew.release.more') }}</h2>
        <div class="more__grid">
          <article v-for="m in release.more" :key="m.id" v-reveal>
            <span class="material-symbols-rounded" aria-hidden="true" :data-icon="m.icon"></span>
            <h3>{{ t(`${root}.more.${m.id}.title`) }}</h3>
            <p>{{ t(`${root}.more.${m.id}.text`) }}</p>
          </article>
        </div>
      </div>
    </section>

    <section class="closing">
      <div class="container">
        <ReleaseCredits />
        <p class="fixes"><strong>{{ t('whatsNew.release.fixed') }}</strong> {{ t(`${root}.fixes`) }}</p>
        <p class="closing__links">
          <a :href="`https://github.com/tdoukinitsas/liveplay/releases/tag/${release.tag}`" target="_blank" rel="noopener noreferrer">{{ t('whatsNew.release.github') }}</a>
          <NuxtLink to="/whats-new/">{{ t('whatsNew.index.older') }}</NuxtLink>
        </p>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from '../../composables/useI18n';
import { usePageSeo, type PageKey } from '../../composables/usePageSeo';
import { useSiteInfo } from '../../composables/useSiteInfo';
import { releases } from '../../data/releases';

const route = useRoute();
const { t, tl } = useI18n();
const { shot } = useSiteInfo();

const release = computed(() => releases.find(r => r.slug === route.params.version));
const root = computed(() => `whatsNew.${release.value?.key}`);

if (!release.value) navigateTo('/whats-new/', { replace: true });

const jump = (id: string) => {
  const top = (document.getElementById(id)?.getBoundingClientRect().top ?? 0) + window.scrollY - 118;
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  window.scrollTo({ top, behavior: reduced ? 'auto' : 'smooth' });
};

usePageSeo(() => (release.value?.slug ?? 'whatsNew') as PageKey);
</script>
