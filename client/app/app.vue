<template>
  <div id="app" :data-theme="theme">
    <!-- Cart-window mode: standalone detached cart player -->
    <template v-if="isCartWindow">
      <div class="cart-window-root">
        <CartPlayer v-if="currentProject" :is-detached-window="true" />
        <div v-else class="cart-window-loading">
          <span class="material-symbols-rounded">queue_music</span>
        </div>
      </div>

      <!-- Multi-item delete confirm, also needed in the detached cart window -->
      <DeleteSelectionModal
        :visible="deleteDialogVisible"
        :count="deleteDialogCount"
        :name="deleteDialogName"
        :allow-only="deleteDialogAllowOnly"
        @delete-all="deleteDialogConfirmAll"
        @delete-only="deleteDialogConfirmOnly"
        @cancel="deleteDialogCancel"
      />
    </template>

    <!-- Normal mode -->
    <template v-else>
      <WelcomeScreen v-if="!currentProject" />
      <MainWorkspace v-else />
    
    <!-- Accent Color Picker Modal -->
    <div v-if="showColorPicker" class="color-picker-overlay" @click="showColorPicker = false">
      <div class="color-picker-dialog" @click.stop>
        <h3>{{ t('colors.chooseAccent') }}</h3>
        <div class="color-grid">
          <button
            v-for="color in accentColors"
            :key="color"
            class="color-option"
            :style="{ backgroundColor: color }"
            @click="changeAccentColor(color)"
          ></button>
        </div>
        <button class="close-dialog" @click="showColorPicker = false">{{ t('common.cancel') }}</button>
      </div>
    </div>
    
    <!-- About Modal -->
    <AboutModal v-if="showAboutModal" @close="showAboutModal = false" />
    
    <!-- Update Modal -->
    <UpdateModal
      v-if="showUpdateModal"
      :current-version="updateInfo.currentVersion"
      :new-version="updateInfo.newVersion"
      :release-notes="updateInfo.releaseNotes"
      :release-date="updateInfo.releaseDate"
      :is-manual-update="updateInfo.isManualUpdate"
      :download-url="updateInfo.downloadUrl"
      @close="showUpdateModal = false"
    />
    
    <!-- Progress Modal for Import/Export -->
    <ProgressModal
      :visible="progressModal.visible"
      :title="progressModal.title"
      :message="progressModal.message"
      :percentage="progressModal.percentage"
    />
    
    <!-- Project Selection Modal -->
    <ProjectSelectionModal
      :visible="showProjectSelection"
      :projects="availableProjects"
      @select="handleProjectSelection"
      @cancel="handleProjectSelectionCancel"
    />

    <!-- Loading overlay (project open / create / save) -->
    <LoadingOverlay :visible="isLoading" :title="loadingMessage" />

    <!-- Project repair dialog -->
    <ProjectRepairModal
      :visible="repairDialogVisible"
      :issues="repairDialogIssues"
      @confirm="confirmRepair"
      @cancel="cancelRepair"
    />

    <!-- Unsaved-changes prompt (shown when leaving a project with autosave off) -->
    <UnsavedChangesModal
      :visible="unsavedDialogVisible"
      @save="unsavedSave"
      @discard="unsavedDiscard"
      @cancel="unsavedCancel"
    />

    <!-- Confirm prompt shown when deleting a multi-item selection -->
    <DeleteSelectionModal
      :visible="deleteDialogVisible"
      :count="deleteDialogCount"
      :name="deleteDialogName"
      :allow-only="deleteDialogAllowOnly"
      @delete-all="deleteDialogConfirmAll"
      @delete-only="deleteDialogConfirmOnly"
      @cancel="deleteDialogCancel"
    />

    <!-- Background audio-loading progress (when document already rendered) -->
    <AudioLoadProgress />

    <!-- LocalServerStatus pill intentionally not mounted. The local server
         now runs as a visible application with its own console window /
         taskbar entry, so the user has a direct way to see and stop it.
         Keep the component on disk in case we want to re-enable it later. -->
    <!-- <LocalServerStatus /> -->

    <!-- Persistent disconnect → ask the user how to recover -->
    <ConnectionLostModal />

    <!-- App-quit confirmation: unsaved changes, then (in local mode) whether
         to also shut the local audio server down. -->
    <QuitConfirmModal
      :visible="quitUnsavedVisible"
      :title="t('quitModal.unsavedTitle')"
      :message="t('quitModal.unsavedMessage')"
      :buttons="quitUnsavedButtons"
      @pick="onQuitUnsavedPick"
    />
    <QuitConfirmModal
      :visible="quitServerVisible"
      :title="t('quitModal.serverTitle')"
      :message="t('quitModal.serverMessage')"
      :buttons="quitServerButtons"
      @pick="onQuitServerPick"
    />

    <!-- Import: server vs client choice (only for remote servers). -->
    <LocationChoiceModal
      :visible="importChoiceVisible"
      :title="t('importProject.chooseSourceTitle')"
      :message="t('importProject.chooseSourceMessage')"
      :server-label="t('importProject.fromServer')"
      :client-label="t('importProject.fromThisComputer')"
      :cancel-label="t('common.cancel')"
      @pick="onImportChoice"
      @cancel="importChoiceVisible = false"
    />

    <!-- Server file picker — used both to choose a .lpa on the server,
         and to choose the extract destination. -->
    <ServerFilePickerModal
      :open="importServerPickerOpen"
      :mode="importServerPickerStage === 'destination' ? 'directory' : 'file'"
      :filter="importServerPickerStage === 'destination' ? 'all' : '.lpa'"
      :filter-options="importServerPickerStage === 'destination' ? ['all'] : ['.lpa', 'all']"
      start-path=""
      @pick="onImportServerPickerPick"
      @close="importServerPickerOpen = false"
    />
    </template>
  </div>
