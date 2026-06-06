<template>
  <Teleport to="body">
    <div v-if="visible" class="qm-backdrop" :data-theme="theme" @click.self="onBackdrop">
      <div class="qm-modal" role="dialog" aria-modal="true">
        <header class="qm-header">
          <h3>{{ title }}</h3>
        </header>
        <p class="qm-message">{{ message }}</p>
        <div class="qm-actions">
          <button
            v-for="btn in buttons"
            :key="btn.key"
            class="qm-btn"
            :class="btn.variant"
            @click="$emit('pick', btn.key)"
          >
            <span v-if="btn.icon" class="material-symbols-rounded">{{ btn.icon }}</span>
            <span>{{ btn.label }}</span>
          </button>
        </div>
      </div>
    </div>
  </Teleport>
</template>

<!--
  QuitConfirmModal.vue
  ---------------------------------------------------------------------
  Generic confirmation modal used by the app-quit flow (see app.vue):
  one instance asks about discarding unsaved changes, another asks
  whether to also shut the local audio server down. The parent owns all
  copy (already localised) and the button set, so this component stays
  presentation-only.

  Theme note: the project's theme CSS variables (--color-surface etc.)
  are defined under `[data-theme='dark']` / `[data-theme='light']`
  rules that live on `#app`. Because <Teleport to="body"> hoists this
  modal OUT of #app, those vars would resolve to nothing and the modal
  would render unstyled/transparent. We mirror the app's current theme
  onto the teleported root so the same selectors match here too (and
  every var carries a literal fallback as a second line of defence).
-->
<script setup lang="ts">
interface QuitButton {
  key: string;
  label: string;
  variant?: 'ghost' | 'primary' | 'danger';
  icon?: string;
}

const props = defineProps<{
  visible: boolean;
  title: string;
  message: string;
  buttons: QuitButton[];
  /** Key emitted when the backdrop is clicked or Escape is pressed. */
  cancelKey?: string;
}>();

const emit = defineEmits<{ pick: [key: string] }>();

// Mirror the app-wide theme so [data-theme='…'] CSS variables resolve
// inside the teleported subtree.
const theme = useState<string>('theme', () => 'dark');

function onBackdrop() {
  emit('pick', props.cancelKey ?? 'cancel');
}

function onKeydown(e: KeyboardEvent) {
  if (!props.visible) return;
  if (e.key === 'Escape') {
    e.preventDefault();
    emit('pick', props.cancelKey ?? 'cancel');
  }
}

onMounted(() => { if (import.meta.client) window.addEventListener('keydown', onKeydown, true); });
onUnmounted(() => { if (import.meta.client) window.removeEventListener('keydown', onKeydown, true); });
</script>

<!--
  Styles are intentionally NOT scoped — the modal's root is teleported to
  <body>, where Vue's scoped `data-v-*` attribute doesn't reliably land on
  the teleported root in packaged builds. The `.qm-*` class prefix keeps
  these selectors namespaced.
-->
<style lang="scss">
.qm-backdrop {
  position: fixed; inset: 0;
  background: rgba(0,0,0,0.6);
  display: flex; align-items: center; justify-content: center;
  z-index: 10000;
  backdrop-filter: blur(4px);
}
.qm-modal {
  background: var(--color-surface, #262626);
  border: 1px solid var(--color-border, #525252);
  color: var(--color-text-primary, #f4f4f4);
  border-radius: 8px;
  padding: 24px;
  min-width: 440px;
  max-width: 560px;
  box-shadow: 0 8px 32px rgba(0,0,0,0.5);
  display: flex; flex-direction: column; gap: 16px;
}
.qm-header h3 {
  margin: 0;
  color: var(--color-text-primary, #f4f4f4);
  font-size: 18px;
  font-weight: 600;
}
.qm-message {
  margin: 0;
  color: var(--color-text-secondary, #c6c6c6);
  font-size: 14px;
  line-height: 1.5;
}
.qm-actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 12px;
}
.qm-btn {
  padding: 10px 16px;
  border-radius: 6px;
  border: 1px solid var(--color-border, #525252);
  background: var(--color-background, #161616);
  color: var(--color-text-primary, #f4f4f4);
  font-size: 14px;
  font-weight: 500;
  cursor: pointer;
  display: flex; align-items: center; justify-content: center; gap: 8px;
  transition: all 0.15s ease;

  .material-symbols-rounded { font-size: 18px; }

  &:hover {
    background: var(--color-surface-hover, var(--color-surface, #333333));
  }

  &.primary {
    border-color: var(--color-accent, #da1e28);
    &:hover { background: var(--color-accent, #da1e28); color: white; }
  }
  &.danger {
    border-color: var(--color-danger, #da1e28);
    color: var(--color-danger, #fa4d56);
    &:hover { background: var(--color-danger, #da1e28); color: white; }
  }
  &.ghost {
    background: transparent;
    border-color: transparent;
    color: var(--color-text-secondary, #c6c6c6);
    &:hover { color: var(--color-text-primary, #f4f4f4); }
  }
}
</style>
