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
          :max="10"
          step="0.1"
          :value="volumeDB"
          @input="handleVolumeChange"
          :style="{ '--volume-handle-color': volumeHandleColor }"
        />
        <div class="volume-markers">
          <span>+10</span>
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
        
        <!-- Audio Tools -->
        <div class="audio-tools">
          <!-- Trim Silence Button -->
          <button class="trim-silence-btn" @click="trimSilence" :title="t('properties.trimSilence')">
            <span class="material-symbols-rounded">content_cut</span>
            <span>{{ t('properties.trimSilence') }}</span>
          </button>
          
          <!-- Normalize Button -->
          <button class="normalize-btn" @click="normalizeAudio" :title="t('properties.normalize')">
            <span class="material-symbols-rounded">tune</span>
            <span>{{ t('properties.normalize') }}</span>
          </button>
        </div>
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
        
        <!-- Fade Handles (hidden for cart items) -->
        <template v-if="!isCartItem">
          <!-- Play Fade Handle (fade in end) -->
          <div 
            v-if="playFade > 0"
            class="fade-handle fade-handle-play"
            :style="{ left: playFadePosition + 'px' }"
            @mousedown.prevent="startDragFade('play', $event)"
            :title="`Play Fade: ${playFade.toFixed(1)}s`"
          >
            <div class="fade-line fade-line-red"></div>
            <div class="fade-grip fade-grip-red">
              <span class="material-symbols-rounded">trending_up</span>
            </div>
          </div>
          
          <!-- Stop Fade Handle (fade out start) -->
          <div 
            v-if="stopFade > 0"
            class="fade-handle fade-handle-stop"
            :style="{ left: stopFadePosition + 'px' }"
            @mousedown.prevent="startDragFade('stop', $event)"
            :title="`Stop Fade: ${stopFade.toFixed(1)}s`"
          >
            <div class="fade-line fade-line-red"></div>
            <div class="fade-grip fade-grip-red">
              <span class="material-symbols-rounded">trending_down</span>
            </div>
          </div>
          
          <!-- Cross Fade Handle (crossfade start) -->
          <div 
            v-if="crossFade > 0"
            class="fade-handle fade-handle-cross"
            :style="{ left: crossFadePosition + 'px' }"
            @mousedown.prevent="startDragFade('cross', $event)"
            :title="`Cross Fade: ${crossFade.toFixed(1)}s`"
          >
            <div class="fade-line fade-line-yellow"></div>
            <div class="fade-grip fade-grip-yellow">
              <span class="material-symbols-rounded">swap_horiz</span>
            </div>
          </div>
        </template>
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
    </div>

    <!-- Time Display (moved to right side) -->
    <div class="time-display-section">
      <div class="time-field">
        <label>{{ t('properties.inPoint') }}</label>
        <div class="time-input-with-buttons">
          <button class="time-decrement" @click="adjustInPoint(-0.5)" title="Decrease by 0.5s">
            <span class="material-symbols-rounded">remove</span>
          </button>
          <input 
            type="text"
            class="time-input"
            :value="formatTimeDetailed(inPoint)"
            @change="handleInPointTextChange"
            @focus="($event.target as HTMLInputElement).select()"
          />
          <button class="time-increment" @click="adjustInPoint(0.5)" title="Increase by 0.5s">
            <span class="material-symbols-rounded">add</span>
          </button>
        </div>
      </div>
      <div class="time-field">
        <label>{{ t('properties.outPoint') }}</label>
        <div class="time-input-with-buttons">
          <button class="time-decrement" @click="adjustOutPoint(-0.5)" title="Decrease by 0.5s">
            <span class="material-symbols-rounded">remove</span>
          </button>
          <input 
            type="text"
            class="time-input"
            :value="formatTimeDetailed(outPoint)"
            @change="handleOutPointTextChange"
            @focus="($event.target as HTMLInputElement).select()"
          />
          <button class="time-increment" @click="adjustOutPoint(0.5)" title="Increase by 0.5s">
            <span class="material-symbols-rounded">add</span>
          </button>
        </div>
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

    <!-- Fade Controls (hidden for cart items) -->
    <div v-if="!isCartItem" class="fade-controls-section">
      <div class="fade-control-group">
        <label>{{ t('properties.playFade') }}</label>
        <div class="time-input-with-buttons">
          <button class="time-decrement" @click="adjustPlayFade(-0.5)" title="Decrease by 0.5s">
            <span class="material-symbols-rounded">remove</span>
          </button>
          <input 
            type="text"
            class="time-input fade-input"
            :value="formatTimeDetailed(playFade)"
            @change="handlePlayFadeTextChange"
            @focus="($event.target as HTMLInputElement).select()"
          />
          <button class="time-increment" @click="adjustPlayFade(0.5)" title="Increase by 0.5s">
            <span class="material-symbols-rounded">add</span>
          </button>
        </div>
      </div>
      <div class="fade-control-group">
        <label>{{ t('properties.stopFade') }}</label>
        <div class="time-input-with-buttons">
          <button class="time-decrement" @click="adjustStopFade(-0.5)" title="Decrease by 0.5s">
            <span class="material-symbols-rounded">remove</span>
          </button>
          <input 
            type="text"
            class="time-input fade-input"
            :value="formatTimeDetailed(stopFade)"
            @change="handleStopFadeTextChange"
            @focus="($event.target as HTMLInputElement).select()"
          />
          <button class="time-increment" @click="adjustStopFade(0.5)" title="Increase by 0.5s">
            <span class="material-symbols-rounded">add</span>
          </button>
        </div>
      </div>
      <div class="fade-control-group">
        <label>{{ t('properties.crossFade') }}</label>
        <div class="time-input-with-buttons">
          <button class="time-decrement" @click="adjustCrossFade(-0.5)" title="Decrease by 0.5s">
            <span class="material-symbols-rounded">remove</span>
          </button>
          <input 
            type="text"
            class="time-input fade-input"
            :value="formatTimeDetailed(crossFade)"
            @change="handleCrossFadeTextChange"
            @focus="($event.target as HTMLInputElement).select()"
          />
          <button class="time-increment" @click="adjustCrossFade(0.5)" title="Increase by 0.5s">
            <span class="material-symbols-rounded">add</span>
          </button>
        </div>
      </div>
    </div>

  </div>