</template>

<script setup lang="ts">
import 'material-symbols';
import CartPlayer from './components/CartPlayer.vue';

const {
  currentProject, saveProject, openProject, closeProject, confirmUnsavedChanges,
  hasUnsavedChanges,
  isLoading, loadingMessage,
  repairDialogVisible, repairDialogIssues, confirmRepair, cancelRepair,
  unsavedDialogVisible, unsavedSave, unsavedDiscard, unsavedCancel,
  deleteDialogVisible, deleteDialogCount, deleteDialogName, deleteDialogAllowOnly,
  deleteDialogConfirmAll, deleteDialogConfirmOnly, deleteDialogCancel,
} = useProject();

// Shared pending file-open state (set here when a .liveplay/.lpa is double-
// clicked, consumed by WelcomeScreen which owns the server-connection flow).
const pendingFileOpen = useState<{ path: string; kind: 'liveplay' | 'lpa' } | null>(
  'liveplay:pendingFileOpen', () => null);

// Route a double-clicked file to the welcome-screen flow. If a project is
// already open we close it first (after the unsaved-changes prompt) so
// WelcomeScreen re-mounts and picks up the pending file.
async function routePendingFile(data: { filePath: string; kind: 'liveplay' | 'lpa' }) {
  if (!data?.filePath) return;
  pendingFileOpen.value = { path: data.filePath, kind: data.kind };
  if (currentProject.value) {
    const ok = await confirmUnsavedChanges();
    if (!ok) { pendingFileOpen.value = null; return; }
    await closeProject();
  }
}
const { cartOnlyItems, clearCartOnlyItems, addCartOnlyItem } = useCartItems();
import LoadingOverlay from './components/LoadingOverlay.vue';
import AudioLoadProgress from './components/AudioLoadProgress.vue';
import LocationChoiceModal from './components/LocationChoiceModal.vue';
import ServerFilePickerModal from './components/ServerFilePickerModal.vue';
const { currentLocale, setLocale, getDirection, t } = useLocalization();
const theme = useState('theme', () => 'dark');

// Detect if this window is the detached cart player window
const isCartWindow = import.meta.client
  ? new URLSearchParams(window.location.search).get('cartWindow') === '1'
  : false;

// Initialize state viewer for dev mode
useStateViewer();

// Progress modal state
const progressModal = ref({
  visible: false,
  title: '',
  message: '',
  percentage: 0
});

