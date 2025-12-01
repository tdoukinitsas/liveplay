<template>
  <div v-if="visible" class="modal-overlay" @click="handleOverlayClick">
    <div class="modal-content" @click.stop>
      <h3>{{ title }}</h3>
      <div class="progress-info">
        <p>{{ message }}</p>
        <div class="progress-percentage">{{ percentage }}%</div>
      </div>
      <div class="progress-bar-container">
        <div class="progress-bar" :style="{ width: percentage + '%' }"></div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';

const props = defineProps<{
  visible: boolean;
  title: string;
  message: string;
  percentage: number;
  allowCancel?: boolean;
}>();

const emit = defineEmits<{
  cancel: [];
}>();

const handleOverlayClick = () => {
  if (props.allowCancel) {
    emit('cancel');
  }
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
  min-width: 400px;
  max-width: 500px;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.4);

  h3 {
    margin: 0 0 16px 0;
    color: var(--color-text-primary);
    font-size: 18px;
    font-weight: 600;
  }
}

.progress-info {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;

  p {
    margin: 0;
    color: var(--color-text-secondary);
    font-size: 14px;
    flex: 1;
  }

  .progress-percentage {
    color: var(--color-text-primary);
    font-size: 16px;
    font-weight: 600;
    margin-left: 16px;
  }
}

.progress-bar-container {
  width: 100%;
  height: 8px;
  background: var(--color-background);
  border-radius: 4px;
  overflow: hidden;
}

.progress-bar {
  height: 100%;
  background: var(--color-accent);
  border-radius: 4px;
  transition: width 0.3s ease;
}
</style>
