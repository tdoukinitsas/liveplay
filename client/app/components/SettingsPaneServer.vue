<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionServer') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionServerHelp') }}</p>

    <!-- ================================================================
         1. This client's connection.
         Electron's own config, NOT liveplay.json: which server this app
         talks to is a property of the installation in front of you, it
         has to be changeable while disconnected, and userData is writable
         where the directory beside the server binary may not be.
         ================================================================ -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">lan</span>
        {{ t('serverSettings.title') }}
      </label>
      <p class="settings-status" :class="statusClass">
        <span v-if="server.connected">{{ t('serverSettings.connected') }}</span>
        <span v-else-if="server.reconnecting">{{ t('serverSettings.reconnecting') }}</span>
        <span v-else>{{ t('serverSettings.disconnected') }}</span>
      </p>

      <div v-if="hasElectron" class="settings-modes">
        <label class="settings-label settings-label--checkbox">
          <input type="radio" value="local" v-model="draftMode" />
          {{ t('serverSettings.localModeDesc') }}
        </label>
        <label class="settings-label settings-label--checkbox">
          <input type="radio" value="remote" v-model="draftMode" />
          {{ t('serverSettings.remoteModeDesc') }}
        </label>
      </div>

      <template v-if="hasElectron && draftMode === 'local'">
        <label class="settings-label">{{ t('serverSettings.localPort') }}</label>
        <input class="settings-input" type="number" min="1" max="65535"
               v-model.number="draftLocalPort" />
        <p class="settings-help">
          <span v-if="serverStatus?.running">
            {{ t('serverSettings.engineRunning', { pid: serverStatus.pid }) }}
          </span>
          <span v-else>{{ t('serverSettings.engineNotRunning') }}</span>
        </p>
      </template>
      <template v-else>
        <label class="settings-label">{{ t('serverSettings.remoteUrl') }}</label>
        <input class="settings-input" type="text" v-model="draftRemoteUrl" />
        <p class="settings-help">{{ t('serverSettings.externalHint') }}</p>
      </template>

      <div class="settings-actions">
        <button class="settings-btn settings-btn--primary" @click="applyConnection">
          {{ t('serverSettings.apply') }}
        </button>
        <button class="settings-btn" @click="server.connect()">
          {{ t('serverSettings.retryConnect') }}
        </button>
        <button v-if="hasElectron && draftMode === 'local'"
                class="settings-btn" @click="restartLocal">
          {{ t('serverSettings.restartEngine') }}
        </button>
      </div>
    </section>

    <!-- ================================================================
         2. This connection's meter rate (U2's set_meter_hz, which has had
         a tested server side and no control since it shipped).
         Per-connection on purpose: the desk can run at 60 Hz without every
         tablet on the Wi-Fi paying for it.
         ================================================================ -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">speed</span>
        {{ t('serverConfig.meterRate') }}
      </label>
      <select class="settings-select" :value="meterHz" @change="onMeterHzChange">
        <option :value="0">{{ t('serverConfig.meterRateAuto') }}</option>
        <option v-for="hz in METER_RATES" :key="hz" :value="hz">{{ hz }} Hz</option>
      </select>
      <p class="settings-help">{{ t('serverConfig.meterRateHelp') }}</p>
    </section>

    <!-- ================================================================
         3. The machine's own configuration (P3a).
         ================================================================ -->
    <h4 class="settings-group-title">{{ t('serverConfig.title') }}</h4>

    <p v-if="configError" class="settings-help settings-help--warn">{{ configError }}</p>

    <template v-else-if="config">
      <p class="settings-pane-intro">
        {{ t('serverConfig.intro') }}
        <code class="settings-path">{{ config.path }}</code>
      </p>

      <p v-if="config.locked" class="settings-notice settings-notice--locked">
        <span class="material-symbols-rounded">lock</span>
        {{ t('serverConfig.locked') }}
      </p>

      <section v-for="f in config.fields" :key="f.key" class="settings-field">
        <label class="settings-label">
          {{ fieldLabel(f) }}
          <span v-if="f.policy" class="settings-badge settings-badge--policy">
            {{ t('serverConfig.policyField') }}
          </span>
          <span v-if="f.overridden" class="settings-badge settings-badge--warn">
            {{ t(f.source === 'cli' ? 'serverConfig.sourceCli' : 'serverConfig.sourceEnv') }}
          </span>
        </label>

        <!-- fsRoots is a list. One path per line is the only editor that does
             not need its own component, and a newline cannot appear in a path
             on either platform, so it is a safe separator where a comma or a
             semicolon would not be. -->
        <textarea
          v-if="f.type === 'pathList'"
          class="settings-input settings-input--multiline"
          rows="3"
          :disabled="isReadOnly(f)"
          :placeholder="t('serverConfig.unrestricted')"
          :value="pathListText(f)"
          @change="onFieldChange(f, ($event.target as HTMLTextAreaElement).value)"
        ></textarea>

        <label v-else-if="f.type === 'bool'" class="settings-label settings-label--checkbox">
          <input
            type="checkbox"
            :disabled="isReadOnly(f)"
            :checked="draftOf(f) === true"
            @change="onFieldChange(f, ($event.target as HTMLInputElement).checked)"
          />
          {{ t('serverConfig.enabled') }}
        </label>

        <input
          v-else
          class="settings-input"
          :type="f.type === 'text' ? 'text' : 'number'"
          :min="f.min" :max="f.max"
          :step="f.type === 'real' ? 0.1 : 1"
          :disabled="isReadOnly(f)"
          :value="draftOf(f)"
          @change="onFieldChange(f, ($event.target as HTMLInputElement).value)"
        />

        <!-- The server's own help text, untranslated on purpose: it is the same
             string `--help` prints, and it describes flags that are themselves
             English. Translating a copy of it here would be a second owner for
             the wording that could drift from the one the console shows. -->
        <p class="settings-help">{{ f.help }}</p>
        <p class="settings-help settings-help--muted">
          {{ t('serverConfig.inForce') }}: <strong>{{ displayValue(f.value) }}</strong>
          · {{ t('serverConfig.setBy') }}: {{ t(SOURCE_KEYS[f.source] ?? 'serverConfig.sourceDefault') }}
          <template v-if="f.overridden">
            — {{ t('serverConfig.overriddenHelp') }}
          </template>
        </p>
      </section>

      <div v-if="!config.locked" class="settings-actions settings-actions--sticky">
        <button
          class="settings-btn settings-btn--primary"
          :disabled="!isDirty || saving"
          @click="save"
        >{{ saving ? t('serverConfig.saving') : t('serverConfig.save') }}</button>
        <button class="settings-btn" :disabled="!isDirty || saving" @click="resetDrafts">
          {{ t('serverConfig.discard') }}
        </button>
        <span v-if="savedNote" class="settings-saved">{{ savedNote }}</span>
      </div>
    </template>

    <p v-else class="settings-help">{{ t('serverConfig.loading') }}</p>
  </div>
