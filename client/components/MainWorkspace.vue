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

    <!-- Export: server vs client choice (only shown for remote servers). -->
    <LocationChoiceModal
      :visible="exportChoiceVisible"
      :title="t('exportProject.chooseLocationTitle')"
      :message="t('exportProject.chooseLocationMessage')"
      :server-label="t('exportProject.saveOnServer')"
      :client-label="t('exportProject.downloadHere')"
      :cancel-label="t('common.cancel')"
      @pick="onExportChoice"
      @cancel="exportChoiceVisible = false"
    />

    <!-- Server file picker (directory mode) for "save on server" path. -->
    <ServerFilePickerModal
      :open="exportServerPickerOpen"
      mode="directory"
      filter="all"
      :filter-options="['all']"
      :start-path="currentProject?.folderPath ?? ''"
      @pick="onExportServerPath"
      @close="exportServerPickerOpen = false"
    />
  </div>
</template>

<script setup lang="ts">
import LocationChoiceModal from './LocationChoiceModal.vue';
import ServerFilePickerModal from './ServerFilePickerModal.vue';

const { selectedItem, saveProject, closeProject, currentProject, findItemByUuid } = useProject();
const { triggerByUuid, triggerByIndex, stopCue, stopAllCues, playCue } = useAudioEngine();
const { getCartItem, cartOnlyItems, updateCartOnlyItem } = useCartItems();
const { t } = useLocalization();
const server = useLiveplayServer();

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
  window.electronAPI.onMenuSaveProject(() => {
    saveProject();
  });

  window.electronAPI.onMenuExportProject(() => {
    startExportFlow();
  });

  window.electronAPI.onMenuCloseProject(() => {
    void closeProject();
  });

  // File > New and File > Open while a project is already open: close the
  // current project (locally + on the server) and stash the intent so the
  // welcome screen pops the corresponding picker as soon as it mounts.
  // Without this, these menu items were silent when something was open —
  // only WelcomeScreen used to subscribe, and it isn't mounted right now.
  window.electronAPI.onMenuNewProject(async () => {
    try { sessionStorage.setItem('liveplay:welcomeIntent', 'new'); } catch {}
    await closeProject();
  });

  window.electronAPI.onMenuOpenProject(async () => {
    try { sessionStorage.setItem('liveplay:welcomeIntent', 'open'); } catch {}
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
    }
  });

  // Trigger a cart slot by slot number (from HTTP API)
  window.electronAPI.onTriggerCartSlot((_event, data) => {
    const item = getCartItem(data.slot);
    if (item) playCue(item);
  });

  // Stop all cues (from HTTP API)
  window.electronAPI.onStopAllCues(() => {
    stopAllCues();
  });

  // Update a cue's properties (from HTTP API PATCH /api/cues/:id)
  const READONLY_ITEM_KEYS = new Set(['uuid', 'type', 'index', 'mediaFileName', 'mediaPath', 'waveformPath', 'waveform', 'duration']);
  window.electronAPI.onApiUpdateItem((_event, { requestId, id, updates }) => {
    const item = findItemByUuid(id);
    if (!item || item.type !== 'audio') {
      window.electronAPI.sendApiResponse({ requestId, success: false, message: 'Cue not found' });
      return;
    }
    for (const [key, value] of Object.entries(updates)) {
      if (!READONLY_ITEM_KEYS.has(key)) (item as any)[key] = value;
    }
    saveProject();
    window.electronAPI.sendApiResponse({ requestId, success: true, cue: item });
  });

  // Update a cart slot's audio item properties (from HTTP API PATCH /api/carts/:slot)
  window.electronAPI.onApiUpdateCartItem((_event, { requestId, slot, updates }) => {
    const item = getCartItem(slot);
    if (!item) {
      window.electronAPI.sendApiResponse({ requestId, success: false, message: 'Cart slot is empty' });
      return;
    }
    for (const [key, value] of Object.entries(updates)) {
      if (!READONLY_ITEM_KEYS.has(key)) (item as any)[key] = value;
    }
    updateCartOnlyItem(item.uuid, item);
    saveProject();
    window.electronAPI.sendApiResponse({ requestId, success: true, cart: { slot, item } });
  });
}

