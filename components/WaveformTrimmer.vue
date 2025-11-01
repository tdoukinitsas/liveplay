<template>
  <div class="waveform-trimmer">
    <!-- Volume Control with dB Display -->
    <div class="volume-control-section">
      <div class="volume-label">
        <span>{{ t('properties.volume') }}</span>
        <span class="db-value">{{ volumeDB.toFixed(1) }} dB</span>
      </div>
      <div class="volume-slider-container">
        <input
          type="range"
          orient="vertical"
          class="volume-slider-vertical"
          :min="-60"
          :max="12"
          step="0.1"
          :value="volumeDB"
          @input="handleVolumeChange"
          :style="{ '--volume-handle-color': volumeHandleColor }"
        />
        <div class="volume-markers">
          <span>+12</span>
          <span>0</span>
          <span>-12</span>
          <span>-24</span>
          <span>-∞</span>
        </div>
      </div>
    </div>

    <!-- Waveform Display -->
    <div class="waveform-section">
      <!-- Zoom Controls -->
      <div class="waveform-controls">
        <div class="zoom-control">
          <span class="material-symbols-rounded">zoom_out</span>
          <input
            type="range"
            class="zoom-slider"
            min="1"
            max="20"
            step="0.5"
            v-model.number="zoomLevel"
          />
          <span class="material-symbols-rounded">zoom_in</span>
        </div>
        <span class="zoom-level-text">{{ Math.round(zoomLevel * 100) }}%</span>
      </div>

      <!-- Waveform Canvas Container -->
      <div 
        class="waveform-container" 
        ref="waveformContainer"
        @wheel.prevent="handleWheel"
      >
        <canvas 
          ref="waveformCanvas"
          class="waveform-canvas"
          @mousedown="handleCanvasMouseDown"
        ></canvas>
        
        <!-- Trim Handles -->
        <div 
          class="trim-handle trim-handle-in"
          :style="{ left: inPointPosition + 'px' }"
          @mousedown.prevent="startDragHandle('in', $event)"
        >
          <div class="trim-line"></div>
          <div class="trim-grip">
            <span class="material-symbols-rounded">arrow_forward</span>
          </div>
        </div>
        
        <div 
          class="trim-handle trim-handle-out"
          :style="{ left: outPointPosition + 'px' }"
          @mousedown.prevent="startDragHandle('out', $event)"
        >
          <div class="trim-line"></div>
          <div class="trim-grip">
            <span class="material-symbols-rounded">arrow_back</span>
          </div>
        </div>

        <!-- Trim Region Overlay -->
        <div 
          class="trim-overlay trim-overlay-left"
          :style="{ width: inPointPosition + 'px' }"
        ></div>
        <div 
          class="trim-overlay trim-overlay-right"
          :style="{ left: outPointPosition + 'px' }"
        ></div>
      </div>

      <!-- Horizontal Scrollbar -->
      <div class="waveform-scrollbar">
        <input
          type="range"
          class="scroll-slider"
          min="0"
          :max="maxScroll"
          step="0.1"
          v-model.number="scrollPosition"
        />
      </div>

      <!-- Time Display -->
      <div class="time-display">
        <div class="time-field">
          <label>{{ t('properties.inPoint') }}</label>
          <input 
            type="text"
            class="time-input"
            :value="formatTimeDetailed(inPoint)"
            @change="handleInPointTextChange"
            @focus="($event.target as HTMLInputElement).select()"
          />
        </div>
        <div class="time-field">
          <label>{{ t('properties.outPoint') }}</label>
          <input 
            type="text"
            class="time-input"
            :value="formatTimeDetailed(outPoint)"
            @change="handleOutPointTextChange"
            @focus="($event.target as HTMLInputElement).select()"
          />
        </div>
        <div class="time-field">
          <label>{{ t('properties.duration') }}</label>
          <input 
            type="text"
            class="time-input"
            :value="formatTimeDetailed(duration)"
            readonly
          />
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import type { AudioItem } from '~/types/project';

const props = defineProps<{
  audioItem: AudioItem;
}>();

const emit = defineEmits<{
  'update:volume': [value: number];
  'update:inPoint': [value: number];
  'update:outPoint': [value: number];
  'change': [];
}>();

const { t } = useLocalization();

// Refs
const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const waveformContainer = ref<HTMLDivElement | null>(null);
const isDrawing = ref(false);