</template>

<script setup lang="ts">
// The Server pane (P3b).
//
// Two different owners live on this page, and keeping them apart is the point
// rather than an implementation detail:
//
//   * WHICH server this app talks to, and whether it starts one — Electron's
//     own config, via the liveplay-server:* IPC. It has to work while
//     disconnected (it is how you fix being disconnected), and it is written to
//     userData, which is writable where the directory beside the server binary
//     may not be on a packaged install.
//
//   * WHAT THAT SERVER DOES once reached — liveplay.json, via
//     GET/PATCH /api/server/config. Administrators only, and refused outright
//     when the machine was started with --lock-server-config.
//
// This replaces ServerSettingsModal.vue, which was unreachable: nothing in the
// app rendered it, so none of what follows was a shipped feature anybody could
// reach. Every control it had is here except one — the output-device list, with
// its raw "open this device" button. That is not dropped, it is going to the
// Outputs pane in P3c, where hardware belongs now that buses and the output map
// exist; the serverSettings.outputDevices / open / noDevices keys stay in the
// locales for it rather than being removed and re-added.
const { t } = useLocalization();
const server = useLiveplayServer();

const electronApi: any = (globalThis as any).electronAPI?.liveplayServer;
const hasElectron = !!electronApi;

// ---- 1. Connection ---------------------------------------------------
const draftMode      = ref<'local' | 'remote'>('local');
const draftRemoteUrl = ref('http://127.0.0.1:4480');
const draftLocalPort = ref(4480);
const serverStatus   = ref<{ running: boolean; pid?: number } | null>(null);
let stopStatusListener: (() => void) | null = null;

const statusClass = computed(() => ({
  'is-up':   server.connected,
  'is-busy': !server.connected && server.reconnecting,
}));

