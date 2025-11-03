<template>
  <div class="playlist-view">
    <div class="playlist-header">
      <h2>{{ t('playlist.title') }}</h2>
      <div class="playlist-actions">
        <button class="action-btn" @click="handleImport" :disabled="!currentProject">
          <span class="material-symbols-rounded">audio_file</span>
          <span>{{ t('playlist.importAudio') }}</span>
        </button>
        <button class="action-btn youtube-btn" @click="showYouTubeModal = true" :disabled="!currentProject">
          <span class="material-symbols-rounded">youtube_activity</span>
          <span>{{ t('youtube.importFromYouTube') }}</span>
        </button>
        <button class="action-btn" @click="handleAddGroup" :disabled="!currentProject">
          <span class="material-symbols-rounded">folder</span>
          <span>{{ t('playlist.addGroup') }}</span>
        </button>
      </div>
    </div>
    
    <div class="playlist-content" @drop="handleDrop" @dragover.prevent">
      <div v-if="currentProject?.items.length === 0" class="empty-state">
        <p>{{ t('playlist.noItems') }}</p>
        <p class="hint">{{ t('playlist.importHint') }}</p>
      </div>
      
      <div v-else class="item-list">
        <PlaylistItem
          v-for="item in currentProject.items"
          :key="item.uuid"
          :item="item"
          :depth="0"
        />
      </div>
    </div>

    <!-- YouTube Import Modal -->
    <YouTubeImportModal :isOpen="showYouTubeModal" @close="showYouTubeModal = false" />
  </div>
</template>

<script setup lang="ts">
import { v4 as uuidv4 } from 'uuid';
import { ref } from 'vue';
import YouTubeImportModal from './YouTubeImportModal.vue';
import { triggerRef } from 'vue';
import type { AudioItem, GroupItem } from '~/types/project';
import { DEFAULT_AUDIO_ITEM, DEFAULT_GROUP_ITEM } from '~/types/project';

const { currentProject, addItem, updateIndices, saveProject, triggerWaveformUpdate } = useProject();
const { t } = useLocalization();

const showYouTubeModal = ref(false);

const handleImport = async () => {
  if (!import.meta.client || !window.electronAPI || !currentProject.value) return;

  const filePaths = await window.electronAPI.selectAudioFiles();
  if (!filePaths || filePaths.length === 0) return;

  for (const filePath of filePaths) {
    await importAudioFile(filePath);
  }
};

const importAudioFile = async (sourcePath: string) => {
  if (!currentProject.value) return;

  try {
    const fileName = sourcePath.split(/[\\/]/).pop() || 'audio.mp3';
    const uuid = uuidv4();
    const destPath = `${currentProject.value.folderPath}/media/${fileName}`;
    
    // Copy file to media folder
    const copyResult = await window.electronAPI.copyFile(sourcePath, destPath);
    if (!copyResult.success) {
      console.error('Failed to copy file:', copyResult.error);
      return;
    }

    // Get audio duration
    const duration = await getAudioDuration(destPath);

    // Create audio item WITHOUT waveform (will be generated async via ffmpeg)
    const audioItem: AudioItem = {
      ...DEFAULT_AUDIO_ITEM,
      uuid,
      index: [currentProject.value.items.length],
      displayName: fileName.replace(/\.[^/.]+$/, ''), // Remove extension
      type: 'audio',
      mediaFileName: fileName,
      mediaPath: `media/${fileName}`, // Store relative path to project folder
      waveformPath: `${currentProject.value.folderPath}/waveforms/${uuid}.json`,
      waveform: undefined, // Will be generated asynchronously
      outPoint: duration,
      duration
    } as AudioItem;

    // Add item immediately (no blocking)
    addItem(audioItem);
    
    // Generate waveform asynchronously using ffmpeg
    generateWaveformAsync(audioItem);
  } catch (error) {
    console.error('Error importing audio:', error);
  }
};

