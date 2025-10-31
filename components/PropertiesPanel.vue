<template>
  <div class="properties-panel">
    <div class="properties-header">
      <h3>Properties</h3>
      <button class="close-btn" @click="handleClose">×</button>
    </div>
    
    <div class="properties-content">
      <!-- Base properties (all items) -->
      <div class="property-section">
        <h4>Basic Info</h4>
        
        <div class="property-field">
          <label>Display Name</label>
          <input 
            v-model="selectedItem.displayName" 
            type="text" 
            @change="handleSave"
          />
        </div>
        
        <div class="property-field">
          <label>Color</label>
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
          <label>UUID</label>
          <div class="uuid-field">
            <input :value="selectedItem.uuid" readonly />
            <button class="copy-btn" @click="copyToClipboard(selectedItem.uuid)">📋</button>
          </div>
        </div>
        
        <div class="property-field">
          <label>Index</label>
          <input :value="selectedItem.index.join(',')" readonly />
        </div>
        
        <div class="property-field" v-if="selectedItem.type === 'audio'">
          <label>API Trigger URL</label>
          <div class="uuid-field">
            <input :value="`http://localhost:8080/api/trigger/uuid/${selectedItem.uuid}`" readonly />
            <button class="copy-btn" @click="copyToClipboard(`http://localhost:8080/api/trigger/uuid/${selectedItem.uuid}`)">📋</button>
          </div>
        </div>
      </div>
      
      <!-- Audio-specific properties -->
      <template v-if="selectedItem.type === 'audio'">
        <div class="property-section">
          <h4>Media</h4>
          
          <div class="property-field">
            <label>File</label>
            <div class="file-field">
              <input :value="audioItem.mediaFileName" readonly />
              <button class="action-btn-small" @click="handleReplaceMedia">Replace</button>
            </div>
          </div>
          
          <div class="property-field">
            <label>Duration</label>
            <input :value="formatTime(audioItem.duration)" readonly />
          </div>
        </div>
        
        <div class="property-section">
          <h4>Playback</h4>
          
          <div class="property-field">
            <label>Volume (0-2)</label>
            <input 
              v-model.number="audioItem.volume" 
              type="number" 
              min="0" 
              max="2" 
              step="0.1"
              @change="handleSave"
            />
          </div>
          
          <div class="property-field">
            <label>In Point (seconds)</label>
            <input 
              v-model.number="audioItem.inPoint" 
              type="number" 
              min="0" 
              :max="audioItem.duration"
              step="0.1"
              @change="handleSave"
            />
          </div>
          
          <div class="property-field">
            <label>Out Point (seconds)</label>
            <input 
              v-model.number="audioItem.outPoint" 
              type="number" 
              :min="audioItem.inPoint" 
              :max="audioItem.duration"
              step="0.1"
              @change="handleSave"
            />
          </div>
        </div>
        
        <div class="property-section">
          <h4>Ducking Behavior</h4>
          
          <div class="property-field">
            <label>Mode</label>
            <select v-model="audioItem.duckingBehavior.mode" @change="handleSave">
              <option value="stop-all">Stop All Other Cues</option>
              <option value="no-ducking">No Ducking</option>
              <option value="duck-others">Duck Other Cues</option>
            </select>
          </div>
          
          <div class="property-field" v-if="audioItem.duckingBehavior.mode === 'duck-others'">
            <label>Duck Level (0-1)</label>
            <input 
              v-model.number="audioItem.duckingBehavior.duckLevel" 
              type="number" 
              min="0" 
              max="1" 
              step="0.1"
              @change="handleSave"
            />
          </div>
        </div>
        
        <div class="property-section">
          <h4>End Behavior</h4>
          
          <div class="property-field">
            <label>Action</label>
            <select v-model="audioItem.endBehavior.action" @change="handleSave">
              <option value="nothing">Do Nothing</option>
              <option value="next">Go to Next Item</option>
              <option value="goto-item">Go to Specific Item (UUID)</option>
              <option value="goto-index">Go to Specific Index</option>
              <option value="loop">Loop</option>
            </select>
          </div>
          
          <div class="property-field" v-if="audioItem.endBehavior.action === 'goto-item'">
            <label>Target UUID</label>
            <input 
              v-model="audioItem.endBehavior.targetUuid" 
              type="text"
              @change="handleSave"
            />
          </div>
          
          <div class="property-field" v-if="audioItem.endBehavior.action === 'goto-index'">
            <label>Target Index (comma-separated)</label>
            <input 
              :value="audioItem.endBehavior.targetIndex?.join(',') || ''"
              @change="(e) => { audioItem.endBehavior.targetIndex = (e.target as HTMLInputElement).value.split(',').map(i => parseInt(i)); handleSave(); }"
              type="text"
            />
          </div>
        </div>
        
        <div class="property-section">
          <h4>Start Behavior</h4>
          
          <div class="property-field">
            <label>Action</label>
            <select v-model="audioItem.startBehavior.action" @change="handleSave">
              <option value="nothing">Do Nothing</option>
              <option value="play-next">Play Next Item</option>
              <option value="play-item">Play Specific Item (UUID)</option>
              <option value="play-index">Play Specific Index</option>
            </select>
          </div>
          
          <div class="property-field" v-if="audioItem.startBehavior.action === 'play-item'">
            <label>Target UUID</label>
            <input 
              v-model="audioItem.startBehavior.targetUuid" 
              type="text"
              @change="handleSave"
            />
          </div>
          
          <div class="property-field" v-if="audioItem.startBehavior.action === 'play-index'">
            <label>Target Index (comma-separated)</label>
            <input 
              :value="audioItem.startBehavior.targetIndex?.join(',') || ''"
              @change="(e) => { audioItem.startBehavior.targetIndex = (e.target as HTMLInputElement).value.split(',').map(i => parseInt(i)); handleSave(); }"
              type="text"
            />
          </div>
        </div>
      </template>
      
      <!-- Group-specific properties -->
      <template v-if="selectedItem.type === 'group'">
        <div class="property-section">
          <h4>Group Behavior</h4>
          
          <div class="property-field">
            <label>Start Action</label>
            <select v-model="groupItem.startBehavior.action" @change="handleSave">
              <option value="play-first">Play First Item</option>
              <option value="play-all">Play All Items</option>
            </select>
          </div>
        </div>
        
        <div class="property-section">
          <h4>End Behavior</h4>
          
          <div class="property-field">
            <label>Action</label>
            <select v-model="groupItem.endBehavior.action" @change="handleSave">
              <option value="nothing">Do Nothing</option>
              <option value="next">Go to Next Item</option>
              <option value="goto-item">Go to Specific Item (UUID)</option>
              <option value="goto-index">Go to Specific Index</option>
            </select>
          </div>
        </div>
      </template>
    </div>
  </div>