// Project selection modal state
const showProjectSelection = ref(false);
const availableProjects = ref<string[]>([]);
const pendingImportPath = ref<string>('');

// Color picker for accent color
const showColorPicker = ref(false);

// About modal
const showAboutModal = ref(false);

// Update modal
const showUpdateModal = ref(false);
const updateInfo = ref({
  currentVersion: '',
  newVersion: '',
  releaseNotes: '',
  releaseDate: '',
  isManualUpdate: false,
  downloadUrl: ''
});

const accentColors = [
  '#0f62fe', '#0353e9', '#002d9c', // Blues
  '#da1e28', '#a2191f', '#750e13', // Reds
  '#24a148', '#198038', '#0e6027', // Greens
  '#f1c21b', '#d2a106', '#b28600', // Yellows
  '#8a3ffc', '#6929c4', '#491d8b', // Purples
  '#ff7eb6', '#ee5396', '#d02670', // Pinks
];

// Cart window: fetch project data from main process and keep in sync
function applyCartWindowProjectData(projectData: any) {
  if (!projectData || !isCartWindow) return;
  clearCartOnlyItems();
  if (Array.isArray(projectData.cartOnlyItems)) {
    for (const item of projectData.cartOnlyItems) {
      addCartOnlyItem(item);
    }
  }
  // In cart window mode, only set currentProject without triggering watchers
  // Use Object.assign to preserve reactivity while avoiding deep-watch triggers
  if (currentProject.value) {
    Object.assign(currentProject.value, projectData);
  } else {
    currentProject.value = projectData;
  }
  // Apply theme from project
  if (projectData.theme?.mode) {
    theme.value = projectData.theme.mode;
  }
  if (projectData.theme?.accentColor) {
    document.documentElement.style.setProperty('--color-accent-custom', projectData.theme.accentColor);
  }
}

// Listen to menu events
onMounted(() => {
  if (import.meta.client && window.electronAPI) {
    // Cart window initialisation: load project data then listen for updates
    if (isCartWindow) {
      window.electronAPI.getCartWindowProjectData().then((projectData: any) => {
        applyCartWindowProjectData(projectData);
      });
      window.electronAPI.onCartWindowProjectUpdate((_event: any, projectData: any) => {
        applyCartWindowProjectData(projectData);
      });
      // Apply locale from localStorage (already handled by useLocalization)
      return; // skip main-window-only event listeners below
    }

    // Main pushes this when the window is about to close; we run the
    // quit-confirmation dialogs then call confirmQuit to actually quit.
    (window as any).electronAPI.app?.onRequestQuit?.(() => { void runQuitFlow(); });

    window.electronAPI.onMenuToggleDarkMode(() => {
      theme.value = theme.value === 'dark' ? 'light' : 'dark';
      if (currentProject.value) {
        currentProject.value.theme.mode = theme.value as 'dark' | 'light';
        saveProject();
      }
    });

    window.electronAPI.onMenuChangeAccentColor(() => {
      showColorPicker.value = true;
    });

    window.electronAPI.onMenuChangeLanguage((event: any, locale: string) => {
      setLocale(locale);
    });
    
    window.electronAPI.onMenuShowAbout(() => {
      showAboutModal.value = true;
    });
    
    // File > Import Project. When the server is on this same machine the
    // .lpa already lives somewhere we can reach — show the server file
    // picker. When the server is remote, ask the user whether they want
    // to browse the server's filesystem OR pick a .lpa from this computer
    // and upload it. The actual archive extraction always happens on the
    // server now (no more Electron-side extract path), because the
    // extracted project folder MUST live next to the server's audio
    // engine — otherwise the audio files don't resolve at playback.
    window.electronAPI.onMenuImportProject(() => {
      startImportFlow();
    });

    // Listen for update events
    window.electronAPI.onUpdateAvailable((event: any, info: any) => {
      updateInfo.value = info;
      showUpdateModal.value = true;
    });
    
    // Listen for manual update events (fallback)
    window.electronAPI.onManualUpdateAvailable((event: any, info: any) => {
      updateInfo.value = info;
      showUpdateModal.value = true;
    });
    
    // File association (.liveplay / .lpa double-click). Both the warm-start
    // push and the cold-start pull funnel into routePendingFile, which hands
    // off to WelcomeScreen for the server-connection + open/import flow.
    window.electronAPI.onOpenFileAssociation((_event: any, data: { filePath: string; kind: 'liveplay' | 'lpa' }) => {
      void routePendingFile(data);
    });
    window.electronAPI.getPendingOpenFile?.().then((pending) => {
      if (pending?.filePath) void routePendingFile(pending);
    }).catch(() => { /* no pending file */ });

    // Sync menu with current UI language on startup
    window.electronAPI.updateMenuLanguage(currentLocale.value);
  }
});

