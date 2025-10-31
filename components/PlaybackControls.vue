<template>
  <div class="playback-controls">
    <div class="controls-left">
      <button class="control-btn play-btn" @click="handlePlay" :disabled="!selectedItem">
        <span class="icon">▶</span>
        <span>PLAY</span>
      </button>
      
      <button class="control-btn" @click="handlePause" :disabled="activeCues.size === 0">
        <span class="icon">⏸</span>
      </button>
      
      <button class="control-btn" @click="handleStop" :disabled="activeCues.size === 0">
        <span class="icon">⏹</span>
      </button>
    </div>
    
    <div class="active-cues">
      <div v-if="activeCues.size === 0" class="no-cues">
        No active cues
      </div>
      
      <div v-else class="cue-list">
        <ActiveCueItem
          v-for="[uuid, cue] in Array.from(activeCues.entries())"
          :key="uuid"
          :cue="cue"
        />
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const { selectedItem } = useProject();
const { activeCues, playCue, stopAllCues } = useAudioEngine();

const handlePlay = () => {
  if (selectedItem.value && selectedItem.value.type === 'audio') {
    playCue(selectedItem.value as any);
  }
};

const handlePause = () => {
  // Pause functionality could be enhanced
  stopAllCues();
};

const handleStop = () => {
  stopAllCues();
};
</script>

<style scoped>
.playback-controls {
  height: var(--playback-controls-height);
  border-bottom: 1px solid var(--color-border);
  display: flex;
  align-items: center;
  gap: var(--spacing-lg);
  padding: 0 var(--spacing-lg);
  background-color: var(--color-surface);
}

.controls-left {
  display: flex;
  gap: var(--spacing-sm);
}

.control-btn {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  padding: var(--spacing-md) var(--spacing-lg);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  font-weight: 500;
  
  &:hover:not(:disabled) {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
  }
  
  &:disabled {
    opacity: 0.5;
  }
}

.play-btn {
  background-color: var(--color-accent);
  border-color: var(--color-accent);
  color: white;
  font-size: 18px;
  font-weight: 600;
  
  &:hover:not(:disabled) {
    background-color: var(--color-accent-hover);
    border-color: var(--color-accent-hover);
  }
}

.icon {
  font-size: 20px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.active-cues {
  flex: 1;
  min-width: 0;
  overflow-y: auto;
  padding: var(--spacing-sm) 0;
}

.no-cues {
  color: var(--color-text-secondary);
  font-style: italic;
  padding: var(--spacing-md);
}

.cue-list {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-sm);
}
</style>
