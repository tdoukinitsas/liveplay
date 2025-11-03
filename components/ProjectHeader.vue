<template>
  <div class="project-header">
    <div class="header-left">
      <img 
        :src="isDark ? './assets/icons/SVG/liveplay-icon-darkmode@web.svg' : './assets/icons/SVG/liveplay-icon-lightmode@web.svg'"
        alt="LivePlay"
        class="header-logo"
      />
      <h2 class="project-name">{{ currentProject?.name || t('project.noProject') }}</h2>
    </div>
    
    <div 
      v-if="silenceWarning" 
      class="silence-warning"
      :class="silenceWarningClass"
    >
      {{ t('project.silenceWarning') }} {{ Math.ceil(silenceWarning) }} {{ t('project.seconds') }}
    </div>
    
    <div class="header-right">
      <div class="digital-clock">{{ currentTime }}</div>
    </div>
  </div>
</template>

<script setup lang="ts">
const { currentProject, findItemByUuid, findItemByIndex } = useProject();
const { t } = useLocalization();
const { activeCues } = useAudioEngine();

const isDark = computed(() => currentProject.value?.theme.mode === 'dark');
const currentTime = ref('00:00:00');

// Silence warning system
const silenceWarning = ref<number | null>(null);

const silenceWarningClass = computed(() => {
  if (!silenceWarning.value) return '';
  
  const seconds = silenceWarning.value;
  if (seconds <= 5) return 'flash-fast'; // Fast red flash
  if (seconds <= 10) return 'flash-medium'; // Medium red flash
  if (seconds <= 30) return 'flash-slow'; // Slow orange flash
  return 'warning-yellow'; // Yellow background
});

// Check if we're approaching silence
const checkForSilence = () => {
  if (!currentProject.value || activeCues.value.size === 0) {
    silenceWarning.value = null;
    return;
  }
  
  // Track when each cue will end
  const cueEndTimes = new Map<string, { time: number; hasValidBehavior: boolean }>();
  
  // Collect end times and behaviors for all cues
  for (const [uuid, cue] of activeCues.value) {
    const item = findItemByUuid(uuid);
    if (!item || item.type !== 'audio') continue;
    
    const audioItem = item as any;
    const timeRemaining = cue.duration - cue.currentTime;
    const hasValidEndBehavior = validateEndBehavior(audioItem);
    
    cueEndTimes.set(uuid, {
      time: timeRemaining,
      hasValidBehavior: hasValidEndBehavior
    });
  }
  
  // Special case: only one cue playing
  if (cueEndTimes.size === 1) {
    const [uuid, { time, hasValidBehavior }] = Array.from(cueEndTimes.entries())[0];
    if (!hasValidBehavior && time <= 60) {
      silenceWarning.value = time;
      return;
    }
    silenceWarning.value = null;
    return;
  }
  
  // Multiple cues: find the minimum time until we have NO active cues
  let minTimeToActualSilence = Infinity;
  
  // Sort cues by end time
  const sortedCues = Array.from(cueEndTimes.entries())
    .sort((a, b) => a[1].time - b[1].time);
  
  // Check each point in time where a cue ends
  for (let i = 0; i < sortedCues.length; i++) {
    const [uuid, { time, hasValidBehavior }] = sortedCues[i];
    
    // Count how many cues will still be playing at this time
    let cuesStillPlaying = 0;
    let willSpawnNewCue = hasValidBehavior;
    
    for (let j = 0; j < sortedCues.length; j++) {
      if (i === j) continue; // Skip the cue that's ending
      const otherTime = sortedCues[j][1].time;
      if (otherTime > time) {
        cuesStillPlaying++;
      }
    }
    
    // If this cue ends and there are no other cues playing and it won't spawn a new cue
    // then we have silence
    if (cuesStillPlaying === 0 && !willSpawnNewCue) {
      minTimeToActualSilence = Math.min(minTimeToActualSilence, time);
      break; // Found the first point of silence
    }
  }
  
  // Only show warning if we're within 60 seconds of actual silence
  if (minTimeToActualSilence <= 60 && minTimeToActualSilence !== Infinity) {
    silenceWarning.value = minTimeToActualSilence;
  } else {
    silenceWarning.value = null;
  }
};

