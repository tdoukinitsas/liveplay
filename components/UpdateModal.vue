<template>
  <div class="modal-overlay" @click.self="handleCancel">
    <div class="modal-container update-modal">
      <div class="modal-header">
        <span class="material-symbols-outlined icon-large">system_update</span>
        <h2>{{ t('update.updateAvailable') }}</h2>
      </div>

      <div class="modal-body">
        <div v-if="!downloading && !downloaded" class="update-info">
          <p class="version-info">
            {{ t('update.currentVersion') }}: <strong>{{ currentVersion }}</strong>
          </p>
          <p class="version-info">
            {{ t('update.newVersion') }}: <strong class="new-version">{{ newVersion }}</strong>
          </p>
          
          <div v-if="releaseNotes" class="release-notes">
            <h3>{{ t('update.whatsNew') }}</h3>
            <div class="notes-content">{{ releaseNotes }}</div>
          </div>

          <p class="update-prompt">
            {{ isManualUpdate ? t('update.manualUpdatePrompt') : t('update.updatePrompt') }}
          </p>
        </div>

        <div v-if="downloading" class="download-progress">
          <div class="progress-info">
            <span class="material-symbols-outlined spinning">sync</span>
            <p>{{ t('update.downloading') }}...</p>
          </div>
          <div class="progress-bar">
            <div class="progress-fill" :style="{ width: downloadPercent + '%' }"></div>
          </div>
          <p class="progress-text">{{ Math.round(downloadPercent) }}%</p>
        </div>

        <div v-if="downloaded" class="download-complete">
          <span class="material-symbols-outlined icon-success">check_circle</span>
          <p>{{ t('update.downloadComplete') }}</p>
          <p class="install-info">{{ t('update.installInfo') }}</p>
        </div>

        <div v-if="error" class="error-message">
          <span class="material-symbols-outlined">error</span>
          <p>{{ error }}</p>
        </div>
      </div>

      <div class="modal-footer">
        <button 
          v-if="!downloading && !downloaded" 
          class="button button-secondary" 
          @click="handleCancel"
        >
          {{ t('update.later') }}
        </button>
        <button 
          v-if="!downloading && !downloaded && !isManualUpdate" 
          class="button button-primary" 
          @click="handleDownload"
        >
          {{ t('update.downloadAndInstall') }}
        </button>
        <button 
          v-if="!downloading && !downloaded && isManualUpdate" 
          class="button button-primary" 
          @click="handleOpenDownloadPage"
        >
          {{ t('update.goToDownloadPage') }}
        </button>
        <button 
          v-if="downloaded" 
          class="button button-primary" 
          @click="handleInstall"
        >
          {{ t('update.installNow') }}
        </button>
        <button 
          v-if="downloaded" 
          class="button button-secondary" 
          @click="handleCancel"
        >
          {{ t('update.installOnExit') }}
        </button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const props = defineProps<{
  currentVersion: string;
  newVersion: string;
  releaseNotes?: string;
  releaseDate?: string;
  isManualUpdate?: boolean;
  downloadUrl?: string;
}>();

const emit = defineEmits<{
  close: [];
  download: [];
  install: [];
}>();

const { t } = useLocalization();

const downloading = ref(false);
const downloaded = ref(false);
const downloadPercent = ref(0);
const error = ref('');

onMounted(() => {
  if (import.meta.client && window.electronAPI) {
    // Listen for download progress
    window.electronAPI.onUpdateDownloadProgress((event: any, progress: any) => {
      downloading.value = true;
      downloadPercent.value = progress.percent;
    });

    // Listen for download complete
    window.electronAPI.onUpdateDownloaded(() => {
      downloading.value = false;
      downloaded.value = true;
    });

    // Listen for errors
    window.electronAPI.onUpdateError((event: any, errorMessage: string) => {
      error.value = errorMessage;
      downloading.value = false;
    });
  }
});

const handleDownload = async () => {
  if (import.meta.client && window.electronAPI) {
    downloading.value = true;
    error.value = '';
    const result = await window.electronAPI.downloadUpdate();
    if (!result.success) {
      error.value = result.error || t('update.downloadFailed');
      downloading.value = false;
    }
  }
};

const handleOpenDownloadPage = async () => {
  if (import.meta.client && window.electronAPI) {
    const url = props.downloadUrl || 'https://tdoukinitsas.github.io/liveplay/';
    await window.electronAPI.openExternal(url);
    emit('close');
  }
};

