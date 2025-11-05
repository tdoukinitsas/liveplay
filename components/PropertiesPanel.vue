<template>
  <div class="properties-panel">
    <div class="properties-header">
      <h3>{{ t('properties.title') }}: {{ selectedItem?.displayName || '' }}</h3>
      <button class="close-btn" @click="handleClose">
        <span class="material-symbols-rounded">close</span>
      </button>
    </div>
    
    <!-- Tab Navigation -->
    <div class="properties-tabs">
      <button 
        v-for="tab in availableTabs" 
        :key="tab.id"
        :class="['tab-btn', { active: activeTab === tab.id }]"
        @click="activeTab = tab.id"
      >
        <span class="material-symbols-rounded">{{ tab.icon }}</span>
        <span>{{ tab.label }}</span>
      </button>
    </div>
    
    <div class="properties-content">
      <!-- Basic Info Tab -->
      <div v-if="activeTab === 'basic'" class="tab-panel">
        <div class="property-field">
          <label>{{ t('properties.displayName') }}</label>
          <input 
            v-model="selectedItem.displayName" 
            type="text" 
            @change="handleSave"
          />
        </div>
        
        <div class="property-field">
          <label>{{ t('properties.color') }}</label>
          <div class="color-picker">
            <button
              v-for="color in PRESET_COLORS"
              :key="color"
              class="color-btn"
              :style="{ backgroundColor: color }"
              :class="{ active: selectedItem.color === color }"
              @click="() => { selectedItem.color = color; handleSave(); }"
            ></button>
          </div>
        </div>
        
        <div class="property-field">
          <label>{{ t('properties.uuid') }}</label>
          <div class="input-with-btn">
            <input :value="selectedItem.uuid" readonly />
            <button class="icon-btn" @click="copyToClipboard(selectedItem.uuid)">
              <span class="material-symbols-rounded">content_copy</span>
            </button>
          </div>
        </div>
        
        <div class="property-field">
          <label>{{ t('properties.index') }}</label>
          <input :value="selectedItem.index.join(',')" readonly />
        </div>
        
        <div class="property-field" v-if="selectedItem.type === 'audio'">
          <label>{{ t('properties.apiTriggerUrl') }}</label>
          <div class="input-with-btn">
            <input :value="`http://localhost:8080/api/trigger/uuid/${selectedItem.uuid}`" readonly />
            <button class="icon-btn" @click="copyToClipboard(`http://localhost:8080/api/trigger/uuid/${selectedItem.uuid}`)">
              <span class="material-symbols-rounded">content_copy</span>
            </button>
          </div>
        </div>
      </div>
      
      <!-- Media Tab -->
      <div v-if="activeTab === 'media' && selectedItem.type === 'audio'" class="tab-panel">
        <div class="property-field">
          <label>{{ t('properties.file') }}</label>
          <div class="input-with-btn">
            <input :value="audioItem.mediaFileName" readonly />
            <button class="icon-btn" @click="handleReplaceMedia">
              <span class="material-symbols-rounded">swap_horiz</span>
            </button>
          </div>
        </div>
        
        <div class="property-field">
          <label>{{ t('properties.duration') }}</label>
          <input :value="formatTime(audioItem.duration)" readonly />
        </div>
      </div>
      
      <!-- Playback Tab -->
      <div v-if="activeTab === 'playback' && selectedItem.type === 'audio'" class="tab-panel">
        <WaveformTrimmer
          v-if="audioItem && audioItem.mediaPath && audioItem.duration > 0"
          :audio-item="audioItem"
          @update:volume="(v) => { audioItem.volume = v; }"
          @update:in-point="(v) => { audioItem.inPoint = v; }"
          @update:out-point="(v) => { audioItem.outPoint = v; }"
          @update:play-fade="handlePlayFadeUpdate"
          @update:stop-fade="handleStopFadeUpdate"
          @update:cross-fade="handleCrossFadeUpdate"
          @change="handleSave"
          @normalize="handleNormalize"
          @trim-silence="handleTrimSilence"
        />
        <div v-else class="loading-message">
          <span class="material-symbols-rounded">pending</span>
          <p>{{ t('properties.loadingAudioData')}}</p>
        </div>
      </div>
      
      <!-- Ducking Tab -->
      <div v-if="activeTab === 'ducking' && selectedItem.type === 'audio'" class="tab-panel">
        <div class="property-field">
          <label>{{ t('properties.mode') }}</label>
          <select v-model="audioItem.duckingBehavior.mode" @change="handleSave">
            <option value="stop-all">{{ t('duckingBehavior.stopAll') }}</option>
            <option value="no-ducking">{{ t('duckingBehavior.noDucking') }}</option>
            <option value="duck-others">{{ t('duckingBehavior.duckOthers') }}</option>
          </select>
        </div>
        
        <div class="property-field" v-if="audioItem.duckingBehavior.mode === 'duck-others'">
          <label>{{ t('properties.duckLevel') }} ({{ duckLevelDB.toFixed(1) }} dB)</label>
          <input 
            v-model.number="duckLevelDB" 
            type="range" 
            min="-60" 
            max="0" 
            step="0.5"
            @change="handleSave"
          />
          <div class="db-range-labels">
            <span>-60 dB</span>
            <span>0 dB</span>
          </div>
        </div>
      </div>
      
      <!-- End Behavior Tab -->
      <div v-if="activeTab === 'endBehavior'" class="tab-panel">
        <div class="property-field">
          <label>{{ t('properties.action') }}</label>
          <select v-model="endBehaviorAction" @change="handleSave">
            <option value="nothing">{{ t('endBehavior.nothing') }}</option>
            <option value="next">{{ t('endBehavior.next') }}</option>
            <option value="goto-item">{{ t('endBehavior.gotoItem') }}</option>
            <option value="goto-index">{{ t('endBehavior.gotoIndex') }}</option>
            <option v-if="selectedItem.type === 'audio'" value="loop">{{ t('endBehavior.loop') }}</option>
          </select>
        </div>
        
        <div class="property-field" v-if="endBehaviorAction === 'goto-item'">
          <label>{{ t('properties.targetUuid') }}</label>
          <input 
            v-model="endBehaviorTargetUuid" 
            type="text"
            @change="handleSave"
          />
        </div>
        
        <div class="property-field" v-if="endBehaviorAction === 'goto-index'">
          <label>{{ t('properties.targetIndex') }}</label>
          <input 
            :value="endBehaviorTargetIndex?.join(',') || ''"
            @change="handleEndBehaviorIndexChange"
            type="text"
          />
        </div>
      </div>
      
      <!-- Start Behavior Tab -->
      <div v-if="activeTab === 'startBehavior'" class="tab-panel">
        <div class="property-field">
          <label>{{ t('properties.action') }}</label>
          <select v-model="startBehaviorAction" @change="handleSave">
            <option v-if="selectedItem.type === 'audio'" value="nothing">{{ t('startBehavior.nothing') }}</option>
            <option v-if="selectedItem.type === 'audio'" value="play-next">{{ t('startBehavior.playNext') }}</option>
            <option v-if="selectedItem.type === 'audio'" value="play-item">{{ t('startBehavior.playItem') }}</option>
            <option v-if="selectedItem.type === 'audio'" value="play-index">{{ t('startBehavior.playIndex') }}</option>
            <option v-if="selectedItem.type === 'group'" value="play-first">{{ t('startBehavior.playFirst') }}</option>
            <option v-if="selectedItem.type === 'group'" value="play-all">{{ t('startBehavior.playAll') }}</option>
          </select>
        </div>
        
        <div class="property-field" v-if="startBehaviorAction === 'play-item'">
          <label>{{ t('properties.targetUuid') }}</label>
          <input 
            v-model="startBehaviorTargetUuid" 
            type="text"
            @change="handleSave"
          />
        </div>
        
        <div class="property-field" v-if="startBehaviorAction === 'play-index'">
          <label>{{ t('properties.targetIndex') }}</label>
          <input 
            :value="startBehaviorTargetIndex?.join(',') || ''"
            @change="handleStartBehaviorIndexChange"
            type="text"
          />
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import type { AudioItem, GroupItem } from '~/types/project';
import { PRESET_COLORS } from '~/types/project';
import { calculatePerceivedLoudness } from '~/utils/audio';

