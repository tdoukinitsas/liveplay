<template>
  <div v-if="visible" class="local-server-status" :class="{ stopping }">
    <span class="lss-dot" :class="{ running: status.running, stopped: !status.running }"></span>
    <div class="lss-meta">
      <div class="lss-title">{{ status.running ? 'Local server running' : 'Local server stopped' }}</div>
      <div class="lss-sub">
        <span v-if="status.running">PID {{ status.pid }} · :{{ status.config?.localPort ?? '?' }}</span>
        <span v-else>Spawn a new one or switch to Remote</span>
      </div>
    </div>
    <div class="lss-actions">
      <button
        v-if="status.running"
        class="lss-btn"
        :disabled="stopping"
        @click="onStop"
        title="Stop the detached liveplay-server process"
      >
        <span class="material-symbols-rounded">stop_circle</span>
        Stop
      </button>
      <button
        v-else
        class="lss-btn"
        @click="onStart"
        title="Start the local liveplay-server"
      >
        <span class="material-symbols-rounded">play_arrow</span>
        Start
      </button>
      <button class="lss-btn-icon" @click="onRestart" title="Restart server">
        <span class="material-symbols-rounded">restart_alt</span>
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
// Small corner pill that surfaces the detached liveplay-server so the user
// can actually see (and quit) the process they spawned. In remote mode the
// pill stays hidden — the server isn't owned by this client.
const status = ref<{ running: boolean; pid?: number; config?: { mode?: string; localPort?: number } }>(
  { running: false, config: { mode: 'local' } },
);
const stopping = ref(false);

const visible = computed(() => status.value.config?.mode === 'local');

async function refresh() {
  try {
    const api = (window as any).electronAPI?.liveplayServer;
    if (!api) return;
    status.value = await api.getStatus();
  } catch (e) {
    // Non-fatal — IPC unavailable in browser-only dev shells.
  }
}

async function onStop() {
  if (stopping.value) return;
  stopping.value = true;
  try {
    await (window as any).electronAPI?.liveplayServer?.shutdown();
  } catch (e) {
    console.warn('[LocalServerStatus] shutdown failed:', e);
  } finally {
    // Status push comes via onStateChange, but refresh as a safety net in
    // case the IPC race left us out of sync.
    setTimeout(() => { stopping.value = false; refresh(); }, 400);
  }
}

async function onStart() {
  try {
    await (window as any).electronAPI?.liveplayServer?.setConfig({ mode: 'local' });
  } catch (e) {
    console.warn('[LocalServerStatus] start failed:', e);
  }
}

async function onRestart() {
  try {
    await (window as any).electronAPI?.liveplayServer?.restart();
  } catch (e) {
    console.warn('[LocalServerStatus] restart failed:', e);
  }
}

let stopSub: (() => void) | null = null;
onMounted(() => {
  refresh();
  try {
    stopSub = (window as any).electronAPI?.liveplayServer?.onStateChange?.((s: any) => {
      if (s) status.value = s;
    }) ?? null;
  } catch {}
});
onUnmounted(() => { if (stopSub) { try { stopSub(); } catch {} stopSub = null; } });
</script>

<style scoped>
.local-server-status {
  position: fixed;
  bottom: 16px;
  left: 16px;
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 12px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
  box-shadow: 0 6px 24px rgba(0, 0, 0, 0.25);
  z-index: 1400;
  font-size: 12px;
  color: var(--color-text-primary);
  user-select: none;
}
.local-server-status.stopping { opacity: 0.7; }

.lss-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}
.lss-dot.running { background: #24a148; box-shadow: 0 0 8px rgba(36, 161, 72, 0.6); }
.lss-dot.stopped { background: #6f6f6f; }

.lss-meta {
  display: flex;
  flex-direction: column;
}
.lss-title { font-weight: 600; }
.lss-sub   { font-size: 11px; color: var(--color-text-secondary); }

.lss-actions {
  display: flex;
  align-items: center;
  gap: 4px;
  margin-left: 6px;
}

.lss-btn {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 4px 8px;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  background: var(--color-background);
  color: var(--color-text-primary);
  cursor: pointer;
  font-size: 11px;
}
.lss-btn:hover:not(:disabled) { background: var(--color-surface-hover); border-color: var(--color-accent); }
.lss-btn:disabled { opacity: 0.5; cursor: default; }
.lss-btn .material-symbols-rounded { font-size: 14px; }

.lss-btn-icon {
  display: inline-flex;
  padding: 4px;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  background: var(--color-background);
  color: var(--color-text-primary);
  cursor: pointer;
}
.lss-btn-icon:hover { background: var(--color-surface-hover); border-color: var(--color-accent); }
.lss-btn-icon .material-symbols-rounded { font-size: 14px; }
</style>
