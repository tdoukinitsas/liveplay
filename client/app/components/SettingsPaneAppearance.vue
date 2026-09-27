<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionAppearance') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionAppearanceHelp') }}</p>

    <!-- Hidden rather than empty when the list has not arrived: the locale
         catalogue comes from the Electron main process, so outside it there is
         genuinely nothing to choose between and a blank select would read as a
         fault. It appears on its own when the load resolves. -->
    <section v-if="availableLocales.length" class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">translate</span>
        {{ t('settings.language') }}
      </label>
      <select class="settings-select" :value="currentLocale" @change="onLocaleChange">
        <!-- Native names, always: somebody hunting for their own language
             cannot read the one currently on screen. -->
        <option v-for="l in availableLocales" :key="l.code" :value="l.code">{{ l.name }}</option>
      </select>
      <p class="settings-help">{{ t('settings.languageHelp') }}</p>
    </section>

    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">contrast</span>
        {{ t('settings.themeMode') }}
      </label>
      <select class="settings-select" :value="themeMode" @change="onThemeModeChange">
        <option value="dark">{{ t('settings.themeModeDark') }}</option>
        <option value="light">{{ t('settings.themeModeLight') }}</option>
      </select>
      <p class="settings-help">{{ t('settings.themeModeHelp') }}</p>
    </section>

    <!-- The swatch grid from the retired picker dialog, with one addition: it
         now shows which colour is in force. The dialog could not, because it
         only existed while you were choosing. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">palette</span>
        {{ t('settings.accentColor') }}
      </label>
      <div class="accent-grid">
        <button
          v-for="color in ACCENT_COLORS"
          :key="color"
          type="button"
          class="accent-swatch"
          :class="{ current: isCurrentAccent(color) }"
          :style="{ backgroundColor: color }"
          :aria-label="t('settings.accentColorSwatch', { color })"
          :aria-pressed="isCurrentAccent(color)"
          @click="setTheme({ accentColor: color })"
        ></button>
      </div>
      <p class="settings-help">{{ t('settings.accentColorHelp') }}</p>
    </section>

    <!-- Relocated from the Audio pane. U4 made the unit this operator's rather
         than the show's and moved the plumbing; the control stayed behind. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">straighten</span>
        {{ t('settings.meterMode') }}
      </label>
      <select class="settings-select" :value="meterMode" @change="onMeterModeChange">
        <option value="LUFS">{{ t('settings.meterModeLufs') }}</option>
        <option value="dBFS">{{ t('settings.meterModeDbfs') }}</option>
        <option value="dBTP">{{ t('settings.meterModeDbtp') }}</option>
        <option value="RMS">{{ t('settings.meterModeRms') }}</option>
      </select>
      <p class="settings-help">{{ t('settings.meterModeHelp') }}</p>
    </section>

    <!-- Relocated from the Project pane, same reason: whose playlist chases the
         playing cue is a property of the person reading it. -->
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
// Everything on this pane belongs to the PERSON at the desk (U4), so nothing
// here goes through useProjectSettings and nothing here dirties the show file.
// usePreferences() is the one funnel: it writes the machine store and pushes
// the signed-in operator's profile, and the detached cart and mixer windows
// follow through `prefs_changed`.
//
// meterMode is the one value on this pane that reaches the audio thread — the
// server gates the true-peak / loudness DSP on the UNION of what every
// connected session asks for. U4 admitted that against R2 because the union can
// only reach a state one project setting could already reach alone.
import { useOutputTarget } from '~/composables/useOutputTarget';

// `availableLocales` had no caller anywhere in the client until now — the
// Electron menu was the only way to change language, which is precisely the
// split this page exists to end.
const { t, currentLocale, setLocale, availableLocales } = useLocalization();
const { theme, setTheme, scrollToPlaying, setScrollToPlaying, setMeterMode } = usePreferences();
// The effective unit, which is "their choice, else the project's output target
// recommends one" — only useOutputTarget can resolve that, so the control shows
// its answer and writes the preference.
const { meterMode } = useOutputTarget();

// Moved here from app.vue, which held it for the accent picker dialog this pane
// replaces. This is now the only place it is read.
const ACCENT_COLORS = [
  '#0f62fe', '#0353e9', '#002d9c', // Blues
  '#da1e28', '#a2191f', '#750e13', // Reds
  '#24a148', '#198038', '#0e6027', // Greens
  '#f1c21b', '#d2a106', '#b28600', // Yellows
  '#8a3ffc', '#6929c4', '#491d8b', // Purples
  '#ff7eb6', '#ee5396', '#d02670', // Pinks
];

const themeMode = computed(() => theme.value.mode);
// Case-insensitively, because a value that arrived from a profile written by an
// older build may not match the casing of the list.
const isCurrentAccent = (color: string) =>
  (theme.value.accentColor || '').toLowerCase() === color.toLowerCase();

function onLocaleChange(e: Event) {
  setLocale((e.target as HTMLSelectElement).value);
}
function onThemeModeChange(e: Event) {
  setTheme({ mode: (e.target as HTMLSelectElement).value as 'light' | 'dark' });
}
function onMeterModeChange(e: Event) {
  setMeterMode((e.target as HTMLSelectElement).value);
}
function onScrollToPlayingChange(e: Event) {
  setScrollToPlaying((e.target as HTMLInputElement).checked);
}
</script>

<style scoped>
.accent-grid {
  display: grid;
  grid-template-columns: repeat(6, minmax(0, 1fr));
  gap: var(--spacing-sm, 8px);
}

.accent-swatch {
  aspect-ratio: 1;
  min-width: 0;
  border: 2px solid var(--color-border);
  border-radius: var(--border-radius-sm, 4px);
  cursor: pointer;
  padding: 0;
  transition: transform var(--transition-fast, 0.15s), border-color 0.15s;
}

.accent-swatch:hover {
  transform: scale(1.08);
  border-color: var(--color-text-primary);
}

/* A ring rather than a tick: a check mark would need a contrasting colour on
   eighteen different grounds, and half of them would swallow it. */
.accent-swatch.current {
  border-color: var(--color-text-primary);
  box-shadow: 0 0 0 2px var(--color-background), 0 0 0 4px var(--color-text-primary);
}

.accent-swatch:focus-visible {
  outline: 2px solid var(--color-text-primary);
  outline-offset: 2px;
}

@media (prefers-reduced-motion: reduce) {
  .accent-swatch {
    transition: none;
  }
  .accent-swatch:hover {
    transform: none;
  }
}
</style>
