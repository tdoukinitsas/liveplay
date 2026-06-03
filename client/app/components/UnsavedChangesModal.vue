<template>
  <Teleport to="body">
    <div v-if="visible" class="uc-backdrop" :data-theme="theme" @click.self="cancel">
      <div class="uc-modal" role="dialog" aria-modal="true">
        <header class="uc-header">
          <h3>{{ t('unsavedModal.title') }}</h3>
        </header>
        <p class="uc-message">{{ t('unsavedModal.message') }}</p>
        <div class="uc-actions">
          <button class="uc-btn ghost" @click="cancel">{{ t('unsavedModal.cancel') }}</button>
          <button class="uc-btn danger" @click="discard">{{ t('unsavedModal.discard') }}</button>
          <button class="uc-btn primary" @click="save">
            <span class="material-symbols-rounded">save</span>
            <span>{{ t('unsavedModal.save') }}</span>
          </button>
        </div>
      </div>
    </div>
  </Teleport>
</template>

<!--
  UnsavedChangesModal.vue
  ---------------------------------------------------------------------
  Shown when the user tries to leave the current project (New / Open /
  Close) while autosave is OFF and there are pending edits. Offers
  Save (force-save, then proceed), Don't Save (discard, proceed) or
  Cancel (abort the navigation).

  Theme note: the project's theme CSS variables (--color-surface etc.)
  are defined under `[data-theme='dark']` / `[data-theme='light']`
  rules that live on `#app`. Because <Teleport to="body"> hoists this
  modal OUT of #app, those vars would resolve to nothing and the modal
  would render unstyled. We mirror the app's current theme onto the
  teleported root so the same selectors match here too (and every var
  carries a literal fallback as a second line of defence).
-->
<script setup lang="ts">
defineProps<{ visible: boolean }>();

const emit = defineEmits<{
  save: [];
  discard: [];
  cancel: [];
}>();

const { t } = useLocalization();
// Mirror the app-wide theme so [data-theme='…'] CSS variables resolve
// inside the teleported subtree.
const theme = useState<string>('theme', () => 'dark');

function save()    { emit('save'); }
function discard() { emit('discard'); }
function cancel()  { emit('cancel'); }
</script>

<!--
  Styles are intentionally NOT scoped — the modal's root is teleported to
  <body>, where Vue's scoped `data-v-*` attribute doesn't reliably land on
  the teleported root in packaged builds. The `.uc-*` class prefix keeps
  these selectors namespaced.
-->
<style lang="scss">
.uc-backdrop {
  position: fixed; inset: 0;
  background: rgba(0,0,0,0.6);
  display: flex; align-items: center; justify-content: center;
  z-index: 10000;
  backdrop-filter: blur(4px);
}
.uc-modal {
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
.uc-header h3 {
  margin: 0;
  color: var(--color-text-primary, #f4f4f4);
  font-size: 18px;
  font-weight: 600;
}
.uc-message {
  margin: 0;
  color: var(--color-text-secondary, #c6c6c6);
  font-size: 14px;
  line-height: 1.5;
}
.uc-actions {
  display: flex;
  justify-content: flex-end;
  gap: 12px;
}
.uc-btn {
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
