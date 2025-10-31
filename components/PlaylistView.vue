<template>
  <div class="playlist-view">
    <div class="playlist-header">
      <h2>Playlist</h2>
      <div class="playlist-actions">
        <button class="action-btn" @click="handleImport">
          <span>Import Audio</span>
        </button>
        <button class="action-btn" @click="handleAddGroup">
          <span>Add Group</span>
        </button>
      </div>
    </div>
    
    <div class="playlist-content" @drop="handleDrop" @dragover.prevent>
      <div v-if="currentProject?.items.length === 0" class="empty-state">
        <p>No items in playlist</p>
        <p class="hint">Import audio files or drag them here</p>
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
  </div>
</template>

<script setup lang="ts">
import { v4 as uuidv4 } from 'uuid';
import type { AudioItem, GroupItem } from '~/types/project';
import { DEFAULT_AUDIO_ITEM, DEFAULT_GROUP_ITEM } from '~/types/project';

const { currentProject, addItem, updateIndices } = useProject();

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

    // Get audio duration and waveform data
    const duration = await getAudioDuration(destPath);
    const waveform = await generateSimpleWaveform(sourcePath);

    // Create audio item
    const audioItem: AudioItem = {
      ...DEFAULT_AUDIO_ITEM,
      uuid,
      index: [currentProject.value.items.length],
      displayName: fileName.replace(/\.[^/.]+$/, ''), // Remove extension
      type: 'audio',
      mediaFileName: fileName,
      mediaPath: destPath,
      waveformPath: `${currentProject.value.folderPath}/waveforms/${uuid}.json`,
      waveform, // Add waveform data directly
      outPoint: duration,
      duration
    } as AudioItem;

    addItem(audioItem);
  } catch (error) {
    console.error('Error importing audio:', error);
  }
};

const generateSimpleWaveform = async (filePath: string): Promise<number[]> => {
  try {
    // Read audio file data
    const audioData = await window.electronAPI.readAudioFile(filePath);
    if (!audioData.success || !audioData.data) {
      // Return simple placeholder waveform
      return Array(100).fill(0).map(() => Math.floor(Math.random() * 255));
    }
    
    // Downsample audio data to ~100 points for waveform
    const samples = audioData.data;
    const targetPoints = 100;
    const blockSize = Math.floor(samples.length / targetPoints);
    const waveform: number[] = [];
    
    for (let i = 0; i < targetPoints; i++) {
      const start = i * blockSize;
      const end = Math.min(start + blockSize, samples.length);
      
      // Find max absolute value in this block
      let max = 0;
      for (let j = start; j < end; j++) {
        max = Math.max(max, Math.abs(samples[j]));
      }
      
      waveform.push(Math.floor(max * 255));
    }
    
    return waveform;
  } catch (error) {
    console.error('Error generating waveform:', error);
    // Return simple placeholder waveform
    return Array(100).fill(0).map(() => Math.floor(Math.random() * 255));
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
    type: 'group'
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
    await importAudioFile(file.path);
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
  
  &:hover {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
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
