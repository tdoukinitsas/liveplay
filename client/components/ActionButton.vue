<template>
  <button
    class="action-btn"
    :class="[
      context === 'Cart' ? 'action-btn--cart' : 'action-btn--playlist',
      { 'action-btn--active': isActive }
    ]"
    :style="computedStyle"
    v-bind="$attrs"
  >
    <span class="material-symbols-rounded">{{ icon }}</span>
  </button>
</template>

<script setup lang="ts">
const props = withDefaults(defineProps<{
  icon: string;
  highlightColor?: string;
  activeTextColor?: string;
  context?: 'Playlist' | 'Cart';
  isActive?: boolean;
}>(), {
  highlightColor: 'var(--color-accent)',
  activeTextColor: 'white',
  context: 'Playlist',
  isActive: false,
});

const computedStyle = computed(() => {
  if (props.isActive) {
    return {
      backgroundColor: props.highlightColor,
      borderColor: props.highlightColor,
      color: props.activeTextColor,
    };
  }
  return { '--action-highlight': props.highlightColor, '--action-text': props.activeTextColor };
});
</script>

<style scoped>
.action-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--border-radius-sm);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  color: var(--color-text-primary);
  transition: all var(--transition-fast);
  cursor: pointer;

  &:hover:not(:disabled) {
    background-color: var(--action-highlight, var(--color-accent));
    border-color: var(--action-highlight, var(--color-accent));
    color: var(--action-text, white);
  }

  &:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }
}

.action-btn--playlist {
  width: 32px;
  height: 32px;

  .material-symbols-rounded {
    font-size: 18px;
  }
}

.action-btn--cart {
  width: 28px;
  height: 28px;

  .material-symbols-rounded {
    font-size: 16px;
  }
}
</style>
