<template>
  <div 
    class="cart-slot"
    :class="{ 
      'has-item': hasItem, 
      'is-playing': isPlaying,
      'warning-yellow': warningState === 'yellow',
      'warning-orange': warningState === 'orange',
      'warning-red': warningState === 'red',
      'drag-over': isDragOver
    }"
    :style="slotStyle"
    @dragover.prevent="handleDragOver"
    @dragleave="handleDragLeave"
    @drop="handleDrop"
  >
    <div v-if="!hasItem" class="empty-slot" @click="handleImport">
      <span class="slot-number">{{ slot + 1 }}</span>
      <span class="slot-hint">{{ t('cart.clickToImport') }}</span>
    </div>
    
    <div 
      v-else 
      class="slot-content"
      draggable="true"
      @dragstart="handleDragStart"
      @dragend="handleDragEnd"
    >
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
        <button class="slot-btn play" @click.stop="handlePlay" :title="t('actions.play')">
          <span class="material-symbols-rounded">play_arrow</span>
        </button>
        <button class="slot-btn stop" @click.stop="handleStop" :title="t('actions.stop')" v-if="isPlaying">
          <span class="material-symbols-rounded">stop</span>
        </button>
        <button class="slot-btn edit" @click.stop="handleEdit" :title="t('actions.edit')">
          <span class="material-symbols-rounded">settings</span>
        </button>
        <button class="slot-btn delete" @click.stop="handleDelete" :title="t('actions.remove')">
          <span class="material-symbols-rounded">delete</span>
        </button>
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

const { currentProject, findItemByUuid, triggerWaveformUpdate } = useProject();
const { playCue, stopCue, activeCues } = useAudioEngine();
const { t } = useLocalization();
const { addCartOnlyItem, updateCartOnlyItem, removeCartOnlyItem } = useCartItems();

const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const currentTime = ref(0);
const duration = ref(0);
const playbackProgress = ref(0);
const warningState = ref<'yellow' | 'orange' | 'red' | null>(null);
const isDragOver = ref(false);

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
  
  try {
    // Extract filename from path
    const fileName = filePath.split(/[/\\]/).pop() || 'audio.wav';
    const mediaPath = `${currentProject.value.folderPath}/media/${fileName}`;
    
    // Copy file to project media folder
    const copyResult = await window.electronAPI.copyFile(filePath, mediaPath);
    if (!copyResult.success) {
      console.error('Failed to copy file:', copyResult.error);
      return;
    }
    
    // Get audio duration - use 60 seconds as temporary default
    // The actual duration will be detected when waveform is generated
    const duration = 60;
    
    // Create new audio item
    const { v4: uuidv4 } = await import('uuid');
    const { DEFAULT_CART_AUDIO_ITEM } = await import('~/types/project');
    
    const uuid = uuidv4();
    const waveformPath = `${currentProject.value.folderPath}/waveforms/${uuid}.json`;
    
    const newItem: AudioItem = {
      ...DEFAULT_CART_AUDIO_ITEM,
      uuid,
      type: 'audio' as const,
      displayName: fileName.replace(/\.[^/.]+$/, ''),
      mediaFileName: fileName,
      mediaPath: `media/${fileName}`, // Store relative path to project folder
      waveformPath,
      duration,
      outPoint: duration,
      waveform: undefined, // Will be generated asynchronously
      index: [-1] // Cart items don't have a real index
    } as AudioItem;
    
    // Store in cart-only items (NOT in project.items)
    addCartOnlyItem(newItem);
    
    // Assign to cart slot
    const existingIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);
    
    if (existingIndex !== -1) {
      currentProject.value.cartItems[existingIndex].itemUuid = uuid;
    } else {
      currentProject.value.cartItems.push({
        slot: props.slot,
        itemUuid: uuid
      });
    }
    
    // Save project
    const { saveProject } = useProject();
    await saveProject();
    
    // Generate waveform asynchronously using ffmpeg (non-blocking)
    // This will also get the correct duration
    generateWaveformForItem(newItem);
  } catch (error) {
    console.error('Error importing audio to cart:', error);
  }
};

