<template>
  <div 
    class="playlist-item"
    :class="{ 
      'is-selected': isSelected, 
      'is-group': item.type === 'group',
      'is-audio': item.type === 'audio',
      'is-playing': isPlaying,
      'drag-over-top': dragPosition === 'top',
      'drag-over-bottom': dragPosition === 'bottom',
      'drag-over-group': dragPosition === 'group',
      'warning-yellow': warningState === 'yellow',
      'warning-orange': warningState === 'orange',
      'warning-red': warningState === 'red'
    }"
    :style="itemStyle"
    @dragover="handleDragOver"
    @dragleave="handleDragLeave"
    @drop="handleDrop"
  >
    <!-- Waveform background for audio items -->
    <canvas 
      v-if="item.type === 'audio' && item.waveform"
      ref="waveformCanvas"
      class="waveform-canvas"
    ></canvas>
    
    <div 
      class="item-content"
      @click="handleSelect"
      :draggable="true"
      @dragstart="handleDragStart"
    >
      <!-- Progress bar for playing items (audio and groups) - only in header -->
      <div v-if="(isPlaying && item.type === 'audio') || (isGroupPlaying && item.type === 'group')" class="item-progress" :style="progressStyle"></div>
      
      <div class="item-left">
        <button 
          v-if="item.type === 'group'" 
          class="expand-btn"
          @click.stop="toggleExpand"
        >
          <span class="material-symbols-rounded">{{ isExpanded ? 'expand_more' : 'chevron_right' }}</span>
        </button>
        
        <span class="item-index">{{ indexDisplay }}</span>
        
        <span v-if="item.type === 'group'" class="item-icon">
          <span class="material-symbols-rounded">folder</span>
        </span>
        
        <span class="item-name">{{ item.displayName }}</span>

        <div class="item-actions">
        <button 
          v-if="!isPlaying"
          class="item-btn play" 
          @click.stop="handlePlay" 
          :title="t('actions.play')"
        >
          <span class="material-symbols-rounded">play_arrow</span>
        </button>
        <button 
          v-if="isPlaying && !isPaused" 
          class="item-btn pause" 
          @click.stop="handlePause" 
          :title="t('actions.pause')"
        >
          <span class="material-symbols-rounded">pause</span>
        </button>
        <button 
          v-if="isPlaying && isPaused" 
          class="item-btn resume" 
          @click.stop="handleResume" 
          :title="t('actions.resume')"
        >
          <span class="material-symbols-rounded">play_arrow</span>
        </button>
        <button class="item-btn stop" @click.stop="handleStop" :title="t('actions.stop')" v-if="isPlaying">
          <span class="material-symbols-rounded">stop</span>
        </button>
        <button class="item-btn delete" @click.stop="handleDelete" :title="t('actions.delete')">
          <span class="material-symbols-rounded">delete</span>
        </button>
      </div>
        
        <!-- Behavior indicators (for audio items) -->
        <div v-if="item.type === 'audio'" class="behavior-indicators">
          <!-- Start behavior -->
          <span 
            v-if="item.startBehavior?.action === 'play-next'" 
            class="material-symbols-rounded behavior-icon"
            :title="`Start: Play Next`"
          >skip_next</span>
          <span 
            v-else-if="item.startBehavior?.action === 'play-item' || item.startBehavior?.action === 'play-index'" 
            class="material-symbols-rounded behavior-icon"
            :title="`Start: Play ${item.startBehavior?.action === 'play-item' ? 'Item' : 'Index'}`"
          >arrow_forward</span>
          
          <!-- Ducking behavior -->
          <span 
            v-if="item.duckingBehavior?.mode === 'duck-others'" 
            class="material-symbols-rounded behavior-icon"
            :title="`Ducking: Duck Others`"
          >volume_down</span>
          
          <!-- End behavior -->
          <span 
            v-if="item.endBehavior?.action === 'next'" 
            class="material-symbols-rounded behavior-icon"
            :title="`End: Play Next`"
          >skip_next</span>
          <span 
            v-else-if="item.endBehavior?.action === 'goto-item' || item.endBehavior?.action === 'goto-index'" 
            class="material-symbols-rounded behavior-icon"
            :title="`End: Go To ${item.endBehavior?.action === 'goto-item' ? 'Item' : 'Index'}`"
          >arrow_forward</span>
          <span 
            v-else-if="item.endBehavior?.action === 'loop'" 
            class="material-symbols-rounded behavior-icon"
            :title="`End: Loop`"
          >replay</span>
        </div>
        
        <span v-if="item.type === 'audio'" class="item-duration">{{ durationDisplay }}</span>
      </div>
      
      
    </div>
    
    <div v-if="item.type === 'group' && isExpanded && item.children.length > 0" class="group-children">
      <PlaylistItem
        v-for="child in item.children"
        :key="child.uuid"
        :item="child"
        :depth="depth + 1"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import type { AudioItem, GroupItem, BaseItem } from '~/types/project';

