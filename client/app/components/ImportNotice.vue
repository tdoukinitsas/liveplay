<template>
  <Teleport to="body">
    <div v-if="rejected.length" class="import-notice" role="status">
      <span class="material-symbols-rounded" aria-hidden="true">music_off</span>
      <p>{{ t('importAudio.notPlayable', { files }) }}</p>
      <button type="button" :aria-label="t('actions.close')" @click="dismiss">
        <span class="material-symbols-rounded" aria-hidden="true">close</span>
      </button>
    </div>
  </Teleport>
</template>

<script setup lang="ts">
const { t } = useLocalization();
const { rejected, dismiss } = useImportNotice();

const MAX_NAMES = 3;
const files = computed(() => {
  const names = rejected.value.slice(0, MAX_NAMES).join(', ');
  const more = rejected.value.length - MAX_NAMES;
  return more > 0 ? `${names} +${more}` : names;
});
</script>

<style scoped>
.import-notice {
  position: fixed;
  inset-block-end: 24px;
  inset-inline: 16px;
  margin-inline: auto;
  z-index: 10001;
  display: flex;
  align-items: center;
  gap: var(--spacing-sm, 8px);
  width: fit-content;
  max-width: 640px;
  padding: 10px 12px 10px 16px;
  background: var(--color-surface);
  border: 1px solid var(--color-warning);
  border-radius: 8px;
  box-shadow: 0 12px 36px rgba(0, 0, 0, 0.5);
  color: var(--color-text-primary);
  font-size: 14px;
  line-height: 1.4;
}

.import-notice > .material-symbols-rounded {
  flex-shrink: 0;
  color: var(--color-warning);
  font-size: 20px;
}

.import-notice p {
  margin: 0;
  min-width: 0;
  overflow-wrap: anywhere;
}

.import-notice button {
  display: inline-flex;
  flex-shrink: 0;
  padding: 4px;
  background: transparent;
  border: 0;
  border-radius: 4px;
  color: inherit;
  cursor: pointer;
}

.import-notice button:hover {
  background: var(--color-surface-hover);
}

.import-notice button .material-symbols-rounded {
  font-size: 18px;
}
</style>