const { selectedItem, selectedItems, getSelectedItems, saveProject } = useProject();
const { t } = useLocalization();

const audioItem = computed(() => selectedItem.value as AudioItem);
const groupItem = computed(() => selectedItem.value as GroupItem);

// Check if selected item is a cart item
const isCartItem = computed(() => {
  if (selectedItem.value && selectedItem.value.type === 'audio') {
    const item = selectedItem.value as AudioItem;
    return item.index && item.index.length > 0 && item.index[0] === -1;
  }
  return false;
});

// Tab management
const activeTab = ref('basic');

interface Tab {
  id: string;
  label: string;
  icon: string;
  audioOnly?: boolean;
}

const allTabs = computed<Tab[]>(() => [
  { id: 'basic', label: t('properties.basicInfo'), icon: 'info' },
  { id: 'media', label: t('properties.media'), icon: 'audio_file', audioOnly: true },
  { id: 'playback', label: t('properties.playback'), icon: 'play_circle', audioOnly: true },
  { id: 'ducking', label: t('properties.ducking'), icon: 'volume_down', audioOnly: true },
  { id: 'endBehavior', label: t('properties.endBehavior'), icon: 'stop_circle' },
  { id: 'startBehavior', label: t('properties.startBehavior'), icon: 'play_arrow' }
]);