// Zoom and scroll
const zoomLevel = ref(1);
const scrollPosition = ref(0);

// Use existing waveform data from audioItem
const waveformData = computed(() => props.audioItem?.waveform?.peaks ?? null);
const hasWaveform = computed(() => waveformData.value && waveformData.value.length > 0);

// Volume in dB
const volumeDB = computed({
  get: () => {
    // Convert linear volume (0-2+) to dB
    const linear = props.audioItem?.volume ?? 1;
    if (linear <= 0) return -60; // -infinity
    return 20 * Math.log10(linear);
  },
  set: (db: number) => {
    // Convert dB to linear volume
    const linear = db <= -60 ? 0 : Math.pow(10, db / 20);
    emit('update:volume', linear);
    emit('change');
  }
});

// Compute handle color based on dB level
const volumeHandleColor = computed(() => {
  const db = volumeDB.value;
  if (db > 0) return '#991b1b';      // Dark red above 0dB
  if (db > -1) return '#dc2626';     // Red 0 to -1dB
  if (db > -6) return '#eab308';     // Yellow -1 to -6dB
  if (db > -18) return '#16a34a';    // Green -6 to -18dB
  if (db > -36) return '#22c55e';    // Dark green -18 to -36dB
  return '#1a4d2e';                  // Darker green below -36dB
});

// Time values
const inPoint = computed(() => props.audioItem?.inPoint ?? 0);
const outPoint = computed(() => props.audioItem?.outPoint ?? props.audioItem?.duration ?? 0);
const duration = computed(() => props.audioItem?.duration ?? 0);

// Canvas dimensions
const canvasWidth = computed(() => {
  if (!waveformContainer.value) return 800;
  return waveformContainer.value.clientWidth;
});

const canvasHeight = 120; // Fixed height for waveform (reduced from 200)

// Calculate visible range based on zoom and scroll
const visibleDuration = computed(() => duration.value / zoomLevel.value);
const visibleStart = computed(() => {
  const maxStart = Math.max(0, duration.value - visibleDuration.value);
  return (scrollPosition.value / 100) * maxStart;
});
const visibleEnd = computed(() => Math.min(duration.value, visibleStart.value + visibleDuration.value));

// Max scroll value
const maxScroll = computed(() => (zoomLevel.value > 1 ? 100 : 0));

