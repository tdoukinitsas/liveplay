<template>
  <div class="vu-meter" :class="{ 'is-master': isMaster }">
    <!-- dB Scale -->
    <div class="db-scale">
      <div class="db-mark" v-for="mark in dbMarks" :key="mark.db" :style="{ bottom: `${mark.position}%` }">
        <span class="db-label">{{ mark.label }}</span>
      </div>
    </div>
    
    <!-- Meter Bar -->
    <div class="meter-container">
      <div class="meter-track">
        <!-- Red zone (0 to -6 dB) -->
        <div class="meter-zone red"></div>
        <!-- Yellow zone (-6 to -18 dB) -->
        <div class="meter-zone yellow"></div>
        <!-- Green zone (-18 to -60 dB) -->
        <div class="meter-zone green"></div>
        
        <!-- Active level -->
        <div class="meter-level" :style="levelStyle"></div>
        
        <!-- Peak hold indicator -->
        <div v-if="showPeakHold && peakHold > -60" class="peak-hold" :style="peakHoldStyle"></div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const props = defineProps<{
  level: number; // Current level in dB (-60 to 0)
  peakLevel?: number; // Peak level in dB (for peak hold)
  isMaster?: boolean; // Is this the master meter (wider)
  showPeakHold?: boolean; // Show peak hold indicator
}>();

// dB marks to show on scale
const dbMarks = [
  { db: 0, label: '0', position: 100 },
  { db: -6, label: '-6', position: 90 },
  { db: -12, label: '-12', position: 80 },
  { db: -18, label: '-18', position: 70 },
  { db: -24, label: '-24', position: 60 },
  { db: -30, label: '-30', position: 50 },
  { db: -40, label: '-40', position: 33.3 },
  { db: -50, label: '-50', position: 16.7 },
  { db: -60, label: '-60', position: 0 }
];

// Convert dB to percentage (0-100) for display
// -60 dB = 0%, 0 dB = 100%
const dbToPercent = (db: number): number => {
  return Math.max(0, Math.min(100, ((db + 60) / 60) * 100));
};

const levelStyle = computed(() => ({
  height: `${dbToPercent(props.level)}%`,
  backgroundColor: getLevelColor(props.level)
}));

const peakHold = computed(() => props.peakLevel ?? -60);

const peakHoldStyle = computed(() => ({
  bottom: `${dbToPercent(peakHold.value)}%`
}));

// Get color based on dB level
const getLevelColor = (db: number): string => {
  if (db >= -6) return '#f44336'; // Red
  if (db >= -18) return '#ffc107'; // Yellow
  return '#4caf50'; // Green
};
</script>

<style scoped lang="scss">
.vu-meter {
  display: flex;
  gap: 4px;
  height: 100%;
  min-height: 80px;
  
  &.is-master {
    .meter-container {
      width: 16px;
    }
    
    .db-scale {
      font-size: 11px;
    }
  }
}

.db-scale {
  position: relative;
  height: 100%;
  min-width: 24px;
  font-size: 9px;
  color: var(--color-text-secondary);
}

.db-mark {
  position: absolute;
  right: 0;
  transform: translateY(50%);
  
  .db-label {
    display: block;
    text-align: right;
    line-height: 1;
    user-select: none;
    opacity: .3;
    font-size: .6em;
  }
}

.meter-container {
  width: 12px;
  height: 100%;
  position: relative;
  display: flex;
  flex-direction: column;
}

.meter-track {
  width: 100%;
  height: 100%;
  background-color: rgba(0, 0, 0, 0.3);
  border-radius: 2px;
  position: relative;
  overflow: hidden;
  display: flex;
  flex-direction: column-reverse;
}

.meter-zone {
  width: 100%;
  opacity: 0.2;
  
  &.red {
    height: 10%; // 0 to -6 dB
    background-color: #f44336;
  }
  
  &.yellow {
    height: 20%; // -6 to -18 dB
    background-color: #ffc107;
  }
  
  &.green {
    height: 70%; // -18 to -60 dB
    background-color: #4caf50;
  }
}

.meter-level {
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  transition: height 50ms linear, background-color 100ms ease;
  border-radius: 2px;
}

.peak-hold {
  position: absolute;
  left: 0;
  right: 0;
  height: 2px;
  background-color: white;
  box-shadow: 0 0 4px rgba(255, 255, 255, 0.8);
  transition: bottom 100ms ease-out;
}
</style>