const props = defineProps<{
  item: AudioItem | GroupItem;
  depth: number;
}>();

const { selectedItem, selectedItems, toggleItemSelection, removeItem, findItemByUuid, currentProject, waveformUpdateKey, triggerWaveformUpdate } = useProject();
const { playCue, stopCue, pauseCue, resumeCue, activeCues, activeGroups, triggerGroup } = useAudioEngine();
const { t } = useLocalization();

const isExpanded = ref(props.item.type === 'group' ? props.item.isExpanded : false);
const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const dragPosition = ref<'top' | 'bottom' | 'group' | null>(null);

const isSelected = computed(() => selectedItems.value.has(props.item.uuid));
const isPlaying = computed(() => activeCues.value.has(props.item.uuid));
const isPaused = computed(() => {
  const cue = activeCues.value.get(props.item.uuid);
  return cue?.isPaused || false;
});
const isGroupPlaying = computed(() => props.item.type === 'group' && activeGroups.value.has(props.item.uuid));

const indexDisplay = computed(() => {
  return props.item.index.join(',');
});

const durationDisplay = computed(() => {
  if (props.item.type !== 'audio') return '';
  
  const audioItem = props.item as AudioItem;
  
  // If playing, show countdown
  if (isPlaying.value) {
    const timeRemaining = playbackDuration.value - currentPlaybackTime.value;
    const totalSeconds = Math.floor(timeRemaining);
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    
    if (hours > 0) {
      return `-${hours}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`;
    } else {
      return `-${minutes}:${seconds.toString().padStart(2, '0')}`;
    }
  }
  
  // Otherwise show trimmed duration
  const totalDuration = audioItem.duration;
  const inPoint = audioItem.inPoint || 0;
  const outPoint = audioItem.outPoint || totalDuration;
  const trimmedDuration = outPoint - inPoint;
  
  const totalSeconds = Math.floor(trimmedDuration);
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  
  if (hours > 0) {
    return `${hours}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`;
  } else {
    return `${minutes}:${seconds.toString().padStart(2, '0')}`;
  }
});

// Draw waveform
const drawWaveform = () => {
  if (!waveformCanvas.value || props.item.type !== 'audio') return;
  
  const audioItem = props.item as AudioItem;
  if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) return;
  
  const canvas = waveformCanvas.value;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  
  // Set canvas size to match element size (use actual pixels for clarity)
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  ctx.scale(dpr, dpr);
  
  // Clear canvas
  ctx.clearRect(0, 0, rect.width, rect.height);
  
  // Get computed text color for waveform
  const computedStyle = getComputedStyle(canvas);
  const textColor = computedStyle.getPropertyValue('color');
  
  // Parse RGB values - use full opacity, canvas element itself has opacity set via CSS
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
    const barHeight = value * rect.height * 0.8 * volumeMultiplier; // Use 80% of height, scaled by volume
    const x = i * barWidth;
    const y = centerY - barHeight / 2;
    
    ctx.fillRect(x, y, Math.max(barWidth, 1), barHeight);
  });
};