</template>

<script setup lang="ts">
import type { AudioItem } from '~/types/project';
import { calculatePerceivedLoudness, calculateNormalizationGain } from '~/utils/audio';

const props = defineProps<{
  audioItem: AudioItem;
}>();

const emit = defineEmits<{
  'update:volume': [value: number];
  'update:inPoint': [value: number];
  'update:outPoint': [value: number];
  'update:playFade': [value: number];
  'update:stopFade': [value: number];
  'update:pauseFade': [value: number];
  'update:crossFade': [value: number];
  'change': [];
  'normalize': [];
  'trimSilence': [];
}>();

const { t } = useLocalization();

// Get audio engine for playback position
const { activeCues } = useAudioEngine();

// Check if this is a cart item
const isCartItem = computed(() => {
  return props.audioItem.index && props.audioItem.index.length > 0 && props.audioItem.index[0] === -1;
});

// Fade values
const playFade = computed(() => props.audioItem.playFade || 0);
const stopFade = computed(() => props.audioItem.stopFade || 0);
const crossFade = computed(() => props.audioItem.crossFade || 0);

// Refs
const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const waveformContainer = ref<HTMLDivElement | null>(null);
const isDrawing = ref(false);

// Zoom and scroll
const zoomLevel = ref(1);
const scrollPosition = ref(0);

// Get playback position for playhead
const playbackPosition = computed(() => {
  const cue = activeCues.value.get(props.audioItem.uuid);
  if (!cue) return null;
  
  // currentTime is relative to inPoint, we need absolute position in file
  const inPoint = props.audioItem.inPoint || 0;
  return cue.currentTime + inPoint;
});

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

// Canvas dimensions - use reactive ref to ensure handle positions update on resize
const containerWidth = ref(800);
const canvasWidth = computed(() => containerWidth.value);

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

