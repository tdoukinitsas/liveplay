<template>
  <div id="app" :data-theme="theme">
    <!-- Outside the three window modes on purpose: every one of them opens its
         own socket, so every one of them can be the window that finds the
         server wants a login. Renders nothing until that happens. -->
    <LoginScreen />

    <!-- Each window can open Settings, including the detached mixer's outputs editor. -->
    <SettingsPage />

    <!-- Cart-window mode: standalone detached cart player -->
    <template v-if="isCartWindow">
      <div class="cart-window-root">
        <CartPlayer v-if="currentProject" :is-detached-window="true" />
        <div v-else class="cart-window-loading">
          <span class="material-symbols-rounded">queue_music</span>
        </div>
      </div>

      <!-- The detached cart window fires cues too, so it needs the same
           lockout: without it, cart pads here would keep accepting presses
           that silently never reach the server. -->
      <ConnectionLostModal />

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

    <!-- Mixer-window mode: the mixer on its own, always full width.
         It needs no project data to work — buses, meters and fader moves all
         go over this window's own WebSocket — so it renders as soon as the
         socket is up, whether or not the main window has a project open. -->
    <template v-else-if="isMixerWindow">
      <div class="mixer-window-root">
        <MixerPanel mode="full" :detached="true" />
      </div>

      <!-- Faders here move the live rig, so the same lockout applies: without
           it a fader would keep accepting drags that never reach the server. -->
      <ConnectionLostModal />
    </template>

    <!-- Normal mode -->
    <template v-else>
      <WelcomeScreen v-if="!currentProject" />
      <MainWorkspace v-else />
    
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

    <!-- Reconnected, but the server no longer holds our project (it restarted)
         → resume from where we left off, or go back to the welcome screen. -->
    <SessionRecoveryModal />

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
import MixerPanel from './components/MixerPanel.vue';

