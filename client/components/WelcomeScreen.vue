<template>
  <div class="welcome-screen">
    <div class="welcome-container">
      <div class="welcome-header">
        <img
          :src="isDark ? './assets/icons/SVG/liveplay-icon-darkmode@web.svg' : './assets/icons/SVG/liveplay-icon-lightmode@web.svg'"
          alt="LivePlay"
          class="welcome-logo"
        />
        <div class="welcome-text">
          <h1 class="welcome-title">
            {{ t('welcome.title') }}
            <span class="version-badge">v{{ appVersion }}</span>
          </h1>
          <p class="welcome-subtitle">{{ t('welcome.subtitle') }}</p>
        </div>
      </div>

      <!-- Stage 1: mode picker. Hidden once we've connected. -->
      <div v-if="stage === 'mode'" class="welcome-stage">
        <h2 class="stage-title">{{ t('welcome.modeTitle') }}</h2>
        <p class="stage-subtitle">{{ t('welcome.modeSubtitle') }}</p>
        <div class="welcome-actions">
          <button
            class="welcome-button primary"
            :disabled="connecting"
            @click="chooseLocal"
          >
            <span class="button-icon">
              <span v-if="connecting && mode === 'local'" class="material-symbols-rounded spin">progress_activity</span>
              <span v-else class="material-symbols-rounded">computer</span>
            </span>
            <span class="button-label">
              <span class="button-label-line">
                {{ connecting && mode === 'local' ? t('welcome.startingLocalServer') : t('welcome.localMode') }}
              </span>
              <span class="button-label-sub">{{ t('welcome.localModeDescription') }}</span>
            </span>
          </button>

          <button class="welcome-button" :disabled="connecting" @click="chooseRemote">
            <span class="button-icon"><span class="material-symbols-rounded">lan</span></span>
            <span class="button-label">
              <span class="button-label-line">{{ t('welcome.remoteMode') }}</span>
              <span class="button-label-sub">{{ t('welcome.remoteModeDescription') }}</span>
            </span>
          </button>
        </div>
        <p v-if="connectionError" class="remote-error">{{ connectionError }}</p>
      </div>

      <!-- Stage 2: remote address entry. -->
      <div v-else-if="stage === 'remote'" class="welcome-stage">
        <h2 class="stage-title">{{ t('welcome.remoteConnect') }}</h2>
        <p class="stage-subtitle">{{ t('welcome.remoteAddressHint') }}</p>

        <!-- Auto-discovered servers on this LAN. Populated by the UDP beacon
             and active solicitation. Header always shown so the user can
             rescan even when nothing has been found yet. -->
        <div class="discovered-servers">
          <div class="discovered-header">
            <span class="material-symbols-rounded" :class="{ spin: scanning }">radar</span>
            <span>{{ t('welcome.serversOnThisNetwork') }}</span>
            <button class="discovered-rescan" :disabled="scanning" @click="rescan" :title="t('welcome.rescan')">
              <span class="material-symbols-rounded">refresh</span>
            </button>
          </div>
          <button
            v-for="srv in discoveredServers"
            :key="srv.instanceId"
            class="discovered-row"
            @click="connectToDiscovered(srv)"
            :disabled="connecting"
          >
            <span class="material-symbols-rounded discovered-icon">dns</span>
            <span class="discovered-main">
              <span class="discovered-name">{{ srv.name }}</span>
              <span class="discovered-meta">
                {{ srv.host }}:{{ srv.port }}
                <span v-if="srv.hasOpenProject" class="discovered-project">
                  · {{ srv.projectName || 'project open' }} ({{ srv.itemCount }})
                </span>
              </span>
            </span>
            <span class="material-symbols-rounded discovered-arrow">arrow_forward</span>
          </button>
          <p v-if="discoveredServers.length === 0" class="discovered-empty">
            {{ scanning ? t('welcome.scanning') : t('welcome.noServersFound') }}
          </p>
        </div>

        <!-- Recently-connected servers. The robust fallback when discovery
             can't reach a server (different subnet, VPN, locked-down WiFi). -->
        <div v-if="recentServers.length > 0" class="discovered-servers">
          <div class="discovered-header">
            <span class="material-symbols-rounded">history</span>
            <span>{{ t('welcome.recentServers') }}</span>
          </div>
          <button
            v-for="srv in recentServers"
            :key="srv.url"
            class="discovered-row"
            @click="connectToRecent(srv)"
            :disabled="connecting"
          >
            <span class="material-symbols-rounded discovered-icon">lan</span>
            <span class="discovered-main">
              <span class="discovered-name">{{ srv.name || srv.host || srv.url }}</span>
              <span class="discovered-meta">{{ srv.host }}:{{ srv.port }}</span>
            </span>
            <span class="material-symbols-rounded discovered-remove" @click.stop="forgetRecent(srv)" :title="t('welcome.forget')">close</span>
          </button>
        </div>

        <div class="remote-form">
          <label class="remote-field-label">{{ t('welcome.serverAddress') }}</label>
          <input
            v-model="remoteAddress"
            type="text"
            class="remote-field-input"
            :placeholder="t('welcome.serverAddressPlaceholder')"
            @keydown.enter="connectToRemote"
          />
          <p v-if="connectionError" class="remote-error">{{ connectionError }}</p>
        </div>
        <div class="remote-actions">
          <button class="welcome-button" @click="stage = 'mode'">
            <span class="material-symbols-rounded">arrow_back</span>
            <span>{{ t('welcome.back') }}</span>
          </button>
          <button
            class="welcome-button primary"
            :disabled="!remoteAddress || connecting"
            @click="connectToRemote"
          >
            <span class="material-symbols-rounded">link</span>
            <span>{{ connecting ? t('welcome.connecting') : t('welcome.connect') }}</span>
          </button>
        </div>
      </div>

      <!-- Stage 3: project picker. -->
      <div v-else-if="stage === 'project'" class="welcome-stage">
        <p class="stage-subtitle connection-summary">
          <span class="material-symbols-rounded connection-icon">{{ mode === 'remote' ? 'lan' : 'computer' }}</span>
          {{ mode === 'remote'
              ? t('welcome.connectedTo', { url: serverUrlDisplay })
              : t('welcome.connectedLocal') }}
          <button class="link-button" @click="changeMode">{{ t('welcome.changeMode') }}</button>
        </p>
        <div class="welcome-actions">
          <button class="welcome-button primary" @click="handleNewProject">
            <span class="button-icon"><span class="material-symbols-rounded">add</span></span>
            <span>{{ t('welcome.newProject') }}</span>
          </button>

          <button class="welcome-button" @click="handleOpenProject">
            <span class="button-icon"><span class="material-symbols-rounded">folder</span></span>
            <span>{{ t('welcome.openProject') }}</span>
          </button>
        </div>
      </div>
    </div>

    <!-- Server-side file picker. Used to choose either the project's parent
         folder (New) or a .liveplay file (Open). The picker browses the
         server's filesystem (drives, network paths, anywhere). -->
    <ServerFilePickerModal
      :open="showPicker"
      :mode="pickerMode"
      :filter="pickerFilter"
      :filter-options="pickerFilterOptions"
      :start-path="pickerStart"
      @pick="onPickerPick"
      @close="showPicker = false"
    />

    <!-- New-project name dialog.
         data-theme is mirrored onto the teleported root: <Teleport to="body">
         hoists this OUT of #app (which carries the data-theme attribute), so
         without it the [data-theme='…'] CSS variables resolve to nothing and
         the dialog renders transparent. -->
    <Teleport to="body">
      <div v-if="showNameDialog" class="name-dialog-backdrop" :data-theme="theme" @click.self="cancelNameDialog">
        <div class="name-dialog">
          <h3 class="name-dialog__title">{{ t('project.enterName') }}</h3>
          <input
            ref="nameDialogInput"
            class="name-dialog__input"
            v-model="nameDialogValue"
            :placeholder="t('project.placeholder')"
            @keydown.enter="confirmNameDialog"
            @keydown.escape="cancelNameDialog"
          />
          <div class="name-dialog__actions">
            <button class="name-dialog__btn" @click="cancelNameDialog">{{ t('project.cancel') }}</button>
            <button class="name-dialog__btn name-dialog__btn--primary" :disabled="!nameDialogValue.trim()" @click="confirmNameDialog">{{ t('project.ok') }}</button>
          </div>
        </div>
      </div>
    </Teleport>
  </div>