// Redraw waveform when component mounts or updates
let resizeObserver: ResizeObserver | null = null;
let waveformPollInterval: NodeJS.Timeout | null = null;

// Watch for canvas availability and set up observer
watch(waveformCanvas, (canvas) => {
  if (canvas && props.item.type === 'audio') {
    // Canvas is now available, draw waveform
    nextTick(drawWaveform);
    
    // Set up resize observer if not already set up
    if (!resizeObserver) {
      resizeObserver = new ResizeObserver(() => {
        drawWaveform();
      });
      resizeObserver.observe(canvas);
    }
  }
});

onMounted(() => {
  if (props.item.type === 'audio' && waveformCanvas.value) {
    nextTick(drawWaveform);
    
    // Redraw on resize
    resizeObserver = new ResizeObserver(() => {
      drawWaveform();
    });
    resizeObserver.observe(waveformCanvas.value);
  }
  
  // Start polling for waveform if not available yet
  if (props.item.type === 'audio') {
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
    if (props.item.type !== 'audio') {
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
          if (waveformData.peaks && waveformData.peaks.length > 0) {
            // Find the actual item in the project to ensure reactivity
            const projectItem = findItemByUuid(audioItem.uuid);
            if (projectItem && projectItem.type === 'audio') {
              projectItem.waveform = waveformData;
              
              // Update duration from waveform data if available (more accurate than Audio API)
              if (waveformData.duration && waveformData.duration > 0) {
                projectItem.duration = waveformData.duration;
                projectItem.outPoint = waveformData.duration;
              }
              
              // Save the project to persist changes
              const { saveProject } = useProject();
              await saveProject();
            }
            
            clearInterval(waveformPollInterval!);
            waveformPollInterval = null;
            
            // Force reactivity update
            triggerWaveformUpdate();
            nextTick(drawWaveform);
            console.log(`Waveform polling found data for ${audioItem.displayName}`);
          }
        }
      } catch (error) {
        // Silently ignore, will retry on next poll
      }
    }
  }, 2000); // Poll every 2 seconds
  
  // Stop polling after 30 seconds
  setTimeout(() => {
    if (waveformPollInterval) {
      clearInterval(waveformPollInterval);
      waveformPollInterval = null;
    }
  }, 30000);
};

watch(() => props.item, () => {
  if (props.item.type === 'audio') {
    nextTick(drawWaveform);
  }
}, { deep: true });

// Watch for waveform updates
watch(() => waveformUpdateKey.value, () => {
  if (props.item.type === 'audio') {
    nextTick(drawWaveform);
  }
});

// Calculate playback progress
const playbackProgress = ref(0);
const currentPlaybackTime = ref(0);
const playbackDuration = ref(0);
let progressInterval: any = null;

// Warning state based on time remaining
const warningState = computed(() => {
  if (!isPlaying.value || props.item.type !== 'audio') return null;
  
  const cue = activeCues.value.get(props.item.uuid);
  if (!cue) return null;
  
  const timeRemaining = cue.duration - cue.currentTime;
  if (timeRemaining <= 5) return 'red';
  if (timeRemaining <= 10) return 'orange';
  if (timeRemaining <= 30) return 'yellow';
  return null;
});

watch(isPlaying, (playing) => {
  if (playing && props.item.type === 'audio') {
    const cue = activeCues.value.get(props.item.uuid);
    if (cue) {
      progressInterval = setInterval(() => {
        // Now we get currentTime directly from the cue state updated by IPC events
        const current = cue.currentTime;
        const duration = cue.duration;
        currentPlaybackTime.value = current;
        playbackDuration.value = duration;
        playbackProgress.value = duration > 0 ? Math.min((current / duration) * 100, 100) : 0;
      }, 100);
    }
  } else {
    if (progressInterval) {
      clearInterval(progressInterval);
      progressInterval = null;
    }
    playbackProgress.value = 0;
    currentPlaybackTime.value = 0;
    playbackDuration.value = 0;
  }
});