// ---------------------------------------------------------------------------
// Export project flow (dual-dialog when the server is on another machine).
// ---------------------------------------------------------------------------
// When server runs locally, jump straight to a server-side directory picker.
// Otherwise, ask the user where to save: on the server, or back to this
// computer (via a one-shot download token). This replaces the old purely-
// Electron archiver path, which only worked when the project files were
// reachable from this machine.
const exportChoiceVisible   = ref(false);
const exportServerPickerOpen = ref(false);

async function startExportFlow() {
  if (!currentProject.value) return;
  if (server.isLocalServer.value) {
    // Local: skip the choice modal and go straight to the server picker
    // (the "server" here is this same computer, so this matches the user's
    // expectation of a familiar OS-style directory chooser).
    exportServerPickerOpen.value = true;
  } else {
    exportChoiceVisible.value = true;
  }
}

async function onExportChoice(choice: 'server' | 'client') {
  exportChoiceVisible.value = false;
  if (!currentProject.value) return;

  if (choice === 'server') {
    exportServerPickerOpen.value = true;
    return;
  }

  // client → server packages to its temp dir, returns a token, we download
  // the blob and save it via Electron's native save dialog.
  await exportToClientDownload();
}

async function onExportServerPath(serverDir: string) {
  exportServerPickerOpen.value = false;
  if (!serverDir || !currentProject.value) return;
  const project = currentProject.value;
  const outPath = `${serverDir.replace(/[\\/]+$/, '')}/${project.name}.lpa`;
  await runExport({ outputPath: outPath });
}

async function exportToClientDownload() {
  if (!currentProject.value) return;
  const project = currentProject.value;
  const defaultName = `${project.name}.lpa`;
  // Pick the local destination FIRST so a cancelled save dialog doesn't
  // leave a stray .lpa sitting in the server's temp dir.
  const localDest = await window.electronAPI.showSaveArchiveDialog(defaultName);
  if (!localDest) return;
  await runExport({ outputPath: '', downloadTo: localDest });
}

async function runExport(opts: { outputPath: string; downloadTo?: string }) {
  if (!currentProject.value) return;
  const project = currentProject.value;
  progressModal.value = {
    visible: true,
    title: t('exportProgress.title'),
    message: `${t('exportProgress.message')} ${project.name}.lpa…`,
    percentage: 30,
  };
  try {
    const result = await server.exportProjectArchive(
      project.folderPath, project.name, opts.outputPath);
    progressModal.value.percentage = opts.downloadTo ? 60 : 100;

    if (opts.downloadTo && result.downloadToken) {
      progressModal.value.message =
        `${t('exportProgress.downloading')} ${project.name}.lpa…`;
      const blob = await server.downloadArchive(result.downloadToken);
      const buf  = await blob.arrayBuffer();
      const w = await window.electronAPI.writeBinaryFile(opts.downloadTo, buf);
      if (!w.success) throw new Error(w.error || 'write failed');
    }
    progressModal.value.percentage = 100;
  } catch (e) {
    console.error('Export failed:', e);
  } finally {
    setTimeout(() => { progressModal.value.visible = false; }, 400);
  }
}

// Keep the main process HTTP API server up-to-date with the full project state.
// Waveform peak arrays are stripped since they are large and not needed by API consumers.
const stripWaveforms = (items: any[]): any[] =>
  items.map(item => {
    const copy = { ...item, waveform: null };
    if (copy.children) copy.children = stripWaveforms(copy.children);
    return copy;
  });

watch(currentProject, (project) => {
  if (!import.meta.client || !window.electronAPI || !project) return;
  const data = {
    ...project,
    items: stripWaveforms(project.items || []),
    cartOnlyItems: Array.from(cartOnlyItems.value.values()).map(i => ({ ...i, waveform: null }))
  };
  window.electronAPI.syncProjectData(JSON.parse(JSON.stringify(data)));
}, { deep: true, immediate: true });

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
