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
    
    <!-- Master Section (MIX meter + OUT volume) -->
    <div class="master-section">
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
      
      <!-- Master Output Volume -->
      <div class="master-volume">
        <div class="master-volume-label">OUT</div>
        <div class="master-volume-slider-container">
          <input
            type="range"
            orient="vertical"
            class="master-volume-slider-vertical"
            :min="-60"
            :max="0"
            step="0.1"
            :value="masterGainDb"
            @input="handleMasterVolumeChange"
            :title="`${masterGainDb <= -60 ? '-∞' : masterGainDb.toFixed(1)} dB`"
            :style="{ '--volume-handle-color': masterVolumeHandleColor }"
          />
          <div class="master-volume-markers">
            <span>0</span>
            <span>-12</span>
            <span>-24</span>
            <span>-∞</span>
          </div>
        </div>
        <div class="master-db-value">{{ masterGainDb <= -60 ? '-∞' : masterGainDb.toFixed(1) }} dB</div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const { activeCues, panicStop, masterOutputLevel, masterPeakLevel, masterGainDb, setMasterGain } = useAudioEngine();
const { t } = useLocalization();

const handlePanic = () => {
  panicStop();
};

const handleMasterVolumeChange = (event: Event) => {
  const target = event.target as HTMLInputElement;
  setMasterGain(parseFloat(target.value));
};

const masterVolumeHandleColor = computed(() => {
  const db = masterGainDb.value;
  if (db <= -60) return '#666';
  if (db < -6) return '#22c55e';
  if (db < -1) return '#eab308';
  return '#dc2626';
});
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

.master-section {
  display: flex;
  gap: var(--spacing-md);
  align-items: stretch;
  height: 100%;
  padding-left: var(--spacing-md);
  border-left: 2px solid var(--color-border);
}

.master-meter {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacing-xs);
  height: 100%;
  justify-content: center;
  padding-bottom: 16px;
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

.master-volume {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacing-xs);
  height: 100%;
  justify-content: center;
  min-width: 70px;
}

.master-volume-label {
  font-size: 11px;
  font-weight: 600;
  color: var(--color-text-secondary);
  letter-spacing: 0.5px;
  margin-bottom: var(--spacing-xs);
}

.master-db-value {
  font-size: 11px;
  font-weight: 400;
  color: var(--color-text-secondary);
  margin-top: var(--spacing-xs);
}

.master-volume-slider-container {
  display: flex;
  gap: var(--spacing-xs);
  align-items: center;
  flex: 1;
  min-height: 0;
}

.master-volume-slider-vertical {
  writing-mode: vertical-lr;
  direction: rtl;
  width: 20px;
  height: 100%;
  cursor: pointer;
  -webkit-appearance: slider-vertical;
  appearance: slider-vertical;
  background: transparent;
  border-radius: 4px;
  position: relative;
}

.master-volume-slider-vertical::-webkit-slider-runnable-track {
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
    #dc2626 100%
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

.master-volume-slider-vertical::-moz-range-track {
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
    #dc2626 100%
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

.master-volume-slider-vertical::-webkit-slider-thumb {
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

.master-volume-slider-vertical::-moz-range-thumb {
  width: 24px;
  height: 14px;
  background: var(--volume-handle-color, var(--color-text-primary));
  cursor: pointer;
  border-radius: 3px;
  border: 2px solid rgba(255, 255, 255, 0.3);
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.4);
}

.master-volume-markers {
  display: flex;
  flex-direction: column;
  justify-content: space-between;
  height: 100%;
  font-size: 9px;
  color: var(--color-text-secondary);
}
</style>