const generateWaveformForItem = async (item: AudioItem) => {
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
            if (waveformData.peaks && waveformData.peaks.length && waveformData.duration) {
              item.waveform = waveformData;
              
              // Update duration from waveform data
              if (waveformData.duration) {
                item.duration = waveformData.duration;
                item.outPoint = waveformData.duration;
              }
              
              // Update the cart-only item with waveform data
              updateCartOnlyItem(item.uuid, item);
              
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
  
  // Remove from cart-only items
  removeCartOnlyItem(props.item.uuid);
  
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
  
  ctx.fillStyle = `rgb(${rgb[0]}, ${rgb[1]}, ${rgb[2]})`;
  
  const peaks = audioItem.waveform.peaks;
  
  // Calculate trimmed region if in/out points are set
  const totalDuration = audioItem.duration;
  const inPoint = audioItem.inPoint || 0;
  const outPoint = audioItem.outPoint || totalDuration;
  const trimmedDuration = outPoint - inPoint;
  
  // Calculate which peaks to show (slice based on in/out ratios)
  const startIndex = Math.floor((inPoint / totalDuration) * peaks.length);
  const endIndex = Math.ceil((outPoint / totalDuration) * peaks.length);
  const trimmedPeaks = peaks.slice(startIndex, endIndex);
  
  const barWidth = rect.width / trimmedPeaks.length;
  const centerY = rect.height / 2;
  
  // Apply volume scaling to waveform display
  const volumeMultiplier = audioItem.volume || 1.0;
  
  trimmedPeaks.forEach((value, i) => {
    // Values are already normalized 0-1, scale by volume
    const barHeight = value * rect.height * 0.8 * volumeMultiplier;
    const x = i * barWidth;
    const y = centerY - barHeight / 2;
    
    ctx.fillRect(x, y, Math.max(barWidth, 1), barHeight);
  });
};

// Watch for item changes and redraw
let resizeObserver: ResizeObserver | null = null;
let waveformPollInterval: NodeJS.Timeout | null = null;

onMounted(() => {
  if (props.item && waveformCanvas.value) {
    nextTick(drawWaveform);
    resizeObserver = new ResizeObserver(() => {
      drawWaveform();
    });
    resizeObserver.observe(waveformCanvas.value);
    
    // Start polling for waveform if not available yet
    const audioItem = props.item as AudioItem;
    if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) {
      startWaveformPolling();
    }
  }
});

onUnmounted(() => {
  if (resizeObserver && waveformCanvas.value) {
    resizeObserver.unobserve(waveformCanvas.value);
    resizeObserver.disconnect();
  }
  
  if (waveformPollInterval) {
    clearInterval(waveformPollInterval);
  }
});

// Poll for waveform data if not yet available
const startWaveformPolling = () => {
  if (waveformPollInterval) return; // Already polling
  
  waveformPollInterval = setInterval(async () => {
    if (!props.item || props.item.type !== 'audio') {
      clearInterval(waveformPollInterval!);
      waveformPollInterval = null;
      return;
    }
    
    const audioItem = props.item as AudioItem;
    
    // Check if waveform is now available
    if (audioItem.waveform && audioItem.waveform.peaks && audioItem.waveform.peaks.length > 0) {
      // Waveform loaded, stop polling and redraw
      clearInterval(waveformPollInterval!);
      waveformPollInterval = null;
      nextTick(drawWaveform);
      return;
    }
    
    // Try to load waveform from file
    if (window.electronAPI && audioItem.waveformPath) {
      try {
        const result = await window.electronAPI.readFile(audioItem.waveformPath);
        if (result.success && result.data) {
          const waveformData = JSON.parse(result.data);
          if (waveformData.peaks && waveformData.peaks.length && waveformData.duration) {
            audioItem.waveform = waveformData;
            
            // Update duration from waveform
            if (waveformData.duration) {
              audioItem.duration = waveformData.duration;
              audioItem.outPoint = waveformData.duration;
            }
            
            clearInterval(waveformPollInterval!);
            waveformPollInterval = null;
            nextTick(drawWaveform);
          }
        }
      } catch (error) {
        // Silently ignore, will retry on next poll
      }
    }
  }, 3000); // Poll every 3 seconds
  
  // Stop polling after 30 seconds
  setTimeout(() => {
    if (waveformPollInterval) {
      clearInterval(waveformPollInterval);
      waveformPollInterval = null;
    }
  }, 30000);
};

watch(() => props.item, (newItem, oldItem) => {
  if (newItem && newItem.type === 'audio') {
    nextTick(drawWaveform);
    
    // Start polling if waveform not available
    const audioItem = newItem as AudioItem;
    if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) {
      startWaveformPolling();
    }
  }
}, { deep: true });

const handleDragStart = (e: DragEvent) => {
  if (!e.dataTransfer || !props.item) return;
  
  // Set the drag data to include slot number and item UUID
  e.dataTransfer.effectAllowed = 'move';
  e.dataTransfer.setData('cart-slot', props.slot.toString());
  e.dataTransfer.setData('item-uuid', props.item.uuid);
};

const handleDragEnd = () => {
  isDragOver.value = false;
};

const handleDragOver = (e: DragEvent) => {
  e.preventDefault();
  if (!e.dataTransfer) return;
  
  // Check if it's a cart item being dragged
  const isCartDrag = e.dataTransfer.types.includes('cart-slot');
  const isFileDrag = e.dataTransfer.types.includes('Files');
  const isPlaylistDrag = e.dataTransfer.types.includes('item-uuid') && !isCartDrag;
  
  if (isCartDrag || isFileDrag || isPlaylistDrag) {
    isDragOver.value = true;
    e.dataTransfer.dropEffect = isCartDrag ? 'move' : 'copy';
  }
};

