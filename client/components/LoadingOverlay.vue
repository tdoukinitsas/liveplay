<template>
  <div v-if="visible" class="loading-overlay">
    <div class="loading-panel">
      <div class="spinner">
        <div class="spinner-ring"></div>
      </div>
      <div class="loading-text">
        <h2>{{ title || t('common.loading') }}</h2>
        <p v-if="subtitle" class="loading-subtitle">{{ subtitle }}</p>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
defineProps<{
  visible: boolean;
  title?: string;
  subtitle?: string;
}>();

const { t } = useLocalization();
</script>

<style scoped>
.loading-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.55);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 2000;
}

.loading-panel {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 18px;
  padding: 28px 36px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  color: var(--color-text-primary);
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.4);
  min-width: 260px;
}

.spinner {
  width: 48px;
  height: 48px;
  position: relative;
}
.spinner-ring {
  width: 100%;
  height: 100%;
  border: 4px solid var(--color-border);
  border-top-color: var(--color-accent);
  border-radius: 50%;
  animation: spin 0.85s linear infinite;
}
@keyframes spin {
  to { transform: rotate(360deg); }
}

.loading-text {
  text-align: center;
}
.loading-text h2 {
  margin: 0;
  font-size: 16px;
  color: var(--color-text-primary);
}
.loading-subtitle {
  margin: 6px 0 0;
  font-size: 13px;
  color: var(--color-text-secondary);
}
</style>
