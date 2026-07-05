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
        <div class="cue-actions">
          <button 
            v-if="!cue.isPaused" 
            class="action-btn pause-btn" 
            @click="handlePause" 
            :title="t('actions.pause')"
          >
            <span class="material-symbols-rounded">pause</span>
          </button>
          <button 
            v-if="cue.isPaused" 
            class="action-btn resume-btn" 
            @click="handleResume" 
            :title="t('actions.resume')"
          >
            <span class="material-symbols-rounded">play_arrow</span>
          </button>
          <button class="action-btn stop-btn" @click="handleStop" :title="t('actions.stop')">
            <span class="material-symbols-rounded">stop</span>
          </button>
        </div>
      </div>
      
      <div class="cue-progress">
        <div class="time-info">
          <span>{{ formatTime(cue.currentTime) }}</span>
          <!-- Segue countdown: time until the Start Next marker fires -->
          <span
            v-if="segueCountdown !== null"
            class="segue-countdown"
            :class="{ 'segue-countdown--imminent': segueCountdown <= 5 }"
            :title="t('playback.startNextCountdown')"
          >
            <span class="material-symbols-rounded">skip_next</span>
            {{ segueCountdown.toFixed(1) }}s
          </span>
          <span>-{{ formatTime(cue.duration - cue.currentTime) }}</span>
        </div>

        <div class="progress-bar" @click="handleSeek">
          <div class="progress-fill" :style="progressStyle"></div>
          <!-- Start Next marker tick on the progress bar -->
          <div
            v-if="seguePercent !== null"
            class="segue-tick"
            :style="{ left: `${seguePercent}%` }"
          ></div>
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
    
    <!-- VU Meter — drawn from the server's live meter stream so it tracks
         what the audio engine is actually outputting, not a waveform-based
         estimate. Stereo widget shows L/R per source channel. -->
    <div class="cue-meter">
      <StereoMeter
        :cue-id="serverCueId"
        :min-db="-60"
        :max-db="0"
      />
    </div>

    <!-- End-of-cue warning border. Inset overlay so the thick border stays
         inside the item box and is never clipped by the active-cue strip's
         overflow: hidden. -->
    <div
      v-if="warningState"
      class="warning-border"
      :class="`warning-border--${warningState}`"
    ></div>
  </div>
</template>

<script setup lang="ts">
// Projection of the server's view of an active cue. Owned by useAudioEngine
// which rebuilds it from cue_state / playback_snapshot / meters broadcasts.
// No client-side playback state lives here.
interface ActiveCueState {
  uuid: string;
  displayName: string;
  duration: number;
  currentTime: number;
  isPaused: boolean;
  color?: string;
  inPoint?: number;
  outPoint?: number;
  serverCueId?: string | null;
}

const props = defineProps<{
  cue: ActiveCueState;
}>();

const { stopCue, pauseCue, resumeCue, seekCue } = useAudioEngine();
const { t } = useLocalization();
const { findItemByUuid } = useProject();

// Start Next marker (absolute file time) from the project item, if armed.
const startNextTime = computed<number | null>(() => {
  const item = findItemByUuid(props.cue.uuid) as any;
  if (!item || item.type !== 'audio') return null;
  if (!item.startNextEnabled || !(item.startNextTime > 0)) return null;
  return item.startNextTime as number;
});

// Seconds until the marker fires; null once passed (or when not armed).
// currentTime is relative to the in point, the marker is absolute file time.
const segueCountdown = computed<number | null>(() => {
  if (startNextTime.value === null) return null;
  const absolutePos = props.cue.currentTime + (props.cue.inPoint || 0);
  const remaining = startNextTime.value - absolutePos;
  return remaining > 0 ? remaining : null;
});

// Marker position on the (trimmed) progress bar, 0–100.
const seguePercent = computed<number | null>(() => {
  if (startNextTime.value === null || !props.cue.duration) return null;
  const rel = (startNextTime.value - (props.cue.inPoint || 0)) / props.cue.duration;
  if (rel <= 0 || rel >= 1) return null;
  return rel * 100;
});

// Server engine cue ID — populated in onload once the server registers the
// cue and returns its ID. Used by StereoMeter to subscribe to the right
// WS meter frame.
const serverCueId = computed<string | null>(() => props.cue.serverCueId ?? null);

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

const handlePause = () => {
  pauseCue(props.cue.uuid);
};

const handleResume = () => {
  resumeCue(props.cue.uuid);
};

const handleSeek = (e: MouseEvent) => {
  const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
  const x = e.clientX - rect.left;
  const percent = x / rect.width;
  // Trimmed → absolute file time.
  const absoluteSeekTime = percent * props.cue.duration + (props.cue.inPoint || 0);
  seekCue(props.cue.uuid, absoluteSeekTime);
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
  position: relative;
}

/* Solid 4px end-of-cue warning border. Inset overlay pinned inside the item
   box so it cannot be clipped by the active-cue strip's overflow. */
.warning-border {
  position: absolute;
  inset: 0;
  z-index: 10;
  pointer-events: none;
  border: 4px solid transparent;
  border-radius: var(--border-radius-md);

  /* Blink rates mirror the ProjectHeader silence-warning banner so the border
     and banner pulse in sync (yellow ≤30s, orange ≤10s, red ≤5s). */
  &.warning-border--yellow {
    border-color: rgb(255, 193, 7);
    animation: warning-border-flash 2s ease-in-out infinite;
  }

  &.warning-border--orange {
    border-color: rgb(255, 152, 0);
    animation: warning-border-flash 1s ease-in-out infinite;
  }

  &.warning-border--red {
    border-color: rgb(244, 67, 54);
    animation: warning-border-flash 0.5s ease-in-out infinite;
  }
}

@keyframes warning-border-flash {
  0%, 100% { opacity: 0; }
  50% { opacity: 1; }
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

.cue-actions {
  display: flex;
  gap: 4px;
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

.action-btn {
  width: 24px;
  height: 24px;
  border-radius: 50%;
  color: white;
  font-size: 20px;
  line-height: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  
  &.pause-btn, &.resume-btn {
    background-color: #ff9800; /* Orange color for pause/resume */
  }
  
  &.stop-btn {
    background-color: var(--color-danger);
  }
  
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

.segue-countdown {
  display: inline-flex;
  align-items: center;
  gap: 2px;
  color: rgb(22, 163, 74);
  font-weight: 600;

  .material-symbols-rounded {
    font-size: 14px;
  }

  &.segue-countdown--imminent {
    animation: segue-pulse 1s ease-in-out infinite;
  }
}

@keyframes segue-pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.35; }
}

.segue-tick {
  position: absolute;
  top: -2px;
  bottom: -2px;
  width: 2px;
  background: rgb(22, 163, 74);
  pointer-events: none;
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