const handleDragLeave = () => {
  isDragOver.value = false;
};

const handleDrop = async (e: DragEvent) => {
  e.preventDefault();
  isDragOver.value = false;
  
  if (!e.dataTransfer || !currentProject.value) return;
  
  // Check if it's a cart item being reordered
  const sourceSlotStr = e.dataTransfer.getData('cart-slot');
  if (sourceSlotStr) {
    const sourceSlot = parseInt(sourceSlotStr);
    const targetSlot = props.slot;
    
    if (sourceSlot === targetSlot) return; // Same slot, do nothing
    
    // Get the source cart item
    const sourceIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === sourceSlot);
    if (sourceIndex === -1) return;
    
    const sourceCartItem = currentProject.value.cartItems[sourceIndex];
    
    // Check if target slot is occupied
    const targetIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === targetSlot);
    
    if (targetIndex === -1) {
      // Target slot is empty - simple move
      currentProject.value.cartItems[sourceIndex].slot = targetSlot;
    } else {
      // Target slot is occupied - push/insert behavior
      // Remove source item first
      currentProject.value.cartItems.splice(sourceIndex, 1);
      
      // Shift all items at target slot and beyond forward by 1
      for (const cartItem of currentProject.value.cartItems) {
        if (cartItem.slot >= targetSlot) {
          cartItem.slot += 1;
        }
      }
      
      // Insert source item at target slot
      currentProject.value.cartItems.push({
        slot: targetSlot,
        itemUuid: sourceCartItem.itemUuid
      });
    }
    
    // Save the project
    const { saveProject } = useProject();
    await saveProject();
    return;
  }
  
  // Check if it's a file drop
  if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
    const file = e.dataTransfer.files[0];
    // Check if it's an audio file
    if (file.type.startsWith('audio/') || /\.(mp3|wav|flac|ogg|m4a|aac)$/i.test(file.name)) {
      // In Electron, we can get the file path from the File object using webUtils
      if (window.electronAPI && window.electronAPI.getFilePath) {
        const filePath = window.electronAPI.getFilePath(file);
        if (filePath) {
          await importAudioFileToSlot(filePath);
          return;
        }
      }
    }
  }
  
  // Otherwise, check if it's an item UUID from the playlist
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
  
  &.drag-over {
    background-color: var(--color-accent);
    opacity: 0.5;
    transform: scale(1.05);
    border-color: var(--color-accent);
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

@keyframes flash-yellow {
  0%, 100% { border-color: var(--color-border); }
  50% { border-color: #FFC107; }
}

@keyframes flash-orange {
  0%, 100% { border-color: var(--color-border); }
  50% { border-color: #FF9800; }
}

@keyframes flash-red {
  0%, 100% { border-color: var(--color-border); }
  50% { border-color: #F44336; }
}

.empty-slot {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-sm);
  
  .slot-number {
    font-size: 32px;
    font-weight: 700;
    color: var(--color-text-primary);
  }
  
  .slot-hint {
    font-size: 12px;
    font-style: italic;
    color: var(--color-text-secondary);
    padding-left: 4px;
    text-align: center;
  }
}

.slot-content {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  position: relative;
  padding: var(--spacing-sm);
  cursor: move;
  
  &:active {
    cursor: grabbing;
  }
}

.slot-header {
  display: flex;
  align-items: center;
  gap: var(--spacing-xs);
  margin-bottom: var(--spacing-xs);
  cursor: pointer;
  
  .slot-number {
    font-size: 16px;
    font-weight: 700;
    color: var(--color-text-primary);
    min-width: 24px;
  }
  
  .slot-name {
    font-size: 14px;
    font-weight: 600;
    color: var(--color-text-primary);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    flex: 1;
  }
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
  justify-content: flex-end;
  z-index: 2;
  position: relative;
  
  button.slot-btn {
    width: 28px;
    height: 28px;
    border: 1px solid var(--color-border);
    border-radius: var(--border-radius-sm);
    background-color: var(--color-surface);
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
      border-color: var(--color-accent);
    }
    
    &.play {
      &:hover {
        background-color: var(--color-success);
        border-color: var(--color-success);
      }
    }
    
    &.stop {
      &:hover {
        background-color: var(--color-danger);
        border-color: var(--color-danger);
      }
    }
    
    &.delete {
      &:hover {
        background-color: var(--color-danger);
        border-color: var(--color-danger);
      }
    }
  }
}

.slot-waveform-area {
  flex: 1;
  display: flex;
  flex-direction: column;
  justify-content: flex-end;
  min-height: 30px;
}

.slot-time-info {
  display: flex;
  justify-content: space-between;
  font-size: 11px;
  color: var(--color-text-secondary);
  margin-bottom: var(--spacing-xs);
}

.cart-waveform-canvas {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  opacity: 0.3;
  pointer-events: none;
  z-index: 0;
}

.cart-progress {
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
