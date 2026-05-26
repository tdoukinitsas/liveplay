<template>
  <div v-if="visible" class="modal-overlay">
    <div class="modal-content" role="dialog" aria-modal="true">
      <div class="modal-header">
        <span class="material-symbols-outlined warning-icon">warning</span>
        <h3>{{ t('repair.title') }}</h3>
      </div>

      <p class="modal-message">{{ t('repair.message') }}</p>

      <ul v-if="issues.length" class="issue-list">
        <li v-for="(issue, i) in issues" :key="i" class="issue-item">
          <span class="material-symbols-outlined">error</span>
          {{ issue }}
        </li>
      </ul>

      <p class="warning-note">{{ t('repair.dataLossWarning') }}</p>

      <div class="modal-actions">
        <button class="btn-secondary" @click="emit('cancel')">
          {{ t('repair.openAnyway') }}
        </button>
        <button class="btn-primary" @click="emit('confirm')">
          {{ t('repair.repairAndSave') }}
        </button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
defineProps<{
  visible: boolean;
  issues: string[];
}>();

const emit = defineEmits<{
  confirm: [];
  cancel: [];
}>();

const { t } = useLocalization();
</script>

<style scoped lang="scss">
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.75);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 10000;
  backdrop-filter: blur(4px);
}

.modal-content {
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
  padding: 24px;
  min-width: 420px;
  max-width: 560px;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.5);
}

.modal-header {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 14px;

  h3 {
    margin: 0;
    color: var(--color-text-primary);
    font-size: 18px;
    font-weight: 600;
  }
}

.warning-icon {
  color: #f0a500;
  font-size: 26px;
}

.modal-message {
  margin: 0 0 14px 0;
  color: var(--color-text-secondary);
  font-size: 14px;
  line-height: 1.5;
}

.issue-list {
  list-style: none;
  margin: 0 0 16px 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 6px;
  background: var(--color-background);
  border-radius: 6px;
  padding: 12px;
  border: 1px solid var(--color-border);
}

.issue-item {
  display: flex;
  align-items: flex-start;
  gap: 8px;
  color: var(--color-text-secondary);
  font-size: 13px;
  line-height: 1.4;

  .material-symbols-outlined {
    color: #e05252;
    font-size: 16px;
    flex-shrink: 0;
    margin-top: 1px;
  }
}

.warning-note {
  margin: 0 0 20px 0;
  color: #f0a500;
  font-size: 12px;
  line-height: 1.4;
  font-style: italic;
}

.modal-actions {
  display: flex;
  justify-content: flex-end;
  gap: 12px;

  button {
    padding: 8px 20px;
    border-radius: 6px;
    font-size: 14px;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.2s ease;
    border: none;
  }

  .btn-secondary {
    background: var(--color-background);
    color: var(--color-text-primary);
    border: 1px solid var(--color-border);

    &:hover {
      background: var(--color-surface);
    }
  }

  .btn-primary {
    background: var(--color-accent);
    color: white;

    &:hover {
      opacity: 0.9;
    }
  }
}
</style>
