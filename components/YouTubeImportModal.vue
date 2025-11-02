<template>
  <div v-if="isOpen" class="modal-overlay" @click.self="closeModal">
    <div class="modal-content youtube-import-modal">
      <div class="modal-header">
        <h2>{{ t('youtube.importFromYouTube') }}</h2>
        <button class="close-btn" @click="closeModal">
          <span class="material-symbols-rounded">close</span>
        </button>
      </div>

      <div class="modal-body">
        <!-- Search Bar -->
        <div class="search-section">
          <div class="search-bar">
            <input
              v-model="searchQuery"
              type="text"
              :placeholder="t('youtube.searchPlaceholder')"
              @keyup.enter="performSearch"
            />
            <button class="search-btn" @click="performSearch" :disabled="isSearching || !searchQuery">
              <span class="material-symbols-rounded">search</span>
            </button>
          </div>
        </div>

        <div class="content-container">
          <!-- Search Results -->
          <div class="results-section">
            <div v-if="isSearching" class="loading-state">
              <span class="material-symbols-rounded spinning">progress_activity</span>
              <p>{{ t('youtube.searching') }}</p>
            </div>

            <div v-else-if="searchError" class="error-state">
              <span class="material-symbols-rounded">error</span>
              <p>{{ searchError }}</p>
            </div>

            <div v-else-if="searchResults.length === 0 && hasSearched" class="empty-state">
              <span class="material-symbols-rounded">search_off</span>
              <p>{{ t('youtube.noResults') }}</p>
            </div>

            <div v-else-if="searchResults.length > 0" class="results-list">
              <div
                v-for="video in searchResults"
                :key="video.id"
                class="video-item"
                :class="{ selected: selectedVideo?.id === video.id }"
              >
                <img :src="video.thumbnail" :alt="video.title" class="video-thumbnail" />
                <div class="video-info">
                  <h3 class="video-title">{{ video.title }}</h3>
                  <p class="video-channel">{{ video.channelTitle }}</p>
                  <p v-if="video.length" class="video-duration">{{ video.length }}</p>
                </div>
                <div class="video-actions">
                  <button class="action-btn preview-btn" @click="previewVideo(video)">
                    <span class="material-symbols-rounded">play_circle</span>
                    <span>{{ t('youtube.preview') }}</span>
                  </button>
                  <button 
                    class="action-btn download-btn" 
                    @click="downloadVideo(video)"
                    :disabled="isDownloading(video.id)"
                  >
                    <span class="material-symbols-rounded">download</span>
                    <span>{{ isDownloading(video.id) ? t('youtube.downloading') : t('youtube.download') }}</span>
                  </button>
                </div>
              </div>
            </div>
          </div>

          <!-- Preview Section -->
          <div class="preview-section">
            <div v-if="!previewVideoId" class="preview-placeholder">
              <span class="material-symbols-rounded">play_circle</span>
              <p>{{ t('youtube.previewHint') }}</p>
            </div>
            <iframe
              v-else
              :src="`https://www.youtube.com/embed/${previewVideoId}`"
              frameborder="0"
              allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture"
              allowfullscreen
              class="preview-iframe"
            ></iframe>
          </div>
        </div>

        <!-- Download Queue -->
        <div v-if="downloadQueue.length > 0" class="download-queue">
          <h3>{{ t('youtube.downloadQueue') }}</h3>
          <div class="queue-list">
            <div v-for="download in downloadQueue" :key="download.videoId" class="queue-item">
              <div class="queue-info">
                <span class="video-title">{{ download.title }}</span>
                <span class="queue-status">{{ getDownloadStatus(download) }}</span>
              </div>
              <div class="progress-bar">
                <div class="progress-fill" :style="{ width: download.progress + '%' }"></div>
              </div>
              <span class="progress-text">{{ download.progress.toFixed(1) }}%</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed } from 'vue';
import { useProject } from '~/composables/useProject';

const { t } = useLocalization();
const { currentProject, addItem, triggerWaveformUpdate } = useProject();

interface YouTubeVideo {
  id: string;
  title: string;
  thumbnail: string;
  channelTitle: string;
  length?: string;
}

interface DownloadProgress {
  videoId: string;
  title: string;
  progress: number;
  status: 'downloading' | 'converting' | 'completed' | 'error';
  error?: string;
}

const props = defineProps<{
  isOpen: boolean;
}>();

const emit = defineEmits<{
  close: [];
}>();

const searchQuery = ref('');
const searchResults = ref<YouTubeVideo[]>([]);
const isSearching = ref(false);
const hasSearched = ref(false);
const searchError = ref('');
const selectedVideo = ref<YouTubeVideo | null>(null);
const previewVideoId = ref('');
const downloadQueue = ref<DownloadProgress[]>([]);