</template>

<script setup lang="ts">
import ServerFilePickerModal from './ServerFilePickerModal.vue';

const { createNewProject, openProject, tryRejoinExistingProject } = useProject();
const { t } = useLocalization();
const server = useLiveplayServer();

// Three-stage flow: mode picker → (optional remote address) → project picker.
type Stage = 'mode' | 'remote' | 'project';
const stage = ref<Stage>('mode');
const mode  = ref<'local' | 'remote'>('local');

const remoteAddress   = ref('');
const connecting      = ref(false);
const connectionError = ref<string>('');

// File-association open. `pendingFileOpen` is set by app.vue when a .liveplay
// or .lpa is double-clicked; this screen owns the server-connection flow.
//  * .liveplay → force local, start the server, open directly (no UI).
//  * .lpa      → ask local/remote, then (after connect) hand the path back to
//                app.vue's import destination-picker via pendingLpaImportReady.
const pendingFileOpen = useState<{ path: string; kind: 'liveplay' | 'lpa' } | null>(
  'liveplay:pendingFileOpen', () => null);
const pendingLpaImportReady = useState<string | null>(
  'liveplay:pendingLpaImportReady', () => null);
const importAfterConnect = ref(false);
const pendingLpaPath     = ref('');

// LAN-discovered servers (populated from the UDP beacon via Electron IPC).
type DiscoveredServer = {
  instanceId: string;
  name: string;
  host: string;
  port: number;
  version: string;
  projectName: string;
  hasOpenProject: boolean;
  itemCount: number;
  url: string;
};
const discoveredServers = ref<DiscoveredServer[]>([]);
let stopDiscoverySub: (() => void) | null = null;

