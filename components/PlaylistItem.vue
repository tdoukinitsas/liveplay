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
    
    <!-- Progress bar for playing items -->
    <div v-if="isPlaying && item.type === 'audio'" class="item-progress" :style="progressStyle"></div>
    
    <div 
      class="item-content"
      @click="handleSelect"
      :draggable="true"
      @dragstart="handleDragStart"
    >
      <div class="item-left">
        <button 
          v-if="item.type === 'group'" 
          class="expand-btn"
          @click.stop="toggleExpand"
        >
          {{ isExpanded ? '▼' : '▶' }}
        </button>
        
        <span class="item-index">{{ indexDisplay }}</span>
        
        <span class="item-name">{{ item.displayName }}</span>
      </div>
      
      <div class="item-actions">
        <button class="item-btn play" @click.stop="handlePlay" :title="t('actions.play')">▶</button>
        <button class="item-btn stop" @click.stop="handleStop" :title="t('actions.stop')" v-if="isPlaying">⏹</button>
        <button class="item-btn delete" @click.stop="handleDelete" :title="t('actions.delete')">🗑</button>
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

const { selectedItem, removeItem, findItemByUuid, currentProject, waveformUpdateKey } = useProject();
const { playCue, stopCue, activeCues, triggerGroup } = useAudioEngine();
const { t } = useLocalization();

const isExpanded = ref(props.item.type === 'group' ? props.item.isExpanded : false);
const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const dragPosition = ref<'top' | 'bottom' | 'group' | null>(null);

const isSelected = computed(() => selectedItem.value?.uuid === props.item.uuid);
const isPlaying = computed(() => activeCues.value.has(props.item.uuid));

const indexDisplay = computed(() => {
  return props.item.index.join(',');
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
  
  // Parse RGB values to apply 30% opacity
  const rgb = textColor.match(/\d+/g);
  if (!rgb) return;
  
  ctx.fillStyle = `rgba(${rgb[0]}, ${rgb[1]}, ${rgb[2]}, 0.1)`;
  
  const peaks = audioItem.waveform.peaks;
  const barWidth = rect.width / peaks.length;
  const centerY = rect.height / 2;
  
  peaks.forEach((value, i) => {
    // Values are already normalized 0-1
    const barHeight = value * rect.height * 0.8; // Use 80% of height for better visibility
    const x = i * barWidth;
    const y = centerY - barHeight / 2;
    
    ctx.fillRect(x, y, Math.max(barWidth, 1), barHeight);
  });
};

// Redraw waveform when component mounts or updates
let resizeObserver: ResizeObserver | null = null;

onMounted(() => {
  if (props.item.type === 'audio' && waveformCanvas.value) {
    nextTick(drawWaveform);
    
    // Redraw on resize
    resizeObserver = new ResizeObserver(() => {
      drawWaveform();
    });
    resizeObserver.observe(waveformCanvas.value);
  }
});

onUnmounted(() => {
  if (resizeObserver && waveformCanvas.value) {
    resizeObserver.unobserve(waveformCanvas.value);
    resizeObserver.disconnect();
  }
});

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
        playbackProgress.value = duration > 0 ? Math.min((current / duration) * 100, 100) : 0;
      }, 100);
    }
  } else {
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
  
  if (isPlaying.value) {
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

const handleSelect = () => {
  selectedItem.value = props.item;
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
  
  const draggedItem = findItemByUuid(draggedUuid);
  if (!draggedItem) return;
  
  // Remove from current location
  removeItem(draggedUuid);
  
  // Determine insertion point
  const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
  const y = e.clientY - rect.top;
  const height = rect.height;
  
  if (props.item.type === 'group' && y > height * 0.3 && y < height * 0.7) {
    // Drop inside group
    props.item.children.push(draggedItem);
    const { updateIndices } = useProject();
    updateIndices(props.item.children, props.item.index);
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
        parentArray = parentGroup.children;
        parentIndex = parentGroup.index;
      }
    }
    
    // Find position in parent array
    const itemPosInArray = parentArray.findIndex(i => i.uuid === props.item.uuid);
    const insertPos = insertAfter ? itemPosInArray + 1 : itemPosInArray;
    
    // Insert item
    parentArray.splice(insertPos, 0, draggedItem);
    
    // Update all indices
    const { updateIndices } = useProject();
    updateIndices(parentArray, parentIndex);
  }
  
  // Save project
  const { saveProject } = useProject();
  saveProject();
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

.item-actions {
  display: flex;
  gap: var(--spacing-xs);
  opacity: 0;
  transition: opacity var(--transition-fast);
  
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