async function loadConnection() {
  if (!electronApi) {
    // A browser cannot spawn a binary, so only remote mode is meaningful.
    draftMode.value = 'remote';
    draftRemoteUrl.value = server.serverUrl;
    return;
  }
  const cfg    = await electronApi.getConfig();
  const status = await electronApi.getStatus();
  draftMode.value      = cfg.mode;
  draftRemoteUrl.value = cfg.remoteUrl || 'http://127.0.0.1:4480';
  draftLocalPort.value = cfg.localPort || 4480;
  serverStatus.value   = { running: status.running, pid: status.pid };
}

async function applyConnection() {
  if (electronApi) {
    await electronApi.setConfig({
      mode:      draftMode.value,
      remoteUrl: draftRemoteUrl.value.trim(),
      localPort: draftLocalPort.value,
    });
  } else {
    server.setServerUrl(draftRemoteUrl.value.trim());
  }
}
async function restartLocal() { if (electronApi) await electronApi.restart(); }

// ---- 2. This connection's meter rate ---------------------------------
// Offered as a short list rather than a free number: the useful values are the
// ones that trade smoothness against a tablet's Wi-Fi, and the server clamps to
// its own tick rate anyway, so a box accepting 1000 would only invite a number
// that silently becomes 30.
const METER_RATES = [10, 15, 20, 30, 60];
// Read from the client, not from a local draft: the server replies with what it
// actually granted after clamping to its own tick rate, so the control tracks
// what is happening rather than what was asked for.
const meterHz = computed(() => server.meterHz);
function onMeterHzChange(e: Event) {
  server.setMeterHz(Number((e.target as HTMLSelectElement).value) || 0);
}

// ---- 3. Machine configuration ----------------------------------------
const config     = ref<any | null>(null);
const configError = ref<string | null>(null);
const drafts     = ref<Record<string, any>>({});
const saving     = ref(false);
const savedNote  = ref('');

const SOURCE_KEYS: Record<string, string> = {
  default: 'serverConfig.sourceDefault',
  file:    'serverConfig.sourceFile',
  env:     'serverConfig.sourceEnv',
  cli:     'serverConfig.sourceCli',
};

// The server ships an English `help` for every field but no label — the label
// is presentation, so it is translated here, keyed by the field name the
// schema already owns. An unknown key falls back to the key itself rather than
// rendering blank, so a server one version ahead shows a usable form.
function fieldLabel(f: any): string {
  const key = `serverConfig.field.${f.key}`;
  const label = t(key);
  return label === key ? f.key : label;
}

// A field is read-only when the machine is locked, or when the environment or a
// flag is supplying the value. The second is not really read-only on the server
// — the write would succeed — but offering an edit that cannot take effect
// until someone removes a flag they cannot see from here is worse than not
// offering it. The badge and the help line say which.
function isReadOnly(f: any): boolean {
  return !!config.value?.locked || !!f.overridden;
}

const draftOf = (f: any) => (f.key in drafts.value ? drafts.value[f.key] : f.stored ?? f.value);

const pathListText = (f: any) => {
  const v = draftOf(f);
  return Array.isArray(v) ? v.join('\n') : '';
};

function onFieldChange(f: any, raw: any) {
  let next: any = raw;
  if (f.type === 'int')      next = raw === '' ? null : Math.round(Number(raw));
  else if (f.type === 'real') next = raw === '' ? null : Number(raw);
  else if (f.type === 'pathList') {
    const lines = String(raw).split('\n').map(s => s.trim()).filter(Boolean);
    // An empty list means "unrestricted" on the server, and it refuses [] as a
    // value. Clearing the box is therefore a clear, not a store.
    next = lines.length ? lines : null;
  } else if (f.type === 'text') {
    next = String(raw).trim() || null;
  }
  drafts.value = { ...drafts.value, [f.key]: next };
}

const isDirty = computed(() => Object.keys(drafts.value).length > 0);
function resetDrafts() { drafts.value = {}; savedNote.value = ''; }

const displayValue = (v: any) => {
  if (v === null || v === undefined) return '—';
  if (Array.isArray(v)) return v.length ? v.join(', ') : t('serverConfig.unrestricted');
  if (typeof v === 'boolean') return v ? t('serverConfig.on') : t('serverConfig.off');
  return String(v);
};

