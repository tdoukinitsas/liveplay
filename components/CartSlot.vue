<template>
  <div 
    class="cart-slot"
    :class="{ 
      'has-item': hasItem, 
      'is-playing': isPlaying,
      'is-warning': warningState
    }"
    :style="slotStyle"
    @dragover.prevent
    @drop="handleDrop"
  >
    <div v-if="!hasItem" class="empty-slot" @click="handleImport">
      <span class="slot-number">{{ slot + 1 }}</span>
      <span class="slot-hint">Click to import or drag item here</span>
    </div>
    
    <div v-else class="slot-content">
      <!-- Waveform canvas -->
      <canvas 
        v-if="item.type === 'audio' && item.waveform"
        ref="waveformCanvas"
        class="cart-waveform-canvas"
      ></canvas>
      
      <!-- Progress overlay -->
      <div v-if="isPlaying" class="cart-progress" :style="progressStyle"></div>
      
      <!-- Item info section -->
      <div class="slot-header" @click="handlePlay">
        <span class="slot-number">{{ slot + 1 }}</span>
        <span class="slot-name">{{ item.displayName }}</span>
      </div>
      
      <!-- Waveform/Progress section at bottom -->
      <div class="slot-waveform-area">
        <!-- Progress info -->
        <div v-if="isPlaying" class="slot-time-info">
          <span>{{ formatTime(currentTime) }}</span>
          <span>-{{ formatTime(duration - currentTime) }}</span>
        </div>
      </div>
      
      <!-- Action buttons -->
      <div class="slot-actions">
        <button class="slot-btn play" @click.stop="handlePlay" title="Play">▶</button>
        <button class="slot-btn stop" @click.stop="handleStop" title="Stop" v-if="isPlaying">⏹</button>
        <button class="slot-btn edit" @click.stop="handleEdit" title="Edit">⚙</button>
        <button class="slot-btn delete" @click.stop="handleDelete" title="Remove">×</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { triggerRef } from 'vue';
import type { AudioItem } from '~/types/project';

const props = defineProps<{
  slot: number;
  item: AudioItem | null;
}>();

const { currentProject, findItemByUuid, addItem, triggerWaveformUpdate } = useProject();
const { playCue, stopCue, activeCues } = useAudioEngine();

const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const currentTime = ref(0);
const duration = ref(0);
const playbackProgress = ref(0);
const warningState = ref<'yellow' | 'orange' | 'red' | null>(null);

const hasItem = computed(() => props.item !== null);
const isPlaying = computed(() => props.item ? activeCues.value.has(props.item.uuid) : false);

// Helper to convert hex to rgba
const hexToRgba = (hex: string, alpha: number) => {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
};

const slotStyle = computed(() => {
  if (!props.item) return {};
  
  const styles: any = {
    borderColor: props.item.color
  };
  
  if (isPlaying.value) {
    styles.backgroundColor = hexToRgba(props.item.color, 0.3);
  } else {
    styles.backgroundColor = hexToRgba(props.item.color, 0.15);
  }
  
  return styles;
});

const progressStyle = computed(() => {
  if (!props.item) return {};
  return {
    width: `${playbackProgress.value}%`,
    backgroundColor: hexToRgba(props.item.color, 0.5),
  };
});

// Watch for playback
let progressInterval: any = null;
watch(isPlaying, (playing) => {
  if (playing && props.item) {
    const cue = activeCues.value.get(props.item.uuid);
    if (cue) {
      duration.value = cue.duration;
      progressInterval = setInterval(() => {
        if (!props.item) return;
        const cue = activeCues.value.get(props.item.uuid);
        if (cue) {
          currentTime.value = cue.currentTime;
          duration.value = cue.duration;
          playbackProgress.value = duration.value > 0 ? (currentTime.value / duration.value) * 100 : 0;
          
          // Update warning state based on time remaining
          const timeRemaining = duration.value - currentTime.value;
          if (timeRemaining <= 5) {
            warningState.value = 'red';
          } else if (timeRemaining <= 10) {
            warningState.value = 'orange';
          } else if (timeRemaining <= 30) {
            warningState.value = 'yellow';
          } else {
            warningState.value = null;
          }
        }
      }, 100);
    }
  } else {
    if (progressInterval) {
      clearInterval(progressInterval);
      progressInterval = null;
    }
    playbackProgress.value = 0;
    currentTime.value = 0;
    warningState.value = null;
  }
});

onUnmounted(() => {
  if (progressInterval) {
    clearInterval(progressInterval);
  }
});

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
  
  // Read audio file to get duration
  const audioData = await window.electronAPI.readAudioFile(filePath);
  if (!audioData.success || !audioData.data) {
    console.error('Failed to read audio file');
    return;
  }
  
  // Calculate duration (assuming 44100 Hz sample rate, stereo)
  const duration = audioData.data.length / 44100 / 2;
  
  // Create new audio item
  const { v4: uuidv4 } = await import('uuid');
  const { DEFAULT_AUDIO_ITEM } = await import('~/types/project');
  
  const uuid = uuidv4();
  const waveformPath = `${currentProject.value.folderPath}/waveforms/${uuid}.json`;
  
  const newItem: any = {
    ...DEFAULT_AUDIO_ITEM,
    uuid,
    type: 'audio',
    displayName: fileName.replace(/\.[^/.]+$/, ''),
    mediaFileName: fileName,
    mediaPath: fileName,
    waveformPath,
    duration,
    outPoint: duration,
    waveform: undefined, // Will be generated asynchronously
    index: [currentProject.value.items.length]
  };
  
  // Add item to project
  addItem(newItem);
  
  // Generate waveform asynchronously using ffmpeg
  generateWaveformForItem(newItem);
  
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

