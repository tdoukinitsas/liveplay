<template>
  <div>
    <section class="hero">
      <div class="container hero__copy">
        <p class="eyebrow">{{ t('home.hero.eyebrow') }}</p>
        <h1 class="display display--xl">{{ t('home.hero.title') }}</h1>
        <p class="hero__lead">{{ t('header.intro') }}</p>
        <div class="hero__cta">
          <NuxtLink :to="{ path: '/', hash: '#download' }" class="btn btn--red">
            <span class="material-symbols-rounded" aria-hidden="true" data-icon="download"></span>
            {{ t('home.hero.download') }}
          </NuxtLink>
          <NuxtLink to="/whats-new/2-6/" class="btn btn--go">
            {{ t('home.hero.seeNew', { version: shortVersion }) }}
          </NuxtLink>
        </div>
      </div>
      <div class="hero__stage">
        <WaveBand class="hero__wave" live />
        <div class="container">
          <ShotFrame class="shot--hero" :src="shot('')" :alt="t('header.screenshotAlt')" eager />
        </div>
      </div>
    </section>

    <section class="audience" :aria-label="t('home.audience.label')">
      <div class="container audience__grid">
        <div v-for="item in audience" :key="item.key" v-reveal class="audience__item">
          <span class="material-symbols-rounded" aria-hidden="true" :data-icon="item.icon"></span>
          <h2>{{ t(`home.audience.${item.key}.title`) }}</h2>
          <p>{{ t(`home.audience.${item.key}.text`) }}</p>
        </div>
      </div>
    </section>

    <section class="how">
      <div class="container">
        <h2 v-reveal class="display display--lg">{{ t('home.how.title') }}</h2>
        <ol class="how__steps">
          <li v-for="(step, i) in ['one', 'two', 'three']" :key="step" v-reveal>
            <span class="how__num" dir="ltr">{{ i + 1 }}</span>
            <h3>{{ t(`home.how.${step}.title`) }}</h3>
            <p>{{ t(`home.how.${step}.text`) }}</p>
          </li>
        </ol>
      </div>
    </section>

    <div id="features">
      <section v-for="chapter in chapters" :id="chapter.id" :key="chapter.id" class="chapter" :class="`tone-${chapter.tone}`">
        <header class="chapter__head">
          <WaveBand class="chapter__wave" :seed="chapter.seed" :bars="88" />
          <div class="container">
            <h2 class="display display--xl">{{ t(`features.${chapter.group}.title`) }}</h2>
            <p class="chapter__intro">{{ t(`features.${chapter.group}.intro`) }}</p>
          </div>
        </header>
        <div class="container chapter__body">
          <FeatureRow :feature="chapter.lead" lead />
          <FeatureRow
            v-for="(feature, i) in chapter.rows"
            :key="feature.key"
            :feature="feature"
            :flip="i % 2 === 0"
          />
          <div v-if="chapter.tiles.length" class="tiles" :class="{ 'tiles--pairs': chapter.tiles.length % 3 !== 0 }">
            <article v-for="tile in chapter.tiles" :key="tile.key" v-reveal class="tile">
              <ShotFrame :src="shot(tile.shot)" :alt="`LivePlay: ${t(`features.${tile.key}.title`)}`" />
              <NuxtLink v-if="tile.isNew" to="/whats-new/" class="new-badge">{{ t('site.newIn', { version: shortVersion }) }}</NuxtLink>
              <h3>{{ t(`features.${tile.key}.title`) }}</h3>
              <p>{{ t(`features.${tile.key}.description`) }}</p>
            </article>
          </div>
        </div>
      </section>
    </div>

    <section class="numbers" :aria-label="t('home.numbers.label')">
      <div class="container numbers__grid">
        <div v-for="n in numbers" :key="n.key" v-reveal>
          <b dir="ltr">{{ n.value }}</b>
          <span>{{ t(`home.numbers.${n.key}`) }}</span>
        </div>
      </div>
    </section>

    <section class="teaser tone-go">
      <div class="container teaser__grid">
        <div>
          <p class="eyebrow">{{ t('home.teaser.eyebrow') }}</p>
          <h2 class="display display--lg">{{ t('home.teaser.title', { version: shortVersion }) }}</h2>
          <p class="lede">{{ t('home.teaser.text') }}</p>
          <NuxtLink to="/whats-new/2-6/" class="btn btn--ink">{{ t('home.teaser.button') }}</NuxtLink>
        </div>
        <ul class="teaser__list">
          <li v-for="id in ['events', 'presets', 'tabs', 'pages', 'automation', 'relink']" :key="id">
            {{ t(`whatsNew.v26.sections.${id}.kicker`) }}
          </li>
        </ul>
      </div>
    </section>

    <DownloadPanel />

    <section id="api" class="tech">
      <div class="container tech__grid">
        <div>
          <p class="eyebrow">{{ t('home.tech.eyebrow') }}</p>
          <h2 class="display display--md">{{ t('api.title') }}</h2>
          <p class="lede">{{ t('api.description') }}</p>
          <a :href="asset('api/')" class="btn btn--ghost">{{ t('api.referenceLabel') }}</a>
        </div>
        <div class="ports">
          <h3>{{ t('ports.title') }}</h3>
          <p>{{ t('ports.subtitle') }}</p>
          <dl>
            <div>
              <dt dir="auto">{{ t('ports.control.port') }}</dt>
              <dd><strong>{{ t('ports.control.title') }}</strong> {{ t('ports.control.description') }}</dd>
            </div>
            <div>
              <dt dir="auto">{{ t('ports.discovery.port') }}</dt>
              <dd><strong>{{ t('ports.discovery.title') }}</strong> {{ t('ports.discovery.description') }}</dd>
            </div>
          </dl>
          <p class="ports__note">{{ t('ports.note') }}</p>
        </div>
      </div>
    </section>

    <!-- English only, and it mirrors the FAQ structured data in usePageSeo. -->
    <section v-if="locale === 'en'" id="faq" class="faq">
      <div class="container faq__grid">
        <h2 class="display display--lg">Questions, answered</h2>
        <div>
          <details v-for="(item, i) in faq" :key="item.q" :open="i === 0">
            <summary><h3>{{ item.q }}</h3></summary>
            <p>{{ item.a }}</p>
          </details>
        </div>
      </div>
    </section>

    <section class="open">
      <div class="container">
        <h2 class="display display--lg">{{ t('contribute.subtitle') }}</h2>
        <div class="open__grid">
          <div v-for="k in ['free', 'license', 'contribute']" :key="k">
            <h3>{{ t(`contribute.${k}Title`) }}</h3>
            <p>{{ t(`contribute.${k}Text`) }}</p>
          </div>
        </div>
        <a href="https://github.com/tdoukinitsas/liveplay" target="_blank" rel="noopener noreferrer" class="btn btn--red">
          {{ t('contribute.githubButton') }}
        </a>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { useI18n } from '../composables/useI18n';
import { usePageSeo } from '../composables/usePageSeo';
import { useSiteInfo } from '../composables/useSiteInfo';
import { chapters } from '../data/tour';
import seo from '../data/seo.json';

const { t, locale } = useI18n();
const { asset, shot, shortVersion } = useSiteInfo();
const faq = seo.faq;

const audience = [
  { key: 'theatre', icon: 'theater_comedy' },
  { key: 'radio', icon: 'radio' },
  { key: 'events', icon: 'stadia_controller' },
  { key: 'parties', icon: 'celebration' },
];

const numbers = [
  { key: 'carts', value: '64' },
  { key: 'eq', value: '32' },
  { key: 'languages', value: '21' },
  { key: 'platforms', value: '3' },
  { key: 'cost', value: '0' },
];

usePageSeo('home');
</script>
