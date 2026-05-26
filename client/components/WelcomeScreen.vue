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

        <!-- Auto-discovered servers on this LAN. Populated by the UDP beacon. -->
        <div v-if="discoveredServers.length > 0" class="discovered-servers">
          <div class="discovered-header">
            <span class="material-symbols-rounded">radar</span>
            <span>{{ t('welcome.serversOnThisNetwork') }}</span>
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

  // Always go through the mode-picker so the user is in control of the
  // current session's server target. We could skip if connected, but the
  // first-impression UI value of a deliberate choice outweighs the click.
  stage.value = 'mode';

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
    }
  } catch (e) {
    console.warn('[welcome] discovery start failed:', e);
  }
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

async function chooseLocal() {
  mode.value = 'local';
  connectionError.value = '';
  connecting.value = true;
  try {
    const api = (window as any).electronAPI?.liveplayServer;
    if (import.meta.client && api?.setConfig) {
      const cfg = await api.setConfig({ mode: 'local' });
      const url = `http://127.0.0.1:${cfg.localPort ?? 4480}`;
      server.setServerUrl(url);
      // Spawn (or reattach to) the server, then wait until /api/health
      // answers so the WS connect below doesn't race the bind.
      if (api.ensureRunning) {
        const res = await api.ensureRunning();
        if (!res?.ok) {
          connectionError.value = res?.error
            ? `Local server failed to start: ${res.error}`
            : 'Local server failed to start';
          return;
        }
      }
    } else {
      server.setServerUrl('http://127.0.0.1:4480');
    }
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
    if (await tryRejoinExistingProject()) return;
    stage.value = 'project';
  } catch (e: any) {
    console.warn('[welcome] discovered-server connect failed:', e);
    connectionError.value = t('welcome.connectionFailed') + ' (' + (e?.message ?? e) + ')';
  } finally {
    connecting.value = false;
  }
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
  pickerFilter.value        = '.liveplay,.lpa';
  pickerFilterOptions.value = ['.liveplay,.lpa', 'all'];
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

// Simple inline project name dialog
const getProjectName = (): Promise<string | null> => {
  return new Promise((resolve) => {
    const overlay = document.createElement('div');
    overlay.className = 'modal-overlay';

    const dialog = document.createElement('div');
    dialog.className = 'modal-dialog';

    const h3 = document.createElement('h3');
    h3.textContent = t('project.enterName');
    h3.className = 'modal-title';

    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'modal-input';
    input.placeholder = t('project.placeholder');

    const buttonContainer = document.createElement('div');
    buttonContainer.className = 'modal-buttons';

    const cancelBtn = document.createElement('button');
    cancelBtn.className = 'modal-btn modal-btn-cancel';
    cancelBtn.textContent = t('project.cancel');

    const okBtn = document.createElement('button');
    okBtn.className = 'modal-btn modal-btn-primary';
    okBtn.textContent = t('project.ok');

    buttonContainer.appendChild(cancelBtn);
    buttonContainer.appendChild(okBtn);

    dialog.appendChild(h3);
    dialog.appendChild(input);
    dialog.appendChild(buttonContainer);
    overlay.appendChild(dialog);

    const appElement = document.getElementById('app') || document.body;
    appElement.appendChild(overlay);

    input.focus();

    const cleanup = () => {
      appElement.removeChild(overlay);
    };

    okBtn.onclick = () => {
      const value = input.value.trim();
      cleanup();
      resolve(value || null);
    };

    cancelBtn.onclick = () => {
      cleanup();
      resolve(null);
    };

    input.onkeydown = (e) => {
      if (e.key === 'Enter') {
        okBtn.click();
      } else if (e.key === 'Escape') {
        cancelBtn.click();
      }
    };
  });
};

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

@keyframes lp-spin { to { transform: rotate(360deg); } }
.spin { display: inline-block; animation: lp-spin 0.85s linear infinite; }
</style>