async function load() {
  configError.value = null;
  try {
    config.value = await server.fetchServerConfig();
    drafts.value = {};
  } catch (e: any) {
    // 403 is the ordinary answer for an operator, and 401 for a client whose
    // token has gone. Neither is a fault to report as one.
    const msg = String(e?.message ?? e);
    configError.value = /40[13]/.test(msg)
      ? t('serverConfig.adminOnly')
      : t('serverConfig.unavailable');
    config.value = null;
  }
}

async function save() {
  if (!isDirty.value) return;
  saving.value = true;
  savedNote.value = '';
  try {
    const res = await server.patchServerConfig(drafts.value);
    // What was written but is not yet in force. Two distinct reasons, and the
    // server reports them separately because they have different fixes: a
    // restart clears one and will never clear the other.
    const parts: string[] = [];
    if (res?.restartRequired?.length) {
      parts.push(t('serverConfig.restartRequired', { keys: res.restartRequired.join(', ') }));
    }
    if (res?.overriddenAtLaunch?.length) {
      parts.push(t('serverConfig.savedButOverridden',
                   { keys: res.overriddenAtLaunch.join(', ') }));
    }
    if (res?.dropped?.length) parts.push(t('serverConfig.dropped', { list: res.dropped.join('; ') }));
    savedNote.value = parts.length ? parts.join(' ') : t('serverConfig.saved');
    await load();
  } catch (e: any) {
    savedNote.value = /403/.test(String(e?.message ?? e))
      ? t('serverConfig.locked')
      : t('serverConfig.saveFailed');
  } finally {
    saving.value = false;
  }
}

onMounted(() => {
  loadConnection();
  if (electronApi) {
    stopStatusListener = electronApi.onStateChange((p: any) => {
      serverStatus.value = { running: p.running, pid: p.pid };
    });
  }
  void load();
});
onBeforeUnmount(() => { if (stopStatusListener) stopStatusListener(); });

// A reconnect can be a different server, or the same one after somebody signed
// in — either way the configuration on screen may no longer be this machine's.
watch(() => server.connected, (up) => { if (up) void load(); });
</script>

<style lang="scss" scoped>
.settings-group-title {
  margin: 32px 0 4px;
  font-size: 15px;
  font-weight: 600;
  padding-top: 20px;
  border-top: 1px solid var(--color-border, #333);
}
.settings-path {
  font-family: monospace;
  font-size: 12px;
  opacity: 0.75;
  word-break: break-all;
}
.settings-status {
  margin: 4px 0 12px;
  font-size: 13px;
  opacity: 0.7;
  &.is-up   { color: var(--color-success, #00e676); opacity: 1; }
  &.is-busy { color: var(--color-warning, #ffc400); opacity: 1; }
}
.settings-modes { display: flex; flex-direction: column; gap: 6px; margin-bottom: 12px; }
.settings-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
  margin-top: 12px;
}
.settings-actions--sticky {
  position: sticky;
  bottom: 0;
  padding: 12px 0;
  background: var(--color-bg, #1a1a1a);
  border-top: 1px solid var(--color-border, #333);
}
.settings-btn {
  padding: 6px 14px;
  border-radius: 4px;
  border: 1px solid var(--color-border, #444);
  background: transparent;
  color: inherit;
  cursor: pointer;
  font-size: 13px;
  &:disabled { opacity: 0.45; cursor: default; }
  &--primary {
    background: var(--color-accent-custom, #DA1E28);
    border-color: transparent;
    color: #fff;
  }
}
.settings-saved { font-size: 12px; opacity: 0.8; }
.settings-badge {
  margin-left: 8px;
  padding: 1px 6px;
  border-radius: 3px;
  font-size: 10px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  vertical-align: middle;
  &--policy { background: rgba(218, 30, 40, 0.15); color: var(--color-accent-custom, #DA1E28); }
  &--warn   { background: rgba(255, 196, 0, 0.15); color: #ffc400; }
}
.settings-notice {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 12px;
  border-radius: 4px;
  font-size: 13px;
  margin: 8px 0 16px;
  &--locked { background: rgba(255, 196, 0, 0.1); color: #ffc400; }
}
.settings-input--multiline { font-family: monospace; resize: vertical; }
.settings-help--muted { opacity: 0.6; font-size: 12px; }
.settings-help--warn  { color: #ffc400; }
</style>
