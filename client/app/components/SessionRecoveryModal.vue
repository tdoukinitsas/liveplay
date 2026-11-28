<template>
  <transition name="fade">
    <div v-if="visible" class="srm-overlay">
      <div class="srm-dialog">
        <div class="srm-header">
          <span class="material-symbols-rounded srm-icon">restart_alt</span>
          <h2>{{ t('sessionRecovery.title') }}</h2>
        </div>
        <p class="srm-body">{{ t('sessionRecovery.message') }}</p>
        <p class="srm-project">{{ currentProject?.name }}</p>

        <div class="srm-actions">
          <button class="srm-btn primary" :disabled="recovering" @click="onResume">
            <span class="material-symbols-rounded" :class="{ spin: recovering }">
              {{ recovering ? 'progress_activity' : 'restore' }}
            </span>
            <span>{{ t('sessionRecovery.resume') }}</span>
          </button>
          <button class="srm-btn" :disabled="recovering" @click="onFresh">
            <span class="material-symbols-rounded">home</span>
            <span>{{ t('sessionRecovery.startFresh') }}</span>
          </button>
        </div>
        <p class="srm-hint">
          {{ t('sessionRecovery.resumeHint') }}
          {{ t('sessionRecovery.startFreshHint') }}
        </p>
      </div>
    </div>
  </transition>
</template>

<script setup lang="ts">
// Shown when the client reconnects to a server that has forgotten the
// project — almost always because the server process restarted mid-session.
// The client still holds the whole document, so recovery is a choice, not a
// failure: push it back and carry on, or drop it and start over.
const { t } = useLocalization();
const { currentProject } = useProject();
const { sessionLost, recovering, resumeSession, startFresh } = useConnectionGuard();

const visible = computed(() => sessionLost.value && !!currentProject.value);

async function onResume() {
  const ok = await resumeSession();
  if (!ok) console.warn('[SessionRecoveryModal] resume failed; leaving prompt open');
}

async function onFresh() {
  await startFresh();
}
</script>

<style scoped>
.srm-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  /* Above ConnectionLostModal (9500): the connection is back, so this is the
     dialog the operator now needs to act on. */
  z-index: 9600;
}
.srm-dialog {
  background: var(--color-surface);
  color: var(--color-text-primary);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  padding: 24px 28px;
  max-width: 480px;
  width: 90%;
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
}
.srm-header {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 8px;
}
.srm-header h2 { margin: 0; font-size: 18px; }
.srm-icon { color: var(--color-accent, #0f62fe); font-size: 28px; }
.srm-body {
  color: var(--color-text-secondary);
  font-size: 14px;
  line-height: 1.5;
}
.srm-project {
  font-size: 13px;
  font-weight: 600;
  background: var(--color-background);
  padding: 8px 10px;
  border-radius: 6px;
  border: 1px solid var(--color-border);
  word-break: break-all;
}
.srm-actions {
  display: flex;
  gap: 8px;
  margin-top: 18px;
}
.srm-btn {
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
.srm-btn:hover:not(:disabled) {
  background: var(--color-surface-hover);
  border-color: var(--color-accent);
}
.srm-btn.primary {
  background: var(--color-accent, #0f62fe);
  color: var(--color-on-accent, #fff);
  border-color: transparent;
}
.srm-btn.primary:hover:not(:disabled) { filter: brightness(1.1); }
.srm-btn:disabled { opacity: 0.5; cursor: default; }
.srm-btn .material-symbols-rounded { font-size: 16px; }
.srm-hint {
  font-size: 11px;
  color: var(--color-text-secondary);
  margin-top: 12px;
  line-height: 1.4;
}

.spin { animation: srm-spin 1s linear infinite; }
@keyframes srm-spin { to { transform: rotate(360deg); } }

.fade-enter-active, .fade-leave-active { transition: opacity 0.15s ease; }
.fade-enter-from, .fade-leave-to { opacity: 0; }
</style>