// Recently-connected servers (persisted by the Electron main process).
type RecentServer = { url: string; name: string; host: string; port: number; lastSeen: number };
const recentServers = ref<RecentServer[]>([]);
const scanning = ref(false);

// Computed reflection of the currently-configured server URL.
const serverUrlDisplay = computed(() => server.serverUrl ?? 'http://127.0.0.1:4480');

// Server file picker state — shared by New and Open flows.
const showPicker          = ref(false);
const pickerMode          = ref<'file' | 'directory'>('directory');
const pickerFilter        = ref<string>('.liveplay,.lpa');
const pickerFilterOptions = ref<string[]>(['.liveplay,.lpa', 'all']);
const pickerStart         = ref<string>('');
const pickerIntent        = ref<'new' | 'open'>('open');

// Get app version
const appVersion = ref('1.1.3');
onMounted(async () => {
  if (import.meta.client && (window as any).electronAPI?.getAppVersion) {
    appVersion.value = await (window as any).electronAPI.getAppVersion();
  }

  // Read the persisted mode/server config from Electron (if available) and
  // auto-skip the mode picker if the user has already chosen a side.
  try {
    if (import.meta.client && (window as any).electronAPI?.liveplayServer?.getConfig) {
      const cfg = await (window as any).electronAPI.liveplayServer.getConfig();
      if (cfg?.mode === 'remote' && cfg.remoteUrl) {
        mode.value = 'remote';
        remoteAddress.value = stripScheme(cfg.remoteUrl);
        server.setServerUrl(cfg.remoteUrl);
      } else if (cfg?.mode === 'local') {
        mode.value = 'local';
        const url = `http://127.0.0.1:${cfg.localPort ?? 4480}`;
        server.setServerUrl(url);
      }
    }
  } catch (e) {
    console.warn('[welcome] could not read liveplay-server config:', e);
  }

  // A double-clicked .liveplay/.lpa takes precedence over everything below:
  // it drives its own server-connection + open/import flow.
  const pending = pendingFileOpen.value;
  if (pending) {
    await handlePendingFileOpen(pending);
  } else {
    // If the user just hit File > New / Open while a project was open, we
    // closed that project to land them here — skip the mode picker (their
    // server is still configured) and pop the appropriate picker straight
    // away. Without this, File > New would dump them at the mode picker
    // instead of the new-project flow they actually asked for.
    let welcomeIntent: string | null = null;
    try { welcomeIntent = sessionStorage.getItem('liveplay:welcomeIntent'); } catch {}
    if (welcomeIntent === 'new' || welcomeIntent === 'open') {
      try { sessionStorage.removeItem('liveplay:welcomeIntent'); } catch {}
      stage.value = 'project';
      // Defer to next tick so the project-stage UI is mounted before we
      // ask it to open its modal.
      nextTick(() => {
        if (welcomeIntent === 'new') handleNewProject();
        else                          handleOpenProject();
      });
    } else {
      // Always go through the mode-picker so the user is in control of the
      // current session's server target. We could skip if connected, but the
      // first-impression UI value of a deliberate choice outweighs the click.
      stage.value = 'mode';
    }
  }

  // Start LAN discovery so the remote-mode stage immediately shows any
  // servers visible on the network.
  try {
    const disc = (window as any).electronAPI?.liveplayDiscovery;
    if (disc) {
      await disc.start();
      const initial = await disc.list();
      if (Array.isArray(initial)) discoveredServers.value = initial;
      stopDiscoverySub = disc.onServers((list: DiscoveredServer[]) => {
        discoveredServers.value = list ?? [];
      });
      // Load the persisted recent-servers list for the fallback picker.
      try {
        const recent = await disc.recentList?.();
        if (Array.isArray(recent)) recentServers.value = recent;
      } catch {}
    }
  } catch (e) {
    console.warn('[welcome] discovery start failed:', e);
  }
});

