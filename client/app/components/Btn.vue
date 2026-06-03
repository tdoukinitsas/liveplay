<template>
  <button
    class="btn"
    :class="{ 'btn--youtube': bgStyle === 'youtube' }"
    :disabled="disabled"
    v-bind="$attrs"
  >
    <span class="material-symbols-rounded">{{ icon }}</span>
    <span>{{ text }}</span>
  </button>
</template>

<script setup lang="ts">
withDefaults(defineProps<{
  icon: string;
  text: string;
  bgStyle?: 'default' | 'youtube';
  disabled?: boolean;
}>(), {
  bgStyle: 'default',
  disabled: false,
});
</script>

<style scoped>
.btn {
  padding: var(--spacing-sm) var(--spacing-md);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  font-size: 13px;
  display: flex;
  align-items: center;
  gap: 6px;
  cursor: pointer;
  transition: all 0.2s;
  color: var(--color-text-primary);
  white-space: nowrap;

  .material-symbols-rounded {
    font-size: 18px;
  }

  &:hover:not(:disabled) {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
  }

  &:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
}

.btn--youtube {
  background: linear-gradient(135deg, #FF0000 0%, #CC0000 100%);
  color: white;
  border-color: #FF0000;

  &:hover:not(:disabled) {
    background: linear-gradient(135deg, #CC0000 0%, #990000 100%);
    border-color: #CC0000;
  }

  &:disabled {
    background: linear-gradient(135deg, #666666 0%, #555555 100%);
    border-color: #666666;
  }
}
</style>