</template>

<script setup lang="ts">
import type { AudioItem, GroupItem } from '~/types/project';
import { PRESET_COLORS } from '~/types/project';

const { selectedItem, saveProject } = useProject();

const audioItem = computed(() => selectedItem.value as AudioItem);
const groupItem = computed(() => selectedItem.value as GroupItem);

const handleClose = () => {
  selectedItem.value = null;
};

const handleSave = async () => {
  await saveProject();
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
}

.properties-header h3 {
  font-size: 16px;
  font-weight: 600;
}

.close-btn {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  font-size: 24px;
  line-height: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  
  &:hover {
    background-color: var(--color-surface-hover);
  }
}

.properties-content {
  flex: 1;
  overflow-x: auto;
  overflow-y: hidden;
  display: flex;
  gap: var(--spacing-lg);
  padding: var(--spacing-lg);
}

.property-section {
  min-width: 280px;
  flex-shrink: 0;
}

.property-section h4 {
  font-size: 14px;
  font-weight: 600;
  margin-bottom: var(--spacing-md);
  color: var(--color-text-secondary);
  text-transform: uppercase;
  letter-spacing: 0.5px;
}

.property-field {
  margin-bottom: var(--spacing-md);
}

.property-field label {
  display: block;
  font-size: 13px;
  font-weight: 500;
  margin-bottom: var(--spacing-xs);
  color: var(--color-text-secondary);
}

.property-field input,
.property-field select {
  width: 100%;
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
</style>
