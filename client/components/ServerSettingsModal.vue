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

        <label>
          Server URL
          <input v-model="draftUrl" placeholder="http://127.0.0.1:4480" />
        </label>
        <div class="row">
          <button class="btn primary" @click="apply">Apply &amp; reconnect</button>
          <button class="btn" @click="server.connect">Retry connect</button>
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
import { ref, watch } from 'vue';
import { useLiveplayServer } from '~/composables/useLiveplayServer';

const props = defineProps<{ open: boolean }>();
const emit  = defineEmits<{ (e: 'close'): void }>();

const server   = useLiveplayServer();
const draftUrl = ref(server.serverUrl);

watch(() => props.open, o => {
  if (o) {
    draftUrl.value = server.serverUrl;
    server.fetchDevices();
  }
});

function apply() {
  server.setServerUrl(draftUrl.value.trim());
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
  .row { display: flex; gap: 8px; }
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