const availableTabs = computed(() => {
  return allTabs.value.filter(tab => !tab.audioOnly || selectedItem.value?.type === 'audio');
});

// Computed properties for behavior fields
const endBehaviorAction = computed({
  get: () => {
    if (selectedItem.value?.type === 'audio') {
      return audioItem.value.endBehavior.action;
    } else if (selectedItem.value?.type === 'group') {
      return groupItem.value.endBehavior.action;
    }
    return 'nothing';
  },
  set: (value) => {
    if (selectedItem.value?.type === 'audio') {
      audioItem.value.endBehavior.action = value as any;
    } else if (selectedItem.value?.type === 'group') {
      groupItem.value.endBehavior.action = value as any;
    }
  }
});

const endBehaviorTargetUuid = computed({
  get: () => {
    if (selectedItem.value?.type === 'audio') {
      return audioItem.value.endBehavior.targetUuid || '';
    } else if (selectedItem.value?.type === 'group') {
      return groupItem.value.endBehavior.targetUuid || '';
    }
    return '';
  },
  set: (value) => {
    if (selectedItem.value?.type === 'audio') {
      audioItem.value.endBehavior.targetUuid = value;
    } else if (selectedItem.value?.type === 'group') {
      groupItem.value.endBehavior.targetUuid = value;
    }
  }
});

const endBehaviorTargetIndex = computed(() => {
  if (selectedItem.value?.type === 'audio') {
    return audioItem.value.endBehavior.targetIndex;
  } else if (selectedItem.value?.type === 'group') {
    return groupItem.value.endBehavior.targetIndex;
  }
  return undefined;
});

const handleEndBehaviorIndexChange = (e: Event) => {
  const value = (e.target as HTMLInputElement).value;
  const parsed = value.split(',').map(i => parseInt(i));
  if (selectedItem.value?.type === 'audio') {
    audioItem.value.endBehavior.targetIndex = parsed;
  } else if (selectedItem.value?.type === 'group') {
    groupItem.value.endBehavior.targetIndex = parsed;
  }
  handleSave();
};