// Drive a double-clicked file. For .liveplay: force local, start the server,
// open directly. For .lpa: stash the path and show the mode picker so the
// user chooses local/remote; the import resumes once connected.
async function handlePendingFileOpen(p: { path: string; kind: 'liveplay' | 'lpa' }) {
  pendingFileOpen.value = null; // consume so re-entry / the watcher no-ops
  if (p.kind === 'liveplay') {
    mode.value = 'local';
    connectionError.value = '';
    connecting.value = true;
    try {
      if (!(await ensureLocalServer())) { stage.value = 'mode'; return; }
      const ok = await openProject(p.path);
      if (!ok) {
        connectionError.value = t('welcome.connectionFailed');
        stage.value = 'mode';
      }
      // On success openProject sets currentProject and this screen unmounts.
    } catch (e: any) {
      connectionError.value = e?.message ?? String(e);
      stage.value = 'mode';
    } finally {
      connecting.value = false;
    }
  } else {
    pendingLpaPath.value = p.path;
    importAfterConnect.value = true;
    stage.value = 'mode';
  }
}

// Late-arrival case: a file double-clicked while this screen is already
// mounted (e.g. sitting on the welcome screen with no project open). onMounted
// won't re-run, so react to the shared state changing.
watch(pendingFileOpen, (p) => {
  if (p) void handlePendingFileOpen(p);
});

onUnmounted(() => {
  if (stopDiscoverySub) { try { stopDiscoverySub(); } catch {} stopDiscoverySub = null; }
});

// Get theme from app state (works even when no project is open)
const theme = useState('theme', () => 'dark');
const isDark = computed(() => theme.value === 'dark');

