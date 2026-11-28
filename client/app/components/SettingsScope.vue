<template>
  <!-- Who a setting belongs to. One vocabulary on every pane, drawn by the
       .settings-scope rule in SettingsPage.vue. It goes on the pane title when
       the whole pane has one owner and on each group heading otherwise, never
       on a single field, so the page answers "who else does this change?"
       without the reader opening anything. -->
  <span class="settings-scope">
    <span class="material-symbols-rounded" aria-hidden="true">{{ SCOPES[scope].icon }}</span>
    {{ t(SCOPES[scope].labelKey) }}
  </span>
</template>

<script setup lang="ts">
// you      the person at the desk (usePreferences: their profile when signed
//          in, this surface's store when nobody is)
// show     saved in the project file
// computer this machine only (localStorage or the desktop app's own files)
// server   the server's own configuration: outputs.json, liveplay.json, accounts
type SettingsScopeKind = 'you' | 'show' | 'computer' | 'server';

defineProps<{ scope: SettingsScopeKind }>();

const { t } = useLocalization();

const SCOPES: Record<SettingsScopeKind, { icon: string; labelKey: string }> = {
  you:      { icon: 'person',      labelKey: 'settings.scopeYou' },
  show:     { icon: 'description', labelKey: 'settings.scopeShow' },
  computer: { icon: 'computer',    labelKey: 'settings.thisComputer' },
  server:   { icon: 'dns',         labelKey: 'settings.scopeServer' },
};
</script>
