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
  </div>
</template>

<script setup lang="ts">
const { selectedItem, saveProject, closeProject, currentProject } = useProject();
const { triggerByUuid, triggerByIndex, stopCue } = useAudioEngine();

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