// Watch for group playing state
watch(isGroupPlaying, (playing) => {
  if (playing && props.item.type === 'group') {
    const groupState = activeGroups.value.get(props.item.uuid);
    if (groupState) {
      progressInterval = setInterval(() => {
        const state = activeGroups.value.get(props.item.uuid);
        if (state) {
          const current = state.currentTime;
          const duration = state.totalDuration;
          playbackProgress.value = duration > 0 ? Math.min((current / duration) * 100, 100) : 0;
        }
      }, 100);
    }
  } else if (!playing && props.item.type === 'group') {
    if (progressInterval) {
      clearInterval(progressInterval);
      progressInterval = null;
    }
    playbackProgress.value = 0;
  }
});

onUnmounted(() => {
  if (progressInterval) {
    clearInterval(progressInterval);
  }
});

// Helper to convert hex to rgba
const hexToRgba = (hex: string, alpha: number) => {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
};

const itemStyle = computed(() => {
  const styles: any = {
    marginLeft: `${props.depth * 24}px`,
  };
  
  if (isPlaying.value || isGroupPlaying.value) {
    // Playing: 50% opacity background
    styles.backgroundColor = hexToRgba(props.item.color, 0.5);
  } else {
    // Inactive: 25% opacity background
    styles.backgroundColor = hexToRgba(props.item.color, 0.25);
  }
  
  return styles;
});

const progressStyle = computed(() => {
  return {
    width: `${playbackProgress.value}%`,
    backgroundColor: hexToRgba(props.item.color, 0.75),
  };
});

const handleSelect = (event: MouseEvent) => {
  toggleItemSelection(props.item.uuid, event.ctrlKey || event.metaKey, event.shiftKey);
};

const handlePlay = () => {
  if (props.item.type === 'audio') {
    playCue(props.item as AudioItem);
  } else if (props.item.type === 'group') {
    triggerGroup(props.item);
  }
};

const handleStop = () => {
  stopCue(props.item.uuid);
};

const handlePause = () => {
  pauseCue(props.item.uuid);
};

const handleResume = () => {
  resumeCue(props.item.uuid);
};

const handleDelete = () => {
  if (confirm(`Delete "${props.item.displayName}"?`)) {
    removeItem(props.item.uuid);
  }
};

const toggleExpand = () => {
  if (props.item.type === 'group') {
    isExpanded.value = !isExpanded.value;
    props.item.isExpanded = isExpanded.value;
  }
};

const handleDragStart = (e: DragEvent) => {
  if (e.dataTransfer) {
    e.dataTransfer.effectAllowed = 'move';
    e.dataTransfer.setData('item-uuid', props.item.uuid);
    e.dataTransfer.setData('item-depth', props.depth.toString());
    
    // If this item is part of a multi-selection, store all selected UUIDs
    if (selectedItems.value.has(props.item.uuid) && selectedItems.value.size > 1) {
      const selectedUuids = Array.from(selectedItems.value);
      e.dataTransfer.setData('selected-items', JSON.stringify(selectedUuids));
    }
  }
};

const handleDragOver = (e: DragEvent) => {
  e.preventDefault();
  if (!e.dataTransfer) return;
  
  e.dataTransfer.dropEffect = 'move';
  
  // Determine drop position based on mouse position
  const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
  const y = e.clientY - rect.top;
  const height = rect.height;
  
  if (props.item.type === 'group' && y > height * 0.3 && y < height * 0.7) {
    // Middle third of group = drop inside
    dragPosition.value = 'group';
  } else if (y < height / 2) {
    // Top half = insert before
    dragPosition.value = 'top';
  } else {
    // Bottom half = insert after
    dragPosition.value = 'bottom';
  }
};

const handleDragLeave = () => {
  dragPosition.value = null;
};

