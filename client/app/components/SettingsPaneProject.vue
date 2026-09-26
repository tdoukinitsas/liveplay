<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionProject') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionProjectHelp') }}</p>

    <!-- Autosave also has a header toggle, deliberately: it is flipped often
         enough mid-session to deserve one, and this is where someone looks for
         it when they do not know that. Both drive the same project value. -->
    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input
          type="checkbox"
          :checked="autoSaveEnabled"
          :disabled="!currentProject"
          @change="onAutoSaveChange"
        />
        {{ t('project.autosave') }}
      </label>
      <p class="settings-help">{{ t('settings.autosaveHelp') }}</p>
    </section>

    <!-- Cue numbering is shared vocabulary between the operator, the stage
         manager and the paper script, so it belongs to the project rather than
         to whoever is sitting at the machine. Display only: stored indices and
         the REST by-index paths stay zero-based. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">format_list_numbered</span>
        {{ t('settings.indexDisplayStart') }}
      </label>
      <input
        class="settings-input"
        type="number"
        min="0"
        step="1"
        :value="indexDisplayStart"
        @change="onIndexDisplayStartChange"
      />
      <p class="settings-help">{{ t('settings.indexDisplayStartHelp') }}</p>
    </section>

    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input type="checkbox" :checked="scrollToPlaying" @change="onScrollToPlayingChange" />
        {{ t('settings.scrollToPlaying') }}
      </label>
      <p class="settings-help">{{ t('settings.scrollToPlayingHelp') }}</p>
    </section>
  </div>
</template>

<script setup lang="ts">
import { normalizeIndexDisplayStart } from '~/utils/indexDisplay';
import { useProjectSettings } from '~/composables/useProjectSettings';

const { t } = useLocalization();
const { settings, applyPatch } = useProjectSettings();
const { currentProject, autoSaveEnabled, setAutoSave } = useProject();

const indexDisplayStart = computed(() => normalizeIndexDisplayStart(settings.value.indexDisplayStart));
// Whose playlist chases the playing cue is a preference of the person reading
// it, not a property of the show (U4). The control stays on this pane — P3
// does the relocation into Appearance — but it writes to their profile.
const { scrollToPlaying, setScrollToPlaying } = usePreferences();

function onAutoSaveChange(e: Event) {
  setAutoSave((e.target as HTMLInputElement).checked);
}
function onIndexDisplayStartChange(e: Event) {
  const input = e.target as HTMLInputElement;
  const value = normalizeIndexDisplayStart(input.value);
  input.value = String(value);
  applyPatch({ indexDisplayStart: value });
}
function onScrollToPlayingChange(e: Event) {
  setScrollToPlaying((e.target as HTMLInputElement).checked);
}
</script>
