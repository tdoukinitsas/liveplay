<template>
  <Teleport to="body">
    <div v-if="visible" class="ds-backdrop" :data-theme="theme" @click.self="$emit('cancel')">
      <div class="ds-modal" role="dialog" aria-modal="true">
        <header class="ds-header">
          <h3>{{ t('deleteSelection.title') }}</h3>
        </header>
        <p class="ds-message">{{ t('deleteSelection.message', { count }) }}</p>
        <div class="ds-actions">
          <button class="ds-btn ghost" @click="$emit('cancel')">{{ t('deleteSelection.cancel') }}</button>
          <button v-if="allowOnly && name" class="ds-btn" @click="$emit('deleteOnly')">
            {{ t('deleteSelection.deleteOnly', { name: truncatedName }) }}
          </button>
          <button class="ds-btn danger" @click="$emit('deleteAll')">
            <span class="material-symbols-rounded">delete</span>
            <span>{{ t('deleteSelection.deleteAll', { count }) }}</span>
          </button>
        </div>
      </div>
    </div>
  </Teleport>
</template>

<!--
  DeleteSelectionModal.vue
  ---------------------------------------------------------------------
  Shown when a delete is triggered while more than one playlist item is
  selected. From a trash button it offers Delete N Selected Items,
  Delete Only "<name>", and Cancel; from the keyboard DEL key it drops
  the "Delete Only" choice (a deliberate keystroke is firmer intent).

  Theme note: like UnsavedChangesModal, the root is teleported OUT of
  #app, so we mirror the app's current theme onto the teleported root so
  the [data-theme='…'] CSS variables still resolve (every var also
  carries a literal fallback).
-->
<script setup lang="ts">
const props = defineProps<{
  visible: boolean;
  count: number;
  name: string;
  allowOnly: boolean;
}>();

defineEmits<{
  deleteAll: [];
  deleteOnly: [];
  cancel: [];
}>();

const { t } = useLocalization();
// Mirror the app-wide theme so [data-theme='…'] CSS variables resolve
// inside the teleported subtree.
const theme = useState<string>('theme', () => 'dark');

// Keep the "Delete Only" button label from blowing out on long track names.
const truncatedName = computed(() => {
  const max = 32;
  return props.name.length > max ? `${props.name.slice(0, max - 1)}…` : props.name;
});
</script>

<!--
  Styles are intentionally NOT scoped — the modal's root is teleported to
  <body>, where Vue's scoped data-v-* attribute doesn't reliably land on the
  teleported root in packaged builds. The `.ds-*` class prefix keeps these
  selectors namespaced.
-->
<style lang="scss">
.ds-backdrop {
  position: fixed; inset: 0;
  background: rgba(0,0,0,0.6);
  display: flex; align-items: center; justify-content: center;
  z-index: 10000;
  backdrop-filter: blur(4px);
}
.ds-modal {
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
.ds-header h3 {
  margin: 0;
  color: var(--color-text-primary, #f4f4f4);
  font-size: 18px;
  font-weight: 600;
}
.ds-message {
  margin: 0;
  color: var(--color-text-secondary, #c6c6c6);
  font-size: 14px;
  line-height: 1.5;
}
.ds-actions {
  display: flex;
  justify-content: flex-end;
  flex-wrap: wrap;
  gap: 12px;
}
.ds-btn {
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