const handleDrop = (e: DragEvent) => {
  e.preventDefault();
  e.stopPropagation();
  
  dragPosition.value = null;
  
  if (!e.dataTransfer || !currentProject.value) return;
  
  const draggedUuid = e.dataTransfer.getData('item-uuid');
  if (!draggedUuid || draggedUuid === props.item.uuid) return;
  
  // Check if we're dragging multiple items
  const selectedItemsData = e.dataTransfer.getData('selected-items');
  const itemsToMove: string[] = selectedItemsData 
    ? JSON.parse(selectedItemsData) 
    : [draggedUuid];
  
  // Don't drop onto one of the items being moved
  if (itemsToMove.includes(props.item.uuid)) return;
  
  // Collect all items to move (in their current order)
  const allProjectItems = getAllItemsFlattened(currentProject.value.items);
  const itemObjects = itemsToMove
    .map(uuid => findItemByUuid(uuid))
    .filter(item => item !== null);
  
  if (itemObjects.length === 0) return;
  
  // Remove all items from their current locations (in reverse order to maintain indices)
  for (let i = itemObjects.length - 1; i >= 0; i--) {
    const item = itemObjects[i];
    if (item) {
      removeItem(item.uuid);
    }
  }
  
  // Determine insertion point
  const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
  const y = e.clientY - rect.top;
  const height = rect.height;
  
  if (props.item.type === 'group' && y > height * 0.3 && y < height * 0.7) {
    // Drop inside group
    const groupItem = props.item as GroupItem;
    itemObjects.forEach(item => {
      if (item) {
        groupItem.children.push(item);
      }
    });
    const { updateIndices } = useProject();
    updateIndices(groupItem.children, groupItem.index);
  } else {
    // Find parent array and insert before/after
    const insertAfter = y >= height / 2;
    const targetIndex = props.item.index;
    
    // Find parent array (either root items or group children)
    let parentArray = currentProject.value.items;
    let parentIndex: number[] = [];
    
    if (targetIndex.length > 1) {
      // Item is in a group, find the parent group
      const parentGroupIndex = targetIndex.slice(0, -1);
      const parentGroup = findItemByIndex(parentGroupIndex);
      if (parentGroup && parentGroup.type === 'group') {
        const groupParent = parentGroup as GroupItem;
        parentArray = groupParent.children;
        parentIndex = groupParent.index;
      }
    }
    
    // Find position in parent array
    const itemPosInArray = parentArray.findIndex(i => i.uuid === props.item.uuid);
    let insertPos = insertAfter ? itemPosInArray + 1 : itemPosInArray;
    
    // Insert all items at the position
    itemObjects.forEach((item, idx) => {
      if (item) {
        parentArray.splice(insertPos + idx, 0, item);
      }
    });
    
    // Update all indices
    const { updateIndices } = useProject();
    updateIndices(parentArray, parentIndex);
  }
  
  // Save project
  const { saveProject } = useProject();
  saveProject();
};

// Helper to get all items flattened
const getAllItemsFlattened = (items: (AudioItem | GroupItem)[]): (AudioItem | GroupItem)[] => {
  const result: (AudioItem | GroupItem)[] = [];
  for (const item of items) {
    result.push(item);
    if (item.type === 'group') {
      const groupItem = item as GroupItem;
      result.push(...getAllItemsFlattened(groupItem.children));
    }
  }
  return result;
};

// Helper to find item by index
const findItemByIndex = (index: number[]): AudioItem | GroupItem | null => {
  if (!currentProject.value) return null;
  
  let current: any = { children: currentProject.value.items };
  for (const i of index) {
    if (!current.children || !current.children[i]) return null;
    current = current.children[i];
  }
  return current;
};
</script>

