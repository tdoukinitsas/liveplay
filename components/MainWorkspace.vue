<template>
  <div class="main-workspace">
    <ProjectHeader />
    <PlaybackControls />
    
    <div class="workspace-content">
      <div v-if="!cartFullscreen" class="playlist-section" :style="{ width: cartClosed ? '100%' : `calc(100% - ${cartWidth}px)` }">
        <PlaylistView />
      </div>
      
      <div 
        class="resize-handle"
        :class="{ 'collapsed-left': cartFullscreen, 'collapsed-right': cartClosed }"
        @mousedown="startResize"
      ></div>
      
      <div v-if="!cartClosed" class="cart-section" :style="{ width: cartFullscreen ? '100%' : `${cartWidth}px` }">
        <CartPlayer />
      </div>
    </div>
    
    <PropertiesPanel v-if="selectedItem" />
    
    <ProgressModal
      :visible="progressModal.visible"
      :title="progressModal.title"
      :message="progressModal.message"
      :percentage="progressModal.percentage"
    />
  </div>
</template>

<script setup lang="ts">
const { selectedItem, saveProject, closeProject, currentProject, findItemByIndex, findItemByUuid } = useProject();
const { activeCues, triggerByUuid, triggerByIndex, stopCue, panicStop, pauseCue, resumeCue, seekCue } = useAudioEngine();
const { t } = useLocalization();

// Progress modal state
const progressModal = ref({
  visible: false,
  title: '',
  message: '',
  percentage: 0
});

// Resizable cart width
const cartWidth = ref(500);
const isResizing = ref(false);
const cartClosed = ref(false);
const cartFullscreen = ref(false);

const startResize = (e: MouseEvent) => {
  isResizing.value = true;
  e.preventDefault();
  
  const handleMouseMove = (e: MouseEvent) => {
    if (!isResizing.value) return;
    
    const container = document.querySelector('.workspace-content');
    if (!container) return;
    
    const rect = container.getBoundingClientRect();
    const newWidth = rect.right - e.clientX;
    
    // Snap zones
    const snapThreshold = 100; // pixels from edge to trigger snap
    const minWidth = 300;
    const maxWidth = rect.width * 0.95; // Allow up to 95% to trigger fullscreen
    
    // Check for close snap (dragging very close to right edge)
    if (newWidth < snapThreshold) {
      cartClosed.value = true;
      cartFullscreen.value = false;
      return;
    }
    
    // Check for fullscreen snap (dragging very close to left edge)
    if (newWidth > rect.width - snapThreshold) {
      cartFullscreen.value = true;
      cartClosed.value = false;
      return;
    }
    
    // Normal resize
    cartClosed.value = false;
    cartFullscreen.value = false;
    cartWidth.value = Math.max(minWidth, Math.min(maxWidth, newWidth));
  };
  
  const handleMouseUp = () => {
    isResizing.value = false;
    document.removeEventListener('mousemove', handleMouseMove);
    document.removeEventListener('mouseup', handleMouseUp);
  };
  
  document.addEventListener('mousemove', handleMouseMove);
  document.addEventListener('mouseup', handleMouseUp);
};