const generateWaveformForItem = async (item: any) => {
  try {
    if (!currentProject.value) return;
    
    // Check if generateWaveform is available
    if (!window.electronAPI.generateWaveform) {
      console.warn('generateWaveform not implemented yet - waveform will not be generated');
      return;
    }
    
    const mediaPath = `${currentProject.value.folderPath}/media/${item.mediaFileName}`;
    
    // Generate waveform using ffmpeg (non-blocking)
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
              nextTick(drawWaveform);
              
              // Stop polling once loaded
              clearInterval(pollInterval);
              console.log(`Waveform loaded for cart slot ${props.slot + 1}`);
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

const handlePlay = () => {
  if (!props.item) return;
  playCue(props.item);
};

const handleStop = () => {
  if (!props.item) return;
  stopCue(props.item.uuid);
};

const handleDelete = () => {
  if (!currentProject.value || !props.item) return;
  
  // Remove from cart
  const index = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);
  if (index !== -1) {
    currentProject.value.cartItems.splice(index, 1);
    const { saveProject } = useProject();
    saveProject();
  }
};

const handleEdit = () => {
  // Open properties panel for this item
  const { selectedItem } = useProject();
  selectedItem.value = props.item;
};

const formatTime = (seconds: number): string => {
  const mins = Math.floor(seconds / 60);
  const secs = Math.floor(seconds % 60);
  return `${mins}:${secs.toString().padStart(2, '0')}`;
};

// Draw waveform
const drawWaveform = () => {
  if (!waveformCanvas.value || !props.item || props.item.type !== 'audio') return;
  
  const audioItem = props.item as AudioItem;
  if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) return;
  
  const canvas = waveformCanvas.value;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  
  // Set canvas size to match element size
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  ctx.scale(dpr, dpr);
  
  ctx.clearRect(0, 0, rect.width, rect.height);
  
  // Use text color for waveform
  const computedStyle = getComputedStyle(canvas);
  const textColor = computedStyle.getPropertyValue('color');
  const rgb = textColor.match(/\d+/g);
  if (!rgb) return;
  
  ctx.fillStyle = `rgba(${rgb[0]}, ${rgb[1]}, ${rgb[2]}, 0.3)`;
  
  const peaks = audioItem.waveform.peaks;
  const barWidth = rect.width / peaks.length;
  const centerY = rect.height / 2;
  
  peaks.forEach((value, i) => {
    // Values are already normalized 0-1
    const barHeight = value * rect.height * 0.8;
    const x = i * barWidth;
    const y = centerY - barHeight / 2;
    
    ctx.fillRect(x, y, Math.max(barWidth, 1), barHeight);
  });
};

// Watch for item changes and redraw
let resizeObserver: ResizeObserver | null = null;

onMounted(() => {
  if (props.item && waveformCanvas.value) {
    nextTick(drawWaveform);
    resizeObserver = new ResizeObserver(() => {
      drawWaveform();
    });
    resizeObserver.observe(waveformCanvas.value);
  }
});

watch(() => props.item, () => {
  if (props.item && props.item.type === 'audio') {
    nextTick(drawWaveform);
  }
}, { deep: true });

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

<style scoped lang="scss">
.cart-slot {
  min-height: 120px;
  border: 3px solid var(--color-border);
  border-radius: var(--border-radius-md);
  background-color: var(--color-surface);
  cursor: pointer;
  transition: all var(--transition-fast);
  position: relative;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  
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
  
  &.warning-yellow {
    animation: flash-yellow 2s ease-in-out infinite;
  }
  
  &.warning-orange {
    animation: flash-orange 1s ease-in-out infinite;
  }
  
  &.warning-red {
    animation: flash-red 0.5s ease-in-out infinite;
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

@keyframes flash-yellow {
  0%, 100% { background-color: var(--color-surface); }
  50% { background-color: rgba(255, 193, 7, 0.2); }
}

@keyframes flash-orange {
  0%, 100% { background-color: var(--color-surface); }
  50% { background-color: rgba(255, 152, 0, 0.3); }
}

@keyframes flash-red {
  0%, 100% { background-color: var(--color-surface); }
  50% { background-color: rgba(244, 67, 54, 0.4); }
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
  position: relative;
  padding: var(--spacing-sm);
}

.content-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  margin-bottom: var(--spacing-xs);
}

.slot-title {
  flex: 1;
  
  .slot-number {
    font-size: 12px;
    font-weight: 600;
    color: var(--color-text-secondary);
    margin-bottom: 2px;
  }
  
  .slot-name {
    font-size: 14px;
    font-weight: 600;
    color: var(--color-text-primary);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
}

.slot-actions {
  display: flex;
  gap: 4px;
  opacity: 0;
  transition: opacity var(--transition-fast);
  
  .cart-slot:hover & {
    opacity: 1;
  }
  
  button {
    width: 24px;
    height: 24px;
    border: none;
    border-radius: var(--border-radius-sm);
    background-color: var(--color-background);
    color: var(--color-text-primary);
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 14px;
    transition: all var(--transition-fast);
    
    &:hover {
      background-color: var(--color-accent);
      color: white;
    }
  }
}

.waveform-container {
  flex: 1;
  position: relative;
  min-height: 40px;
  margin-bottom: var(--spacing-xs);
  
  canvas {
    width: 100%;
    height: 100%;
    display: block;
  }
}

.progress-overlay {
  position: absolute;
  top: 0;
  left: 0;
  bottom: 0;
  pointer-events: none;
  z-index: 1;
}

.content-footer {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 11px;
  color: var(--color-text-secondary);
}

.slot-number {
  font-size: 24px;
  font-weight: 700;
  color: var(--color-text-primary);
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