const handleInstall = () => {
  if (import.meta.client && window.electronAPI) {
    window.electronAPI.installUpdate();
  }
};

const handleCancel = () => {
  if (!downloading.value) {
    emit('close');
  }
};
</script>

<style scoped lang="scss">
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.7);
  backdrop-filter: blur(4px);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 10000;
  animation: fadeIn 0.2s ease;
}

.update-modal {
  min-width: 500px;
  max-width: 600px;
}

.modal-container {
  background: var(--color-surface);
  border-radius: 12px;
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
  overflow: hidden;
  animation: slideUp 0.3s ease;
}

.modal-header {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 24px;
  background: linear-gradient(135deg, var(--color-accent-custom), var(--color-accent-dark));
  color: white;

  .icon-large {
    font-size: 48px;
  }

  h2 {
    margin: 0;
    font-size: 24px;
    font-weight: 600;
  }
}

.modal-body {
  padding: 24px;
}

.update-info {
  .version-info {
    margin: 12px 0;
    font-size: 16px;

    .new-version {
      color: var(--color-accent-custom);
    }
  }

  .release-notes {
    margin: 24px 0;
    padding: 16px;
    background: var(--color-background);
    border-radius: 8px;
    border: 1px solid var(--color-border);

    h3 {
      margin: 0 0 12px 0;
      font-size: 16px;
      font-weight: 600;
    }

    .notes-content {
      color: var(--color-text-secondary);
      line-height: 1.6;
      white-space: pre-wrap;
      max-height: 200px;
      overflow-y: auto;
    }
  }

  .update-prompt {
    margin: 16px 0 0 0;
    color: var(--color-text-secondary);
    font-size: 14px;
  }
}

.download-progress {
  text-align: center;
  padding: 20px 0;

  .progress-info {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 12px;
    margin-bottom: 16px;

    .spinning {
      font-size: 32px;
      color: var(--color-accent-custom);
      animation: spin 1s linear infinite;
    }

    p {
      margin: 0;
      font-size: 16px;
      font-weight: 500;
    }
  }

  .progress-bar {
    width: 100%;
    height: 8px;
    background: var(--color-background);
    border-radius: 4px;
    overflow: hidden;
    margin-bottom: 8px;

    .progress-fill {
      height: 100%;
      background: linear-gradient(90deg, var(--color-accent-custom), var(--color-accent-dark));
      transition: width 0.3s ease;
    }
  }

  .progress-text {
    margin: 8px 0 0 0;
    font-size: 14px;
    color: var(--color-text-secondary);
  }
}

.download-complete {
  text-align: center;
  padding: 20px 0;

  .icon-success {
    font-size: 64px;
    color: var(--color-success);
    margin-bottom: 16px;
  }

  p {
    margin: 8px 0;
  }

  .install-info {
    color: var(--color-text-secondary);
    font-size: 14px;
  }
}

.error-message {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 16px;
  background: rgba(218, 30, 40, 0.1);
  border: 1px solid rgba(218, 30, 40, 0.3);
  border-radius: 8px;
  color: #da1e28;
  margin-top: 16px;

  .material-symbols-outlined {
    font-size: 24px;
  }
}

.modal-footer {
  display: flex;
  justify-content: flex-end;
  gap: 12px;
  padding: 24px;
  background: var(--color-background);
  border-top: 1px solid var(--color-border);
}

.button {
  padding: 10px 24px;
  border: none;
  border-radius: 6px;
  font-size: 14px;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.2s ease;

  &.button-primary {
    background: var(--color-accent-custom);
    color: white;

    &:hover {
      background: var(--color-accent-dark);
      transform: translateY(-1px);
    }
  }

  &.button-secondary {
    background: transparent;
    color: var(--color-text-secondary);
    border: 1px solid var(--color-border);

    &:hover {
      background: var(--color-surface-hover);
    }
  }

  &:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
}

@keyframes fadeIn {
  from {
    opacity: 0;
  }
  to {
    opacity: 1;
  }
}

@keyframes slideUp {
  from {
    transform: translateY(20px);
    opacity: 0;
  }
  to {
    transform: translateY(0);
    opacity: 1;
  }
}

@keyframes spin {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}
</style>
