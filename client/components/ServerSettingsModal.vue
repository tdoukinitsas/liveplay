<template>
  <Teleport to="body">
    <div v-if="open" class="modal-backdrop" @click.self="close">
      <div class="modal">
        <header>
          <h2>LivePlay Server</h2>
          <button class="x" @click="close">✕</button>
        </header>

        <p class="status" :class="{ ok: server.connected, bad: !server.connected }">
          <span v-if="server.connected">● Connected</span>
          <span v-else-if="server.reconnecting">● Reconnecting…</span>
          <span v-else>● Disconnected</span>
          <span v-if="server.lastError" class="err">  ({{ server.lastError }})</span>
        </p>

        <!-- Mode selector: Local (Electron spawns the C++ server) vs Remote -->
        <div class="mode-group">
          <label class="mode-option">
            <input type="radio" value="local" v-model="draftMode" />
            <div>
              <strong>Local</strong>
              <small>App starts its own audio engine. Best for solo operators.</small>
            </div>
          </label>
          <label class="mode-option">
            <input type="radio" value="remote" v-model="draftMode" />
            <div>
              <strong>Remote</strong>
              <small>Connect to a LivePlay server running on another machine.</small>
            </div>
          </label>
        </div>

        <label v-if="draftMode === 'local'">
          Local port
          <input v-model.number="draftLocalPort" type="number" min="1" max="65535" placeholder="4480" />
        </label>
        <label v-else>
          Remote URL
          <input v-model="draftRemoteUrl" placeholder="http://192.168.1.42:4480" />
        </label>

        <p v-if="serverStatus" class="server-pid">
          <template v-if="draftMode === 'local'">
            <span v-if="serverStatus.running">Engine running (pid {{ serverStatus.pid }})</span>
            <span v-else class="warn">Engine not running — Apply to start it</span>
          </template>
          <template v-else>
            <span class="hint">Targeting external server. Make sure the remote URL is reachable.</span>
          </template>
        </p>

        <div class="row">
          <button class="btn primary" @click="apply">Apply</button>
          <button class="btn" @click="server.connect">Retry connect</button>
          <button v-if="draftMode === 'local' && hasElectron" class="btn" @click="restartLocal">Restart engine</button>
        </div>

        <section v-if="server.connected">
          <h3>Output devices</h3>
          <ul class="devices">
            <li v-for="d in server.devices" :key="d.id">
              <span :class="{ default: d.is_default }">{{ d.display_name }}</span>
              <small>{{ d.channel_count }} ch @ {{ d.sample_rate }} Hz</small>
              <button class="btn small" @click="server.openDevice(d.display_name, d.channel_count)">Open</button>
            </li>
            <li v-if="server.devices.length === 0" class="empty">(no devices yet)</li>
          </ul>
        </section>
      </div>
    </div>
  </Teleport>
</template>

<!--
  ServerSettingsModal.vue
  -----------------------------------------------------------------------
  Configures the C++ server URL the client talks to, shows connection
  health, lets the operator open hardware devices on the server side.
-->
<script setup lang="ts">
import { ref, watch, onMounted, onBeforeUnmount } from 'vue';
import { useLiveplayServer } from '~/composables/useLiveplayServer';

const props = defineProps<{ open: boolean }>();
const emit  = defineEmits<{ (e: 'close'): void }>();

const server = useLiveplayServer();

// Local/Remote configuration is owned by the Electron main process (it
// also spawns the child server when mode === 'local'). In a pure-web
// context (`electronAPI` undefined), Local mode is hidden because the
// browser can't spawn binaries — we fall through to Remote-only.
const electronApi: any = (globalThis as any).electronAPI?.liveplayServer;
const hasElectron = !!electronApi;

const draftMode      = ref<'local' | 'remote'>('local');
const draftRemoteUrl = ref('http://127.0.0.1:4480');
const draftLocalPort = ref(4480);
const serverStatus   = ref<{ running: boolean; pid?: number } | null>(null);

let stopStatusListener: (() => void) | null = null;

