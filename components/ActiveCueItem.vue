<template>
  <div 
    class="active-cue-item" 
    :class="{
      'warning-yellow': warningState === 'yellow',
      'warning-orange': warningState === 'orange',
      'warning-red': warningState === 'red'
    }"
    :style="itemStyle"
  >
    <div class="cue-content">
      <div class="cue-header">
        <span class="cue-name">{{ cue.displayName }}</span>
        <button class="stop-btn" @click="handleStop" :title="t('actions.stop')">
          <span class="material-symbols-rounded">stop</span>
        </button>
      </div>
      
      <div class="cue-progress">
        <div class="time-info">
          <span>{{ formatTime(cue.currentTime) }}</span>
          <span>{{ formatTime(cue.duration - cue.currentTime) }}</span>
        </div>
        
        <div class="progress-bar" @click="handleSeek">
          <div class="progress-fill" :style="progressStyle"></div>
          <div 
            class="progress-handle" 
            :style="{ 
              left: `${progress}%`,
              borderColor: cue.color || 'var(--color-accent)'
            }"
          ></div>
        </div>
      </div>
    </div>
    
    <!-- VU Meter -->
    <div class="cue-meter">
      <VUMeter 
        :level="cue.currentLevel ?? -60" 
        :peakLevel="cue.peakLevel ?? -60"
        :showPeakHold="true"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
interface ActiveCueState {
  uuid: string;
  displayName: string;
  duration: number;
  currentTime: number;
  volume: number;
  isDucked: boolean;
  originalVolume: number;
  howl?: any;
  progressInterval?: any;
  color?: string;
  inPoint?: number;
  outPoint?: number;
  currentLevel?: number;
  peakLevel?: number;
}

const props = defineProps<{
  cue: ActiveCueState;
}>();

const { stopCue } = useAudioEngine();
const { t } = useLocalization();

// Use the cue's currentTime directly (updated by the audio engine)
const progress = computed(() => {
  if (!props.cue.duration || props.cue.duration === 0) return 0;
  return (props.cue.currentTime / props.cue.duration) * 100;
});

// Warning state based on time remaining
// Note: This is per-cue visual feedback only
// The ProjectHeader handles the actual silence detection across all cues
const warningState = computed(() => {
  const timeRemaining = props.cue.duration - props.cue.currentTime;
  if (timeRemaining <= 5) return 'red';
  if (timeRemaining <= 10) return 'orange';
  if (timeRemaining <= 30) return 'yellow';
  return null;
});

// Helper to convert hex to rgba
const hexToRgba = (hex: string, alpha: number): string => {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
};

// Use item color for background if available
const itemStyle = computed(() => {
  if (props.cue.color) {
    return {
      backgroundColor: hexToRgba(props.cue.color, 0.15),
      borderColor: props.cue.color
    };
  }
  return {};
});

// Use item color for progress if available
const progressStyle = computed(() => {
  const width = `${progress.value}%`;
  if (props.cue.color) {
    return {
      width,
      backgroundColor: props.cue.color
    };
  }
  return { width };
});

const handleStop = () => {
  stopCue(props.cue.uuid);
};

const handleSeek = (e: MouseEvent) => {
  // Seeking with Howler
  if (props.cue.howl) {
    const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
    const x = e.clientX - rect.left;
    const percent = x / rect.width;
    
    // Calculate seek time relative to trimmed duration
    const relativeSeekTime = percent * props.cue.duration;
    
    // For trimmed items, add inPoint to get absolute position in file
    const absoluteSeekTime = relativeSeekTime + (props.cue.inPoint || 0);
    
    props.cue.howl.seek(absoluteSeekTime);
  }
};

const formatTime = (seconds: number): string => {
  const mins = Math.floor(seconds / 60);
  const secs = Math.floor(seconds % 60);
  return `${mins}:${secs.toString().padStart(2, '0')}`;
};
</script>

<style scoped lang="scss">
.active-cue-item {
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  padding: var(--spacing-sm) var(--spacing-md);
  transition: all var(--transition-fast);
  min-width: 400px;
  max-width: 400px;
  display: flex;
  gap: var(--spacing-sm);
  
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

.cue-content {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
}

.cue-meter {
  display: flex;
  align-items: stretch;
  padding-left: var(--spacing-sm);
  border-left: 1px solid var(--color-border);
}

.cue-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: var(--spacing-sm);
}

.cue-name {
  font-weight: 500;
  flex: 1;
  min-width: 0;
  color: var(--color-text-primary);
  position: relative;
  
  /* Nice fade-out effect with gradient mask */
  display: block;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  
  /* Gradient fade at the end */
  mask-image: linear-gradient(to right, black 80%, transparent 100%);
  -webkit-mask-image: linear-gradient(to right, black 80%, transparent 100%);
}

.stop-btn {
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background-color: var(--color-danger);
  color: white;
  font-size: 20px;
  line-height: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  
  &:hover {
    opacity: 0.8;
  }
}

.cue-progress {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
}

.time-info {
  display: flex;
  justify-content: space-between;
  font-size: 12px;
  color: var(--color-text-secondary);
}

.progress-bar {
  height: 8px;
  background-color: var(--color-surface);
  border-radius: var(--border-radius-sm);
  position: relative;
  cursor: pointer;
  /* Force LTR direction for progress bars in RTL languages */
  direction: ltr;
  
  &:hover {
    .progress-handle {
      opacity: 1;
    }
  }
}

.progress-fill {
  height: 100%;
  background-color: var(--color-accent);
  border-radius: var(--border-radius-sm);
  transition: width 100ms linear;
}

.progress-handle {
  position: absolute;
  top: 50%;
  transform: translate(-50%, -50%);
  width: 16px;
  height: 16px;
  background-color: white;
  border: 2px solid var(--color-accent);
  border-radius: 50%;
  opacity: 0;
  transition: opacity var(--transition-fast);
  pointer-events: none;
}
</style>
