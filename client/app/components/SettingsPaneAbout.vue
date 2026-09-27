<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionAbout') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionAboutHelp') }}</p>

    <div class="about-header">
      <img
        :src="isDark ? './assets/icons/SVG/liveplay-icon-darkmode@web.svg'
                     : './assets/icons/SVG/liveplay-icon-lightmode@web.svg'"
        alt="LivePlay"
        class="about-logo"
      />
      <div class="about-text">
        <h4 class="about-title">
          LivePlay
          <span class="about-version">v{{ appVersion }}</span>
        </h4>
        <p class="about-subtitle">{{ t('welcome.subtitle') }}</p>
      </div>
    </div>

    <section class="about-credits">
      <p class="about-line">
        <strong>{{ t('about.developedBy') }}:</strong> {{ t('about.developerName') }}
      </p>
      <!-- Each locale names its own translator, so this line is absent in the
           ones that have not been credited rather than showing an empty label. -->
      <p v-if="t('translationContributor.name')" class="about-line">
        <strong>{{ t('translationContributor.title') }}:</strong>
        <a
          :href="formatContributorLink(t('translationContributor.contributeLink'))"
          class="about-link"
          @click.prevent="openContributorLink(t('translationContributor.contributeLink'))"
        >{{ t('translationContributor.name') }}</a>
      </p>
      <p v-if="contributors.length" class="about-line">
        <strong>{{ t('about.contributors') }}:</strong>
        <template v-for="(contributor, index) in contributors" :key="contributor.name">
          <a
            :href="contributor.link"
            class="about-link"
            @click.prevent="openExternal(contributor.link)"
          >{{ contributor.name }}</a><template v-if="index < contributors.length - 1">, </template>
        </template>
      </p>
    </section>

    <section class="about-links">
      <a
        :href="REPO_URL"
        class="about-info-link"
        @click.prevent="openExternal(REPO_URL)"
      >
        <span class="material-symbols-rounded">code</span>
        <span>{{ t('about.githubRepo') }}</span>
      </a>
      <a
        :href="LICENSE_URL"
        class="about-info-link"
        @click.prevent="openExternal(LICENSE_URL)"
      >
        <span class="material-symbols-rounded">description</span>
        <span>{{ t('about.license') }}</span>
      </a>
    </section>
  </div>
</template>

<script setup lang="ts">
// Folded in from AboutModal.vue, which this retires. Same content; what goes is
// the overlay, the close button and its own Escape handler, all of which the
// Settings page already provides.
import contributorsJson from '~~/assets/json/contributors.json';

const REPO_URL = 'https://github.com/tdoukinitsas/liveplay';
const LICENSE_URL = 'https://www.gnu.org/licenses/agpl-3.0.en.html';

const { t } = useLocalization();
// The modal read the `theme` useState mirror app.vue maintains. A pane reads
// the preference itself (U4) — one fewer thing depending on that mirror.
const { theme } = usePreferences();
const isDark = computed(() => theme.value.mode === 'dark');

const contributors = computed(() =>
  Object.values(contributorsJson.contributors) as { name: string; link: string }[]
);

// Falls back to the last version that shipped without this call answering,
// which is what a browser-only session (no Electron) sees.
const appVersion = ref('1.1.3');
onMounted(async () => {
  if (import.meta.client && window.electronAPI?.getAppVersion) {
    appVersion.value = await window.electronAPI.getAppVersion();
  }
});

const openExternal = (url: string) => {
  if (import.meta.client && window.electronAPI?.openExternal) {
    window.electronAPI.openExternal(url);
  }
};

// A translator may be credited with a website, an email address or a phone
// number, so the href has to be worked out from the shape of the value.
const formatContributorLink = (link: string): string => {
  if (!link) return '#';
  if (link.includes('@') && !link.startsWith('mailto:')) return `mailto:${link}`;
  if (link.match(/^[\+\d\s\-\(\)]+$/)) return `tel:${link.replace(/\s/g, '')}`;
  if (!link.startsWith('http://') && !link.startsWith('https://') &&
      !link.startsWith('mailto:') && !link.startsWith('tel:')) {
    return `https://${link}`;
  }
  return link;
};

// mailto: and tel: want the OS handler, which navigating to them gets. A web
// address goes through Electron so it opens in a browser rather than replacing
// the app's own window with it.
const openContributorLink = (link: string) => {
  const formatted = formatContributorLink(link);
  if (formatted.startsWith('mailto:') || formatted.startsWith('tel:')) {
    window.location.href = formatted;
  } else {
    openExternal(formatted);
  }
};
</script>

<style scoped>
.about-header {
  display: flex;
  align-items: center;
  gap: var(--spacing-lg, 16px);
  padding-bottom: 18px;
  border-bottom: 1px solid var(--color-border);
}

.about-logo {
  width: 64px;
  height: 64px;
  object-fit: contain;
  flex-shrink: 0;
}

.about-text {
  flex: 1;
  min-width: 0;
}

.about-title {
  font-size: 30px;
  font-weight: 600;
  margin: 0 0 4px;
  color: var(--color-text-primary);
  letter-spacing: -1px;
  line-height: 1;
  display: flex;
  align-items: baseline;
  gap: var(--spacing-sm, 8px);
  flex-wrap: wrap;
}

.about-version {
  font-size: 14px;
  font-weight: 400;
  color: var(--color-text-secondary);
  letter-spacing: 0;
  font-variant-numeric: tabular-nums;
}

.about-subtitle {
  font-size: 15px;
  color: var(--color-text-secondary);
  margin: 0;
  line-height: 1.4;
}

.about-credits {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-sm, 8px);
}

.about-line {
  margin: 0;
  color: var(--color-text-primary);
  font-size: 14px;
}
.about-line strong {
  font-weight: 600;
}

.about-link {
  color: var(--color-accent);
  text-decoration: none;
}
.about-link:hover {
  text-decoration: underline;
}

.about-links {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs, 4px);
}

.about-info-link {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm, 8px);
  padding: var(--spacing-sm, 8px) var(--spacing-md, 12px);
  background-color: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md, 6px);
  color: var(--color-text-primary);
  text-decoration: none;
  font-size: 14px;
  transition: background-color var(--transition-fast, 0.15s), border-color 0.15s, color 0.15s;
}

.about-info-link:hover {
  background-color: var(--color-surface-hover);
  border-color: var(--color-accent);
  color: var(--color-accent);
}

.about-info-link .material-symbols-rounded {
  font-size: 20px;
}
</style>
