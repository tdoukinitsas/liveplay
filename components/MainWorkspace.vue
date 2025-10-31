<template>
  <div class="main-workspace">
    <PlaybackControls />
    
    <div class="workspace-content">
      <div class="playlist-section">
        <PlaylistView />
      </div>
      
      <div class="cart-section">
        <CartPlayer />
      </div>
    </div>
    
    <PropertiesPanel v-if="selectedItem" />
  </div>
</template>

<script setup lang="ts">
const { selectedItem, saveProject, closeProject, currentProject } = useProject();
const { triggerByUuid, triggerByIndex, stopCue } = useAudioEngine();

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

<style scoped>
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
}

.playlist-section {
  flex: 1;
  min-width: 0;
  overflow: hidden;
}

.cart-section {
  width: var(--cart-player-width);
  border-left: 1px solid var(--color-border);
  overflow: hidden;
}
</style>
