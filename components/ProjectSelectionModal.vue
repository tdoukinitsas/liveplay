<template>
  <div v-if="visible" class="modal-overlay" @click.self="handleCancel">
    <div class="modal-content">
      <h3>{{ t('selectProject.title') }}</h3>
      <p class="modal-message">{{ t('selectProject.message') }}</p>
      
      <div class="project-list">
        <button
          v-for="(project, index) in projects"
          :key="index"
          class="project-item"
          :class="{ selected: selectedIndex === index }"
          @click="selectedIndex = index"
        >
          <span class="material-symbols-outlined">description</span>
          <span class="project-name">{{ project }}</span>
        </button>
      </div>
      
      <div class="modal-actions">
        <button class="btn-secondary" @click="handleCancel">
          {{ t('selectProject.cancel') }}
        </button>
        <button class="btn-primary" :disabled="selectedIndex === null" @click="handleSelect">
          {{ t('selectProject.open') }}
        </button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, watch } from 'vue';

const props = defineProps<{
  visible: boolean;
  projects: string[];
}>();

const emit = defineEmits<{
  select: [projectName: string];
  cancel: [];
}>();

const { t } = useLocalization();
const selectedIndex = ref<number | null>(null);

watch(() => props.visible, (newVal) => {
  if (newVal) {
    selectedIndex.value = null;
  }
});

const handleSelect = () => {
  if (selectedIndex.value !== null) {
    emit('select', props.projects[selectedIndex.value]);
  }
};

const handleCancel = () => {
  emit('cancel');
};
</script>

<style scoped lang="scss">
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.7);
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
  min-width: 450px;
  max-width: 600px;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.4);

  h3 {
    margin: 0 0 12px 0;
    color: var(--color-text-primary);
    font-size: 18px;
    font-weight: 600;
  }
}

.modal-message {
  margin: 0 0 20px 0;
  color: var(--color-text-secondary);
  font-size: 14px;
  line-height: 1.5;
}

.project-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
  margin-bottom: 24px;
  max-height: 300px;
  overflow-y: auto;
  padding: 4px;
}

.project-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 16px;
  background: var(--color-background);
  border: 2px solid var(--color-border);
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.2s ease;
  color: var(--color-text-primary);
  font-size: 14px;
  text-align: left;

  &:hover {
    background: var(--color-surface);
    border-color: var(--color-accent);
  }

  &.selected {
    background: var(--color-accent);
    border-color: var(--color-accent);
    color: white;

    .material-symbols-outlined {
      color: white;
    }
  }

  .material-symbols-outlined {
    font-size: 20px;
    color: var(--color-text-secondary);
    flex-shrink: 0;
  }

  .project-name {
    flex: 1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
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

    &:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
  }

  .btn-secondary {
    background: var(--color-background);
    color: var(--color-text-primary);
    border: 1px solid var(--color-border);

    &:hover:not(:disabled) {
      background: var(--color-surface);
    }
  }

  .btn-primary {
    background: var(--color-accent);
    color: white;

    &:hover:not(:disabled) {
      opacity: 0.9;
    }
  }
}
</style>