const performSearch = async () => {
  if (!searchQuery.value.trim()) return;

  isSearching.value = true;
  searchError.value = '';
  hasSearched.value = true;
  searchResults.value = [];

  try {
    const results = await window.electronAPI.searchYouTube(searchQuery.value);
    searchResults.value = results;
  } catch (error: any) {
    searchError.value = error.message || t('youtube.searchError');
    console.error('YouTube search error:', error);
  } finally {
    isSearching.value = false;
  }
};

const previewVideo = (video: YouTubeVideo) => {
  selectedVideo.value = video;
  previewVideoId.value = video.id;
};

const downloadVideo = async (video: YouTubeVideo) => {
  if (!currentProject.value) return;
  
  // Add to download queue
  const downloadItem: DownloadProgress = {
    videoId: video.id,
    title: video.title,
    progress: 0,
    status: 'downloading'
  };
  downloadQueue.value.push(downloadItem);

  try {
    // Check if project is loaded
    console.log('Current project:', currentProject.value);
    console.log('Project folderPath:', currentProject.value?.folderPath);
    
    if (!currentProject.value || !currentProject.value.folderPath) {
      throw new Error('No project is currently open. Please open or create a project first.');
    }
    
    console.log('Downloading to project:', currentProject.value.folderPath);
    
    // Start download
    const result = await window.electronAPI.downloadYouTubeAudio(
      video.id,
      video.title,
      currentProject.value.folderPath,
      (progress: any) => {
        // Update progress
        const item = downloadQueue.value.find(d => d.videoId === video.id);
        if (item) {
          item.progress = progress.percentage || 0;
          item.status = progress.status || 'downloading';
        }
      }
    );

    // Mark as completed
    const item = downloadQueue.value.find(d => d.videoId === video.id);
    if (item) {
      item.progress = 100;
      item.status = 'completed';
      
      // Import the downloaded file to the playlist
      await importDownloadedFile(result.fileName, result.file);
      
      // Remove from queue after 2 seconds
      setTimeout(() => {
        downloadQueue.value = downloadQueue.value.filter(d => d.videoId !== video.id);
      }, 2000);
    }
  } catch (error: any) {
    const item = downloadQueue.value.find(d => d.videoId === video.id);
    if (item) {
      item.status = 'error';
      item.error = error.message || t('youtube.downloadError');
    }
    console.error('YouTube download error:', error);
  }
};

const importDownloadedFile = async (fileName: string, filePath: string) => {
  if (!currentProject.value) return;

  try {
    const { v4: uuidv4 } = await import('uuid');
    const { DEFAULT_AUDIO_ITEM } = await import('~/types/project');
    
    // Get audio duration
    const duration = await getAudioDuration(filePath);

    // Create audio item
    const uuid = uuidv4();
    const audioItem: any = {
      ...DEFAULT_AUDIO_ITEM,
      uuid,
      index: [currentProject.value.items.length],
      displayName: fileName.replace(/\.[^/.]+$/, ''), // Remove extension
      type: 'audio',
      mediaFileName: fileName,
      mediaPath: `media/${fileName}`, // Relative path
      waveformPath: `${currentProject.value.folderPath}/waveforms/${uuid}.json`,
      waveform: undefined,
      outPoint: duration,
      duration
    };

    // Add to playlist
    addItem(audioItem);
    
    // Generate waveform asynchronously
    generateWaveformAsync(audioItem);
  } catch (error) {
    console.error('Failed to import downloaded file:', error);
  }
};

const getAudioDuration = async (filePath: string): Promise<number> => {
  // Use Audio API to get duration
  return new Promise((resolve) => {
    const audio = new Audio(`file:///${filePath.replace(/\\/g, '/')}`);
    audio.addEventListener('loadedmetadata', () => {
      resolve(audio.duration);
    });
    audio.addEventListener('error', () => {
      resolve(60); // Default to 60 seconds if can't determine
    });
  });
};

const generateWaveformAsync = async (audioItem: any) => {
  if (!currentProject.value) return;
  
  try {
    const mediaPath = `${currentProject.value.folderPath}/media/${audioItem.mediaFileName}`;
    const result = await window.electronAPI.generateWaveform(mediaPath, audioItem.waveformPath);
    
    if (result.success) {
      const waveformFile = await window.electronAPI.readFile(audioItem.waveformPath);
      if (waveformFile.success && waveformFile.data) {
        audioItem.waveform = JSON.parse(waveformFile.data);
        console.log('Waveform loaded and applied to item');
        triggerWaveformUpdate();
        
        // Force a save to trigger reactivity
        const { saveProject } = useProject();
        await saveProject();
      }
    }
  } catch (error) {
    console.error('Failed to generate waveform:', error);
  }
};

const isDownloading = (videoId: string) => {
  return downloadQueue.value.some(d => d.videoId === videoId && d.status !== 'completed' && d.status !== 'error');
};