const startBehaviorAction = computed({
  get: () => {
    if (selectedItem.value?.type === 'audio') {
      return audioItem.value.startBehavior.action;
    } else if (selectedItem.value?.type === 'group') {
      return groupItem.value.startBehavior.action;
    }
    return 'nothing';
  },
  set: (value) => {
    if (selectedItem.value?.type === 'audio') {
      audioItem.value.startBehavior.action = value as any;
    } else if (selectedItem.value?.type === 'group') {
      groupItem.value.startBehavior.action = value as any;
    }
  }
});

const startBehaviorTargetUuid = computed({
  get: () => {
    if (selectedItem.value?.type === 'audio') {
      return audioItem.value.startBehavior.targetUuid || '';
    }
    return '';
  },
  set: (value) => {
    if (selectedItem.value?.type === 'audio') {
      audioItem.value.startBehavior.targetUuid = value;
    }
  }
});

const startBehaviorTargetIndex = computed(() => {
  if (selectedItem.value?.type === 'audio') {
    return audioItem.value.startBehavior.targetIndex;
  }
  return undefined;
});

const handleStartBehaviorIndexChange = (e: Event) => {
  const value = (e.target as HTMLInputElement).value;
  const parsed = value.split(',').map(i => parseInt(i));
  if (selectedItem.value?.type === 'audio') {
    audioItem.value.startBehavior.targetIndex = parsed;
  }
  handleSave();
};

// Duck level in dB
const duckLevelDB = computed({
  get: () => {
    const linear = audioItem.value.duckingBehavior.duckLevel;
    if (linear <= 0) return -60;
    return 20 * Math.log10(linear);
  },
  set: (db: number) => {
    const linear = db <= -60 ? 0 : Math.pow(10, db / 20);
    audioItem.value.duckingBehavior.duckLevel = linear;
  }
});

// Store a snapshot of the original values when properties panel opens
const originalSnapshot = ref<any>(null);
const isInitializing = ref(false);

// When selectedItem changes, take a snapshot
watch(selectedItem, (newItem, oldItem) => {
  if (newItem) {
    // Only reset tab if it's a different item (not just property updates)
    const isDifferentItem = !oldItem || newItem.uuid !== oldItem.uuid;
    
    if (isDifferentItem) {
      isInitializing.value = true;
      originalSnapshot.value = JSON.parse(JSON.stringify(newItem));
      
      // Only reset to basic tab if properties panel was previously closed (no oldItem)
      // If panel was already open, keep the current tab
      if (!oldItem) {
        activeTab.value = 'basic';
      }
      
      setTimeout(() => {
        isInitializing.value = false;
      }, 0);
    }
  } else {
    originalSnapshot.value = null;
  }
}, { immediate: true });

const handleClose = () => {
  selectedItem.value = null;
  selectedItems.value.clear();
  originalSnapshot.value = null;
};