const generateWaveformAsync = async (item: AudioItem) => {
  try {
    if (!currentProject.value) return;
    
    // Ensure waveforms directory exists
    const waveformsDir = `${currentProject.value.folderPath}/waveforms`;
    await window.electronAPI.ensureDirectory(waveformsDir);
    
    // Check if waveform file already exists and is valid
    const existingWaveform = await window.electronAPI.readFile(item.waveformPath);
    if (existingWaveform.success && existingWaveform.data) {
      try {
        const waveformData = JSON.parse(existingWaveform.data);
        
        // Validate waveform format
        if (waveformData.peaks && waveformData.length && waveformData.duration) {
          item.waveform = waveformData;
          return;
        }
        console.warn('Invalid waveform format, regenerating...');
      } catch (e) {
        console.warn('Failed to parse existing waveform, regenerating...');
      }
    }
    
    // Check if generateWaveform is available
    if (!window.electronAPI.generateWaveform) {
      console.warn('generateWaveform not implemented yet - waveform will not be generated');
      console.info('Please implement the generateWaveform IPC handler in your Electron main process');
      return;
    }
    
    // Generate waveform using ffmpeg (non-blocking)
    const mediaPath = `${currentProject.value.folderPath}/media/${item.mediaFileName}`;
    const result = await window.electronAPI.generateWaveform(mediaPath, item.waveformPath);
    
    if (result.success) {
      // Start polling for waveform file (check every 3 seconds)
      const pollInterval = setInterval(async () => {
        try {
          const waveformFile = await window.electronAPI.readFile(item.waveformPath);
          if (waveformFile.success && waveformFile.data) {
            const waveformData = JSON.parse(waveformFile.data);
            
            // Validate waveform format
            if (waveformData.peaks && waveformData.length && waveformData.duration) {
              item.waveform = waveformData;
              
              // Force Vue reactivity update
              triggerWaveformUpdate();
              
              // Stop polling once loaded
              clearInterval(pollInterval);
              console.log(`Waveform loaded for ${item.displayName}`);
            }
          }
        } catch (error) {
          console.error('Error polling for waveform:', error);
        }
      }, 3000);
      
      // Stop polling after 30 seconds to prevent infinite polling
      setTimeout(() => {
        clearInterval(pollInterval);
      }, 30000);
    } else {
      console.error('Failed to generate waveform:', result.error);
    }
  } catch (error) {
    console.error('Error generating waveform:', error);
  }
};

const getAudioDuration = async (filePath: string): Promise<number> => {
  // Simplified - would use proper audio decoding
  return new Promise((resolve) => {
    if (import.meta.client) {
      const audio = new Audio(`file://${filePath}`);
      audio.addEventListener('loadedmetadata', () => {
        resolve(audio.duration);
      });
      audio.addEventListener('error', () => {
        resolve(60); // Default fallback
      });
    } else {
      resolve(60);
    }
  });
};

const handleAddGroup = () => {
  if (!currentProject.value) return;

  const groupItem: GroupItem = {
    ...DEFAULT_GROUP_ITEM,
    uuid: uuidv4(),
    index: [currentProject.value.items.length],
    displayName: 'New Group',
    type: 'group',
    children: [] // Create a new array for each group to avoid shared references
  } as GroupItem;

  addItem(groupItem);
};

const handleDrop = async (e: DragEvent) => {
  e.preventDefault();
  
  if (!e.dataTransfer) return;
  
  const files = Array.from(e.dataTransfer.files);
  const audioFiles = files.filter(file => 
    /\.(mp3|wav|ogg|flac|m4a|aac)$/i.test(file.name)
  );

  for (const file of audioFiles) {
    // Get the file path using webUtils in Electron
    if (window.electronAPI && window.electronAPI.getFilePath) {
      const filePath = window.electronAPI.getFilePath(file);
      if (filePath) {
        await importAudioFile(filePath);
      }
    }
  }
};
</script>

<style scoped>
.playlist-view {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  background-color: var(--color-background);
}

.playlist-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-md) var(--spacing-lg);
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
}

.playlist-header h2 {
  font-size: 18px;
  font-weight: 600;
}

.playlist-actions {
  display: flex;
  gap: var(--spacing-sm);
}

.action-btn {
  padding: var(--spacing-sm) var(--spacing-md);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  font-size: 13px;
  display: flex;
  align-items: center;
  gap: 6px;
  cursor: pointer;
  transition: all 0.2s;
  
  &:hover:not(:disabled) {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
  }
  
  &:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
}

.youtube-btn {
  background: linear-gradient(135deg, #FF0000 0%, #CC0000 100%);
  color: white;
  border-color: #FF0000;
  
  &:hover:not(:disabled) {
    background: linear-gradient(135deg, #CC0000 0%, #990000 100%);
    border-color: #CC0000;
  }
  
  &:disabled {
    background: linear-gradient(135deg, #666666 0%, #555555 100%);
    border-color: #666666;
  }
}

.playlist-content {
  flex: 1;
  overflow-y: auto;
  padding: var(--spacing-md);
}

.empty-state {
  text-align: center;
  padding: var(--spacing-xxl);
  color: var(--color-text-secondary);
  
  p {
    margin-bottom: var(--spacing-sm);
  }
  
  .hint {
    font-size: 13px;
    font-style: italic;
  }
}

.item-list {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
}
</style>
