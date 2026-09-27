<template>
  <!-- Note: NOT inside <Teleport> — Vue scoped styles don't reach teleported
       nodes, which would leave this unstyled in production builds. Same
       constraint the modals it replaces were written under. -->
  <div v-if="isOpen" class="settings-page" role="dialog" aria-modal="true" :aria-label="t('settings.title')">
    <header class="settings-page-header">
      <h2>{{ t('settings.title') }}</h2>
      <button class="close-x" :aria-label="t('settings.close')" @click="close">✕</button>
    </header>

    <div class="settings-page-body">
      <!-- The rail persists across panes: that is the whole point of a page
           rather than a stack of modals — you can see what else is here. -->
      <nav class="settings-nav" :aria-label="t('settings.title')">
        <button
          v-for="s in sections"
          :key="s.id"
          type="button"
          class="settings-nav-item"
          :class="{ active: section === s.id }"
          :aria-current="section === s.id ? 'page' : undefined"
          @click="show(s.id)"
        >
          <span class="material-symbols-rounded">{{ s.icon }}</span>
          <span class="settings-nav-label">{{ t(s.labelKey) }}</span>
        </button>
      </nav>

      <div class="settings-panes">
        <SettingsPaneAppearance v-if="section === 'appearance'" />
        <SettingsPanePlayback v-else-if="section === 'playback'" />
        <SettingsPaneAudio v-else-if="section === 'audio'" />
        <SettingsPaneOutputs v-else-if="section === 'outputs'" />
        <SettingsPaneKeyboard v-else-if="section === 'keyboard'" />
        <SettingsPaneSurfaces v-else-if="section === 'surfaces'" />
        <SettingsPaneProject v-else-if="section === 'project'" />
        <SettingsPaneServer v-else-if="section === 'server'" />
        <SettingsPaneUsers v-else-if="section === 'users'" />
        <SettingsPaneAbout v-else-if="section === 'about'" />
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import SettingsPaneAppearance from './SettingsPaneAppearance.vue';
import SettingsPanePlayback from './SettingsPanePlayback.vue';
import SettingsPaneAudio from './SettingsPaneAudio.vue';
import SettingsPaneOutputs from './SettingsPaneOutputs.vue';
import SettingsPaneKeyboard from './SettingsPaneKeyboard.vue';
import SettingsPaneSurfaces from './SettingsPaneSurfaces.vue';
import SettingsPaneProject from './SettingsPaneProject.vue';
import SettingsPaneServer from './SettingsPaneServer.vue';
import SettingsPaneUsers from './SettingsPaneUsers.vue';
import SettingsPaneAbout from './SettingsPaneAbout.vue';

const { t } = useLocalization();
const { section, isOpen, show, close, sections } = useSettingsPage();

// Escape closes, matching every other dismissable surface in the app. Capture
// phase for the same reason the modals use it: the playback hotkey handler
// listens on window in the bubble phase, and Escape is the stop-all default —
// closing Settings must not also panic the show.
function onKeydown(e: KeyboardEvent) {
  if (!isOpen.value) return;
  if (e.key === 'Escape') {
    e.preventDefault();
    e.stopPropagation();
    close();
  }
}
onMounted(() => window.addEventListener('keydown', onKeydown, true));
onBeforeUnmount(() => window.removeEventListener('keydown', onKeydown, true));
</script>

<style scoped>
.settings-page {
  position: fixed;
  inset: 0;
  z-index: 1000;
  background: var(--color-background);
  color: var(--color-text-primary);
  display: flex;
  flex-direction: column;
}

.settings-page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 16px 20px;
  border-bottom: 1px solid var(--color-border);
  flex: 0 0 auto;
}
.settings-page-header h2 {
  margin: 0;
  font-size: 18px;
}
.close-x {
  background: none;
  border: none;
  color: var(--color-text-secondary);
  font-size: 18px;
  cursor: pointer;
  line-height: 1;
  padding: 4px 8px;
}
.close-x:hover {
  color: var(--color-text-primary);
}

.settings-page-body {
  flex: 1 1 auto;
  display: flex;
  min-height: 0;
}

/* ---- The rail ---------------------------------------------------------- */
.settings-nav {
  flex: 0 0 220px;
  border-right: 1px solid var(--color-border);
  background: var(--color-surface);
  padding: 12px 8px;
  display: flex;
  flex-direction: column;
  gap: 2px;
  overflow-y: auto;
}
.settings-nav-item {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm, 8px);
  padding: 10px 12px;
  background: none;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  color: var(--color-text-secondary);
  font-size: 13px;
  font-weight: 500;
  text-align: left;
  width: 100%;
  transition: background-color 0.15s, color 0.15s;
}
.settings-nav-item .material-symbols-rounded {
  font-size: 18px;
  color: inherit;
}
.settings-nav-item:hover {
  color: var(--color-text-primary);
  background-color: var(--color-surface-hover);
}
.settings-nav-item.active {
  color: var(--color-accent);
  background-color: var(--color-surface-hover);
}
.settings-nav-item:focus-visible {
  outline: 2px solid var(--color-accent);
  outline-offset: -2px;
}

/* ---- Pane area --------------------------------------------------------- */
.settings-panes {
  flex: 1 1 auto;
  overflow-y: auto;
  padding: 20px 24px 40px;
}

/* Field styling is defined once here and reaches the panes through :deep(),
   because scoped styles stop at the component boundary and every pane builds
   from the same handful of shapes. */
.settings-panes :deep(.settings-pane) {
  display: flex;
  flex-direction: column;
  gap: 18px;
  /* Settings read like prose; a full-width field on a 2560px display is
     unreadable and looks unfinished. */
  max-width: 620px;
}
.settings-panes :deep(.settings-pane-title) {
  margin: 0;
  font-size: 20px;
  font-weight: 600;
}
.settings-panes :deep(.settings-pane-intro) {
  margin: -10px 0 0;
  font-size: 13px;
  color: var(--color-text-secondary);
}
.settings-panes :deep(.settings-field) {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.settings-panes :deep(.settings-label) {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: var(--color-text-secondary);
  font-weight: 500;
}
.settings-panes :deep(.settings-label--checkbox) {
  flex-direction: row;
  align-items: center;
  color: var(--color-text-primary);
  cursor: pointer;
}
.settings-panes :deep(.settings-select),
.settings-panes :deep(.settings-input) {
  width: 100%;
  padding: 10px 12px;
  background: var(--color-surface);
  color: var(--color-text-primary);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  font-size: 14px;
}
.settings-panes :deep(.settings-select:focus),
.settings-panes :deep(.settings-input:focus) {
  outline: none;
  border-color: var(--color-accent);
}
.settings-panes :deep(.settings-help) {
  margin: 0;
  font-size: 12px;
  color: var(--color-text-secondary);
}
.settings-panes :deep(.settings-actions) {
  display: flex;
  margin-top: 4px;
}

/* Narrow windows: the rail becomes a strip of icons rather than wrapping or
   eating the pane. The floor is 1280px wide, so this is the detached-window
   and split-screen case, not a phone. */
@media (max-width: 900px) {
  .settings-nav {
    flex-basis: 56px;
  }
  .settings-nav-label {
    display: none;
  }
  .settings-nav-item {
    justify-content: center;
  }
}
</style>
