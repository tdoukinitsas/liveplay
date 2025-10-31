<template>
  <div 
    class="cart-slot"
    :class="{ 'has-item': hasItem, 'is-playing': isPlaying }"
    :style="{ borderColor: item?.color }"
    @click="hasItem ? handlePlay() : handleImport()"
    @dragover.prevent
    @drop="handleDrop"
  >
    <div v-if="!hasItem" class="empty-slot">
      <span class="slot-number">{{ slot + 1 }}</span>
      <span class="slot-hint">Click to import or drag item here</span>
    </div>
    
    <div v-else class="slot-content">
      <button class="edit-btn" @click.stop="handleEdit">⚙</button>
      
      <div class="slot-info">
        <span class="slot-number">{{ slot + 1 }}</span>
        <span class="slot-name">{{ item.displayName }}</span>
      </div>
      
      <div class="slot-indicator" v-if="isPlaying">
        <span class="playing-dot"></span>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import type { BaseItem } from '~/types/project';

const props = defineProps<{
  slot: number;
  item: BaseItem | null;
}>();

const { currentProject, findItemByUuid, addItem } = useProject();
const { playCue, triggerGroup, activeCues } = useAudioEngine();

const hasItem = computed(() => props.item !== null);
const isPlaying = computed(() => props.item ? activeCues.value.has(props.item.uuid) : false);

const handleImport = async () => {
  if (!import.meta.client || !window.electronAPI || !currentProject.value) return;
  
  const filePaths = await window.electronAPI.selectAudioFiles();
  if (!filePaths || filePaths.length === 0) return;
  
  // Import first file to this slot
  const filePath = filePaths[0];
  await importAudioFileToSlot(filePath);
};

const importAudioFileToSlot = async (filePath: string) => {
  if (!currentProject.value) return;
  
  // Extract filename from path (browser-compatible)
  const fileName = filePath.split(/[/\\]/).pop() || 'audio.wav';
  const mediaPath = `${currentProject.value.folderPath}/media/${fileName}`;
  
  // Copy file to project media folder
  const copyResult = await window.electronAPI.copyFile(filePath, mediaPath);
  if (!copyResult.success) {
    console.error('Failed to copy file:', copyResult.error);
    return;
  }
  
  // Read audio file to get duration and waveform
  const audioData = await window.electronAPI.readAudioFile(filePath);
  if (!audioData.success || !audioData.data) {
    console.error('Failed to read audio file');
    return;
  }
  
  // Calculate duration (assuming 44100 Hz sample rate, stereo)
  const duration = audioData.data.length / 44100 / 2;
  
  // Generate simple waveform from audio data
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
  
  // Create new audio item
  const { v4: uuidv4 } = await import('uuid');
  const { DEFAULT_AUDIO_ITEM } = await import('~/types/project');
  
  const newItem: any = {
    ...DEFAULT_AUDIO_ITEM,
    uuid: uuidv4(),
    type: 'audio',
    displayName: fileName.replace(/\.[^/.]+$/, ''),
    mediaFileName: fileName,
    mediaPath: fileName,
    waveformPath: `waveforms/${uuidv4()}.json`,
    duration,
    outPoint: duration,
    waveform, // Add waveform data
    index: [currentProject.value.items.length]
  };
  
  // Add item to project
  addItem(newItem);
  
  // Assign to cart slot
  const existingIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);
  
  if (existingIndex !== -1) {
    currentProject.value.cartItems[existingIndex].itemUuid = newItem.uuid;
  } else {
    currentProject.value.cartItems.push({
      slot: props.slot,
      itemUuid: newItem.uuid
    });
  }
  
  // Save project
  const { saveProject } = useProject();
  saveProject();
};

const handlePlay = () => {
  if (!props.item) return;
  
  if (props.item.type === 'audio') {
    playCue(props.item as any);
  } else if (props.item.type === 'group') {
    triggerGroup(props.item);
  }
};

const handleEdit = () => {
  // Open properties panel for this item
  const { selectedItem } = useProject();
  selectedItem.value = props.item;
};

const handleDrop = (e: DragEvent) => {
  e.preventDefault();
  
  if (!e.dataTransfer || !currentProject.value) return;
  
  const itemUuid = e.dataTransfer.getData('item-uuid');
  if (!itemUuid) return;
  
  // Add or replace cart item
  const existingIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);
  
  if (existingIndex !== -1) {
    currentProject.value.cartItems[existingIndex].itemUuid = itemUuid;
  } else {
    currentProject.value.cartItems.push({
      slot: props.slot,
      itemUuid
    });
  }
  
  // Save the project
  const { saveProject } = useProject();
  saveProject();
};
</script>

<style scoped>
.cart-slot {
  min-height: 100px;
  border: 3px solid var(--color-border);
  border-radius: var(--border-radius-md);
  background-color: var(--color-surface);
  cursor: pointer;
  transition: all var(--transition-fast);
  position: relative;
  overflow: hidden;
  
  &:hover {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
    transform: scale(1.02);
  }
  
  &.has-item {
    border-width: 4px;
  }
  
  &.is-playing {
    animation: pulse 1s ease-in-out infinite;
  }
}

@keyframes pulse {
  0%, 100% {
    box-shadow: 0 0 0 0 var(--color-accent);
  }
  50% {
    box-shadow: 0 0 0 4px var(--color-accent);
  }
}

.empty-slot {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-sm);
  color: var(--color-text-secondary);
}

.slot-content {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  padding: var(--spacing-md);
  position: relative;
}

.edit-btn {
  position: absolute;
  top: var(--spacing-sm);
  right: var(--spacing-sm);
  width: 28px;
  height: 28px;
  border-radius: 50%;
  background-color: var(--color-background);
  display: flex;
  align-items: center;
  justify-content: center;
  opacity: 0;
  transition: opacity var(--transition-fast);
  
  .cart-slot:hover & {
    opacity: 1;
  }
  
  &:hover {
    background-color: var(--color-accent);
    color: white;
  }
}

.slot-info {
  flex: 1;
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  gap: var(--spacing-sm);
}

.slot-number {
  font-size: 24px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.slot-name {
  font-size: 14px;
  font-weight: 500;
  text-align: center;
  overflow: hidden;
  text-overflow: ellipsis;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
}

.slot-hint {
  font-size: 12px;
  font-style: italic;
}

.slot-indicator {
  position: absolute;
  bottom: var(--spacing-sm);
  left: 50%;
  transform: translateX(-50%);
}

.playing-dot {
  display: block;
  width: 12px;
  height: 12px;
  border-radius: 50%;
  background-color: var(--color-success);
  animation: blink 1s ease-in-out infinite;
}

@keyframes blink {
  0%, 100% {
    opacity: 1;
  }
  50% {
    opacity: 0.3;
  }
}
</style>
