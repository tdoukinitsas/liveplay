<template>
  <transition name="fade">
    <div v-if="visible" class="clm-overlay">
      <div class="clm-dialog">
        <div class="clm-header">
          <span class="material-symbols-rounded clm-icon">cloud_off</span>
          <h2>Connection lost</h2>
        </div>
        <p class="clm-body">
          The connection to the LivePlay server at
          <code>{{ server.serverUrl }}</code>
          has been lost.
          <span v-if="reconnecting">Attempting to reconnect…</span>
        </p>
        <p v-if="server.lastError" class="clm-error">{{ server.lastError }}</p>

        <div class="clm-actions">
          <button class="clm-btn primary" :disabled="reconnecting" @click="onReconnect">
            <span class="material-symbols-rounded">sync</span>
            <span>Reconnect</span>
          </button>
          <button class="clm-btn" @click="onRestart">
            <span class="material-symbols-rounded">restart_alt</span>
            <span>Restart</span>
          </button>
          <button class="clm-btn" @click="onExit">
            <span class="material-symbols-rounded">logout</span>
            <span>Exit</span>
          </button>
        </div>
        <p class="clm-hint">
          Reconnect tries the same server again.
          Restart relaunches just the client (the audio server keeps running).
          Exit quits the client.
        </p>
      </div>
    </div>
  </transition>
</template>

<script setup lang="ts">
const server = useLiveplayServer();
const visible = computed(() => !!server.connectionLost);
const reconnecting = computed(() => !!server.reconnecting);

function onReconnect() {
  try {
    server.forceReconnect();
  } catch (e) {
    console.warn('[ConnectionLostModal] reconnect failed:', e);
  }
}

async function onRestart() {
  try {
    await (window as any).electronAPI?.app?.relaunch?.();
  } catch (e) {
    console.warn('[ConnectionLostModal] relaunch failed:', e);
    // Browser fallback: hard reload.
    if (typeof window !== 'undefined') window.location.reload();
  }
}

async function onExit() {
  try {
    await (window as any).electronAPI?.app?.exit?.();
  } catch (e) {
    console.warn('[ConnectionLostModal] exit failed:', e);
    if (typeof window !== 'undefined') window.close();
  }
}
</script>

<style scoped>
.clm-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 2000;
}
.clm-dialog {
  background: var(--color-surface);
  color: var(--color-text-primary);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  padding: 24px 28px;
  max-width: 480px;
  width: 90%;
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
}
.clm-header {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 8px;
}
.clm-header h2 { margin: 0; font-size: 18px; }
.clm-icon { color: #da1e28; font-size: 28px; }
.clm-body {
  color: var(--color-text-secondary);
  font-size: 14px;
  line-height: 1.5;
}
.clm-body code {
  background: var(--color-background);
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 12px;
  color: var(--color-text-primary);
}
.clm-error {
  font-size: 12px;
  color: #da1e28;
  background: rgba(218, 30, 40, 0.1);
  padding: 8px 10px;
  border-radius: 6px;
  border: 1px solid rgba(218, 30, 40, 0.3);
}
.clm-actions {
  display: flex;
  gap: 8px;
  margin-top: 18px;
}
.clm-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  flex: 1;
  justify-content: center;
  padding: 10px 12px;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-background);
  color: var(--color-text-primary);
  font-size: 13px;
  cursor: pointer;
}
.clm-btn:hover:not(:disabled) {
  background: var(--color-surface-hover);
  border-color: var(--color-accent);
}
.clm-btn.primary {
  background: var(--color-accent, #0f62fe);
  color: #fff;
  border-color: transparent;
}
.clm-btn.primary:hover:not(:disabled) { filter: brightness(1.1); }
.clm-btn:disabled { opacity: 0.5; cursor: default; }
.clm-btn .material-symbols-rounded { font-size: 16px; }
.clm-hint {
  font-size: 11px;
  color: var(--color-text-secondary);
  margin-top: 12px;
  line-height: 1.4;
}

.fade-enter-active, .fade-leave-active { transition: opacity 0.15s ease; }
.fade-enter-from, .fade-leave-to { opacity: 0; }
</style>