// Position calculations for trim handles
const inPointPosition = computed(() => {
  const relativeTime = inPoint.value - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

const outPointPosition = computed(() => {
  const relativeTime = outPoint.value - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

// Handle dragging
const dragState = ref<{ handle: 'in' | 'out' | null; startX: number; startValue: number }>({
  handle: null,
  startX: 0,
  startValue: 0
});

const startDragHandle = (handle: 'in' | 'out', event: MouseEvent) => {
  dragState.value = {
    handle,
    startX: event.clientX,
    startValue: handle === 'in' ? inPoint.value : outPoint.value
  };

  const handleMouseMove = (e: MouseEvent) => {
    if (!dragState.value.handle) return;

    const deltaX = e.clientX - dragState.value.startX;
    const deltaTime = (deltaX / canvasWidth.value) * visibleDuration.value;
    const newValue = Math.max(0, Math.min(duration.value, dragState.value.startValue + deltaTime));

    if (dragState.value.handle === 'in') {
      emit('update:inPoint', Math.min(newValue, outPoint.value - 0.01));
    } else {
      emit('update:outPoint', Math.max(newValue, inPoint.value + 0.01));
    }
  };

  const handleMouseUp = () => {
    if (dragState.value.handle) {
      emit('change');
    }
    dragState.value.handle = null;
    document.removeEventListener('mousemove', handleMouseMove);
    document.removeEventListener('mouseup', handleMouseUp);
  };

  document.addEventListener('mousemove', handleMouseMove);
  document.addEventListener('mouseup', handleMouseUp);
};

// Handle canvas click for setting trim points
const handleCanvasMouseDown = (event: MouseEvent) => {
  if (dragState.value.handle) return;

  const rect = waveformCanvas.value?.getBoundingClientRect();
  if (!rect) return;

  const x = event.clientX - rect.left;
  const clickedTime = visibleStart.value + (x / canvasWidth.value) * visibleDuration.value;

  // If closer to in point, move in point, otherwise move out point
  const distToIn = Math.abs(clickedTime - inPoint.value);
  const distToOut = Math.abs(clickedTime - outPoint.value);

  if (distToIn < distToOut) {
    emit('update:inPoint', Math.max(0, Math.min(clickedTime, outPoint.value - 0.01)));
  } else {
    emit('update:outPoint', Math.max(inPoint.value + 0.01, Math.min(clickedTime, duration.value)));
  }
  emit('change');
};

// Handle wheel zoom
const handleWheel = (event: WheelEvent) => {
  const delta = event.deltaY > 0 ? -0.5 : 0.5;
  zoomLevel.value = Math.max(1, Math.min(20, zoomLevel.value + delta));
};

// Handle volume change
const handleVolumeChange = (event: Event) => {
  const target = event.target as HTMLInputElement;
  volumeDB.value = parseFloat(target.value);
};

// Format time as HH:MM:SS.mmm
const formatTimeDetailed = (seconds: number): string => {
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const secs = Math.floor(seconds % 60);
  const milliseconds = Math.floor((seconds % 1) * 1000);

  return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}.${milliseconds.toString().padStart(3, '0')}`;
};

// Parse time from HH:MM:SS.mmm format
const parseTimeDetailed = (timeStr: string): number => {
  const parts = timeStr.split(':');
  if (parts.length !== 3) return 0;

  const hours = parseInt(parts[0]) || 0;
  const minutes = parseInt(parts[1]) || 0;
  const secondsParts = parts[2].split('.');
  const seconds = parseInt(secondsParts[0]) || 0;
  const milliseconds = parseInt(secondsParts[1]) || 0;

  return hours * 3600 + minutes * 60 + seconds + milliseconds / 1000;
};

// Handle time input changes
const handleInPointTextChange = (event: Event) => {
  const value = (event.target as HTMLInputElement).value;
  const parsed = parseTimeDetailed(value);
  emit('update:inPoint', Math.max(0, Math.min(parsed, outPoint.value - 0.01)));
  emit('change');
};

const handleOutPointTextChange = (event: Event) => {
  const value = (event.target as HTMLInputElement).value;
  const parsed = parseTimeDetailed(value);
  emit('update:outPoint', Math.max(inPoint.value + 0.01, Math.min(parsed, duration.value)));
  emit('change');
};

// Draw waveform on canvas
const drawWaveform = () => {
  const canvas = waveformCanvas.value;
  if (!canvas) return;

  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  // Set canvas dimensions with device pixel ratio
  const dpr = window.devicePixelRatio || 1;
  canvas.width = canvasWidth.value * dpr;
  canvas.height = canvasHeight * dpr;
  canvas.style.width = `${canvasWidth.value}px`;
  canvas.style.height = `${canvasHeight}px`;
  ctx.scale(dpr, dpr);

  // Clear canvas with background color
  const bgColor = getComputedStyle(document.documentElement).getPropertyValue('--color-background').trim();
  ctx.fillStyle = bgColor || '#000';
  ctx.fillRect(0, 0, canvasWidth.value, canvasHeight);

  const middleY = canvasHeight / 2;

  if (hasWaveform.value && waveformData.value && duration.value > 0) {
    // Draw waveform bars from existing data (like in PlaylistItem)
    const peaks = waveformData.value;
    const totalPeaks = peaks.length;
    
    // Calculate visible peak range
    const startPeak = Math.floor((visibleStart.value / duration.value) * totalPeaks);
    const endPeak = Math.floor((visibleEnd.value / duration.value) * totalPeaks);
    const visiblePeaks = endPeak - startPeak;
    const visiblePeaksArray = peaks.slice(startPeak, endPeak);

    if (visiblePeaksArray.length > 0) {
      const barWidth = canvasWidth.value / visiblePeaksArray.length;
      const volumeMultiplier = props.audioItem?.volume ?? 1;

      // Function to get color based on dB level
      const getColorForDB = (db: number): string => {
        if (db > 0) return '#991b1b';        // Dark red above 0 dB
        if (db > -1) return '#dc2626';       // Red 0 to -1 dB
        if (db > -6) return '#eab308';       // Yellow -1 to -6 dB
        if (db > -18) return '#16a34a';      // Green -6 to -18 dB
        if (db > -36) return '#22c55e';      // Dark green -18 to -36 dB
        return '#1a4d2e';                    // Darker green under -36 dB
      };
      
      // Parse color to get RGB values
      const parseColor = (color: string) => {
        const temp = document.createElement('div');
        temp.style.color = color;
        document.body.appendChild(temp);
        const computed = getComputedStyle(temp).color;
        document.body.removeChild(temp);
        const match = computed.match(/\d+/g);
        return match ? match.map(Number) : [128, 128, 128];
      };

      // Draw each bar with individual coloring
      visiblePeaksArray.forEach((value, i) => {
        const normalizedPeak = value; // Already normalized 0-1
        
        // Base waveform bar height (use 80% of canvas height like PlaylistItem)
        const baseBarHeight = normalizedPeak * canvasHeight * 0.8;
        const baseY = middleY - baseBarHeight / 2;
        const x = i * barWidth;
        
        // Draw base waveform (subtle gray)
        ctx.fillStyle = 'rgba(128, 128, 128, 0.15)';
        ctx.fillRect(x, baseY, Math.max(barWidth, 1), baseBarHeight);
        
        // Calculate bar height after volume multiplication
        const amplifiedPeak = Math.min(normalizedPeak * volumeMultiplier, 1); // Clamp to 1
        const amplifiedBarHeight = amplifiedPeak * canvasHeight * 0.8;
        const amplifiedY = middleY - amplifiedBarHeight / 2;
        
        // Convert this bar's amplitude to dB for color selection
        const linearAmplitude = normalizedPeak * volumeMultiplier;
        const barDB = linearAmplitude <= 0 ? -60 : 20 * Math.log10(linearAmplitude);
        
        // Get color for this specific bar's level
        const barColor = getColorForDB(barDB);
        const [r, g, b] = parseColor(barColor);
        
        // Draw colored bar with opacity based on whether it's clipping
        const alpha = linearAmplitude > 1 ? 0.8 : 0.5; // More opaque if clipping
        ctx.fillStyle = `rgba(${r}, ${g}, ${b}, ${alpha})`;
        ctx.fillRect(x, amplifiedY, Math.max(barWidth, 1), amplifiedBarHeight);
      });
    }
  } else {
    // Draw time grid as fallback
    ctx.strokeStyle = 'rgba(128, 128, 128, 0.3)';
    ctx.lineWidth = 1;
    ctx.font = '10px sans-serif';
    ctx.fillStyle = 'rgba(128, 128, 128, 0.6)';

    // Draw vertical lines for each second/minute
    const timeStep = duration.value > 60 ? 10 : 1; // 10-second intervals for long files, 1 second for short
    const pixelsPerSecond = canvasWidth.value / visibleDuration.value;

    for (let time = Math.ceil(visibleStart.value / timeStep) * timeStep; time <= visibleEnd.value; time += timeStep) {
      const x = (time - visibleStart.value) * pixelsPerSecond;
      
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, canvasHeight);
      ctx.stroke();

      // Draw time label
      const minutes = Math.floor(time / 60);
      const seconds = Math.floor(time % 60);
      const label = `${minutes}:${seconds.toString().padStart(2, '0')}`;
      ctx.fillText(label, x + 4, 12);
    }

    // Draw "No Waveform Data" message
    ctx.font = '14px sans-serif';
    ctx.fillStyle = 'rgba(128, 128, 128, 0.5)';
    ctx.textAlign = 'center';
    ctx.fillText('No waveform data available', canvasWidth.value / 2, middleY - 10);
    ctx.font = '12px sans-serif';
    ctx.fillText('Time grid displayed', canvasWidth.value / 2, middleY + 10);
    ctx.textAlign = 'left';
  }

  // Draw center line
  ctx.strokeStyle = 'rgba(128, 128, 128, 0.3)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, middleY);
  ctx.lineTo(canvasWidth.value, middleY);
  ctx.stroke();
};

// Throttle drawWaveform to prevent excessive redraws
let drawTimeout: NodeJS.Timeout | null = null;
const throttledDraw = () => {
  if (drawTimeout) clearTimeout(drawTimeout);
  drawTimeout = setTimeout(() => {
    drawWaveform();
  }, 16); // ~60fps
};

// Watch for changes and redraw
watch([zoomLevel, scrollPosition, () => props.audioItem?.volume, () => props.audioItem?.inPoint, () => props.audioItem?.outPoint, waveformData], () => {
  throttledDraw();
});

// Watch for canvas width changes
const resizeObserver = ref<ResizeObserver | null>(null);

onMounted(() => {
  // Initial draw
  setTimeout(() => {
    drawWaveform();
  }, 100);

  if (waveformContainer.value) {
    resizeObserver.value = new ResizeObserver(() => {
      throttledDraw();
    });
    resizeObserver.value.observe(waveformContainer.value);
  }
});

onUnmounted(() => {
  // Clear any pending draw operations
  if (drawTimeout) clearTimeout(drawTimeout);
  
  // Clean up resize observer
  if (resizeObserver.value && waveformContainer.value) {
    resizeObserver.value.unobserve(waveformContainer.value);
    resizeObserver.value.disconnect();
  }
});
</script>

<style scoped>
.waveform-trimmer {
  display: flex;
  gap: var(--spacing-sm);
  padding: 0;
  background: transparent;
  max-height: 220px;
  overflow: hidden;
}

/* Volume Control */
.volume-control-section {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  min-width: 70px;
}

.volume-label {
  display: flex;
  flex-direction: column;
  gap: 2px;
  font-size: 11px;
  color: var(--color-text-secondary);
}

.db-value {
  font-size: 13px;
  font-weight: 600;
  color: var(--color-text-primary);
}

.volume-slider-container {
  display: flex;
  gap: var(--spacing-xs);
  align-items: center;
  flex: 1;
  min-height: 0;
}

.volume-slider-vertical {
  writing-mode: vertical-lr;
  direction: rtl;
  width: 150px;
  height: 100%;
  cursor: pointer;
  -webkit-appearance: slider-vertical;
  appearance: slider-vertical;
  background: transparent;
  border-radius: 4px;
  position: relative;
}

/* Volume slider track with dB steps */
.volume-slider-vertical::-webkit-slider-runnable-track {
  width: 20px;
  height: 100%;
  background: linear-gradient(to top,
    #1a4d2e 0%,      /* Darker green under -36db */
    #1a4d2e 33%,     /* -36db */
    #22c55e 33%,     /* Dark green -36 to -18 */
    #22c55e 50%,     /* -18db */
    #16a34a 50%,     /* Green -18 to -6 */
    #16a34a 75%,     /* -6db */
    #eab308 75%,     /* Yellow -6 to -1 */
    #eab308 93%,     /* -1db */
    #dc2626 93%,     /* Red -1 to 0 */
    #dc2626 97%,     /* 0db */
    #991b1b 97%,     /* Dark red above 0 */
    #991b1b 100%
  );
  border-radius: 4px;
  /* Add horizontal lines for dB steps */
  background-image: 
    repeating-linear-gradient(0deg,
      rgba(0, 0, 0, 0.3) 0px,
      rgba(0, 0, 0, 0.3) 1px,
      transparent 1px,
      transparent 10%
    );
}

.volume-slider-vertical::-moz-range-track {
  width: 20px;
  height: 100%;
  background: linear-gradient(to top,
    #1a4d2e 0%,
    #1a4d2e 33%,
    #22c55e 33%,
    #22c55e 50%,
    #16a34a 50%,
    #16a34a 75%,
    #eab308 75%,
    #eab308 93%,
    #dc2626 93%,
    #dc2626 97%,
    #991b1b 97%,
    #991b1b 100%
  );
  border-radius: 4px;
  background-image: 
    repeating-linear-gradient(0deg,
      rgba(0, 0, 0, 0.3) 0px,
      rgba(0, 0, 0, 0.3) 1px,
      transparent 1px,
      transparent 10%
    );
}

.volume-slider-vertical::-webkit-slider-thumb {
  -webkit-appearance: none;
  appearance: none;
  width: 24px;
  height: 14px;
  background: var(--volume-handle-color, var(--color-text-primary));
  cursor: pointer;
  border-radius: 3px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.4);
  border: 2px solid rgba(255, 255, 255, 0.3);
}

.volume-slider-vertical::-moz-range-thumb {
  width: 24px;
  height: 14px;
  background: var(--volume-handle-color, var(--color-text-primary));
  cursor: pointer;
  border-radius: 3px;
  border: 2px solid rgba(255, 255, 255, 0.3);
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.4);
}

.volume-markers {
  display: flex;
  flex-direction: column;
  justify-content: space-between;
  height: 100%;
  font-size: 9px;
  color: var(--color-text-secondary);
}

/* Waveform Section */
.waveform-section {
  flex: 1;
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  min-width: 0;
}

.waveform-controls {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-xs) var(--spacing-sm);
  background: var(--color-surface);
  border-radius: var(--border-radius-sm);
}

.zoom-control {
  display: flex;
  align-items: center;
  gap: var(--spacing-xs);
}

.zoom-control .material-symbols-rounded {
  font-size: 18px;
  color: var(--color-text-secondary);
}

.zoom-slider {
  width: 120px;
  cursor: pointer;
}

.zoom-slider::-webkit-slider-thumb {
  -webkit-appearance: none;
  appearance: none;
  width: 14px;
  height: 14px;
  background: var(--color-accent-custom, var(--color-accent));
  cursor: pointer;
  border-radius: 50%;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
}

.zoom-slider::-moz-range-thumb {
  width: 14px;
  height: 14px;
  background: var(--color-accent-custom, var(--color-accent));
  cursor: pointer;
  border-radius: 50%;
  border: none;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
}

.zoom-slider::-webkit-slider-track {
  background: var(--color-border);
  height: 4px;
  border-radius: 2px;
}

.zoom-slider::-moz-range-track {
  background: var(--color-border);
  height: 4px;
  border-radius: 2px;
}

.zoom-level-text {
  font-size: 11px;
  color: var(--color-text-secondary);
  min-width: 45px;
  text-align: right;
}

/* Waveform Container */
.waveform-container {
  position: relative;
  width: 100%;
  height: 120px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  overflow: hidden;
  cursor: crosshair;
}

.waveform-canvas {
  display: block;
  width: 100%;
  height: 100%;
}

/* Trim Handles */
.trim-handle {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  cursor: ew-resize;
  z-index: 10;
  user-select: none;
}

.trim-handle-in {
  background: rgba(34, 197, 94, 0.3);
}

.trim-handle-out {
  background: rgba(239, 68, 68, 0.3);
}

.trim-line {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  background: currentColor;
}

.trim-handle-in .trim-line {
  background: rgb(34, 197, 94);
}

.trim-handle-out .trim-line {
  background: rgb(239, 68, 68);
}

.trim-grip {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  width: 20px;
  height: 32px;
  background: var(--color-surface);
  border: 2px solid currentColor;
  border-radius: var(--border-radius-sm);
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
  user-select: none;
}

.trim-handle-in .trim-grip {
  left: -10px;
  color: rgb(34, 197, 94);
}

.trim-handle-out .trim-grip {
  right: -10px;
  color: rgb(239, 68, 68);
}

.trim-grip .material-symbols-rounded {
  font-size: 14px;
}

/* Trim Overlays */
.trim-overlay {
  position: absolute;
  top: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.5);
  pointer-events: none;
}

.trim-overlay-left {
  left: 0;
}

.trim-overlay-right {
  right: 0;
  width: auto;
  left: auto;
}

/* Scrollbar */
.waveform-scrollbar {
  width: 100%;
}

.scroll-slider {
  width: 100%;
  cursor: pointer;
}

.scroll-slider::-webkit-slider-thumb {
  -webkit-appearance: none;
  appearance: none;
  width: 14px;
  height: 14px;
  background: var(--color-accent-custom, var(--color-accent));
  cursor: pointer;
  border-radius: 50%;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
}

.scroll-slider::-moz-range-thumb {
  width: 14px;
  height: 14px;
  background: var(--color-accent-custom, var(--color-accent));
  cursor: pointer;
  border-radius: 50%;
  border: none;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
}

.scroll-slider::-webkit-slider-track {
  background: var(--color-border);
  height: 4px;
  border-radius: 2px;
}

.scroll-slider::-moz-range-track {
  background: var(--color-border);
  height: 4px;
  border-radius: 2px;
}

/* Time Display */
.time-display {
  display: flex;
  gap: var(--spacing-sm);
  padding: var(--spacing-xs) var(--spacing-sm);
  background: var(--color-surface);
  border-radius: var(--border-radius-sm);
}

.time-field {
  display: flex;
  flex-direction: column;
  gap: 2px;
  flex: 1;
  min-width: 0;
}

.time-field label {
  font-size: 10px;
  color: var(--color-text-secondary);
  text-transform: uppercase;
  font-weight: 500;
}

.time-input {
  padding: 4px var(--spacing-xs);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-primary);
  font-family: 'Courier New', monospace;
  font-size: 11px;
  text-align: center;
}

.time-input:focus {
  outline: none;
  border-color: var(--color-accent);
}

.time-input:read-only {
  opacity: 0.6;
  cursor: default;
}
</style>
