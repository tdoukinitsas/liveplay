<template>
  <transition name="fade">
    <div v-if="visible" class="audio-load-progress">
      <div class="audio-load-spinner"></div>
      <div class="audio-load-text">
        <div class="audio-load-title">{{ t('common.loading') }}</div>
        <div class="audio-load-meta">
          {{ progress.loaded }} / {{ progress.total }}
          <span class="audio-load-percent">({{ percent }}%)</span>
        </div>
      </div>
      <div class="audio-load-bar">
        <div class="audio-load-bar-fill" :style="{ width: percent + '%' }"></div>
      </div>
    </div>
  </transition>
</template>

<script setup lang="ts">
const { t } = useLocalization();
const { audioLoadingProgress } = useProject();

const visible = computed(() =>
  audioLoadingProgress.value.loading && audioLoadingProgress.value.total > 0,
);
const progress = computed(() => audioLoadingProgress.value);
const percent  = computed(() => {
  const p = audioLoadingProgress.value;
  if (!p.total) return 0;
  return Math.round((p.loaded / p.total) * 100);
});
</script>

<style scoped>
.audio-load-progress {
  position: fixed;
  bottom: 16px;
  right: 16px;
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 14px;
  background: var(--color-surface);
  color: var(--color-text-primary);
  border: 1px solid var(--color-border);
  border-radius: 8px;
  box-shadow: 0 6px 24px rgba(0, 0, 0, 0.3);
  z-index: 1500;
  min-width: 220px;
}

.audio-load-spinner {
  width: 18px;
  height: 18px;
  border: 2px solid var(--color-border);
  border-top-color: var(--color-accent);
  border-radius: 50%;
  animation: audio-load-spin 0.85s linear infinite;
  flex-shrink: 0;
}
@keyframes audio-load-spin {
  to { transform: rotate(360deg); }
}

.audio-load-text {
  display: flex;
  flex-direction: column;
  flex: 1;
}

.audio-load-title {
  font-size: 12px;
  font-weight: 600;
}
.audio-load-meta {
  font-size: 11px;
  color: var(--color-text-secondary);
}
.audio-load-percent {
  margin-left: 4px;
}

.audio-load-bar {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 0;
  height: 2px;
  background: var(--color-border);
  border-radius: 0 0 8px 8px;
  overflow: hidden;
}
.audio-load-bar-fill {
  height: 100%;
  background: var(--color-accent);
  transition: width 0.2s ease;
}

.fade-enter-active, .fade-leave-active {
  transition: opacity 0.2s ease;
}
.fade-enter-from, .fade-leave-to {
  opacity: 0;
}
</style>