const {
  currentProject, saveProject, flushPendingSave, openProject, closeProject, confirmUnsavedChanges,
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
// The colour scheme belongs to the person at the desk, not to the show (U4).
// `theme` stays a useState key so nothing that already binds to it has to
// change; what moved is where its value comes from.
const { theme: userTheme, resolvedThemeMode, setTheme } = usePreferences();
const theme = useState('theme', () => 'dark');
// The Help and View menus open Settings at a section rather than raising
// modals of their own; see the listeners below.
const { open: openSettings } = useSettingsPage();

// Detect if this window is the detached cart player window
const isCartWindow = import.meta.client
  ? new URLSearchParams(window.location.search).get('cartWindow') === '1'
  : false;

// …or the detached mixer window.
const isMixerWindow = import.meta.client
  ? new URLSearchParams(window.location.search).get('mixerWindow') === '1'
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

// Detached windows: fetch project data from the main process and keep in sync.
// The mixer window takes this too — not because it needs the cue list, but for
// the theme and for settings.outputTargetLevels, which drive the meter zone
// colours. Without it a detached meter would colour its zones off the EBU
// defaults and disagree with the same meter in the main window.
function applyDetachedWindowProjectData(projectData: any) {
  if (!projectData || !(isCartWindow || isMixerWindow)) return;
  if (isCartWindow) {
    clearCartOnlyItems();
    if (Array.isArray(projectData.cartOnlyItems)) {
      for (const item of projectData.cartOnlyItems) {
        addCartOnlyItem(item);
      }
    }
  }
  // In detached windows, only set currentProject without triggering watchers
  // Use Object.assign to preserve reactivity while avoiding deep-watch triggers
  if (currentProject.value) {
    Object.assign(currentProject.value, projectData);
  } else {
    currentProject.value = projectData;
  }
  // The theme deliberately does NOT come through here any more (U4). A
  // detached window is the same person at the same desk, so it reads the same
  // preferences the main window does — from their server profile if they are
  // signed in, otherwise from this machine's own store, which the `storage`
  // event keeps in step across windows. Routing it through the project data
  // meant the colour scheme arrived as a property of whatever file was open.
}

// Listen to menu events
onMounted(() => {
  if (import.meta.client && window.electronAPI) {
    // Detached-window initialisation: load project data then listen for
    // updates. Both the cart and mixer windows stop here — the listeners
    // below (menu commands, updates, file association, quit flow) belong to
    // the main window and would double-fire if a second window took them too.
    if (isCartWindow || isMixerWindow) {
      window.electronAPI.getCartWindowProjectData().then((projectData: any) => {
        applyDetachedWindowProjectData(projectData);
      });
      window.electronAPI.onCartWindowProjectUpdate((_event: any, projectData: any) => {
        applyDetachedWindowProjectData(projectData);
      });
      // Apply locale from localStorage (already handled by useLocalization)
      return; // skip main-window-only event listeners below
    }

    // Main pushes this when the window is about to close; we run the
    // quit-confirmation dialogs then call confirmQuit to actually quit.
    (window as any).electronAPI.app?.onRequestQuit?.(() => { void runQuitFlow(); });

    window.electronAPI.onMenuToggleDarkMode(() => {
      // Straight to the person's preferences, and no saveProject() with it —
      // flipping to light mode used to mark the show dirty (U4).
      //
      // Toggles against what is SHOWING, not against the preference, so that
      // from "system" it goes to the opposite of what is on screen rather than
      // to whichever branch the preference happens to read as. Landing on an
      // explicit mode is correct: asking for dark is asking for dark, not for
      // "follow the OS and hope".
      setTheme({ mode: resolvedThemeMode.value === 'dark' ? 'light' : 'dark' });
    });

    // Both of these used to raise a modal of their own. They are panes now, so
    // the menu item is a deep link — the same move the mixer's output-map
    // action made in P3d, and the reason the accent swatches and the About
    // panel each have exactly one home.
    window.electronAPI.onMenuChangeAccentColor(() => {
      openSettings('appearance');
    });

    window.electronAPI.onMenuChangeLanguage((event: any, locale: string) => {
      setLocale(locale);
    });
    
    window.electronAPI.onMenuShowAbout(() => {
      openSettings('about');
    });

    // The Settings menu. One channel for all ten panes, with the section id as
    // its argument; no id means "open Settings", and `open()` falls back to
    // DEFAULT_SETTINGS_SECTION on its own.
    //
    // It also VALIDATES the id against SETTINGS_SECTIONS, which is what makes
    // the menu's mirrored list safe to be a mirror: a pane the main process
    // still offers after this side stopped registering it opens the default
    // pane rather than a blank one.
    window.electronAPI.onMenuOpenSettings((_event: any, section?: string) => {
      openSettings(section);
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
    // Step 0 — D13: flush any whole-document save still sitting in the
    // 300 ms debounce window (autosave on, edit made in the instant before
    // quit). hasUnsavedChanges only tracks the autosave-off case below, so
    // this has to run unconditionally; it's a no-op when nothing is pending.
    try { await flushPendingSave(); } catch (e) { console.warn('[quit] flush failed:', e); }

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
  if (server.isLocalServer) {
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

// Paint the operator's own theme, from wherever usePreferences resolved it.
// Was `watch(currentProject, ...)` until U4, which is why opening a colleague's
// show used to change your colours.
//
// Watches the RESOLVED mode, not the preference: "system" is not a palette, and
// the stylesheet only defines [data-theme='light'] and [data-theme='dark'].
// Because the resolved value also depends on the OS, this fires on its own when
// the desktop flips at sunset mid-show — which is the whole point of the
// setting, and would not happen if this watched the preference.
watch([resolvedThemeMode, () => userTheme.value?.accentColor], ([mode, accent]) => {
  if (!mode) return;
  theme.value = mode;
  // Mirror onto <html> too: the theme variables are scoped to [data-theme],
  // and with it only on #app, `body { background: var(--color-background) }`
  // resolved to nothing and anything transparent showed the window's white.
  if (import.meta.client) document.documentElement.setAttribute('data-theme', mode);
  if (import.meta.client && accent) {
    document.documentElement.style.setProperty('--color-accent-custom', accent);
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

.cart-window-root {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  background-color: var(--color-background);
}

.mixer-window-root {
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
