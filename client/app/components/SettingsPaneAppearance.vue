<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionAppearance') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionAppearanceHelp') }}</p>

    <!-- ================================================================
         This screen's own choices. The language is kept in this computer's
         storage (useLocalization), not in the operator's profile, so it sits
         with the layout values rather than with the theme below.
         ================================================================ -->
    <h4 class="settings-group-title">
      {{ t('settings.groupLanguageLayout') }}
      <SettingsScope scope="computer" />
    </h4>

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

    <!-- The mixer's layout, from the one-setting Mixer pane this replaced. It
         is the MACHINE's value, not the person's: "fill the workspace" is right
         on the console display and wrong on the laptop carried to rehearsal,
         so useMixerView stores it with the workspace layout on this computer.
         Nothing here dirties the show file or reaches the audio thread. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">dashboard</span>
        {{ t('settings.mixerView') }}
      </label>
      <select class="settings-select" :value="mixerView" @change="onMixerViewChange">
        <option value="side">{{ t('settings.mixerViewSide') }}</option>
        <option value="full">{{ t('settings.mixerViewFull') }}</option>
        <!-- Offered but disabled outside Electron rather than hidden: a missing
             third choice reads as a mixer that cannot be undocked at all, when
             the truth is that this surface has no second window to put it in. -->
        <option value="window" :disabled="!canDetach">{{ t('settings.mixerViewWindow') }}</option>
      </select>
      <p class="settings-help">
        {{ canDetach ? t('settings.mixerViewHelp') : t('settings.mixerViewNoWindow') }}
      </p>
    </section>

    <!-- Which tab this desk shows is its own, unless it chooses to follow.
         Kept on this computer, not in the account: one person at two desks
         can have each follow someone different. The group's badge says so. -->
    <section class="settings-field">
      <label class="settings-label" for="follow-operator">
        <span class="material-symbols-rounded" aria-hidden="true">visibility</span>
        {{ t('tabs.followSetting') }}
      </label>
      <select
        id="follow-operator"
        class="settings-select"
        :value="followValue"
        @change="onFollowChange"
      >
        <option value="">{{ t('tabs.followOff') }}</option>
        <option v-for="v in followOptions" :key="v.id" :value="String(v.id)">{{ optionLabel(v) }}</option>
        <option v-if="followLost" :value="LOST">
          {{ followLabel }} ({{ t('tabs.followOffline') }})
        </option>
      </select>
      <p class="settings-help">{{ t('tabs.followHelp') }} {{ t('tabs.followThisComputer') }}</p>
    </section>

    <!-- ================================================================
         The person's own (U4): these follow the operator to whatever desk
         they sign in at, through usePreferences.
         ================================================================ -->
    <h4 class="settings-group-title">
      {{ t('settings.groupDisplay') }}
      <SettingsScope scope="you" />
    </h4>

    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">contrast</span>
        {{ t('settings.themeMode') }}
      </label>
      <select class="settings-select" :value="themeMode" @change="onThemeModeChange">
        <option value="dark">{{ t('settings.themeModeDark') }}</option>
        <option value="light">{{ t('settings.themeModeLight') }}</option>
        <option value="system">{{ t('settings.themeModeSystem') }}</option>
      </select>
      <!-- On "system", say which one that is right now. Otherwise the only way
           to find out is to look at the screen and guess whether it is
           following or has simply been set that way. -->
      <p class="settings-help">
        {{ themeMode === 'system'
             ? t('settings.themeModeSystemNow', {
                 mode: resolvedThemeMode === 'dark' ? t('settings.themeModeDark')
                                                    : t('settings.themeModeLight') })
             : t('settings.themeModeHelp') }}
      </p>
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
          :style="{ backgroundColor: swatchColor(color) }"
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
// Nothing on this pane goes through useProjectSettings and nothing here dirties
// the show file. Two owners, one group each:
//   * Display: the PERSON at the desk (U4). usePreferences() is the one funnel:
//     it writes the machine store and pushes the signed-in operator's profile,
//     and the detached cart and mixer windows follow through `prefs_changed`.
//   * Language and layout: THIS COMPUTER. The language (useLocalization's
//     localStorage), the mixer view (useMixerView, stored with the workspace
//     layout) and which operator this desk follows (useShowTabs).
//
// meterMode is the one value on this pane that reaches the audio thread — the
// server gates the true-peak / loudness DSP on the UNION of what every
// connected session asks for. U4 admitted that against R2 because the union can
// only reach a state one project setting could already reach alone.
import type { Theme } from '~/types/project';
import type { ClientView } from '~/composables/useShowTabs';
import { ACCENT_COLORS, accentForTheme } from '~/utils/accentPalette';
import { useOutputTarget } from '~/composables/useOutputTarget';
import SettingsScope from './SettingsScope.vue';

// `availableLocales` had no caller anywhere in the client until now — the
// Electron menu was the only way to change language, which is precisely the
// split this page exists to end.
const { t, currentLocale, setLocale, availableLocales } = useLocalization();
const { followOptions, followLabel, followLost, followedView, setFollowing } = useShowTabs();

// The list is by session: one person signed in at two desks is two entries
// with the same name, told apart by their session number.
const LOST = '__offline';
const followValue = computed(() =>
  followedView.value ? String(followedView.value.id) : followLost.value ? LOST : '');
const optionLabel = (v: ClientView) =>
  followOptions.value.filter(o => o.label === v.label).length > 1
    ? t('tabs.followSession', { name: v.label, n: v.id })
    : v.label;
function onFollowChange(e: Event) {
  const value = (e.target as HTMLSelectElement).value;
  if (value === LOST) return;
  if (!value) { setFollowing(null); return; }
  const view = followOptions.value.find(v => String(v.id) === value);
  if (view) setFollowing(view);
}
const { theme, resolvedThemeMode, setTheme, scrollToPlaying, setScrollToPlaying,
        setMeterMode } = usePreferences();
// The effective unit, which is "their choice, else the project's output target
// recommends one" — only useOutputTarget can resolve that, so the control shows
// its answer and writes the preference.
const { meterMode } = useOutputTarget();

// The swatches live in utils/accentPalette.ts with the contrast rules that
// decide how each is drawn. A swatch shows its colour AS DRAWN in the current
// theme, so what the operator picks is what the interface then wears.
const swatchColor = (color: string) => accentForTheme(color, resolvedThemeMode.value).accent;

const themeMode = computed(() => theme.value.mode);
// Case-insensitively, because a value that arrived from a profile written by an
// older build may not match the casing of the list.
const isCurrentAccent = (color: string) =>
  (theme.value.accentColor || '').toLowerCase() === color.toLowerCase();

function onLocaleChange(e: Event) {
  setLocale((e.target as HTMLSelectElement).value);
}
function onThemeModeChange(e: Event) {
  setTheme({ mode: (e.target as HTMLSelectElement).value as Theme['mode'] });
}
function onMeterModeChange(e: Event) {
  setMeterMode((e.target as HTMLSelectElement).value);
}
function onScrollToPlayingChange(e: Event) {
  setScrollToPlaying((e.target as HTMLInputElement).checked);
}

// The mixer view, moved here from its own pane in 2.6. A view over
// useWorkspaceLayout, so picking "fill the workspace" here is exactly pressing
// the mixer's Expand button.
const { mixerView, canDetach, setMixerView } = useMixerView();
function onMixerViewChange(e: Event) {
  setMixerView((e.target as HTMLSelectElement).value as 'side' | 'full' | 'window');
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
   twenty-four different grounds, and half of them would swallow it. */
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