async function loadConfig() {
  if (!electronApi) {
    // Web fallback: only remote mode is meaningful.
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

onMounted(() => {
  loadConfig();
  if (electronApi) {
    stopStatusListener = electronApi.onStateChange((p: any) => {
      serverStatus.value = { running: p.running, pid: p.pid };
    });
  }
});

onBeforeUnmount(() => { if (stopStatusListener) stopStatusListener(); });

watch(() => props.open, o => {
  if (o) {
    loadConfig();
    server.fetchDevices();
  }
});

async function apply() {
  if (electronApi) {
    // Main process persists the choice and starts/stops the child as needed.
    // The plugin's onStateChange listener will retarget the WebSocket.
    await electronApi.setConfig({
      mode:      draftMode.value,
      remoteUrl: draftRemoteUrl.value.trim(),
      localPort: draftLocalPort.value,
    });
  } else {
    // Web fallback: just point the client at the typed URL.
    server.setServerUrl(draftRemoteUrl.value.trim());
  }
}

async function restartLocal() {
  if (electronApi) await electronApi.restart();
}

function close() { emit('close'); }
</script>

<style lang="scss" scoped>
.modal-backdrop {
  position: fixed; inset: 0;
  background: rgba(0, 0, 0, 0.6);
  display: flex; align-items: center; justify-content: center;
  z-index: 9000;
}
.modal {
  width: min(520px, 90vw);
  background: #1a1a1a;
  border: 1px solid #2a2a2a;
  border-radius: 8px;
  padding: 20px;
  color: #ddd;
  display: flex; flex-direction: column; gap: 12px;

  header { display: flex; justify-content: space-between; align-items: center; }
  h2 { margin: 0; font-size: 18px; }
  .x { background: transparent; border: none; color: #aaa; cursor: pointer; font-size: 18px; }
  .status { font-family: monospace; font-size: 12px;
    &.ok  { color: #6ad48d; }
    &.bad { color: #ff8080; }
    .err { color: #ff8080; }
  }
  label { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: #aaa; }
  input {
    background: #1d1d1d; border: 1px solid #333;
    color: #eee; padding: 6px 10px; border-radius: 4px;
    font-family: monospace;
  }
  .mode-group {
    display: grid; gap: 6px;
    border: 1px solid #2a2a2a; border-radius: 6px;
    padding: 6px; background: #161616;
  }
  .mode-option {
    display: flex; align-items: flex-start; gap: 10px;
    padding: 8px; border-radius: 4px; cursor: pointer;
    color: #ddd; font-size: 13px;
    &:hover { background: #1d1d1d; }
    input[type="radio"] { margin-top: 4px; }
    div { display: flex; flex-direction: column; gap: 2px; }
    strong { font-size: 13px; }
    small { font-size: 11px; color: #888; }
  }
  .server-pid {
    font-size: 11px; color: #aaa; margin: 0;
    .warn { color: #ffd58a; }
    .hint { color: #9ec5ff; }
  }
  .row { display: flex; gap: 8px; flex-wrap: wrap; }
  .btn {
    background: #2a2a2a; border: 1px solid #3a3a3a;
    border-radius: 4px; padding: 6px 12px; color: #ddd; cursor: pointer;
    &:hover:not(:disabled) { background: #353535; }
    &.primary { background: #2a5e9a; border-color: #2a5e9a; }
    &.small   { padding: 2px 8px; font-size: 12px; }
  }
  h3 { margin: 8px 0 4px; font-size: 13px; color: #9ec5ff; }
  .devices {
    list-style: none; padding: 0; margin: 0; max-height: 200px; overflow: auto;
    border: 1px solid #2a2a2a; border-radius: 4px; background: #161616;
    li {
      display: grid; grid-template-columns: 1fr auto auto;
      gap: 8px; align-items: center; padding: 6px 10px;
      border-bottom: 1px solid #222;
      &:last-child { border-bottom: none; }
      small { color: #888; font-size: 11px; }
      .default { color: #ffd58a; }
    }
    .empty { color: #888; padding: 12px; text-align: center; font-style: italic; }
  }
}
</style>