const getDownloadStatus = (download: DownloadProgress) => {
  switch (download.status) {
    case 'downloading':
      return t('youtube.statusDownloading');
    case 'converting':
      return t('youtube.statusConverting');
    case 'completed':
      return t('youtube.statusCompleted');
    case 'error':
      return download.error || t('youtube.statusError');
    default:
      return '';
  }
};

const closeModal = () => {
  emit('close');
  // Reset state
  searchQuery.value = '';
  searchResults.value = [];
  hasSearched.value = false;
  previewVideoId.value = '';
  selectedVideo.value = null;
};
</script>

<style scoped>
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.youtube-import-modal {
  background: var(--color-background);
  border-radius: 8px;
  width: 90%;
  max-width: 1200px;
  height: 80vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 4px 20px rgba(0, 0, 0, 0.5);
}

.modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 20px;
  border-bottom: 1px solid var(--color-border);
}

.modal-header h2 {
  margin: 0;
  color: var(--color-text-primary);
}

.close-btn {
  background: transparent;
  border: none;
  color: var(--color-text-secondary);
  cursor: pointer;
  padding: 4px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 4px;
  transition: background 0.2s;
}

.close-btn:hover {
  background: var(--color-hover);
}

.modal-body {
  flex: 1;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  padding: 20px;
  gap: 20px;
}

.search-section {
  flex-shrink: 0;
}

.search-bar {
  display: flex;
  gap: 10px;
}

.search-bar input {
  flex: 1;
  padding: 12px;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  background: var(--color-surface);
  color: var(--color-text-primary);
  font-size: 14px;
}

.search-btn {
  padding: 12px 24px;
  background: var(--color-accent);
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 8px;
  transition: opacity 0.2s;
}

.search-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.content-container {
  display: flex;
  gap: 20px;
  flex: 1;
  min-height: 0;
}

.results-section {
  flex: 1;
  overflow-y: auto;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  padding: 10px;
}

.preview-section {
  width: 400px;
  flex-shrink: 0;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--color-surface);
}

.preview-placeholder {
  text-align: center;
  color: var(--color-text-secondary);
  padding: 20px;
}

.preview-placeholder .material-symbols-rounded {
  font-size: 64px;
  opacity: 0.3;
  display: block;
  margin-bottom: 10px;
}

.preview-iframe {
  width: 100%;
  height: 100%;
  border-radius: 4px;
}

.loading-state,
.error-state,
.empty-state {
  text-align: center;
  padding: 40px;
  color: var(--color-text-secondary);
}

.loading-state .material-symbols-rounded,
.error-state .material-symbols-rounded,
.empty-state .material-symbols-rounded {
  font-size: 48px;
  display: block;
  margin-bottom: 10px;
}

.spinning {
  animation: spin 1s linear infinite;
}

@keyframes spin {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}

.results-list {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.video-item {
  display: flex;
  gap: 12px;
  padding: 12px;
  background: var(--color-surface);
  border-radius: 4px;
  border: 1px solid transparent;
  transition: border-color 0.2s;
}

.video-item.selected {
  border-color: var(--color-accent);
}

.video-thumbnail {
  width: 120px;
  height: 90px;
  object-fit: cover;
  border-radius: 4px;
  flex-shrink: 0;
}

.video-info {
  flex: 1;
  min-width: 0;
}

.video-title {
  margin: 0 0 4px 0;
  font-size: 14px;
  font-weight: 500;
  color: var(--color-text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  line-clamp: 2;
  -webkit-box-orient: vertical;
}

.video-channel,
.video-duration {
  margin: 0;
  font-size: 12px;
  color: var(--color-text-secondary);
}

.video-actions {
  display: flex;
  flex-direction: column;
  gap: 8px;
  flex-shrink: 0;
}

.action-btn {
  padding: 8px 12px;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  transition: opacity 0.2s;
  white-space: nowrap;
}

.preview-btn {
  background: var(--color-hover);
  color: var(--color-text-primary);
}

.download-btn {
  background: var(--color-accent);
  color: white;
}

.action-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.download-queue {
  border-top: 1px solid var(--color-border);
  padding-top: 20px;
  flex-shrink: 0;
}

.download-queue h3 {
  margin: 0 0 10px 0;
  font-size: 14px;
  color: var(--color-text-primary);
}

.queue-list {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.queue-item {
  background: var(--color-surface);
  padding: 12px;
  border-radius: 4px;
  display: flex;
  align-items: center;
  gap: 12px;
}

.queue-info {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.queue-info .video-title {
  font-size: 13px;
  color: var(--color-text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.queue-status {
  font-size: 11px;
  color: var(--color-text-secondary);
}

.progress-bar {
  width: 200px;
  height: 6px;
  background: var(--color-border);
  border-radius: 3px;
  overflow: hidden;
}

.progress-fill {
  height: 100%;
  background: var(--color-accent);
  transition: width 0.3s;
}

.progress-text {
  font-size: 12px;
  color: var(--color-text-secondary);
  width: 50px;
  text-align: right;
}
</style>