<style scoped>
.playlist-item {
  border-radius: var(--border-radius-sm);
  margin-bottom: var(--spacing-xs);
  transition: all var(--transition-fast);
  position: relative;
  overflow: hidden;
  
  &.is-selected {
    box-shadow: 0 0 0 2px var(--color-accent);
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
  
  &.drag-over-top::before {
    content: '';
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    height: 3px;
    background-color: var(--color-accent);
    z-index: 10;
  }
  
  &.drag-over-bottom::after {
    content: '';
    position: absolute;
    bottom: 0;
    left: 0;
    right: 0;
    height: 3px;
    background-color: var(--color-accent);
    z-index: 10;
  }
  
  &.drag-over-group {
    box-shadow: inset 0 0 0 3px var(--color-accent);
  }
}

@keyframes flash-yellow {
  0%, 100% { 
    box-shadow: 0 0 0 0 rgba(255, 193, 7, 0.4);
  }
  50% { 
    box-shadow: 0 0 8px 4px rgba(255, 193, 7, 0.6);
  }
}

@keyframes flash-orange {
  0%, 100% { 
    box-shadow: 0 0 0 0 rgba(255, 152, 0, 0.4);
  }
  50% { 
    box-shadow: 0 0 12px 6px rgba(255, 152, 0, 0.7);
  }
}

@keyframes flash-red {
  0%, 100% { 
    box-shadow: 0 0 0 0 rgba(244, 67, 54, 0.5);
  }
  50% { 
    box-shadow: 0 0 16px 8px rgba(244, 67, 54, 0.8);
  }
}

.waveform-canvas {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
  z-index: 1;
  color: var(--color-text-primary);
  opacity: 0.1; /* Control opacity at canvas level instead of individual bars */
}

.item-progress {
  position: absolute;
  top: 0;
  left: 0;
  height: 100%;
  transition: width 100ms linear;
  pointer-events: none;
  z-index: 2;
}

.item-content {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-sm) var(--spacing-md);
  min-height: 44px;
  position: relative;
  z-index: 5;
  cursor: pointer;
}

.item-left {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  flex: 1;
  z-index: 5;
  min-width: 0;
}

.expand-btn {
  width: 20px;
  height: 20px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 12px;
  color: var(--color-text-secondary);
  
  &:hover {
    color: var(--color-text-primary);
  }
}

.item-index {
  font-size: 12px;
  font-size: 1.5em;
  color: var(--color-text-secondary);
  min-width: 40px;
}

.item-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--color-text-secondary);
  
  .material-symbols-rounded {
    font-size: 20px;
  }
}

.item-name {
  font-weight: 700;
  font-size: 1.5em;
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: var(--color-text-primary);
}

.item-duration {
  font-size: 12px;
  font-size: 1.5em;
  color: var(--color-text-secondary);
  margin-left: var(--spacing-xs);
  white-space: nowrap;
}

.behavior-indicators {
  display: flex;
  gap: 2px;
  align-items: center;
  margin-left: auto;
  
  .behavior-icon {
    font-size: 14px;
    color: var(--color-text-secondary);
    opacity: 0.7;
  }
}

.item-actions {
  display: flex;
  gap: var(--spacing-xs);
  opacity: 0;
  transition: opacity var(--transition-fast);
  z-index: 5;
  
  .playlist-item:hover & {
    opacity: 1;
  }
}

.item-btn {
  width: 32px;
  height: 32px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--border-radius-sm);
  font-size: 14px;
  
  &.play {
    background-color: var(--color-success);
    color: white;
    
    &:hover {
      opacity: 0.8;
    }
  }
  
  &.pause, &.resume {
    background-color: #ff9800; /* Orange color for pause/resume */
    color: white;
    
    &:hover {
      opacity: 0.8;
    }
  }
  
  &.stop {
    background-color: var(--color-danger);
    color: white;
    
    &:hover {
      opacity: 0.8;
    }
  }
  
  &.delete {
    background-color: var(--color-surface-hover);
    
    &:hover {
      background-color: var(--color-danger);
      color: white;
    }
  }
}

.group-children {
  padding-left: var(--spacing-md);
}
</style>