const handleSave = async () => {
  // If multiple items are selected, update all of them with ONLY changed properties
  const items = getSelectedItems();
  if (items.length > 1 && originalSnapshot.value && selectedItem.value) {
    const current = selectedItem.value;
    const original = originalSnapshot.value;
    
    items.forEach(item => {
      // Only update properties that have changed
      if (current.displayName !== original.displayName) {
        item.displayName = current.displayName;
      }
      if (current.color !== original.color) {
        item.color = current.color;
      }
      
      // Copy type-specific properties only if they changed
      if (item.type === 'audio' && current.type === 'audio') {
        const sourceAudio = current as AudioItem;
        const originalAudio = original as AudioItem;
        const targetAudio = item as AudioItem;
        
        if (sourceAudio.volume !== originalAudio.volume) {
          targetAudio.volume = sourceAudio.volume;
        }
        if (sourceAudio.inPoint !== originalAudio.inPoint) {
          targetAudio.inPoint = sourceAudio.inPoint;
        }
        if (sourceAudio.outPoint !== originalAudio.outPoint) {
          targetAudio.outPoint = sourceAudio.outPoint;
        }
        if (JSON.stringify(sourceAudio.duckingBehavior) !== JSON.stringify(originalAudio.duckingBehavior)) {
          targetAudio.duckingBehavior = { ...sourceAudio.duckingBehavior };
        }
        if (JSON.stringify(sourceAudio.endBehavior) !== JSON.stringify(originalAudio.endBehavior)) {
          targetAudio.endBehavior = { ...sourceAudio.endBehavior };
        }
        if (JSON.stringify(sourceAudio.startBehavior) !== JSON.stringify(originalAudio.startBehavior)) {
          targetAudio.startBehavior = { ...sourceAudio.startBehavior };
        }
      } else if (item.type === 'group' && current.type === 'group') {
        const sourceGroup = current as GroupItem;
        const originalGroup = original as GroupItem;
        const targetGroup = item as GroupItem;
        
        if (JSON.stringify(sourceGroup.startBehavior) !== JSON.stringify(originalGroup.startBehavior)) {
          targetGroup.startBehavior = { ...sourceGroup.startBehavior };
        }
        if (JSON.stringify(sourceGroup.endBehavior) !== JSON.stringify(originalGroup.endBehavior)) {
          targetGroup.endBehavior = { ...sourceGroup.endBehavior };
        }
      }
    });
    
    // Update the snapshot to the current state after saving
    originalSnapshot.value = JSON.parse(JSON.stringify(current));
  }
  
  await saveProject();
};

// Handle normalize: normalize ALL selected audio items individually
const handleNormalize = () => {
  let items = getSelectedItems();
  
  // Fallback to selectedItem if no items in selectedItems set (shouldn't happen now, but safe)
  if (items.length === 0 && selectedItem.value) {
    items = [selectedItem.value];
  }
  
  const targetLoudness = -10; // Our "0dB" with headroom
  
  let normalizedCount = 0;
  
  items.forEach(item => {
    if (item.type !== 'audio') return;
    
    const audioItem = item as AudioItem;
    
    // Skip if no waveform data
    if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) {
      console.warn(`Skipping ${audioItem.displayName}: no waveform data`);
      return;
    }
    
    const peaks = audioItem.waveform.peaks;
    const duration = audioItem.duration;
    
    // Get trimmed region
    const inPoint = audioItem.inPoint || 0;
    const outPoint = audioItem.outPoint || duration;
    const startIndex = Math.floor((inPoint / duration) * peaks.length);
    const endIndex = Math.ceil((outPoint / duration) * peaks.length);
    const trimmedPeaks = peaks.slice(startIndex, endIndex);
    
    // Calculate INTRINSIC perceived loudness
    const intrinsicLoudness = calculatePerceivedLoudness(trimmedPeaks);
    
    // Calculate the ABSOLUTE volume needed
    const gainDb = targetLoudness - intrinsicLoudness;
    const newVolume = Math.pow(10, gainDb / 20);
    
    // Clamp to reasonable range (0.001 to 3.162, where 3.162 = +10dB max)
    const maxVolume = Math.pow(10, 10 / 20); // +10dB = 3.162
    const clampedVolume = Math.min(Math.max(newVolume, 0.001), maxVolume);
    audioItem.volume = clampedVolume;
    
    normalizedCount++;
    console.log(`Normalized ${audioItem.displayName}: ${intrinsicLoudness.toFixed(1)}dB -> ${targetLoudness}dB (volume: ${clampedVolume.toFixed(3)})`);
  });
  
  if (normalizedCount > 0) {
    saveProject();
    console.log(`Normalized ${normalizedCount} item(s)`);
  }
};