// ---- Mode handlers ---------------------------------------------------------
function stripScheme(url: string): string {
  return url.replace(/^https?:\/\//, '').replace(/\/+$/, '');
}
function normaliseRemoteUrl(input: string): string {
  let v = input.trim();
  if (!v) return '';
  // Allow bare host or host:port. Default to http:// and port 4480.
  if (!/^https?:\/\//i.test(v)) v = 'http://' + v;
  // If no explicit port, append :4480 for convenience.
  try {
    const u = new URL(v);
    if (!u.port) u.port = '4480';
    return u.origin;
  } catch {
    return v;
  }
}

// Configure local mode, set the server URL, and spawn (or reattach to) the
// local server, waiting until /api/health answers so a follow-up WS connect
// doesn't race the bind. Returns false (and sets connectionError) on failure.
// Shared by the Local button and the .liveplay file-association path.
async function ensureLocalServer(): Promise<boolean> {
  const api = (window as any).electronAPI?.liveplayServer;
  if (import.meta.client && api?.setConfig) {
    const cfg = await api.setConfig({ mode: 'local' });
    const url = `http://127.0.0.1:${cfg.localPort ?? 4480}`;
    server.setServerUrl(url);
    if (api.ensureRunning) {
      const res = await api.ensureRunning();
      if (!res?.ok) {
        connectionError.value = res?.error
          ? `Local server failed to start: ${res.error}`
          : 'Local server failed to start';
        return false;
      }
    }
  } else {
    server.setServerUrl('http://127.0.0.1:4480');
  }
  return true;
}

async function chooseLocal() {
  mode.value = 'local';
  connectionError.value = '';
  connecting.value = true;
  try {
    if (!(await ensureLocalServer())) return;
    // .lpa double-click: skip the project picker, go straight to extraction.
    if (importAfterConnect.value) { beginImportDestination(); return; }
    // If a project is already open server-side (e.g. the user kept the
    // detached server running between renderer reloads), drop straight
    // into the workspace.
    if (await tryRejoinExistingProject()) return;
    stage.value = 'project';
  } catch (e: any) {
    connectionError.value = e?.message ?? String(e);
  } finally {
    connecting.value = false;
  }
}

// Called once a server is connected (local or remote) while a .lpa import is
// pending. Hands the local .lpa path to app.vue, which owns the destination
// picker + upload + extract + open (with progress). We land on the project
// stage so cancelling the import leaves the user in a usable state.
function beginImportDestination() {
  importAfterConnect.value = false;
  const lpa = pendingLpaPath.value;
  pendingLpaPath.value = '';
  stage.value = 'project';
  if (lpa) pendingLpaImportReady.value = lpa;
}

function chooseRemote() {
  mode.value = 'remote';
  connectionError.value = '';
  if (!remoteAddress.value) {
    const fallback = stripScheme(server.serverUrl ?? '');
    if (fallback && fallback !== '127.0.0.1:4480') remoteAddress.value = fallback;
  }
  stage.value = 'remote';
}

function changeMode() {
  stage.value = 'mode';
  connectionError.value = '';
}

async function connectToRemote() {
  if (!remoteAddress.value) return;
  const url = normaliseRemoteUrl(remoteAddress.value);
  if (!url) {
    connectionError.value = t('welcome.connectionFailed');
    return;
  }

  connecting.value = true;
  connectionError.value = '';
  try {
    // Probe the server's /api/health before committing.
    const r = await fetch(url + '/api/health', { method: 'GET' });
    if (!r.ok) throw new Error('HTTP ' + r.status);

    server.setServerUrl(url);
    if (import.meta.client && (window as any).electronAPI?.liveplayServer?.setConfig) {
      await (window as any).electronAPI.liveplayServer.setConfig({
        mode: 'remote',
        remoteUrl: url,
      });
    }
    void rememberServer(url);
    // .lpa double-click: skip the project picker, go straight to extraction.
    if (importAfterConnect.value) { beginImportDestination(); return; }
    // Multi-client: if the remote server is already running a project,
    // join the live session directly instead of showing New/Open.
    if (await tryRejoinExistingProject()) return;
    stage.value = 'project';
  } catch (e: any) {
    console.warn('[welcome] remote connect failed:', e);
    connectionError.value = t('welcome.connectionFailed') + ' (' + (e?.message ?? e) + ')';
  } finally {
    connecting.value = false;
  }
}

// Click handler on a row in the discovered-servers list. Skips the manual
// URL probe (the beacon proved the server is up) and dives straight into
// the rejoin flow.
async function connectToDiscovered(srv: DiscoveredServer) {
  if (connecting.value) return;
  connecting.value = true;
  connectionError.value = '';
  try {
    const url = srv.url;
    remoteAddress.value = stripScheme(url);
    server.setServerUrl(url);
    if (import.meta.client && (window as any).electronAPI?.liveplayServer?.setConfig) {
      await (window as any).electronAPI.liveplayServer.setConfig({
        mode: 'remote',
        remoteUrl: url,
      });
    }
    void rememberServer(url, { name: srv.name });
    if (importAfterConnect.value) { beginImportDestination(); return; }
    if (await tryRejoinExistingProject()) return;
    stage.value = 'project';
  } catch (e: any) {
    console.warn('[welcome] discovered-server connect failed:', e);
    connectionError.value = t('welcome.connectionFailed') + ' (' + (e?.message ?? e) + ')';
  } finally {
    connecting.value = false;
  }
}

// ---- Discovery + recent-server helpers -------------------------------------

// Persist a freshly-connected server to the recent list, and refresh the ref.
async function rememberServer(url: string, meta?: { name?: string }) {
  try {
    const disc = (window as any).electronAPI?.liveplayDiscovery;
    if (!disc?.recentAdd) return;
    let host = '', port = 4480;
    try { const u = new URL(url); host = u.hostname; port = Number(u.port) || 4480; } catch {}
    const updated = await disc.recentAdd({ url, host, port, name: meta?.name ?? host });
    if (Array.isArray(updated)) recentServers.value = updated;
  } catch {}
}

// Fire a fresh solicitation burst and show a brief scanning spinner.
async function rescan() {
  if (scanning.value) return;
  scanning.value = true;
  try {
    await (window as any).electronAPI?.liveplayDiscovery?.solicit?.();
  } catch {}
  setTimeout(() => { scanning.value = false; }, 1500);
}

// Connect to a remembered server. Like the manual path but pre-filled.
async function connectToRecent(srv: RecentServer) {
  remoteAddress.value = stripScheme(srv.url);
  await connectToRemote();
}

async function forgetRecent(srv: RecentServer) {
  try {
    const disc = (window as any).electronAPI?.liveplayDiscovery;
    const updated = await disc?.recentRemove?.(srv.url);
    if (Array.isArray(updated)) recentServers.value = updated;
  } catch {}
}

// ---- Project pickers -------------------------------------------------------
const handleNewProject = () => {
  pickerIntent.value        = 'new';
  pickerMode.value          = 'directory';
  pickerFilter.value        = 'all';
  pickerFilterOptions.value = ['all'];
  pickerStart.value         = '';
  showPicker.value          = true;
};

const handleOpenProject = () => {
  pickerIntent.value        = 'open';
  pickerMode.value          = 'file';
  pickerFilter.value        = '.liveplay';
  pickerFilterOptions.value = ['.liveplay', 'all'];
  pickerStart.value         = '';
  showPicker.value          = true;
};

const onPickerPick = async (fullPath: string) => {
  showPicker.value = false;
  if (!fullPath) return;

  if (pickerIntent.value === 'new') {
    const projectName = await getProjectName();
    if (!projectName) return;
    const ok = await createNewProject(projectName, fullPath);
    if (!ok) alert('Failed to create project');
  } else {
    const ok = await openProject(fullPath);
    if (!ok) alert('Failed to open project');
  }
};

// Vue-reactive project-name dialog — replaces the old imperative DOM version.
const showNameDialog   = ref(false);
const nameDialogValue  = ref('');
const nameDialogInput  = ref<HTMLInputElement | null>(null);
let   nameDialogResolve: ((v: string | null) => void) | null = null;

const getProjectName = (): Promise<string | null> => {
  nameDialogValue.value = '';
  showNameDialog.value  = true;
  nextTick(() => nameDialogInput.value?.focus());
  return new Promise((resolve) => {
    nameDialogResolve = resolve;
  });
};

function confirmNameDialog() {
  const v = nameDialogValue.value.trim();
  showNameDialog.value = false;
  nameDialogResolve?.(v || null);
  nameDialogResolve = null;
}

function cancelNameDialog() {
  showNameDialog.value = false;
  nameDialogResolve?.(null);
  nameDialogResolve = null;
}

// Listen for menu events
if (import.meta.client && (window as any).electronAPI) {
  (window as any).electronAPI.onMenuNewProject(() => {
    if (stage.value === 'project') handleNewProject();
  });

  (window as any).electronAPI.onMenuOpenProject(() => {
    if (stage.value === 'project') handleOpenProject();
  });
}
</script>

<style scoped>
.welcome-screen {
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(135deg, var(--color-background) 0%, var(--color-surface) 100%);
}

.welcome-container {
  text-align: center;
  max-width: 600px;
  padding: var(--spacing-xxl);
}

.welcome-header {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-lg);
  margin-bottom: var(--spacing-xxl);
}

.welcome-logo {
  width: 80px;
  height: 80px;
  object-fit: contain;
}

.welcome-text {
  text-align: left;
}

.welcome-title {
  font-size: 64px;
  font-weight: 600;
  margin-bottom: var(--spacing-xs);
  color: var(--color-text-primary);
  letter-spacing: -2px;
  line-height: 1;
  display: flex;
  align-items: baseline;
  gap: var(--spacing-sm);
}

.version-badge {
  font-size: 16px;
  font-weight: 400;
  color: var(--color-text-secondary);
  opacity: 0.6;
  letter-spacing: 0;
}

.welcome-subtitle {
  font-size: 20px;
  color: var(--color-text-secondary);
  margin: 0;
}

.welcome-actions {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-md);
}

