<template>
  <div class="modal-overlay" @click.self="close">
    <div class="modal-content about-modal">
      <button class="modal-close" @click="close" :title="t('actions.close')">
        <span class="material-symbols-rounded">close</span>
      </button>
      
      <div class="about-header">
        <img 
          :src="isDark ? './assets/icons/SVG/liveplay-icon-darkmode@web.svg' : './assets/icons/SVG/liveplay-icon-lightmode@web.svg'"
          alt="LivePlay"
          class="about-logo"
        />
        <div class="about-text">
          <h1 class="about-title">
            LivePlay
            <span class="version-badge">v1.1.0</span>
          </h1>
          <p class="about-subtitle">{{ t('welcome.subtitle') }}</p>
        </div>
      </div>
      
      <div class="about-info">
        <div class="info-section">
          <p class="developer">
            <strong>{{ t('about.developedBy') }}:</strong> {{t('about.developerName') }}
          </p>
        </div>
        
        <div class="info-section links">
          <a 
            href="https://github.com/tdoukinitsas/liveplay" 
            class="info-link"
            @click.prevent="openExternal('https://github.com/tdoukinitsas/liveplay')"
          >
            <span class="material-symbols-rounded">code</span>
            <span>{{ t('about.githubRepo') }}</span>
          </a>
          
          <a 
            href="https://www.gnu.org/licenses/agpl-3.0.en.html" 
            class="info-link"
            @click.prevent="openExternal('https://www.gnu.org/licenses/agpl-3.0.en.html')"
          >
            <span class="material-symbols-rounded">description</span>
            <span>AGPL-3.0-only License</span>
          </a>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const emit = defineEmits<{
  close: []
}>();

const { t } = useLocalization();

// Get theme from app state
const theme = useState('theme', () => 'dark');
const isDark = computed(() => theme.value === 'dark');

const close = () => {
  emit('close');
};

const openExternal = (url: string) => {
  if (import.meta.client && window.electronAPI?.openExternal) {
    window.electronAPI.openExternal(url);
  }
};

// Close on Escape key
onMounted(() => {
  const handleEscape = (e: KeyboardEvent) => {
    if (e.key === 'Escape') {
      close();
    }
  };
  window.addEventListener('keydown', handleEscape);
  onUnmounted(() => {
    window.removeEventListener('keydown', handleEscape);
  });
});
</script>

<style scoped lang="scss">
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background-color: rgba(0, 0, 0, 0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 10000;
  backdrop-filter: blur(4px);
}

.modal-content {
  background-color: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-lg);
  padding: var(--spacing-xxl);
  max-width: 500px;
  width: 90%;
  position: relative;
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
}

.modal-close {
  position: absolute;
  top: var(--spacing-md);
  right: var(--spacing-md);
  width: 32px;
  height: 32px;
  border-radius: 50%;
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  color: var(--color-text-primary);
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  transition: all var(--transition-fast);
  
  &:hover {
    background-color: var(--color-danger);
    border-color: var(--color-danger);
    color: white;
  }
}

.about-header {
  display: flex;
  align-items: center;
  gap: var(--spacing-lg);
  margin-bottom: var(--spacing-xl);
  padding-bottom: var(--spacing-xl);
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
  font-size: 36px;
  font-weight: 600;
  margin: 0 0 var(--spacing-xs) 0;
  color: var(--color-text-primary);
  letter-spacing: -1px;
  line-height: 1;
  display: flex;
  align-items: baseline;
  gap: var(--spacing-sm);
  flex-wrap: wrap;
}

.version-badge {
  font-size: 14px;
  font-weight: 400;
  color: var(--color-text-secondary);
  opacity: 0.6;
  letter-spacing: 0;
}

.about-subtitle {
  font-size: 16px;
  color: var(--color-text-secondary);
  margin: 0;
  line-height: 1.4;
}

.about-info {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-lg);
}

.info-section {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-sm);
}

.developer {
  margin: 0;
  color: var(--color-text-primary);
  font-size: 14px;
  
  strong {
    font-weight: 600;
  }
}

.links {
  gap: var(--spacing-xs);
}

.info-link {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  padding: var(--spacing-sm) var(--spacing-md);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  color: var(--color-text-primary);
  text-decoration: none;
  font-size: 14px;
  transition: all var(--transition-fast);
  
  &:hover {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  
  .material-symbols-rounded {
    font-size: 20px;
  }
}
</style>