// Handle trim silence: trim ALL selected audio items individually
const handleTrimSilence = () => {
  let items = getSelectedItems();
  
  // Fallback to selectedItem if no items in selectedItems set (shouldn't happen now, but safe)
  if (items.length === 0 && selectedItem.value) {
    items = [selectedItem.value];
  }
  
  const padding = 0.1; // Padding in seconds
  
  let trimmedCount = 0;
  
  items.forEach(item => {
    if (item.type !== 'audio') return;
    
    const audioItem = item as AudioItem;
    
    // Skip if no waveform data
    if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) {
      console.warn(`Skipping ${audioItem.displayName}: no waveform data`);
      return;
    }
    
    const peaks = audioItem.waveform.peaks;
    const duration = audioItem.duration;
    
    // Find the maximum peak value to calculate relative threshold
    const maxPeak = Math.max(...peaks);
    
    // Use 5% of max peak as threshold (more sensitive to actual silence)
    const threshold = maxPeak * 0.05;
    
    // Find first non-silent sample from start
    let startIndex = 0;
    for (let i = 0; i < peaks.length; i++) {
      if (peaks[i] > threshold) {
        startIndex = i;
        break;
      }
    }
    
    // Find first non-silent sample from end
    let endIndex = peaks.length - 1;
    for (let i = peaks.length - 1; i >= 0; i--) {
      if (peaks[i] > threshold) {
        endIndex = i;
        break;
      }
    }
    
    // Convert indices to time
    const newInPoint = (startIndex / peaks.length) * duration;
    const newOutPoint = ((endIndex + 1) / peaks.length) * duration;
    
    // Apply with padding
    audioItem.inPoint = Math.max(0, newInPoint - padding);
    audioItem.outPoint = Math.min(duration, newOutPoint + padding);
    
    trimmedCount++;
    console.log(`Trimmed ${audioItem.displayName}: maxPeak=${maxPeak.toFixed(3)}, threshold=${threshold.toFixed(3)}, ${newInPoint.toFixed(2)}s - ${newOutPoint.toFixed(2)}s`);
  });
  
  if (trimmedCount > 0) {
    saveProject();
    console.log(`Trimmed ${trimmedCount} item(s)`);
  }
};

// Handle fade updates: apply to ALL selected audio items
const handlePlayFadeUpdate = (value: number) => {
  const items = getSelectedItems();
  items.forEach(item => {
    if (item.type === 'audio') {
      (item as AudioItem).playFade = value;
    }
  });
};

const handleStopFadeUpdate = (value: number) => {
  const items = getSelectedItems();
  items.forEach(item => {
    if (item.type === 'audio') {
      (item as AudioItem).stopFade = value;
    }
  });
};

const handleCrossFadeUpdate = (value: number) => {
  const items = getSelectedItems();
  items.forEach(item => {
    if (item.type === 'audio') {
      (item as AudioItem).crossFade = value;
    }
  });
};

const handleReplaceMedia = async () => {
  if (!import.meta.client || !window.electronAPI) return;
  
  const files = await window.electronAPI.selectAudioFiles();
  if (!files || files.length === 0) return;
  
  // Implementation would replace the media file
  console.log('Replace media with:', files[0]);
};

const copyToClipboard = async (text: string) => {
  if (import.meta.client) {
    try {
      await navigator.clipboard.writeText(text);
      // Could show a toast notification here
    } catch (error) {
      console.error('Failed to copy:', error);
    }
  }
};

const formatTime = (seconds: number): string => {
  const mins = Math.floor(seconds / 60);
  const secs = Math.floor(seconds % 60);
  return `${mins}:${secs.toString().padStart(2, '0')}`;
};
</script>

<style scoped>
.properties-panel {
  height: var(--properties-panel-height);
  border-top: 1px solid var(--color-border);
  background-color: var(--color-surface);
  display: flex;
  flex-direction: column;
}

.properties-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-md) var(--spacing-lg);
  border-bottom: 1px solid var(--color-border);
  color: var(--color-text-secondary);
}