.welcome-button {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-md);
  padding: var(--spacing-lg) var(--spacing-xl);
  font-size: 18px;
  font-weight: 500;
  background-color: var(--color-surface);
  border: 2px solid var(--color-border);
  border-radius: var(--border-radius-lg);
  transition: all var(--transition-base);

  &:hover {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
    transform: translateY(-2px);
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.1);
  }

  &.primary {
    background-color: var(--color-accent);
    border-color: var(--color-accent);
    color: white;

    &:hover {
      background-color: var(--color-accent-hover);
      border-color: var(--color-accent-hover);
    }
  }
}

.button-icon {
  font-size: 24px;
}

.welcome-stage {
  display: flex;
  flex-direction: column;
  align-items: stretch;
  gap: 12px;
}

.stage-title {
  font-size: 22px;
  margin: 0;
  color: var(--color-text-primary);
}

.stage-subtitle {
  margin: 0 0 8px;
  color: var(--color-text-secondary);
}

.connection-summary {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  justify-content: center;
}
.connection-icon {
  font-size: 18px;
  vertical-align: middle;
}

.link-button {
  background: none;
  border: none;
  color: var(--color-accent);
  text-decoration: underline;
  cursor: pointer;
  font-size: inherit;
}

.welcome-actions {
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin-top: 8px;
}

