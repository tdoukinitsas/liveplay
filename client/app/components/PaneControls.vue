<template>
  <!--
    The window controls every workspace pane carries, at the right-hand end of
    its header: open in a new window, expand/restore, collapse. The same three,
    in the same order, drawn the same way on the Playlist, the Cart Player and
    the Mixer, so learning one pane teaches the others.

    Icon-only on purpose, and set apart by a divider: these move the pane, not
    the show. The labelled buttons to their left (Import, Add Bus…) are the
    pane's own actions, and the two should never be mistaken for each other.

    In a detached window the only move that means anything is going home, so
    that is the one control, and it is labelled — there is room, and it is the
    way out of the window.
  -->
  <div class="pane-controls" :class="{ 'pane-controls--divided': divided }">
    <template v-if="!inWindow">
      <button
        v-if="canDetach(pane)"
        type="button"
        class="pane-ctl"
        :title="t('workspace.detach')"
        :aria-label="t('workspace.detach')"
        :disabled="pane === 'cart' && !currentProject"
        @click="detachPane(pane)"
      >
        <span class="material-symbols-rounded">open_in_new</span>
      </button>
      <button
        v-if="isExpanded(pane) || canExpand(pane)"
        type="button"
        class="pane-ctl"
        :title="isExpanded(pane) ? t('workspace.restore') : t('workspace.expand')"
        :aria-label="isExpanded(pane) ? t('workspace.restore') : t('workspace.expand')"
        @click="isExpanded(pane) ? restorePanes() : expandPane(pane)"
      >
        <span class="material-symbols-rounded">{{ isExpanded(pane) ? 'close_fullscreen' : 'open_in_full' }}</span>
      </button>
      <button
        type="button"
        class="pane-ctl"
        :title="t('workspace.collapse')"
        :aria-label="t('workspace.collapse')"
        @click="hidePane(pane)"
      >
        <span class="material-symbols-rounded">minimize</span>
      </button>
    </template>
    <button
      v-else
      type="button"
      class="pane-ctl pane-ctl--labelled"
      :title="t('workspace.dockToMain')"
      @click="attachPane(pane)"
    >
      <span class="material-symbols-rounded">dock_to_left</span>
      <span class="pane-ctl__label">{{ t('workspace.dockToMain') }}</span>
    </button>
  </div>
</template>

<script setup lang="ts">
import type { PaneId } from '~/composables/useWorkspaceLayout';

withDefaults(defineProps<{
  pane: PaneId;
  /** Rendered in the pane's own detached window rather than the workspace. */
  inWindow?: boolean;
  /** Draw the divider that separates these from the pane's own actions. */
  divided?: boolean;
}>(), { inWindow: false, divided: true });

const { t } = useLocalization();
const { currentProject } = useProject();
const {
  isExpanded, canExpand, canDetach,
  detachPane, attachPane, expandPane, restorePanes, hidePane,
} = useWorkspaceLayout();
</script>

<style scoped lang="scss">
.pane-controls {
  display: flex;
  align-items: center;
  gap: 2px;
  flex: 0 0 auto;
}

.pane-controls--divided {
  margin-left: var(--spacing-xs);
  padding-left: var(--spacing-sm);
  border-left: 1px solid var(--color-border);
}

.pane-ctl {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  min-width: 32px;
  height: 32px;
  padding: 0 6px;
  box-sizing: border-box;
  border: 1px solid transparent;
  border-radius: var(--border-radius-sm);
  background: transparent;
  color: var(--color-text-secondary);
  font-family: inherit;
  font-size: 13px;
  cursor: pointer;
  transition: background-color var(--transition-fast), color var(--transition-fast),
    border-color var(--transition-fast);

  .material-symbols-rounded { font-size: 20px; }

  &:hover:not(:disabled) {
    background-color: var(--color-surface-hover);
    border-color: var(--color-border);
    color: var(--color-text-primary);
  }

  &:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: 1px;
  }

  &:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }
}

.pane-ctl--labelled {
  padding: 0 var(--spacing-md) 0 var(--spacing-sm);
  border-color: var(--color-border);
  color: var(--color-text-primary);
  white-space: nowrap;
}
</style>
