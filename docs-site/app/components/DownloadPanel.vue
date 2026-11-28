<template>
  <section id="download" class="download">
    <div class="container">
      <p class="eyebrow">{{ t('download.subtitle') }}</p>
      <h2 class="display display--lg">{{ t('download.title') }}</h2>
      <p v-if="detectedOS" class="download__hint">{{ t('download.recommended') }}</p>

      <div class="download__grid" :class="`count-${prominentKeys.length}`">
        <a
          v-for="key in prominentKeys"
          :key="key"
          :href="links[key]"
          class="platform"
          target="_blank"
          rel="noopener noreferrer"
        >
          <span class="platform__name" dir="auto">{{ t(`download.${key}.title`) }}</span>
          <span class="platform__req">{{ t(`download.${key}.description`) }}</span>
          <span class="platform__cta">
            <span class="material-symbols-rounded" aria-hidden="true" data-icon="download"></span>
            {{ t(`download.${key}.buttonText`) }}
            <small>{{ t('download.sizeMB', { n: platformSizes[key] }) }}</small>
          </span>
        </a>
      </div>

      <div v-if="detectedOS === 'mac'" class="note">
        <h3>{{ t('download.macGatekeeper.title') }}</h3>
        <p>{{ t('download.macGatekeeper.intro') }}</p>
        <code dir="ltr">{{ t('download.macGatekeeper.command') }}</code>
        <p>{{ t('download.macGatekeeper.outro') }}</p>
      </div>

      <div v-if="detectedOS === 'windows'" class="note">
        <h3>{{ t('download.winSmartScreen.title') }}</h3>
        <p>{{ t('download.winSmartScreen.intro') }}</p>
        <ol>
          <li>{{ t('download.winSmartScreen.step1') }}</li>
          <li>{{ t('download.winSmartScreen.step2') }}</li>
        </ol>
        <p>{{ t('download.winSmartScreen.outro') }}</p>
      </div>

      <div v-if="otherKeys.length" class="download__more">
        <button type="button" class="btn btn--ghost" :aria-expanded="showAll" @click="showAll = !showAll">
          {{ showAll ? t('download.hideOtherOS') : t('download.showOtherOS') }}
        </button>
        <div v-if="showAll" class="download__grid" :class="`count-${otherKeys.length}`">
          <a
            v-for="key in otherKeys"
            :key="key"
            :href="links[key]"
            class="platform"
            target="_blank"
            rel="noopener noreferrer"
          >
            <span class="platform__name" dir="auto">{{ t(`download.${key}.title`) }}</span>
            <span class="platform__req">{{ t(`download.${key}.description`) }}</span>
            <span class="platform__cta">
              <span class="material-symbols-rounded" aria-hidden="true" data-icon="download"></span>
              {{ t(`download.${key}.buttonText`) }}
              <small>{{ t('download.sizeMB', { n: platformSizes[key] }) }}</small>
            </span>
          </a>
        </div>
      </div>

      <p class="download__formats">
        <span>{{ t('download.otherFormats') }}</span>
        <a :href="links.deb" target="_blank" rel="noopener noreferrer">{{ t('download.debianUbuntu') }}</a>
        <a :href="links.rpm" target="_blank" rel="noopener noreferrer">{{ t('download.fedoraRHEL') }}</a>
        <a :href="links.macArmZip" target="_blank" rel="noopener noreferrer">{{ t('download.macArmZip') }}</a>
        <a :href="links.macIntelZip" target="_blank" rel="noopener noreferrer">{{ t('download.macIntelZip') }}</a>
        <a href="https://github.com/tdoukinitsas/liveplay/releases/latest" target="_blank" rel="noopener noreferrer">{{ t('download.viewReleases') }}</a>
      </p>
    </div>
  </section>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useI18n } from '../composables/useI18n';
import { platformSizes, useDownloads } from '../composables/useDownloads';

const { t } = useI18n();
const { detectedOS, prominentKeys, otherKeys, links } = useDownloads();
const showAll = ref(false);
</script>
