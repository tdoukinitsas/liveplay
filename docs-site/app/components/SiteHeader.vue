<template>
  <header class="nav">
    <a class="skip" href="#main">{{ t('site.skip') }}</a>
    <div class="nav__inner">
      <NuxtLink to="/" class="brand" @click="menuOpen = false">
        <img :src="asset('assets/logo.svg')" :alt="t('header.logoAlt')" width="38" height="29" />
        <span>LivePlay</span>
      </NuxtLink>

      <nav id="site-menu" class="nav__links" :class="{ 'is-open': menuOpen }" :aria-label="t('site.menu')">
        <NuxtLink :to="{ path: '/', hash: '#features' }" @click="menuOpen = false">{{ t('site.features') }}</NuxtLink>
        <NuxtLink :to="{ path: '/', hash: '#download' }" @click="menuOpen = false">{{ t('site.download') }}</NuxtLink>
        <a :href="asset('api/')">{{ t('site.api') }}</a>
        <a href="https://github.com/tdoukinitsas/liveplay" target="_blank" rel="noopener noreferrer">GitHub</a>
      </nav>

      <div class="nav__actions">
        <NuxtLink to="/whats-new/" class="nav__new" @click="menuOpen = false">
          <span>{{ t('site.whatsNew') }}</span>
          <b dir="ltr">{{ shortVersion }}</b>
        </NuxtLink>
        <LanguageSwitcher />
        <button
          type="button"
          class="nav__toggle"
          :aria-expanded="menuOpen"
          aria-controls="site-menu"
          :aria-label="menuOpen ? t('site.close') : t('site.menu')"
          @click="menuOpen = !menuOpen"
        >
          <span class="material-symbols-rounded" aria-hidden="true" :data-icon="menuOpen ? 'close' : 'menu'"></span>
        </button>
      </div>
    </div>
  </header>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useI18n } from '../composables/useI18n';
import { useSiteInfo } from '../composables/useSiteInfo';

const { t } = useI18n();
const { asset, shortVersion } = useSiteInfo();
const menuOpen = ref(false);
</script>