// Validate if end behavior will actually trigger something (not lead to silence)
const validateEndBehavior = (audioItem: any): boolean => {
  if (!audioItem.endBehavior || audioItem.endBehavior.action === 'nothing') {
    return false; // No end behavior = silence
  }
  
  const action = audioItem.endBehavior.action;
  
  if (action === 'next' || action === 'play-next') {
    // Check if there's actually a next item
    const currentIndex = audioItem.index;
    if (!currentIndex || !currentProject.value) return false;
    
    // Find parent and check for next sibling
    const parentIndex = currentIndex.slice(0, -1);
    const currentPosition = currentIndex[currentIndex.length - 1];
    
    if (parentIndex.length === 0) {
      // Top level - check project items
      const nextItem = currentProject.value.items[currentPosition + 1];
      return !!nextItem; // Valid if next item exists
    } else {
      // Inside a group - need to find parent group
      const parent = findItemByIndex(parentIndex);
      if (parent && parent.type === 'group') {
        const nextItem = (parent as any).children[currentPosition + 1];
        return !!nextItem; // Valid if next sibling exists
      }
      return false;
    }
  }
  
  if (action === 'goto-item') {
    // Check if target UUID is valid
    const targetUuid = audioItem.endBehavior.targetUuid;
    if (!targetUuid) return false;
    
    const targetItem = findItemByUuid(targetUuid);
    return !!targetItem; // Valid if target item exists
  }
  
  if (action === 'goto-index') {
    // Check if target index is valid
    const targetIndex = audioItem.endBehavior.targetIndex;
    if (!targetIndex || !Array.isArray(targetIndex)) return false;
    
    const targetItem = findItemByIndex(targetIndex);
    return !!targetItem; // Valid if target index resolves to an item
  }
  
  if (action === 'loop') {
    return true; // Loop always continues
  }
  
  return false; // Unknown action = assume silence
};

const updateClock = () => {
  const now = new Date();
  const hours = now.getHours().toString().padStart(2, '0');
  const minutes = now.getMinutes().toString().padStart(2, '0');
  const seconds = now.getSeconds().toString().padStart(2, '0');
  currentTime.value = `${hours}:${minutes}:${seconds}`;
};

onMounted(() => {
  updateClock();
  const clockInterval = setInterval(updateClock, 1000);
  
  // Check for silence every 100ms for accuracy
  const silenceInterval = setInterval(checkForSilence, 100);
  
  onUnmounted(() => {
    clearInterval(clockInterval);
    clearInterval(silenceInterval);
  });
});
</script>

<style scoped lang="scss">
.project-header {
  position: relative; // For absolute positioning of warning
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-sm) var(--spacing-lg);
  background-color: var(--color-surface);
  border-bottom: 1px solid var(--color-border);
  min-height: 60px;
}

.header-left {
  display: flex;
  align-items: center;
  gap: var(--spacing-md);
}

.header-logo {
  width: 36px;
  height: 36px;
  object-fit: contain;
}

.project-name {
  font-size: 18px;
  font-weight: 600;
  color: var(--color-text-primary);
  margin: 0;
}

.header-right {
  display: flex;
  align-items: center;
}

.digital-clock {
  font-size: 24px;
  font-weight: 700;
  color: var(--color-accent);
  letter-spacing: 0.05em;
  padding: var(--spacing-xs) var(--spacing-md);
  border: 2px solid var(--color-accent);
  border-radius: var(--border-radius-md);
  background-color: var(--color-surface);
  box-shadow: 0 0 10px rgba(0, 0, 0, 0.1);
  transition: all var(--transition-base);
}

.silence-warning {
  position: absolute;
  left: 50%;
  transform: translateX(-50%);
  padding: var(--spacing-xs) var(--spacing-lg);
  border-radius: var(--border-radius-md);
  font-weight: 700;
  font-size: 16px;
  color: #000;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
  z-index: 10;
}

.silence-warning.warning-yellow {
  background-color: #fbbf24; /* Yellow */
}

.silence-warning.flash-slow {
  background-color: #fbbf24; /* Yellow with flash */
  animation: flash-slow 2s ease-in-out infinite;
}

.silence-warning.flash-medium {
  background-color: #f56d1f; /* Red */
  animation: flash-medium 1s ease-in-out infinite;
}

.silence-warning.flash-fast {
  background-color: #dc2626; /* Dark red */
  color: #fff;
  animation: flash-fast 0.5s ease-in-out infinite;
}

@keyframes flash-slow {
  0%, 100% { opacity: 0; }
  50% { opacity: 1; }
}

@keyframes flash-medium {
  0%, 100% { opacity: 0; }
  50% { opacity: 1; }
}

@keyframes flash-fast {
  0%, 100% { opacity: 0; }
  50% { opacity: 1; }
}
</style>
