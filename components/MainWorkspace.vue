<template>
  <div class="main-workspace">
    <PlaybackControls />
    
    <div class="workspace-content">
      <div class="playlist-section" :style="{ width: `calc(100% - ${cartWidth}px)` }">
        <PlaylistView />
      </div>
      
      <div 
        class="resize-handle"
        @mousedown="startResize"
      ></div>
      
      <div class="cart-section" :style="{ width: `${cartWidth}px` }">
        <CartPlayer />
      </div>
    </div>
    
    <PropertiesPanel v-if="selectedItem" />
  </div>
</template>

<script setup lang="ts">
const { selectedItem, saveProject, closeProject, currentProject } = useProject();
const { triggerByUuid, triggerByIndex, stopCue } = useAudioEngine();

// Resizable cart width
const cartWidth = ref(500);
const isResizing = ref(false);

const startResize = (e: MouseEvent) => {
  isResizing.value = true;
  e.preventDefault();
  
  const handleMouseMove = (e: MouseEvent) => {
    if (!isResizing.value) return;
    
    const container = document.querySelector('.workspace-content');
    if (!container) return;
    
    const rect = container.getBoundingClientRect();
    const newWidth = rect.right - e.clientX;
    
    // Min width 300px, max 70% of container
    const minWidth = 300;
    const maxWidth = rect.width * 0.7;
    
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
  window.electronAPI.onMenuSaveProject(() => {
    saveProject();
  });

  window.electronAPI.onMenuCloseProject(() => {
    closeProject();
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
}

.cart-section {
  overflow: hidden;
}
</style>