// ---------------------------------------------------------------------------
// App-quit confirmation flow.
// Main vetoes the window close and pushes `app:request-quit`; we run a
// two-step dialog sequence then call confirmQuit (which actually quits):
//   1. Unsaved-changes prompt — only when there are pending edits. The user
//      can save-then-close, close & discard, or return to the project.
//   2. Local-server prompt — only when the audio server is running locally
//      on this machine (we manage it). The user can close the server too
//      (stops playback + disconnects other clients), close just the client,
//      or return to the project.
// Either prompt's "return" aborts the quit entirely.
// ---------------------------------------------------------------------------
const quitUnsavedVisible = ref(false);
const quitServerVisible  = ref(false);
let _quitUnsavedResolve: ((choice: 'discard' | 'save' | 'cancel') => void) | null = null;
let _quitServerResolve:  ((choice: 'server'  | 'client' | 'cancel') => void) | null = null;
let quitFlowActive = false;

const quitUnsavedButtons = computed(() => [
  { key: 'cancel',  label: t('quitModal.return'),           variant: 'ghost'   as const },
  { key: 'discard', label: t('quitModal.unsavedDiscard'),   variant: 'danger'  as const },
  { key: 'save',    label: t('quitModal.unsavedSaveClose'), variant: 'primary' as const, icon: 'save' },
]);
const quitServerButtons = computed(() => [
  { key: 'cancel', label: t('quitModal.return'),            variant: 'ghost'  as const },
  { key: 'client', label: t('quitModal.serverCloseClient'), variant: undefined },
  { key: 'server', label: t('quitModal.serverCloseServer'), variant: 'danger' as const, icon: 'power_settings_new' },
]);

function onQuitUnsavedPick(choice: string) { _quitUnsavedResolve?.(choice as 'discard' | 'save' | 'cancel'); }
function onQuitServerPick(choice: string)  { _quitServerResolve?.(choice as 'server' | 'client' | 'cancel'); }

function askQuitUnsaved(): Promise<'discard' | 'save' | 'cancel'> {
  quitUnsavedVisible.value = true;
  return new Promise((resolve) => {
    _quitUnsavedResolve = (choice) => {
      quitUnsavedVisible.value = false;
      _quitUnsavedResolve = null;
      resolve(choice);
    };
  });
}

function askQuitServer(): Promise<'server' | 'client' | 'cancel'> {
  quitServerVisible.value = true;
  return new Promise((resolve) => {
    _quitServerResolve = (choice) => {
      quitServerVisible.value = false;
      _quitServerResolve = null;
      resolve(choice);
    };
  });
}

async function runQuitFlow() {
  if (quitFlowActive) return;
  quitFlowActive = true;
  const api = (window as any).electronAPI;
  try {
    // Step 1 — unsaved changes (pending edits with autosave off).
    if (hasUnsavedChanges.value) {
      const choice = await askQuitUnsaved();
      if (choice === 'cancel') return;
      if (choice === 'save') {
        try {
          await saveProject({ force: true });
        } catch (e) {
          console.error('[quit] save failed, aborting quit:', e);
          return;
        }
      }
    }

    // Step 2 — local audio server (only when we manage one that's running).
    let serverIsLocal = false;
    try {
      const status = await api?.liveplayServer?.getStatus?.();
      serverIsLocal = !!status && status.config?.mode === 'local' && !!status.running;
    } catch (e) {
      console.warn('[quit] could not read server status:', e);
    }

    if (serverIsLocal) {
      const choice = await askQuitServer();
      if (choice === 'cancel') return;
      await api?.app?.confirmQuit?.({ stopServer: choice === 'server' });
    } else {
      await api?.app?.confirmQuit?.({ stopServer: false });
    }
  } finally {
    quitFlowActive = false;
  }
}