.welcome-button {
  display: flex;
  align-items: center;
  justify-content: left;
  gap: 12px;
  padding: 14px 20px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 2px;
  cursor: pointer;
  font-size: 15px;
  color: var(--color-text-primary);
  text-align: left;
  transition: background 0.15s, border-color 0.15s;
}
.welcome-button:hover {
  background: var(--color-surface-hover);
  border-color: var(--color-accent);
}
.welcome-button.primary {
  background: var(--color-accent);
  color: white;
  border-color: var(--color-accent);
}
.welcome-button.primary:hover {
  filter: brightness(1.08);
}
.welcome-button:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.button-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

.button-label {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
}
.button-label-line {
  font-weight: 600;
}
.button-label-sub {
  font-size: 12px;
  opacity: 0.8;
}

/* Remote form */
.remote-form {
  display: flex;
  flex-direction: column;
  gap: 8px;
  text-align: left;
}
.remote-field-label {
  font-size: 13px;
  color: var(--color-text-secondary);
}
.remote-field-input {
  padding: 10px 12px;
  border-radius: 6px;
  border: 1px solid var(--color-border);
  background: var(--color-background);
  color: var(--color-text-primary);
  font-size: 14px;
}
.remote-field-input:focus {
  outline: none;
  border-color: var(--color-accent);
}
.remote-error {
  color: #e34c4c;
  font-size: 13px;
  margin: 0;
}
.remote-actions {
  display: flex;
  gap: 12px;
  margin-top: 12px;
  justify-content: flex-end;
}