.properties-header h3 {
  font-size: 16px;
  font-weight: 600;
}

.close-btn {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: none;
  border: none;
  cursor: pointer;
  color: var(--color-text);
  
  &:hover {
    background-color: var(--color-surface-hover);
  }
  
  .material-symbols-rounded {
    font-size: 20px;
    color: var(--color-text);
  }
}

/* Tab Navigation */
.properties-tabs {
  display: flex;
  gap: 2px;
  padding: 0 var(--spacing-lg);
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
  overflow-x: auto;
}

.tab-btn {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  padding: var(--spacing-sm) var(--spacing-md);
  background: none;
  border: none;
  border-bottom: 2px solid transparent;
  cursor: pointer;
  color: var(--color-text-secondary);
  font-size: 13px;
  font-weight: 500;
  white-space: nowrap;
  transition: all 0.2s;
  
  .material-symbols-rounded {
    font-size: 18px;
    color: inherit;
  }
  
  &:hover {
    color: var(--color-text-primary);
    background-color: var(--color-surface-hover);
  }
  
  &.active {
    color: var(--color-accent);
    border-bottom-color: var(--color-accent);
  }
}

/* Tab Content */
.properties-content {
  flex: 1;
  overflow-x: auto;
  overflow-y: auto;
  padding: var(--spacing-lg);
  min-height: 0;
}

.tab-panel {
  display: flex;
  flex-wrap: wrap;
  gap: var(--spacing-md);
  align-content: flex-start;
  min-height: min-content;
}

/* Special handling for playback tab with waveform trimmer */
.tab-panel:has(.waveform-trimmer) {
  display: block;
}

.property-field {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  min-width: 250px;
  flex: 0 0 auto;
  color: var(--color-text-secondary);
}

.property-field label {
  font-size: 13px;
  font-weight: 500;
}

.property-field input,
.property-field select {
  width: 100%;
  padding: var(--spacing-sm);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  color: var(--color-text);
  font-size: 13px;
  
  &:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  
  &[readonly] {
    opacity: 0.6;
    cursor: default;
  }
}

.input-with-btn {
  display: flex;
  gap: var(--spacing-xs);
  
  input {
    flex: 1;
  }
}

.icon-btn {
  padding: var(--spacing-sm);
  background: var(--color-surface-hover);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  cursor: pointer;
  color: var(--color-text);
  display: flex;
  align-items: center;
  justify-content: center;
  
  &:hover {
    background-color: var(--color-accent);
    border-color: var(--color-accent);
    color: white;
  }
  
  .material-symbols-rounded {
    font-size: 18px;
    color: inherit;
  }
}

.color-picker {
  display: grid;
  grid-template-columns: repeat(8, 1fr);
  gap: var(--spacing-xs);
}

.color-btn {
  aspect-ratio: 1;
  border-radius: var(--border-radius-sm);
  border: 2px solid transparent;
  transition: all var(--transition-fast);
  
  &:hover {
    transform: scale(1.1);
  }
  
  &.active {
    border-color: var(--color-text-primary);
    box-shadow: 0 0 0 2px var(--color-background);
  }
}

.uuid-field,
.file-field {
  display: flex;
  gap: var(--spacing-xs);
}

.uuid-field input,
.file-field input {
  flex: 1;
  min-width: 0;
}

.copy-btn,
.action-btn-small {
  padding: var(--spacing-sm);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  white-space: nowrap;
  
  &:hover {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
  }
}

.db-range-labels {
  display: flex;
  justify-content: space-between;
  font-size: 11px;
  color: var(--color-text-secondary);
  margin-top: 4px;
}

.loading-message {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-sm);
  padding: var(--spacing-xl);
  color: var(--color-text-secondary);
}

.loading-message .material-symbols-rounded {
  font-size: 48px;
  animation: spin 2s linear infinite;
}

.loading-message p {
  font-size: 14px;
}

@keyframes spin {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}
</style>