// Listen for menu events
if (import.meta.client && window.electronAPI) {
  window.electronAPI.onMenuSaveProject(async () => {
    await saveProject();
  });

  window.electronAPI.onMenuExportProject(async () => {
    if (!currentProject.value) return;
    
    try {
      // Set up progress listener
      const progressListener = (_event: any, data: { percentage: number; fileName: string }) => {
        progressModal.value = {
          visible: true,
          title: t('exportProgress.title'),
          message: `${t('exportProgress.message')} ${data.fileName}...`,
          percentage: data.percentage
        };
      };
      
      window.electronAPI.onExportProgress(progressListener);
      
      const result = await window.electronAPI.exportProject(currentProject.value.folderPath, currentProject.value.name);
      
      // Clean up listener
      window.electronAPI.removeExportProgressListener(progressListener);
      
      // Add small delay to ensure final progress (100%) is shown
      await new Promise(resolve => setTimeout(resolve, 500));
      
      // Hide modal
      progressModal.value.visible = false;
      
      if (result.success) {
        console.log('Project exported successfully:', result.path);
      }
    } catch (error) {
      console.error('Export failed:', error);
      progressModal.value.visible = false;
    }
  });

  window.electronAPI.onMenuCloseProject(async () => {
    await closeProject();
  });

  window.electronAPI.onMenuOpenProjectFolder(() => {
    if (currentProject.value) {
      window.electronAPI.openFolder(currentProject.value.folderPath);
    }
  });

  // Listen for API triggers
  window.electronAPI.onTriggerItem((_event, data) => {
    if (data.type === 'uuid') {
      triggerByUuid(data.value);
    } else if (data.type === 'index') {
      triggerByIndex(data.value);
    }
  });

  window.electronAPI.onStopItem((_event, data) => {
    if (data.type === 'uuid') {
      stopCue(data.value);
    } else if (data.type === 'index') {
      // Convert index to UUID first
      const item = findItemByIndex(data.value);
      if (item) {
        stopCue(item.uuid);
      }
    }
  });

  window.electronAPI.onStopAllItems((_event) => {
    panicStop();
  });

  window.electronAPI.onPauseCue((_event, data) => {
    if (data.type === 'uuid') {
      pauseCue(data.value);
    } else if (data.type === 'index') {
      const item = findItemByIndex(data.value);
      if (item) {
        pauseCue(item.uuid);
      }
    }
  });

  window.electronAPI.onResumeCue((_event, data) => {
    if (data.type === 'uuid') {
      resumeCue(data.value);
    } else if (data.type === 'index') {
      const item = findItemByIndex(data.value);
      if (item) {
        resumeCue(item.uuid);
      }
    }
  });

  window.electronAPI.onSeekCue((_event, data) => {
    const { time } = data;
    if (data.type === 'uuid') {
      seekCue(data.value, time);
    } else if (data.type === 'index') {
      const item = findItemByIndex(data.value);
      if (item) {
        seekCue(item.uuid, time);
      }
    }
  });

  window.electronAPI.onGetActiveCues((_event, data) => {
    // Convert Map to array of objects for JSON serialization
    const activeCuesArray = Array.from(activeCues.value.entries()).map(([uuid, cue]) => ({
      uuid: cue.uuid,
      displayName: cue.displayName,
      duration: cue.duration,
      currentTime: cue.currentTime,
      volume: cue.volume,
      isDucked: cue.isDucked,
      isPaused: cue.isPaused,
      color: cue.color,
      inPoint: cue.inPoint,
      outPoint: cue.outPoint,
      currentLevel: cue.currentLevel,
      peakLevel: cue.peakLevel
    }));

    // Send back to main process
    window.electronAPI.sendActiveCuesResponse(activeCuesArray);
  });

  window.electronAPI.onUpdateCue(async (_event, data) => {
    const { updates } = data;
    let item = null;

    if (data.type === 'uuid') {
      item = findItemByUuid(data.value);
    } else if (data.type === 'index') {
      item = findItemByIndex(data.value);
    }

    if (item && item.type === 'audio') {
      // Update properties
      Object.keys(updates).forEach(key => {
        if (key in item) {
          item[key] = updates[key];
        }
      });

      // Save project after updates to sync with main process
      await saveProject();
    }
  });
}

// Save on F1 key (alternative to big play button)
const handleKeydown = (e: KeyboardEvent) => {
  if (e.key === 'F1') {
    e.preventDefault();
    if (selectedItem.value && selectedItem.value.type === 'audio') {
      const { playCue } = useAudioEngine();
      playCue(selectedItem.value as any);
    }
  }
};

onMounted(() => {
  if (import.meta.client) {
    window.addEventListener('keydown', handleKeydown);
  }
});

onUnmounted(() => {
  if (import.meta.client) {
    window.removeEventListener('keydown', handleKeydown);
  }
});
</script>

<style scoped lang="scss">
.main-workspace {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.workspace-content {
  flex: 1;
  display: flex;
  overflow: hidden;
  position: relative;
}

.playlist-section {
  min-width: 30%;
  overflow: hidden;
}

.resize-handle {
  width: 5px;
  background-color: var(--color-border);
  cursor: col-resize;
  transition: background-color var(--transition-fast);
  position: relative;
  z-index: 10;
  
  &:hover {
    background-color: var(--color-accent);
  }
  
  &:active {
    background-color: var(--color-accent);
  }
  
  &.collapsed-left {
    /* When cart is fullscreen, show handle at left edge */
    position: absolute;
    left: 0;
    top: 0;
    bottom: 0;
    width: 8px;
    background-color: transparent;
    
    &::after {
      content: '';
      position: absolute;
      left: 0;
      top: 0;
      bottom: 0;
      width: 2px;
      background-color: var(--color-border);
      opacity: 0.5;
    }
    
    &:hover::after {
      width: 4px;
      background-color: var(--color-accent);
      opacity: 1;
    }
  }
  
  &.collapsed-right {
    /* When cart is closed, show handle at right edge */
    position: absolute;
    right: 0;
    top: 0;
    bottom: 0;
    width: 8px;
    background-color: transparent;
    
    &::after {
      content: '';
      position: absolute;
      right: 0;
      top: 0;
      bottom: 0;
      width: 2px;
      background-color: var(--color-border);
      opacity: 0.5;
    }
    
    &:hover::after {
      width: 4px;
      background-color: var(--color-accent);
      opacity: 1;
    }
  }
}

.cart-section {
  overflow: hidden;
}
</style>
