<template>
  <Teleport to="body">
    <div v-if="visible" class="lc-backdrop" :data-theme="theme" @click.self="cancel">
      <div class="lc-modal" role="dialog" aria-modal="true">
        <header class="lc-header">
          <h3>{{ title }}</h3>
        </header>
        <p class="lc-message">{{ message }}</p>
        <div class="lc-actions">
          <button class="lc-btn primary" @click="pick('server')">
            <span class="material-symbols-rounded">cloud</span>
            <span>{{ serverLabel }}</span>
          </button>
          <button class="lc-btn primary" @click="pick('client')">
            <span class="material-symbols-rounded">computer</span>
            <span>{{ clientLabel }}</span>
          </button>
        </div>
        <div class="lc-footer">
          <button class="lc-btn ghost" @click="cancel">{{ cancelLabel }}</button>
        </div>
      </div>
    </div>
  </Teleport>
</template>

<!--
  LocationChoiceModal.vue
  ---------------------------------------------------------------------
  Two-button modal asking the user whether an action targets the SERVER's
  filesystem or this CLIENT computer's filesystem. Used by the dual-dialog
  Import/Export flows when the client and server live on different
  machines (sharing a host means the choice is meaningless, so callers
  skip the modal in that case).

  Theme note: the project's theme CSS variables (--color-surface etc.) are
  defined under `[data-theme='dark']` / `[data-theme='light']` rules that
  live on `#app`. Because <Teleport to="body"> hoists this modal OUT of
  #app, those vars would resolve to nothing and the modal would render
  transparent. We mirror the app's current theme value onto the teleported
  root so the same selectors match here too.
-->
<script setup lang="ts">
defineProps<{
  visible: boolean;
  title: string;
  message: string;
  serverLabel: string;
  clientLabel: string;
  cancelLabel: string;
}>();

const emit = defineEmits<{
  pick: ['server' | 'client'];
  cancel: [];
}>();

// Mirror the app-wide theme so [data-theme='…'] CSS variables resolve
// inside the teleported subtree.
const theme = useState<string>('theme', () => 'dark');

function pick(choice: 'server' | 'client') { emit('pick', choice); }
function cancel() { emit('cancel'); }
</script>

<!--
  Styles are intentionally NOT scoped. The modal's root is teleported to
  <body>, and a previous scoped variant was producing an unstyled box
  (the `data-v-xxx` attribute apparently wasn't landing on the teleported
  root in the packaged Electron build). The `.lc-*` class prefix keeps
  these selectors namespaced enough not to leak.
-->
<style lang="scss">
.lc-backdrop {
  position: fixed; inset: 0;
  background: rgba(0,0,0,0.6);
  display: flex; align-items: center; justify-content: center;
  z-index: 10000;
  backdrop-filter: blur(4px);
}
.lc-modal {
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
.lc-header h3 {
  margin: 0;
  color: var(--color-text-primary, #f4f4f4);
  font-size: 18px;
  font-weight: 600;
}
.lc-message {
  margin: 0;
  color: var(--color-text-secondary, #c6c6c6);
  font-size: 14px;
  line-height: 1.5;
}
.lc-actions {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}
.lc-btn {
  padding: 14px 16px;
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
  &.ghost {
    background: transparent;
    border-color: transparent;
    color: var(--color-text-secondary, #c6c6c6);
    &:hover { color: var(--color-text-primary, #f4f4f4); }
  }
}
.lc-footer {
  display: flex; justify-content: flex-end;
}
</style>