.discovered-servers {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin: 8px 0 14px;
  padding: 10px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 8px;
}
.discovered-header {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: var(--color-text-secondary);
  text-transform: uppercase;
  letter-spacing: 0.5px;
  padding-bottom: 4px;
}
.discovered-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 12px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  color: var(--color-text-primary);
  text-align: left;
  cursor: pointer;
  transition: background var(--transition-fast), border-color var(--transition-fast);
}
.discovered-row:hover:not(:disabled) {
  background: var(--color-surface-hover);
  border-color: var(--color-accent);
}
.discovered-row:disabled { opacity: 0.5; cursor: default; }
.discovered-icon { color: var(--color-accent); }
.discovered-main { display: flex; flex-direction: column; flex: 1; min-width: 0; }
.discovered-name { font-weight: 600; }
.discovered-meta {
  font-size: 12px;
  color: var(--color-text-secondary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.discovered-project { color: var(--color-accent); }
.discovered-arrow { color: var(--color-text-secondary); }
.discovered-rescan {
  margin-left: auto;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  padding: 2px;
  background: transparent;
  border: none;
  color: var(--color-text-secondary);
  cursor: pointer;
  border-radius: 4px;
}
.discovered-rescan:hover:not(:disabled) { color: var(--color-accent); background: var(--color-surface-hover); }
.discovered-rescan:disabled { opacity: 0.5; cursor: default; }
.discovered-rescan .material-symbols-rounded { font-size: 18px; }
.discovered-empty {
  font-size: 12px;
  color: var(--color-text-secondary);
  padding: 4px 2px;
  margin: 0;
}
.discovered-remove {
  color: var(--color-text-secondary);
  font-size: 18px;
  border-radius: 4px;
  padding: 2px;
}
.discovered-remove:hover { color: var(--color-danger, #e5534b); background: var(--color-surface-hover); }

@keyframes lp-spin { to { transform: rotate(360deg); } }
.spin { display: inline-block; animation: lp-spin 0.85s linear infinite; }
</style>

/* New-project name dialog — unscoped due to Teleport to body */
<style>
.name-dialog-backdrop {
  position: fixed;
  inset: 0;
  z-index: 9200;
  background: rgba(0, 0, 0, 0.6);
  display: flex;
  align-items: center;
  justify-content: center;
}

.name-dialog {
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-lg);
  padding: var(--spacing-xl);
  min-width: 360px;
  max-width: 480px;
  width: 90vw;
  display: flex;
  flex-direction: column;
  gap: var(--spacing-md);
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.4);
}

.name-dialog__title {
  margin: 0;
  font-size: 16px;
  font-weight: 600;
  color: var(--color-text-primary);
}

.name-dialog__input {
  padding: 10px 12px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  color: var(--color-text-primary);
  font-size: 14px;
  outline: none;
  transition: border-color var(--transition-fast);
}

.name-dialog__input:focus {
  border-color: var(--color-accent);
}

.name-dialog__actions {
  display: flex;
  justify-content: flex-end;
  gap: var(--spacing-sm);
}

.name-dialog__btn {
  padding: 8px 20px;
  border-radius: var(--border-radius-md);
  font-size: 14px;
  font-weight: 500;
  cursor: pointer;
  border: 1px solid var(--color-border);
  background: var(--color-background);
  color: var(--color-text-primary);
  transition: background var(--transition-fast), border-color var(--transition-fast);
}

.name-dialog__btn:hover:not(:disabled) {
  background: var(--color-surface-hover);
  border-color: var(--color-accent);
}

.name-dialog__btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.name-dialog__btn--primary {
  background: var(--color-accent);
  border-color: var(--color-accent);
  color: #fff;
}

.name-dialog__btn--primary:hover:not(:disabled) {
  filter: brightness(1.1);
  background: var(--color-accent);
  border-color: var(--color-accent);
}
</style>
