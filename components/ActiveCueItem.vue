<template>
  <div class="active-cue-item">
    <div class="cue-header">
      <span class="cue-name">{{ cue.displayName }}</span>
      <button class="stop-btn" @click="handleStop">×</button>
    </div>
    
    <div class="cue-progress">
      <div class="time-info">
        <span>{{ formatTime(cue.currentTime) }}</span>
        <span>{{ formatTime(cue.duration - cue.currentTime) }}</span>
      </div>
      
      <div class="progress-bar" @click="handleSeek">
        <div class="progress-fill" :style="{ width: `${progress}%` }"></div>
        <div class="progress-handle" :style="{ left: `${progress}%` }"></div>
      </div>
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
}

const props = defineProps<{
  cue: ActiveCueState;
}>();

const { stopCue } = useAudioEngine();

// Use the cue's currentTime directly (updated by the audio engine)
const progress = computed(() => {
  if (!props.cue.duration || props.cue.duration === 0) return 0;
  return (props.cue.currentTime / props.cue.duration) * 100;
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
    const seekTime = percent * props.cue.duration;
    props.cue.howl.seek(seekTime);
  }
};

const formatTime = (seconds: number): string => {
  const mins = Math.floor(seconds / 60);
  const secs = Math.floor(seconds % 60);
  return `${mins}:${secs.toString().padStart(2, '0')}`;
};
</script>

<style scoped>
.active-cue-item {
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  padding: var(--spacing-sm) var(--spacing-md);
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
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: var(--color-text-primary);
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