const changeAccentColor = (color: string) => {
  if (currentProject.value) {
    currentProject.value.theme.accentColor = color;
    document.documentElement.style.setProperty('--color-accent-custom', color);
    saveProject();
    showColorPicker.value = false;
  }
};

// ---------------------------------------------------------------------------
// Import project flow (dual-dialog when client and server are on different
// machines). The extraction ALWAYS happens server-side because the
// extracted project folder needs to live next to the audio engine.
// ---------------------------------------------------------------------------
const importChoiceVisible       = ref(false);
const importServerPickerOpen    = ref(false);
const importServerPickerStage   = ref<'archive' | 'destination'>('archive');
const pendingArchiveOnServer    = ref<string>('');           // server-side .lpa path
const pendingArchiveBlob        = ref<File | null>(null);    // client-side .lpa
const pendingArchiveBlobName    = ref<string>('');

// When a .lpa is double-clicked, WelcomeScreen handles the local/remote
// connection then publishes the local .lpa path here. We buffer the file and
// reuse the standard import destination-picker (server owns the extraction).
const pendingLpaImportReady = useState<string | null>('liveplay:pendingLpaImportReady', () => null);
watch(pendingLpaImportReady, async (lpaPath) => {
  if (!lpaPath) return;
  pendingLpaImportReady.value = null; // consume
  try {
    const result = await (window as any).electronAPI.readAudioFile(lpaPath);
    if (!result?.success || !result.data) {
      console.error('Failed to read .lpa:', result?.error);
      return;
    }
    const bytes = new Uint8Array(result.data);
    const name  = lpaPath.split(/[\\/]/).pop() || 'import.lpa';
    pendingArchiveBlob.value      = new File([bytes], name);
    pendingArchiveBlobName.value  = name;
    importServerPickerStage.value = 'destination';
    importServerPickerOpen.value  = true;
  } catch (error) {
    console.error('Failed to open .lpa file:', error);
  }
});

function startImportFlow() {
  const server = useLiveplayServer();
  importServerPickerStage.value = 'archive';
  if (server.isLocalServer.value) {
    importServerPickerOpen.value = true;
  } else {
    importChoiceVisible.value = true;
  }
}

async function onImportChoice(choice: 'server' | 'client') {
  importChoiceVisible.value = false;
  if (choice === 'server') {
    importServerPickerStage.value = 'archive';
    importServerPickerOpen.value = true;
    return;
  }
  // client → ask Electron for an .lpa from this computer, then move on to
  // the server-destination picker so the user chooses WHERE on the server
  // it should be extracted.
  const lpaPath = await (window as any).electronAPI.showOpenArchiveDialog();
  if (!lpaPath) return;
  const result = await (window as any).electronAPI.readAudioFile(lpaPath);
  if (!result?.success || !result.data) {
    console.error('Failed to read .lpa:', result?.error);
    return;
  }
  const bytes = new Uint8Array(result.data);
  const name  = lpaPath.split(/[\\/]/).pop() || 'import.lpa';
  pendingArchiveBlob.value     = new File([bytes], name);
  pendingArchiveBlobName.value = name;
  importServerPickerStage.value = 'destination';
  importServerPickerOpen.value  = true;
}