// Fade handle positions (respecting trim points)
const playFadePosition = computed(() => {
  const fadeEndTime = inPoint.value + playFade.value;
  const relativeTime = fadeEndTime - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

const stopFadePosition = computed(() => {
  const fadeStartTime = outPoint.value - stopFade.value;
  const relativeTime = fadeStartTime - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

const crossFadePosition = computed(() => {
  const crossFadeStartTime = outPoint.value - crossFade.value;
  const relativeTime = crossFadeStartTime - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

// Handle dragging
const dragState = ref<{ handle: 'in' | 'out' | 'play' | 'stop' | 'cross' | null; startX: number; startValue: number }>({
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

// Handle fade dragging
const startDragFade = (fadeType: 'play' | 'stop' | 'cross', event: MouseEvent) => {
  const currentValue = fadeType === 'play' ? playFade.value : fadeType === 'stop' ? stopFade.value : crossFade.value;
  
  dragState.value = {
    handle: fadeType,
    startX: event.clientX,
    startValue: currentValue
  };

  const handleMouseMove = (e: MouseEvent) => {
    if (!dragState.value.handle) return;

    const deltaX = e.clientX - dragState.value.startX;
    const deltaTime = (deltaX / canvasWidth.value) * visibleDuration.value;
    
    if (dragState.value.handle === 'play') {
      // Play fade: drag right increases fade duration
      const newValue = Math.max(0, Math.min(10, dragState.value.startValue + deltaTime));
      emit('update:playFade', newValue);
    } else if (dragState.value.handle === 'stop') {
      // Stop fade: drag left increases fade duration (moving the start point earlier)
      const newValue = Math.max(0, Math.min(10, dragState.value.startValue - deltaTime));
      emit('update:stopFade', newValue);
    } else if (dragState.value.handle === 'cross') {
      // Cross fade: drag left increases fade duration (moving the start point earlier)
      const newValue = Math.max(0, Math.min(10, dragState.value.startValue - deltaTime));
      emit('update:crossFade', newValue);
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

// Fade change handlers
const handlePlayFadeChange = (event: Event) => {
  const target = event.target as HTMLInputElement;
  const value = parseFloat(target.value);
  emit('update:playFade', value);
  emit('change');
};

const handleStopFadeChange = (event: Event) => {
  const target = event.target as HTMLInputElement;
  const value = parseFloat(target.value);
  emit('update:stopFade', value);
  emit('change');
};

const handlePauseFadeChange = (event: Event) => {
  const target = event.target as HTMLInputElement;
  const value = parseFloat(target.value);
  emit('update:pauseFade', value);
  emit('change');
};

const handleCrossFadeChange = (event: Event) => {
  const target = event.target as HTMLInputElement;
  const value = parseFloat(target.value);
  emit('update:crossFade', value);
  emit('change');
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
  emit('update:outPoint', Math.min(props.audioItem.duration, Math.max(parsed, inPoint.value + 0.01)));
  emit('change');
};

// Adjustment functions for increment/decrement buttons
const adjustInPoint = (delta: number) => {
  const newValue = Math.max(0, Math.min(inPoint.value + delta, outPoint.value - 0.01));
  emit('update:inPoint', newValue);
  emit('change');
};

const adjustOutPoint = (delta: number) => {
  const newValue = Math.min(props.audioItem.duration, Math.max(outPoint.value + delta, inPoint.value + 0.01));
  emit('update:outPoint', newValue);
  emit('change');
};

const adjustPlayFade = (delta: number) => {
  const newValue = Math.max(0, Math.min(playFade.value + delta, 10));
  emit('update:playFade', newValue);
  emit('change');
};

const adjustStopFade = (delta: number) => {
  const newValue = Math.max(0, Math.min(stopFade.value + delta, 10));
  emit('update:stopFade', newValue);
  emit('change');
};

const adjustCrossFade = (delta: number) => {
  const newValue = Math.max(0, Math.min(crossFade.value + delta, 10));
  emit('update:crossFade', newValue);
  emit('change');
};

// Text change handlers for fade inputs
const handlePlayFadeTextChange = (event: Event) => {
  const value = (event.target as HTMLInputElement).value;
  const parsed = parseTimeDetailed(value);
  emit('update:playFade', Math.max(0, Math.min(parsed, 10)));
  emit('change');
};

const handleStopFadeTextChange = (event: Event) => {
  const value = (event.target as HTMLInputElement).value;
  const parsed = parseTimeDetailed(value);
  emit('update:stopFade', Math.max(0, Math.min(parsed, 10)));
  emit('change');
};

const handleCrossFadeTextChange = (event: Event) => {
  const value = (event.target as HTMLInputElement).value;
  const parsed = parseTimeDetailed(value);
  emit('update:crossFade', Math.max(0, Math.min(parsed, 10)));
  emit('change');
};

// Trim silence from start and end based on waveform peaks
const trimSilence = () => {
  if (!waveformData.value || waveformData.value.length === 0) {
    console.warn('No waveform data available for trimming');
    return;
  }

  // Emit trimSilence event to trigger batch trimming in parent
  // The parent will handle trimming all selected items individually
  emit('trimSilence');
  emit('change');
};

// Normalize audio to target loudness
const normalizeAudio = () => {
  if (!waveformData.value || waveformData.value.length === 0) {
    console.warn('No waveform data available for normalization');
    return;
  }

  // Emit normalize event to trigger batch normalization in parent
  // The parent will handle normalizing all selected items individually
  emit('normalize');
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
  //canvas.style.height = `${canvasHeight}px`; - commented out to fix height issue
  ctx.scale(dpr, dpr);

  // Clear canvas with background color
  const bgColor = getComputedStyle(document.documentElement).getPropertyValue('--color-background').trim();
  ctx.fillStyle = bgColor || '#000';
  ctx.fillRect(0, 0, canvasWidth.value, canvasHeight);

  const middleY = canvasHeight / 2;

  // Draw time grid (always shown, behind waveform if present)
  ctx.strokeStyle = 'rgba(128, 128, 128, 0.3)';
  ctx.lineWidth = 1;
  ctx.font = '10px sans-serif';
  ctx.fillStyle = 'rgba(128, 128, 128, 0.6)';

  // Calculate dynamic time step based on visible duration and zoom level
  // Goal: Show grid lines every ~50-100 pixels
  const pixelsPerSecond = canvasWidth.value / visibleDuration.value;
  const targetPixelsPerGrid = 75; // Ideal spacing between grid lines
  
  // Calculate initial time step
  let timeStep = targetPixelsPerGrid / pixelsPerSecond;
  
  // Round to nice intervals: 0.1, 0.5, 1, 2, 5, 10, 30, 60, 120, 300, 600 seconds
  const niceIntervals = [0.1, 0.5, 1, 2, 5, 10, 30, 60, 120, 300, 600];
  timeStep = niceIntervals.reduce((prev, curr) => 
    Math.abs(curr - timeStep) < Math.abs(prev - timeStep) ? curr : prev
  );

  for (let time = Math.ceil(visibleStart.value / timeStep) * timeStep; time <= visibleEnd.value; time += timeStep) {
    const x = (time - visibleStart.value) * pixelsPerSecond;
    
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, canvasHeight);
    ctx.stroke();

    // Draw time label - format depends on scale
    let label: string;
    if (timeStep < 1) {
      // Show with decimal for sub-second intervals
      label = time.toFixed(1) + 's';
    } else if (timeStep < 60) {
      // Show seconds
      const minutes = Math.floor(time / 60);
      const seconds = Math.floor(time % 60);
      label = minutes > 0 ? `${minutes}:${seconds.toString().padStart(2, '0')}` : `${seconds}s`;
    } else {
      // Show minutes:seconds
      const minutes = Math.floor(time / 60);
      const seconds = Math.floor(time % 60);
      label = `${minutes}:${seconds.toString().padStart(2, '0')}`;
    }
    ctx.fillText(label, x + 4, 12);
  }

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
      
      // Draw perceived loudness line (RMS level)
      if (visiblePeaksArray.length > 0) {
        const perceivedLoudness = calculatePerceivedLoudness(visiblePeaksArray);
        const volumeMultiplier = props.audioItem?.volume ?? 1;
        
        // Convert perceived loudness (dB) back to linear for display height
        const rmsLinear = perceivedLoudness <= -60 ? 0 : Math.pow(10, perceivedLoudness / 20);
        const rmsAmplified = rmsLinear * volumeMultiplier;
        const rmsHeight = rmsAmplified * canvasHeight * 0.8;
        
        // Draw horizontal line at RMS level (on both sides of center)
        ctx.strokeStyle = 'rgba(255, 165, 0, 0.5)'; // Orange with transparency
        ctx.lineWidth = 1;
        
        // Top line
        const topY = middleY - rmsHeight / 2;
        ctx.beginPath();
        ctx.moveTo(0, topY);
        ctx.lineTo(canvasWidth.value, topY);
        ctx.stroke();
        
        // Bottom line
        const bottomY = middleY + rmsHeight / 2;
        ctx.beginPath();
        ctx.moveTo(0, bottomY);
        ctx.lineTo(canvasWidth.value, bottomY);
        ctx.stroke();
        
        // Reset line dash
        ctx.setLineDash([]);
      }
    }
  } else {
    // Draw "No Waveform Data" message
    ctx.font = '14px sans-serif';
    ctx.fillStyle = 'rgba(128, 128, 128, 0.5)';
    ctx.textAlign = 'center';
    ctx.fillText('No waveform data available', canvasWidth.value / 2, middleY);
    ctx.textAlign = 'left';
  }

  // Draw center line
  ctx.strokeStyle = 'rgba(128, 128, 128, 0.3)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, middleY);
  ctx.lineTo(canvasWidth.value, middleY);
  ctx.stroke();

  // Draw playhead if item is currently playing
  if (playbackPosition.value !== null && duration.value > 0) {
    const playheadX = (playbackPosition.value / duration.value) * canvasWidth.value;
    
    // Use item's color or default to accent color
    const itemColor = props.audioItem.color || 'var(--color-accent)';
    
    // Draw vertical line
    ctx.strokeStyle = itemColor;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(playheadX, 0);
    ctx.lineTo(playheadX, canvasHeight);
    ctx.stroke();
    
    // Draw triangle at top
    ctx.fillStyle = itemColor;
    ctx.beginPath();
    ctx.moveTo(playheadX, 10);
    ctx.lineTo(playheadX - 6, 0);
    ctx.lineTo(playheadX + 6, 0);
    ctx.closePath();
    ctx.fill();
    
    // Draw triangle at bottom
    ctx.beginPath();
    ctx.moveTo(playheadX, canvasHeight - 10);
    ctx.lineTo(playheadX - 6, canvasHeight);
    ctx.lineTo(playheadX + 6, canvasHeight);
    ctx.closePath();
    ctx.fill();
  }

  // Draw fade visualizations if configured
  if (duration.value > 0) {
    const pixelsPerSecond = canvasWidth.value / visibleDuration.value;
    
    // Play Fade (fade in at start) - Red with diagonal line
    if (props.audioItem.playFade && props.audioItem.playFade > 0) {
      const fadeStartTime = inPoint.value;
      const fadeEndTime = inPoint.value + props.audioItem.playFade;
      
      // Only draw if visible in current view
      if (fadeEndTime >= visibleStart.value && fadeStartTime <= visibleEnd.value) {
        const fadeStartX = Math.max(0, (fadeStartTime - visibleStart.value) * pixelsPerSecond);
        const fadeEndX = Math.min(canvasWidth.value, (fadeEndTime - visibleStart.value) * pixelsPerSecond);
        const fadeWidth = fadeEndX - fadeStartX;
        
        if (fadeWidth > 0) {
          // Draw red rectangle with 20% opacity
          ctx.fillStyle = 'rgba(220, 38, 38, 0.2)';
          ctx.fillRect(fadeStartX, 0, fadeWidth, canvasHeight);
          
          // Draw diagonal line from bottom-left to top-right (fade in)
          ctx.strokeStyle = 'rgba(220, 38, 38, 0.8)';
          ctx.lineWidth = 2;
          ctx.beginPath();
          ctx.moveTo(fadeStartX, canvasHeight);
          ctx.lineTo(fadeEndX, 0);
          ctx.stroke();
          
          // Draw red vertical line at fade end boundary
          ctx.beginPath();
          ctx.moveTo(fadeEndX, 0);
          ctx.lineTo(fadeEndX, canvasHeight);
          ctx.stroke();
        }
      }
    }
    
    // Stop Fade (fade out before end) - Red with diagonal line
    if (props.audioItem.stopFade && props.audioItem.stopFade > 0) {
      const fadeStartTime = outPoint.value - props.audioItem.stopFade;
      const fadeEndTime = outPoint.value;
      
      // Only draw if visible in current view
      if (fadeStartTime <= visibleEnd.value && fadeEndTime >= visibleStart.value) {
        const fadeStartX = Math.max(0, (fadeStartTime - visibleStart.value) * pixelsPerSecond);
        const fadeEndX = Math.min(canvasWidth.value, (fadeEndTime - visibleStart.value) * pixelsPerSecond);
        const fadeWidth = fadeEndX - fadeStartX;
        
        if (fadeWidth > 0) {
          // Draw red rectangle with 20% opacity
          ctx.fillStyle = 'rgba(220, 38, 38, 0.2)';
          ctx.fillRect(fadeStartX, 0, fadeWidth, canvasHeight);
          
          // Draw diagonal line from top-left to bottom-right (fade out)
          ctx.strokeStyle = 'rgba(220, 38, 38, 0.8)';
          ctx.lineWidth = 2;
          ctx.beginPath();
          ctx.moveTo(fadeStartX, 0);
          ctx.lineTo(fadeEndX, canvasHeight);
          ctx.stroke();
          
          // Draw red vertical line at fade start boundary
          ctx.beginPath();
          ctx.moveTo(fadeStartX, 0);
          ctx.lineTo(fadeStartX, canvasHeight);
          ctx.stroke();
        }
      }
    }
    
    // Cross Fade visualization - Yellow X with semi-transparent rectangle
    if (props.audioItem.crossFade && props.audioItem.crossFade > 0) {
      const crossFadeStartTime = outPoint.value - props.audioItem.crossFade;
      const crossFadeEndTime = outPoint.value;
      
      // Only draw if visible in current view
      if (crossFadeStartTime <= visibleEnd.value && crossFadeEndTime >= visibleStart.value) {
        const crossStartX = Math.max(0, (crossFadeStartTime - visibleStart.value) * pixelsPerSecond);
        const crossEndX = Math.min(canvasWidth.value, (crossFadeEndTime - visibleStart.value) * pixelsPerSecond);
        const crossWidth = crossEndX - crossStartX;
        
        if (crossWidth > 0) {
          // Draw yellow rectangle with 20% opacity
          ctx.fillStyle = 'rgba(234, 179, 8, 0.2)';
          ctx.fillRect(crossStartX, 0, crossWidth, canvasHeight);
          
          // Draw yellow X pattern
          ctx.strokeStyle = 'rgba(234, 179, 8, 0.8)';
          ctx.lineWidth = 2;
          
          // Diagonal line from top-left to bottom-right
          ctx.beginPath();
          ctx.moveTo(crossStartX, 0);
          ctx.lineTo(crossEndX, canvasHeight);
          ctx.stroke();
          
          // Diagonal line from bottom-left to top-right
          ctx.beginPath();
          ctx.moveTo(crossStartX, canvasHeight);
          ctx.lineTo(crossEndX, 0);
          ctx.stroke();
          
          // Draw vertical lines at boundaries
          ctx.beginPath();
          ctx.moveTo(crossStartX, 0);
          ctx.lineTo(crossStartX, canvasHeight);
          ctx.stroke();
          
          ctx.beginPath();
          ctx.moveTo(crossEndX, 0);
          ctx.lineTo(crossEndX, canvasHeight);
          ctx.stroke();
        }
      }
    }
  }
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
watch([
  zoomLevel, 
  scrollPosition, 
  () => props.audioItem?.volume, 
  () => props.audioItem?.inPoint, 
  () => props.audioItem?.outPoint,
  () => props.audioItem?.playFade,
  () => props.audioItem?.stopFade,
  () => props.audioItem?.crossFade,
  waveformData, 
  playbackPosition
], () => {
  throttledDraw();
});

// Also watch for waveform changes directly (deep watch for reactivity)
watch(() => props.audioItem?.waveform, () => {
  console.log('Waveform changed, redrawing...');
  throttledDraw();
}, { deep: true });

// Watch for canvas width changes
const resizeObserver = ref<ResizeObserver | null>(null);

onMounted(() => {
  // Set initial container width
  if (waveformContainer.value) {
    containerWidth.value = waveformContainer.value.clientWidth;
  }
  
  // Initial draw
  setTimeout(() => {
    drawWaveform();
  }, 100);

  if (waveformContainer.value) {
    resizeObserver.value = new ResizeObserver(() => {
      // Update the reactive container width
      if (waveformContainer.value) {
        containerWidth.value = waveformContainer.value.clientWidth;
      }
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
  max-height: 149px;
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

/* Fade Controls Section */
.fade-controls-section {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: var(--spacing-xs);
  padding: var(--spacing-xs);
  background: var(--color-surface);
  border-radius: var(--border-radius-sm);
  width: 300px;
}

.fade-control-group {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.fade-control-group label {
  font-size: 10px;
  color: var(--color-text-secondary);
  text-transform: uppercase;
  font-weight: 500;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 100px;
  text-wrap: wrap;
}

.fade-input {
  width: 100%;
  padding: 4px var(--spacing-xs);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-primary);
  font-family: 'Courier New', monospace;
  font-size: 11px;
  text-align: center;
}

.fade-input:focus {
  outline: none;
  border-color: var(--color-accent);
}

.fade-unit {
  display: inline-block;
  font-size: 9px;
  color: var(--color-text-secondary);
  margin-top: 2px;
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
  gap: var(--spacing-md);
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

.audio-tools {
  display: flex;
  gap: 8px;
  align-items: center;
}

.trim-silence-btn,
.normalize-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 12px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-secondary);
  font-size: 13px;
  cursor: pointer;
  transition: all 0.2s;
  
  &:hover {
    background: var(--color-surface-hover);
    border-color: var(--color-accent);
  }
  
  .material-symbols-rounded {
    font-size: 18px;
  }
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

/* Fade Handles */
.fade-handle {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  cursor: ew-resize;
  z-index: 11;
  user-select: none;
}

.fade-line {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
}

.fade-line-red {
  background: rgba(220, 38, 38, 0.8);
}

.fade-line-yellow {
  background: rgba(234, 179, 8, 0.8);
}

.fade-grip {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  width: 24px;
  height: 24px;
  background: var(--color-surface);
  border: 2px solid currentColor;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.3);
  user-select: none;
  transition: transform 0.15s ease;
}

.fade-handle:hover .fade-grip {
  transform: translateY(-50%) scale(1.15);
}

.fade-grip-red {
  color: rgb(220, 38, 38);
  border-color: rgb(220, 38, 38);
}

.fade-grip-yellow {
  color: rgb(234, 179, 8);
  border-color: rgb(234, 179, 8);
}

.fade-handle-play .fade-grip {
  left: 50%;
  transform: translate(-50%, -50%);
}

.fade-handle-play:hover .fade-grip {
  transform: translate(-50%, -50%) scale(1.15);
}

.fade-handle-stop .fade-grip,
.fade-handle-cross .fade-grip {
  left: 50%;
  transform: translate(-50%, -50%);
}

.fade-handle-stop:hover .fade-grip,
.fade-handle-cross:hover .fade-grip {
  transform: translate(-50%, -50%) scale(1.15);
}

.fade-grip .material-symbols-rounded {
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

/* Time Display Section (right side) */
.time-display-section {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  width: 150px;
  padding: var(--spacing-xs);
  background: var(--color-surface);
  border-radius: var(--border-radius-sm);
}

.time-field {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.time-field label {
  font-size: 10px;
  color: var(--color-text-secondary);
  text-transform: uppercase;
  font-weight: 500;
}

.time-input-with-buttons {
  display: flex;
  align-items: center;
  gap: 2px;
}

.time-decrement,
.time-increment {
  padding: 2px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-secondary);
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  min-width: 20px;
  height: 22px;
  transition: all 0.15s ease;
}

.time-decrement:hover,
.time-increment:hover {
  background: var(--color-background-hover);
  color: var(--color-text-primary);
  border-color: var(--color-accent);
}

.time-decrement:active,
.time-increment:active {
  transform: scale(0.95);
}

.time-decrement .material-symbols-rounded,
.time-increment .material-symbols-rounded {
  font-size: 16px;
  font-variation-settings: 'FILL' 0, 'wght' 400, 'GRAD' 0, 'opsz' 20;
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
  flex: 1;
  min-width: 0;
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
