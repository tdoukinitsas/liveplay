<template>
  <transition name="fade">
    <div v-if="visible" class="clm-overlay">
      <div class="clm-dialog">
        <div class="clm-header">
          <span class="material-symbols-rounded clm-icon">cloud_off</span>
          <h2>{{ t('connectionLost.title') }}</h2>
        </div>
        <p class="clm-body">
          {{ t('connectionLost.message', { url: server.serverUrl }) }}
        </p>

        <!-- Retry pulse: the reconnect loop never stops on its own, so the
             animation is the honest signal that work is still happening.
             Without it a stalled-looking modal reads as "app has hung". -->
        <p class="clm-retry">
          <span class="material-symbols-rounded clm-spinner">progress_activity</span>
          <span>{{ t('connectionLost.attempting') }}</span>
          <span v-if="attempts > 0" class="clm-attempts">
            {{ t('connectionLost.attemptCount', { count: attempts }) }}
          </span>
        </p>
        <p class="clm-locked">{{ t('connectionLost.locked') }}</p>
        <p v-if="server.lastError" class="clm-error">{{ server.lastError }}</p>

        <div class="clm-actions">
          <button class="clm-btn primary" @click="onReconnect">
            <span class="material-symbols-rounded">sync</span>
            <span>{{ t('connectionLost.reconnect') }}</span>
          </button>
          <button class="clm-btn" @click="onRestart">
            <span class="material-symbols-rounded">restart_alt</span>
            <span>{{ t('connectionLost.restart') }}</span>
          </button>
          <button class="clm-btn" @click="onExit">
            <span class="material-symbols-rounded">logout</span>
            <span>{{ t('connectionLost.exit') }}</span>
          </button>
        </div>
        <p class="clm-hint">
          {{ t('connectionLost.reconnectHint') }}
          {{ t('connectionLost.restartHint') }}
          {{ t('connectionLost.exitHint') }}
        </p>
      </div>
    </div>
  </transition>
</template>

<script setup lang="ts">
const { t } = useLocalization();
const server = useLiveplayServer();
const visible = computed(() => !!server.connectionLost);
const attempts = computed(() => Number(server.failedReconnectAttempts) || 0);

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
  /* Above every content modal (import, file picker, welcome overlays — all in
     the 9000s), because none of them can complete without the server. Server
     Settings deliberately sits higher still: retargeting the URL is one of the
     ways out of this state. */
  z-index: 9500;
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
.clm-retry {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: var(--color-text-primary);
  margin: 12px 0 6px;
}
.clm-spinner {
  font-size: 18px;
  color: var(--color-accent, #0f62fe);
  animation: clm-spin 1.1s linear infinite;
}
@keyframes clm-spin { to { transform: rotate(360deg); } }
.clm-attempts { color: var(--color-text-secondary); font-size: 12px; }
.clm-locked {
  font-size: 12px;
  color: var(--color-text-secondary);
  margin: 0 0 4px;
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
  color: var(--color-on-accent, #fff);
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