async function onImportServerPickerPick(serverPath: string) {
  importServerPickerOpen.value = false;
  if (!serverPath) return;
  const server = useLiveplayServer();

  if (importServerPickerStage.value === 'archive') {
    // The user picked a .lpa that already lives on the server. Now ask
    // WHERE to extract it (also on the server).
    pendingArchiveOnServer.value = serverPath;
    importServerPickerStage.value = 'destination';
    importServerPickerOpen.value  = true;
    return;
  }

  // 'destination': we know the .lpa (either on server or buffered locally)
  // AND now the extraction directory. Kick off the import.
  progressModal.value = {
    visible: true,
    title:   t('importProgress.title'),
    message: `${t('importProgress.message')} ${pendingArchiveBlobName.value ||
              pendingArchiveOnServer.value.split(/[\\/]/).pop() || ''}…`,
    percentage: 40,
  };
  try {
    let result;
    if (pendingArchiveBlob.value) {
      result = await server.importProjectArchiveUpload(
        pendingArchiveBlob.value, serverPath, pendingArchiveBlobName.value);
    } else {
      result = await server.importProjectArchiveFromServer(
        pendingArchiveOnServer.value, serverPath);
    }
    progressModal.value.percentage = 100;
    if (result.projectFiles.length === 0) {
      console.error('No .liveplay file in archive');
      return;
    }
    if (result.projectFiles.length > 1) {
      availableProjects.value = result.projectFiles;
      pendingImportPath.value = result.extractPath;
      showProjectSelection.value = true;
    } else {
      await openProject(`${result.extractPath}/${result.projectFiles[0]}`);
    }
  } catch (e) {
    console.error('Import failed:', e);
  } finally {
    setTimeout(() => { progressModal.value.visible = false; }, 300);
    pendingArchiveOnServer.value = '';
    pendingArchiveBlob.value     = null;
    pendingArchiveBlobName.value = '';
  }
}

// Handle project selection from multiple projects
const handleProjectSelection = async (projectName: string) => {
  showProjectSelection.value = false;
  const projectPath = `${pendingImportPath.value}/${projectName}`;
  await openProject(projectPath);
  pendingImportPath.value = '';
  availableProjects.value = [];
};

const handleProjectSelectionCancel = () => {
  showProjectSelection.value = false;
  pendingImportPath.value = '';
  availableProjects.value = [];
};

// Set initial theme from project
watch(currentProject, (project) => {
  if (project) {
    theme.value = project.theme.mode;
    
    // Set accent color
    if (import.meta.client && project.theme.accentColor) {
      document.documentElement.style.setProperty('--color-accent-custom', project.theme.accentColor);
    }
  }
}, { immediate: true });

// Apply RTL direction when locale changes
watch(currentLocale, () => {
  if (import.meta.client) {
    const direction = getDirection();
    document.documentElement.setAttribute('dir', direction);
  }
}, { immediate: true });

// Enable drag-and-drop globally.
// Chromium requires preventDefault() on EVERY dragover event (including on
// intermediate elements) for the drop event to fire. Using capture phase
// ensures this runs before any component handlers.
onMounted(() => {
  if (import.meta.client) {
    document.addEventListener('dragenter', (e) => {
      e.preventDefault();
    }, true);
    document.addEventListener('dragover', (e) => {
      e.preventDefault();
    }, true);
  }
});
</script>

<style scoped>
#app {
  width: 100%;
  height: 100%;
  overflow: hidden;
}

.color-picker-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: var(--z-modal);
}

.color-picker-dialog {
  background: var(--color-surface);
  padding: var(--spacing-xl);
  border-radius: var(--border-radius-lg);
  min-width: 400px;
  color: var(--color-text-primary);
}

.color-picker-dialog h3 {
  margin-bottom: var(--spacing-md);
  color: var(--color-text-primary);
}

.color-grid {
  display: grid;
  grid-template-columns: repeat(6, 1fr);
  gap: var(--spacing-sm);
  margin-bottom: var(--spacing-md);
}

.color-option {
  width: 50px;
  height: 50px;
  border: 2px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
  transition: transform var(--transition-fast);
}

.color-option:hover {
  transform: scale(1.1);
  border-color: var(--color-text-primary);
}

.close-dialog {
  width: 100%;
  padding: var(--spacing-sm) var(--spacing-md);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-primary);
}

.close-dialog:hover {
  background: var(--color-surface-hover);
}

.cart-window-root {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  background-color: var(--color-background);
}

.cart-window-loading {
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background-color: var(--color-background);
  color: var(--color-text-secondary);

  .material-symbols-rounded {
    font-size: 48px;
    opacity: 0.3;
  }
}
</style>
