<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionPlayback') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionPlaybackHelp') }}</p>

    <!-- Which transition a newly imported track defaults to. Every track can
         still be switched individually in its properties. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">multiple_stop</span>
        {{ t('settings.transitionMode') }}
      </label>
      <select
        class="settings-select"
        :value="defaultTransitionMode"
        @change="onDefaultTransitionModeChange"
      >
        <option value="crossfade">{{ t('settings.transitionModeCrossfade') }}</option>
        <option value="start-next">{{ t('settings.transitionModeStartNext') }}</option>
      </select>
      <p class="settings-help">{{ t('settings.transitionModeHelp') }}</p>
    </section>

    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input
          type="checkbox"
          :checked="autoCueNextWithoutEndBehavior"
          @change="onAutoCueNextChange"
        />
        {{ t('settings.autoCueNext') }}
      </label>
      <p class="settings-help">{{ t('settings.autoCueNextHelp') }}</p>
    </section>

    <!-- Stored in ms, shown in seconds. force_fade on the server, so this
         beats every per-track fade. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">timer</span>
        {{ t('settings.stopAllFade') }}
      </label>
      <input
        class="settings-input"
        type="number"
        min="0"
        step="0.1"
        :value="stopAllFadeSeconds"
        @change="onStopAllFadeChange"
      />
      <p class="settings-help">{{ t('settings.stopAllFadeHelp') }}</p>
    </section>

    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input
          type="checkbox"
          :checked="disableAutoVolumeAndTrim"
          @change="onDisableAutoVolumeAndTrimChange"
        />
        {{ t('settings.disableAutoVolumeAndTrim') }}
      </label>
    </section>

    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input
          type="checkbox"
          :checked="disableSilenceWarning"
          @change="onDisableSilenceWarningChange"
        />
        {{ t('settings.disableSilenceWarning') }}
      </label>
      <p class="settings-help">{{ t('settings.disableSilenceWarningHelp') }}</p>
    </section>
  </div>
</template>

<script setup lang="ts">
import { useProjectSettings } from '~/composables/useProjectSettings';

const { t } = useLocalization();
const { settings, applyPatch } = useProjectSettings();

const defaultTransitionMode = computed(() => settings.value.defaultTransitionMode || 'crossfade');
const disableAutoVolumeAndTrim = computed(() => !!settings.value.disableAutoVolumeAndTrim);
const disableSilenceWarning = computed(() => !!settings.value.disableSilenceWarning);
// Defaults ON (undefined → true) so legacy and new projects both arm the next
// item as "Up Next" for cues without an end behaviour. (#28)
const autoCueNextWithoutEndBehavior = computed(
  () => settings.value.autoCueNextWithoutEndBehavior !== false
);
const stopAllFadeSeconds = computed(() => {
  const ms = settings.value.stopAllFadeMs;
  return (typeof ms === 'number' ? ms : 1000) / 1000;
});

function onDefaultTransitionModeChange(e: Event) {
  applyPatch({ defaultTransitionMode: (e.target as HTMLSelectElement).value });
}
function onAutoCueNextChange(e: Event) {
  applyPatch({ autoCueNextWithoutEndBehavior: (e.target as HTMLInputElement).checked });
}
function onStopAllFadeChange(e: Event) {
  const seconds = parseFloat((e.target as HTMLInputElement).value);
  const ms = Number.isFinite(seconds) ? Math.max(0, Math.round(seconds * 1000)) : 1000;
  applyPatch({ stopAllFadeMs: ms });
}
function onDisableAutoVolumeAndTrimChange(e: Event) {
  applyPatch({ disableAutoVolumeAndTrim: (e.target as HTMLInputElement).checked });
}
function onDisableSilenceWarningChange(e: Event) {
  applyPatch({ disableSilenceWarning: (e.target as HTMLInputElement).checked });
}
</script>
