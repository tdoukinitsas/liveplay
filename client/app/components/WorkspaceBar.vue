<template>
  <!--
    The workspace bar: one toggle per pane, always present, in the order the
    panes sit left to right. Modelled on DaVinci Resolve's page bar — a row of
    icon-over-label tiles, the lit ones marked by an accent bar along the
    bottom — but these are toggles rather than pages: any combination can be
    on, and what is lit is exactly what is showing.

    Three kinds of control share the header, and each has one look:
      * a pane toggle (this bar)      → tile, lit with an accent bar
      * an action (Settings)          → bordered button
      * a mode (Autosave, Show Mode)  → switch
    so nobody has to press one to find out which kind it is.

    A pane in its own window is ON — it is showing, just not here — and says
    so with a small window glyph. Switching it off closes that window.
  -->
  <div class="ws-bar" role="group" :aria-label="t('workspace.barLabel')" :class="{ 'ws-bar--icons': iconOnly }">
    <button
      v-for="p in PANES"
      :key="p.id"
      type="button"
      class="ws-tile"
      :class="{ 'ws-tile--on': panes[p.id], 'ws-tile--away': panes[p.id] && isDetached(p.id) }"
      :aria-pressed="panes[p.id]"
      :title="tileTitle(p)"
      @click="togglePane(p.id)"
    >
      <span class="ws-tile__icon">
        <span class="material-symbols-rounded">{{ p.icon }}</span>
        <span v-if="panes[p.id] && isDetached(p.id)" class="ws-tile__badge material-symbols-rounded" aria-hidden="true">open_in_new</span>
      </span>
      <span v-if="!iconOnly" class="ws-tile__label">{{ t(p.labelKey) }}</span>
    </button>
  </div>
</template>

<script setup lang="ts">
import type { PaneId } from '~/composables/useWorkspaceLayout';

defineProps<{ iconOnly?: boolean }>();

const { t } = useLocalization();
const { panes, isDetached, togglePane } = useWorkspaceLayout();

const PANES: { id: PaneId; icon: string; labelKey: string }[] = [
  { id: 'playlist', icon: 'queue_music', labelKey: 'playlist.title' },
  { id: 'cart',     icon: 'grid_view',   labelKey: 'cart.title' },
  { id: 'mixer',    icon: 'instant_mix', labelKey: 'mixer.title' },
];

function tileTitle(p: { id: PaneId; labelKey: string }): string {
  const name = t(p.labelKey);
  if (!panes.value[p.id]) return t('workspace.show', { pane: name });
  if (isDetached(p.id))   return t('workspace.hideWindow', { pane: name });
  return t('workspace.hide', { pane: name });
}
</script>

<style scoped lang="scss">
.ws-bar {
  display: flex;
  align-items: stretch;
  gap: 2px;
  padding: 2px;
  border-radius: var(--border-radius-md);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  flex: 0 0 auto;
}

.ws-tile {
  position: relative;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 1px;
  min-width: 64px;
  padding: 4px 10px 6px;
  border: none;
  border-radius: var(--border-radius-sm);
  background: transparent;
  color: var(--color-text-secondary);
  font-family: inherit;
  cursor: pointer;
  transition: background-color var(--transition-fast), color var(--transition-fast);

  // The lit marker. Present but invisible when off so the tile never shifts.
  &::after {
    content: '';
    position: absolute;
    left: 10px;
    right: 10px;
    bottom: 2px;
    height: 2px;
    border-radius: 1px;
    background-color: var(--color-accent);
    opacity: 0;
    transition: opacity var(--transition-fast);
  }

  &:hover {
    background-color: var(--color-surface-hover);
    color: var(--color-text-primary);
  }

  &:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
}

.ws-tile--on {
  color: var(--color-accent);
  background-color: color-mix(in srgb, var(--color-accent) 16%, var(--color-surface));

  &::after { opacity: 1; }

  &:hover { color: var(--color-accent); }
}

// In another window: still on, but not here — a dashed marker says "elsewhere".
.ws-tile--away::after {
  background: repeating-linear-gradient(90deg, var(--color-accent) 0 4px, transparent 4px 7px);
}

.ws-tile__icon {
  position: relative;
  display: inline-flex;

  .material-symbols-rounded { font-size: 20px; }
}

.ws-tile__badge {
  position: absolute;
  top: -4px;
  right: -9px;
  font-size: 12px !important;
  font-weight: 700;
}

.ws-tile__label {
  font-size: 11px;
  font-weight: 600;
  line-height: 1.1;
  white-space: nowrap;
}

// Narrow header: icons only, square tiles. The title still names the pane.
.ws-bar--icons .ws-tile {
  min-width: 40px;
  padding: 6px 8px 8px;
}
</style>
