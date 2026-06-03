<template>
  <div class="docs-site" :dir="direction">
    <header class="site-header">
      <div class="container">
        <div class="header-content">
          <div class="logo-section">
            <img
              :src="asset('assets/logo.svg')"
              :alt="t('header.logoAlt')"
              class="logo"
              @error="handleImageError"
            />
            <div class="title-section">
              <h1>LivePlay</h1>
              <p class="version">{{ t('header.version') }} {{ version }}</p>
            </div>
          </div>
          <LanguageSwitcher />
        </div>
        <p class="tagline">{{ t('header.tagline') }}</p>
      </div>
    </header>

    <section class="screenshot-section">
      <div class="container">
        <div class="screenshot-wrapper">
          <img
            :src="asset('screenshots/liveplay_screenshot.jpg')"
            :alt="t('header.screenshotAlt')"
            class="app-screenshot"
            @error="handleImageError"
          />
        </div>
      </div>
    </section>

    <section class="download-section">
      <div class="container">
        <h2>{{ t('download.title') }}</h2>
        <p class="download-subtitle">{{ t('download.subtitle') }}</p>

        <p v-if="detectedOS" class="download-recommended">{{ t('download.recommended') }}</p>

        <div class="download-grid" :class="`count-${prominentKeys.length}`">
          <a
            v-for="key in prominentKeys"
            :key="key"
            :href="downloadLinks[key]"
            class="download-card"
            target="_blank"
            rel="noopener noreferrer"
          >
            <h3>{{ t(`download.${key}.title`) }}</h3>
            <p>{{ t(`download.${key}.description`) }}</p>
            <div class="download-button">
              <span>{{ t(`download.${key}.buttonText`) }}</span>
              <span class="file-size">{{ t(`download.${key}.size`) }}</span>
            </div>
          </a>
        </div>

        <div v-if="detectedOS === 'mac'" class="platform-note mac-gatekeeper-note">
          <h3>{{ t('download.macGatekeeper.title') }}</h3>
          <p>{{ t('download.macGatekeeper.intro') }}</p>
          <code class="gatekeeper-command">{{ t('download.macGatekeeper.command') }}</code>
          <p>{{ t('download.macGatekeeper.outro') }}</p>
        </div>

        <div v-if="detectedOS === 'windows'" class="platform-note win-smartscreen-note">
          <h3>{{ t('download.winSmartScreen.title') }}</h3>
          <p>{{ t('download.winSmartScreen.intro') }}</p>
          <ol class="smartscreen-steps">
            <li>{{ t('download.winSmartScreen.step1') }}</li>
            <li>{{ t('download.winSmartScreen.step2') }}</li>
          </ol>
          <p>{{ t('download.winSmartScreen.outro') }}</p>
        </div>

        <div v-if="otherKeys.length" class="other-os">
          <button
            type="button"
            class="other-os-toggle"
            @click="showAllPlatforms = !showAllPlatforms"
          >
            {{ showAllPlatforms ? t('download.hideOtherOS') : t('download.showOtherOS') }}
          </button>

          <div
            v-if="showAllPlatforms"
            class="download-grid"
            :class="`count-${otherKeys.length}`"
          >
            <a
              v-for="key in otherKeys"
              :key="key"
              :href="downloadLinks[key]"
              class="download-card"
              target="_blank"
              rel="noopener noreferrer"
            >
              <h3>{{ t(`download.${key}.title`) }}</h3>
              <p>{{ t(`download.${key}.description`) }}</p>
              <div class="download-button">
                <span>{{ t(`download.${key}.buttonText`) }}</span>
                <span class="file-size">{{ t(`download.${key}.size`) }}</span>
              </div>
            </a>
          </div>
        </div>

        <div class="other-formats">
          <p>{{ t('download.otherFormats') }}</p>
          <div class="format-links">
            <a :href="downloadLinks.deb" target="_blank" rel="noopener noreferrer">
              {{ t('download.debianUbuntu') }}
            </a>
            <a :href="downloadLinks.rpm" target="_blank" rel="noopener noreferrer">
              {{ t('download.fedoraRHEL') }}
            </a>
            <a :href="downloadLinks.macArmZip" target="_blank" rel="noopener noreferrer">
              {{ t('download.macArmZip') }}
            </a>
            <a :href="downloadLinks.macIntelZip" target="_blank" rel="noopener noreferrer">
              {{ t('download.macIntelZip') }}
            </a>
          </div>
        </div>

        <div class="github-link">
          <a 
            href="https://github.com/tdoukinitsas/liveplay/releases/latest" 
            target="_blank"
            rel="noopener noreferrer"
          >
            {{ t('download.viewReleases') }}
          </a>
        </div>
      </div>
    </section>

    <section class="features-section">
      <div class="container">
        <h2>{{ t('features.title') }}</h2>
        
        <FeatureHighlight
          :title="t('features.interface.title')"
          :image-src="asset('screenshots/liveplay_screenshot.jpg')"
        >
          <p>{{ t('features.interface.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.audioEngine.title')"
          :image-src="asset('screenshots/liveplay_screenshot_audio_engine.jpg')"
        >
          <p>{{ t('features.audioEngine.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.limiter.title')"
          :image-src="asset('screenshots/liveplay_screenshot_limiter.jpg')"
        >
          <p>{{ t('features.limiter.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.meters.title')"
          :image-src="asset('screenshots/liveplay_screenshot.jpg')"
        >
          <p>{{ t('features.meters.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.routing.title')"
          :image-src="asset('screenshots/liveplay_screenshot_routing.jpg')"
        >
          <p>{{ t('features.routing.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.previewLtc.title')"
          :image-src="asset('screenshots/liveplay_screenshot_preview.jpg')"
        >
          <p>{{ t('features.previewLtc.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.waveform.title')"
          :image-src="asset('screenshots/liveplay_screenshot_waveformtrimmer.jpg')"
        >
          <p>{{ t('features.waveform.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.youtube.title')"
          :image-src="asset('screenshots/liveplay_screenshot_youtube.jpg')"
        >
          <p>{{ t('features.youtube.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.properties.title')"
          :image-src="asset('screenshots/liveplay_screenshot_properties.jpg')"
        >
          <p>{{ t('features.properties.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.ducking.title')"
          :image-src="asset('screenshots/liveplay_screenshot_ducking.jpg')"
        >
          <p>{{ t('features.ducking.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.discovery.title')"
          :image-src="asset('screenshots/liveplay_screenshot_server_settings.jpg')"
        >
          <p>{{ t('features.discovery.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.remoteOperation.title')"
          :image-src="asset('screenshots/liveplay_screenshot_decoupled.jpg')"
        >
          <p>{{ t('features.remoteOperation.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.firstLaunch.title')"
          :image-src="asset('screenshots/liveplay_screenshot_nameproject.jpg')"
        >
          <p>{{ t('features.firstLaunch.description') }}</p>
        </FeatureHighlight>
      </div>
    </section>

    <section class="ports-section">
      <div class="container">
        <div class="ports-disclaimer">
          <h3 class="ports-title">{{ t('ports.title') }}</h3>
          <p class="ports-subtitle">{{ t('ports.subtitle') }}</p>
          <ul class="ports-list">
            <li>
              <code class="port-number">{{ t('ports.control.port') }}</code>
              <span><strong>{{ t('ports.control.title') }}</strong> — {{ t('ports.control.description') }}</span>
            </li>
            <li>
              <code class="port-number">{{ t('ports.discovery.port') }}</code>
              <span><strong>{{ t('ports.discovery.title') }}</strong> — {{ t('ports.discovery.description') }}</span>
            </li>
          </ul>
          <p class="ports-note">{{ t('ports.note') }}</p>
        </div>
      </div>
    </section>

    <section class="contribute-section">
      <div class="container">
        <h2>{{ t('contribute.title') }}</h2>
        <p class="contribute-subtitle">{{ t('contribute.subtitle') }}</p>
        <div class="contribute-grid">
          <div class="contribute-card">
            <h3>{{ t('contribute.freeTitle') }}</h3>
            <p>{{ t('contribute.freeText') }}</p>
          </div>
          <div class="contribute-card">
            <h3>{{ t('contribute.licenseTitle') }}</h3>
            <p>{{ t('contribute.licenseText') }}</p>
          </div>
          <div class="contribute-card">
            <h3>{{ t('contribute.contributeTitle') }}</h3>
            <p>{{ t('contribute.contributeText') }}</p>
          </div>
        </div>
        <div class="contribute-cta">
          <a
            href="https://github.com/tdoukinitsas/liveplay"
            target="_blank"
            rel="noopener noreferrer"
            class="contribute-button"
          >
            {{ t('contribute.githubButton') }}
          </a>
        </div>
      </div>
    </section>

    <footer class="site-footer">
      <div class="container">
        <p>
          <strong>LivePlay</strong> {{ t('footer.license') }}
          <a href="https://www.gnu.org/licenses/agpl-3.0.en.html" target="_blank" rel="noopener noreferrer">
            AGPL-3.0
          </a>
        </p>
        <p>
          {{ t('footer.developedBy') }}
          <a href="https://github.com/tdoukinitsas" target="_blank" rel="noopener noreferrer">
            {{ t('footer.developerName') }}
          </a>
        </p>
        <p>
          <a href="https://github.com/tdoukinitsas/liveplay" target="_blank" rel="noopener noreferrer">
            {{ t('footer.viewOnGitHub') }}
          </a>
        </p>
        <p v-if="contributors.length">
          <strong>{{ t('footer.contributors') }}: </strong>
          <template v-for="(contributor, index) in contributors" :key="contributor.name">
            <a :href="contributor.link" target="_blank" rel="noopener noreferrer">{{ contributor.name }}</a><template v-if="index < contributors.length - 1">, </template>
          </template>
        </p>
      </div>
    </footer>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, watch } from 'vue';
import { useI18n } from './composables/useI18n';

const { t, direction, initLocale, isLocaleLoaded } = useI18n();

// Build URLs for files served from the site's public/ folder. The app is hosted
// under a base path (e.g. "/liveplay/"), so we prepend Nuxt's resolved app
// baseURL rather than hardcoding it. (We previously used import.meta.env.BASE_URL,
// but under Nuxt 4 that compiles to a relative "./" — which breaks these images
// whenever the page URL lacks a trailing slash. app.baseURL is always the
// absolute "/liveplay/", matching how Nuxt emits the build asset URLs.)
// Crucially, these URLs must be bound dynamically (`:src`, not `src="..."`): a
// static literal makes Vite/Rollup try to *import* the file from the source tree
// at build time, which fails for anything not physically present in public/.
const baseURL = useRuntimeConfig().app.baseURL;
const asset = (path: string) => `${baseURL}${path.replace(/^\/+/, '')}`;

const version = ref('2.1.1');
const contributors = ref<{ name: string; link: string }[]>([]);

// Platform download cards. When we can detect the visitor's OS we surface only
// the matching build(s) prominently and tuck the rest behind a toggle; when we
// can't, all four are shown and the flex grid keeps them centred and aligned.
type PlatformKey = 'windows' | 'macArm' | 'macIntel' | 'linux';
const allPlatformKeys: PlatformKey[] = ['windows', 'macArm', 'macIntel', 'linux'];
const osPlatformMap: Record<string, PlatformKey[]> = {
  windows: ['windows'],
  mac: ['macArm', 'macIntel'],
  linux: ['linux']
};

const detectedOS = ref<string | null>(null);
const showAllPlatforms = ref(false);

const prominentKeys = computed<PlatformKey[]>(() => {
  if (!detectedOS.value) return [...allPlatformKeys];
  return osPlatformMap[detectedOS.value] ?? [...allPlatformKeys];
});
const otherKeys = computed<PlatformKey[]>(() =>
  allPlatformKeys.filter((k) => !prominentKeys.value.includes(k))
);

const detectOS = (): string | null => {
  if (typeof navigator === 'undefined') return null;
  const uaData = (navigator as any).userAgentData;
  if (uaData?.platform) {
    const p = String(uaData.platform).toLowerCase();
    if (p.includes('win')) return 'windows';
    if (p.includes('mac')) return 'mac';
    if (p.includes('linux') || p.includes('android') || p.includes('chrome os')) return 'linux';
  }
  const ua = (navigator.userAgent || '').toLowerCase();
  const platform = ((navigator as any).platform || '').toLowerCase();
  if (ua.includes('windows') || platform.includes('win')) return 'windows';
  if (ua.includes('mac') || platform.includes('mac')) return 'mac';
  if (ua.includes('linux') || ua.includes('x11') || ua.includes('android') || platform.includes('linux')) return 'linux';
  return null;
};

const downloadLinks = computed(() => {
  const v = version.value;
  const baseUrl = `https://github.com/tdoukinitsas/liveplay/releases/download/v${v}`;
  return {
    // Windows (artifactName uses hyphens so the file, GitHub asset and latest.yml all match)
    windows: `${baseUrl}/LivePlay-Setup-${v}.exe`,
    // macOS — two separate, per-architecture builds
    macArm: `${baseUrl}/LivePlay-${v}-arm64.dmg`,
    macIntel: `${baseUrl}/LivePlay-${v}.dmg`,
    // Linux
    linux: `${baseUrl}/LivePlay-${v}.AppImage`,
    deb: `${baseUrl}/liveplay_${v}_amd64.deb`,
    rpm: `${baseUrl}/liveplay-${v}.x86_64.rpm`,
    // macOS .zip variants (for auto-update / manual installs)
    macArmZip: `${baseUrl}/LivePlay-${v}-arm64-mac.zip`,
    macIntelZip: `${baseUrl}/LivePlay-${v}-mac.zip`
  };
});

const handleImageError = (event: Event) => {
  // Fallback if logo doesn't load
  (event.target as HTMLImageElement).style.display = 'none';
};

// Update SEO meta when locale changes
const updateSeoMeta = () => {
  if (!isLocaleLoaded.value) return;

  const title = `LivePlay - ${t('header.tagline')}`;
  const description = `${t('header.tagline')}. ${t('download.subtitle')}`;
  const ogImage = 'https://tdoukinitsas.github.io/liveplay/screenshots/liveplay_screenshot.jpg';

  useHead({
    title,
    meta: [
      { name: 'description', content: description }
    ]
  });

  useSeoMeta({
    title,
    description,
    ogTitle: title,
    ogDescription: description,
    ogType: 'website',
    ogUrl: 'https://tdoukinitsas.github.io/liveplay/',
    ogImage,
    ogImageWidth: '1920',
    ogImageHeight: '1080',
    ogImageType: 'image/jpeg',
    twitterCard: 'summary_large_image',
    twitterTitle: title,
    twitterDescription: description,
    twitterImage: ogImage
  });
};

onMounted(async () => {
  // Detect the visitor's operating system for the download section
  detectedOS.value = detectOS();

  // Initialize locale
  await initLocale();
  
  // Update SEO after locale is loaded
  updateSeoMeta();

  // Fetch version from package.json
  try {
    const packageRes = await fetch(asset('package.json'));
    const packageData = await packageRes.json();
    version.value = packageData.version;
  } catch (error) {
    console.error('Failed to fetch version:', error);
  }

  // Fetch contributors
  try {
    const contributorsRes = await fetch(asset('contributors.json'));
    const contributorsData = await contributorsRes.json();
    contributors.value = Object.values(contributorsData.contributors) as { name: string; link: string }[];
  } catch (error) {
    console.error('Failed to fetch contributors:', error);
  }
});

// Watch for locale changes and update SEO
watch(isLocaleLoaded, () => {
  if (isLocaleLoaded.value) {
    updateSeoMeta();
  }
});

// Initial SEO with fallback values
useHead({
  title: 'LivePlay - Audio Cue Playback for Live Events',
  meta: [
    { name: 'description', content: 'Free, open-source audio playback system for live sound operators. Available for Windows, macOS, and Linux.' }
  ]
});

useSeoMeta({
  title: 'LivePlay - Audio Cue Playback for Live Events',
  description: 'Free, open-source audio playback system for live sound operators. Available for Windows, macOS, and Linux.',
  ogTitle: 'LivePlay - Audio Cue Playback for Live Events',
  ogDescription: 'Free, open-source audio playback system for live sound operators. Available for Windows, macOS, and Linux.',
  ogType: 'website',
  ogUrl: 'https://tdoukinitsas.github.io/liveplay/',
  ogImage: 'https://tdoukinitsas.github.io/liveplay/screenshots/liveplay_screenshot.jpg',
  ogImageWidth: '1920',
  ogImageHeight: '1080',
  ogImageType: 'image/jpeg',
  twitterCard: 'summary_large_image',
  twitterTitle: 'LivePlay - Audio Cue Playback for Live Events',
  twitterDescription: 'Free, open-source audio playback system for live sound operators. Available for Windows, macOS, and Linux.',
  twitterImage: 'https://tdoukinitsas.github.io/liveplay/screenshots/liveplay_screenshot.jpg'
});

</script>

<style scoped lang="scss">


.docs-site {
  min-height: 100vh;
  background: linear-gradient(135deg, #1a1a1a 0%, #2d2d2d 100%);
  color: #ffffff;
}

.container {
  max-width: 1200px;
  margin: 0 auto;
  padding: 0 2rem;
}

.site-header {
  padding: 4rem 0 2rem;
  text-align: center;
  border-bottom: 1px solid rgba(255, 255, 255, 0.1);

  .header-content {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    margin-bottom: 1rem;
  }

  .logo-section {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 2rem;
  }

  .logo {
    width: 120px;
    height: 120px;
    object-fit: contain;
  }

  .title-section {
    text-align: left;
  }

  h1 {
    font-size: 4rem;
    font-weight: 700;
    margin: 0;
    color: #DA1E28;
    text-shadow: 0 2px 4px rgba(0, 0, 0, 0.3);
  }

  .version {
    font-size: 1.25rem;
    color: rgba(255, 255, 255, 0.7);
    margin: 0.5rem 0 0;
  }

  .tagline {
    font-size: 1.5rem;
    color: rgba(255, 255, 255, 0.8);
    margin: 1rem 0 0;
  }
}

.screenshot-section {
  padding: 4rem 0;
  background: rgba(0, 0, 0, 0.2);

  .screenshot-wrapper {
    max-width: 1000px;
    margin: 0 auto;
    border-radius: 16px;
    overflow: hidden;
    box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
    border: 1px solid rgba(255, 255, 255, 0.1);
    transition: transform 0.3s ease, box-shadow 0.3s ease;

    &:hover {
      transform: translateY(-8px);
      box-shadow: 0 24px 80px rgba(218, 30, 40, 0.3);
    }
  }

  .app-screenshot {
    width: 100%;
    height: auto;
    display: block;
  }
}

.download-section {
  padding: 4rem 0;
  background: rgba(255, 255, 255, 0.02);

  h2 {
    text-align: center;
    font-size: 2.5rem;
    margin-bottom: 1rem;
    color: #DA1E28;
  }

  .download-subtitle {
    text-align: center;
    font-size: 1.25rem;
    color: rgba(255, 255, 255, 0.7);
    margin-bottom: 3rem;
  }

  .download-recommended {
    text-align: center;
    font-size: 1rem;
    font-weight: 600;
    color: rgba(255, 255, 255, 0.85);
    margin-bottom: 1.5rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }

  .download-grid {
    display: flex;
    flex-wrap: wrap;
    justify-content: center;
    gap: 2rem;
    margin-bottom: 2rem;
  }

  .platform-note {
    max-width: 720px;
    margin: 0 auto 2.5rem;
    padding: 1.25rem 1.5rem;
    border: 1px solid rgba(255, 255, 255, 0.15);
    border-radius: 8px;
    background: rgba(255, 255, 255, 0.04);
    text-align: left;

    h3 {
      font-size: 1.05rem;
      margin: 0 0 0.75rem;
      color: #ffffff;
    }

    p {
      font-size: 0.95rem;
      color: rgba(255, 255, 255, 0.8);
      margin: 0.5rem 0;
    }

    .gatekeeper-command {
      display: block;
      font-family: "IBM Plex Mono", ui-monospace, SFMono-Regular, Menlo, monospace;
      font-size: 0.9rem;
      background: rgba(0, 0, 0, 0.4);
      color: #f0f0f0;
      padding: 0.75rem 1rem;
      border-radius: 6px;
      margin: 0.75rem 0;
      overflow-x: auto;
      white-space: nowrap;
      user-select: all;
    }

    .smartscreen-steps {
      margin: 0.75rem 0;
      padding-inline-start: 1.5rem;

      li {
        font-size: 0.95rem;
        color: rgba(255, 255, 255, 0.8);
        margin: 0.4rem 0;
      }
    }
  }

  .other-os {
    text-align: center;
    margin-bottom: 3rem;

    .other-os-toggle {
      background: transparent;
      border: 2px solid rgba(255, 255, 255, 0.2);
      color: rgba(255, 255, 255, 0.85);
      padding: 0.75rem 1.75rem;
      border-radius: 8px;
      font-size: 1rem;
      font-weight: 500;
      cursor: pointer;
      transition: all 0.2s ease;
      font-family: inherit;

      &:hover {
        border-color: #DA1E28;
        color: #ffffff;
      }
    }

    .download-grid {
      margin-top: 2rem;
      margin-bottom: 0;
    }
  }

  .download-card {
    background: rgba(255, 255, 255, 0.05);
    border: 2px solid rgba(255, 255, 255, 0.1);
    border-radius: 12px;
    padding: 2rem;
    text-align: center;
    transition: all 0.3s ease;
    text-decoration: none;
    color: inherit;
    flex: 0 1 300px;
    max-width: 340px;
    display: block;

    &:hover {
      transform: translateY(-4px);
      border-color: #DA1E28;
      box-shadow: 0 8px 24px rgba(218, 30, 40, 0.3);
    }

    .platform-icon {
      font-size: 4rem;
      margin-bottom: 1rem;
    }

    h3 {
      font-size: 1.75rem;
      margin: 0 0 0.5rem;
      color: #ffffff;
    }

    p {
      color: rgba(255, 255, 255, 0.6);
      margin: 0 0 1.5rem;
    }

    .download-button {
      background: #DA1E28;
      color: white;
      padding: 1rem 2rem;
      border-radius: 8px;
      font-weight: 600;
      display: flex;
      flex-direction: column;
      gap: 0.25rem;

      .file-size {
        font-size: 0.875rem;
        opacity: 0.8;
        font-weight: 400;
      }
    }
  }

  .other-formats {
    text-align: center;
    margin: 2rem 0;

    p {
      color: rgba(255, 255, 255, 0.7);
      margin-bottom: 1rem;
    }

    .format-links {
      display: flex;
      justify-content: center;
      gap: 2rem;
      flex-wrap: wrap;

      a {
        color: #DA1E28;
        text-decoration: none;
        font-weight: 500;
        transition: opacity 0.2s;

        &:hover {
          opacity: 0.8;
          text-decoration: underline;
        }
      }
    }
  }

  .github-link {
    text-align: center;
    margin-top: 2rem;

    a {
      color: rgba(255, 255, 255, 0.8);
      text-decoration: none;
      font-size: 1.125rem;
      transition: color 0.2s;

      &:hover {
        color: #DA1E28;
      }
    }
  }
}

.features-section {
  padding: 4rem 0;
  background: rgba(255, 255, 255, 0.02);

  h2 {
    text-align: center;
    font-size: 2.5rem;
    margin-bottom: 3rem;
    color: #DA1E28;
  }
}

.ports-section {
  padding: 3rem 0;

  .ports-disclaimer {
    max-width: 820px;
    margin: 0 auto;
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-left: 3px solid rgba(218, 30, 40, 0.5);
    border-radius: 8px;
    padding: 1.5rem 2rem;
  }

  .ports-title {
    font-size: 1.1rem;
    font-weight: 600;
    color: rgba(255, 255, 255, 0.85);
    margin: 0 0 0.5rem;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }

  .ports-subtitle {
    font-size: 0.95rem;
    color: rgba(255, 255, 255, 0.6);
    margin: 0 0 1.25rem;
  }

  .ports-list {
    list-style: none;
    margin: 0 0 1.25rem;
    padding: 0;

    li {
      display: flex;
      align-items: baseline;
      gap: 0.85rem;
      margin: 0.75rem 0;
      color: rgba(255, 255, 255, 0.65);
      font-size: 0.9rem;
      line-height: 1.5;
    }

    .port-number {
      flex-shrink: 0;
      font-family: 'Courier New', monospace;
      font-size: 0.85rem;
      font-weight: 700;
      color: #DA1E28;
      background: rgba(218, 30, 40, 0.1);
      padding: 0.25rem 0.6rem;
      border-radius: 5px;
    }

    strong {
      color: rgba(255, 255, 255, 0.85);
      font-weight: 600;
    }
  }

  .ports-note {
    color: rgba(255, 255, 255, 0.5);
    margin: 0;
    font-size: 0.85rem;
    line-height: 1.6;
  }
}

.contribute-section {
  padding: 4rem 0;
  background: rgba(0, 0, 0, 0.2);

  h2 {
    text-align: center;
    font-size: 2.5rem;
    margin-bottom: 1rem;
    color: #DA1E28;
  }

  .contribute-subtitle {
    text-align: center;
    font-size: 1.25rem;
    color: rgba(255, 255, 255, 0.7);
    margin-bottom: 3rem;
  }

  .contribute-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
    gap: 2rem;
    max-width: 1000px;
    margin: 0 auto 3rem;
  }

  .contribute-card {
    background: rgba(255, 255, 255, 0.05);
    border: 2px solid rgba(255, 255, 255, 0.1);
    border-radius: 12px;
    padding: 2rem;

    h3 {
      font-size: 1.35rem;
      margin: 0 0 0.75rem;
      color: #ffffff;
    }

    p {
      color: rgba(255, 255, 255, 0.65);
      margin: 0;
      line-height: 1.7;
    }
  }

  .contribute-cta {
    text-align: center;

    .contribute-button {
      display: inline-block;
      background: #DA1E28;
      color: #ffffff;
      padding: 1rem 2.5rem;
      border-radius: 8px;
      font-size: 1.125rem;
      font-weight: 600;
      text-decoration: none;
      transition: all 0.3s ease;

      &:hover {
        transform: translateY(-2px);
        box-shadow: 0 8px 24px rgba(218, 30, 40, 0.4);
      }
    }
  }
}

.readme-section {
  padding: 4rem 0;

  .readme-content {
    background: rgba(255, 255, 255, 0.03);
    border-radius: 12px;
    padding: 3rem;
    line-height: 1.8;

    :deep(h1) {
      font-size: 2.5rem;
      color: #DA1E28;
      margin: 2rem 0 1rem;
      
      &:first-child {
        margin-top: 0;
      }
    }

    :deep(h2) {
      font-size: 2rem;
      color: #DA1E28;
      margin: 2rem 0 1rem;
      border-bottom: 2px solid rgba(218, 30, 40, 0.3);
      padding-bottom: 0.5rem;
    }

    :deep(h3) {
      font-size: 1.5rem;
      color: rgba(255, 255, 255, 0.9);
      margin: 1.5rem 0 1rem;
    }

    :deep(p) {
      margin: 1rem 0;
      color: rgba(255, 255, 255, 0.8);
    }

    :deep(a) {
      color: #DA1E28;
      text-decoration: none;
      
      &:hover {
        text-decoration: underline;
      }
    }

    :deep(code) {
      background: rgba(0, 0, 0, 0.3);
      padding: 0.2rem 0.4rem;
      border-radius: 4px;
      font-family: 'Courier New', monospace;
      font-size: 0.9em;
    }

    :deep(pre) {
      background: rgba(0, 0, 0, 0.4);
      padding: 1.5rem;
      border-radius: 8px;
      overflow-x: auto;
      margin: 1.5rem 0;

      code {
        background: none;
        padding: 0;
      }
    }

    :deep(ul) {
      margin: 1rem 0;
      padding-left: 2rem;

      li {
        margin: 0.5rem 0;
        color: rgba(255, 255, 255, 0.8);
      }
    }

    :deep(hr) {
      border: none;
      border-top: 1px solid rgba(255, 255, 255, 0.1);
      margin: 2rem 0;
    }
  }
}

.site-footer {
  padding: 3rem 0;
  text-align: center;
  border-top: 1px solid rgba(255, 255, 255, 0.1);
  background: rgba(0, 0, 0, 0.2);

  p {
    margin: 0.5rem 0;
    color: rgba(255, 255, 255, 0.7);
  }

  a {
    color: #DA1E28;
    text-decoration: none;

    &:hover {
      text-decoration: underline;
    }
  }
}

@media (max-width: 768px) {
  .site-header {
    .header-content {
      flex-direction: column;
      align-items: center;
      gap: 1rem;
    }

    .logo-section {
      flex-direction: column;
      gap: 1rem;
    }

    .title-section {
      text-align: center;
    }

    h1 {
      font-size: 2.5rem;
    }

    .tagline {
      font-size: 1.125rem;
    }
  }

  .download-section {
    .download-grid {
      grid-template-columns: 1fr;
    }

    .other-formats .format-links {
      flex-direction: column;
      gap: 1rem;
    }
  }

  .features-section h2 {
    font-size: 2rem;
  }

  .readme-section .readme-content {
    padding: 1.5rem;
  }
}
</style>
