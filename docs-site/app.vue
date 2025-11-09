<template>
  <div class="docs-site" :dir="direction">
    <header class="site-header">
      <div class="container">
        <div class="header-content">
          <div class="logo-section">
            <img 
              src="/assets/logo.svg" 
              alt="LivePlay Logo" 
              class="logo"
              @error="handleImageError"
            />
            <div class="title-section">
              <h1>LivePlay</h1>
              <p class="version">Version {{ version }}</p>
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
            src="/screenshots/liveplay_screenshot.jpg" 
            alt="LivePlay main interface showing playlist editor, cue cart, and properties panel" 
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
        
        <div class="download-grid">
          <a 
            :href="downloadLinks.windows" 
            class="download-card"
            target="_blank"
            rel="noopener noreferrer"
          >
            <h3>{{ t('download.windows.title') }}</h3>
            <p>{{ t('download.windows.description') }}</p>
            <div class="download-button">
              <span>{{ t('download.windows.buttonText') }}</span>
              <span class="file-size">{{ t('download.windows.size') }}</span>
            </div>
          </a>

          <a 
            :href="downloadLinks.mac" 
            class="download-card"
            target="_blank"
            rel="noopener noreferrer"
          >
            <h3>{{ t('download.mac.title') }}</h3>
            <p>{{ t('download.mac.description') }}</p>
            <div class="download-button">
              <span>{{ t('download.mac.buttonText') }}</span>
              <span class="file-size">{{ t('download.mac.size') }}</span>
            </div>
          </a>

          <a 
            :href="downloadLinks.linux" 
            class="download-card"
            target="_blank"
            rel="noopener noreferrer"
          >
            <h3>{{ t('download.linux.title') }}</h3>
            <p>{{ t('download.linux.description') }}</p>
            <div class="download-button">
              <span>{{ t('download.linux.buttonText') }}</span>
              <span class="file-size">{{ t('download.linux.size') }}</span>
            </div>
          </a>
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
            <a :href="downloadLinks.macZip" target="_blank" rel="noopener noreferrer">
              {{ t('download.macZip') }}
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
          image-src="/liveplay/screenshots/liveplay_screenshot.jpg"
        >
          <p>{{ t('features.interface.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.waveform.title')"
          image-src="/liveplay/screenshots/liveplay_screenshot_waveformtrimmer.jpg"
        >
          <p>{{ t('features.waveform.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.youtube.title')"
          image-src="/liveplay/screenshots/liveplay_screenshot_youtube.jpg"
        >
          <p>{{ t('features.youtube.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.properties.title')"
          image-src="/liveplay/screenshots/liveplay_screenshot_properties.jpg"
        >
          <p>{{ t('features.properties.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.ducking.title')"
          image-src="/liveplay/screenshots/liveplay_screenshot_ducking.jpg"
        >
          <p>{{ t('features.ducking.description') }}</p>
        </FeatureHighlight>

        <FeatureHighlight
          :title="t('features.firstLaunch.title')"
          image-src="/liveplay/screenshots/liveplay_screenshot_welcomescreen.jpg"
        >
          <p>{{ t('features.firstLaunch.description') }}</p>
        </FeatureHighlight>
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
      </div>
    </footer>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, watch } from 'vue';
import { useI18n } from './composables/useI18n';

const { t, direction, initLocale, isLocaleLoaded } = useI18n();

const version = ref('1.2.4');

const downloadLinks = computed(() => {
  const baseUrl = `https://github.com/tdoukinitsas/liveplay/releases/download/v${version.value}`;
  return {
    windows: `${baseUrl}/LivePlay.Setup.${version.value}.exe`,
    mac: `${baseUrl}/LivePlay-${version.value}.dmg`,
    linux: `${baseUrl}/LivePlay-${version.value}.AppImage`,
    deb: `${baseUrl}/liveplay_${version.value}_amd64.deb`,
    rpm: `${baseUrl}/liveplay-${version.value}.x86_64.rpm`,
    macZip: `${baseUrl}/LivePlay-${version.value}-mac.zip`
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
  // Initialize locale
  await initLocale();
  
  // Update SEO after locale is loaded
  updateSeoMeta();

  // Fetch version from package.json
  try {
    const packageRes = await fetch('/liveplay/package.json');
    const packageData = await packageRes.json();
    version.value = packageData.version;
  } catch (error) {
    console.error('Failed to fetch version:', error);
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

  .download-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    gap: 2rem;
    margin-bottom: 3rem;
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
