<template>
  <div class="playback-controls">
    <div class="controls-left">
      <button class="control-btn panic-btn" @click="handlePanic" :disabled="activeCues.size === 0">
        <span class="icon">⚠</span>
        <span>{{ t('playback.panic') }}</span>
      </button>
    </div>
    
    <div class="active-cues">
      <div v-if="activeCues.size === 0" class="no-cues">
        {{ t('playback.noActiveCues') }}
      </div>
      
      <div v-else class="cue-list">
        <ActiveCueItem
          v-for="[uuid, cue] in Array.from(activeCues.entries())"
          :key="uuid"
          :cue="cue"
        />
      </div>
    </div>
    
    <!-- Master Mix Meter -->
    <div class="master-meter" v-if="activeCues.size > 0">
      <div class="master-label">MIX</div>
      <div class="master-meter-wrapper">
        <VUMeter 
          :level="masterOutputLevel" 
          :peakLevel="masterPeakLevel"
          :isMaster="true"
          :showPeakHold="true"
        />
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const { activeCues, panicStop, masterOutputLevel, masterPeakLevel } = useAudioEngine();
const { t } = useLocalization();

const handlePanic = () => {
  panicStop();
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

.panic-btn {
  background-color: var(--color-danger);
  border-color: var(--color-danger);
  color: white;
  font-size: 18px;
  font-weight: 600;
  
  &:hover:not(:disabled) {
    opacity: 0.8;
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
  overflow-x: auto;
  overflow-y: hidden;
  padding: var(--spacing-sm) 0;
}

.no-cues {
  color: var(--color-text-secondary);
  font-style: italic;
  padding: var(--spacing-md);
}

.cue-list {
  display: flex;
  flex-direction: row;
  gap: var(--spacing-sm);
}

.master-meter {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacing-xs);
  padding-left: var(--spacing-md);
  border-left: 2px solid var(--color-border);
  height: 100%;
  justify-content: center;
}

.master-label {
  font-size: 11px;
  font-weight: 600;
  color: var(--color-text-secondary);
  letter-spacing: 0.5px;
  margin-bottom: var(--spacing-xs);
}

.master-meter-wrapper {
  flex: 1;
  display: flex;
  max-height: calc(var(--playback-controls-height) - 40px);
}
</style>
