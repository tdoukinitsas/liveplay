<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionMixer') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionMixerHelp') }}</p>

    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">dashboard</span>
        {{ t('settings.mixerView') }}
      </label>
      <select class="settings-select" :value="mixerView" @change="onViewChange">
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
  </div>
</template>

<script setup lang="ts">
// The mixer's layout, and the first pane whose value belongs to the MACHINE
// rather than to the person or the show.
//
// That is the whole reason this is not on the Appearance pane beside the theme:
// Appearance is U4's tier, where every value follows the operator to whatever
// desk they sign in at. A mixer view must not. "Fill the workspace" is right on
// the console display and wrong on the laptop carried to rehearsal, so it is
// stored per-device — see useMixerView, and useUiMode before it, which puts Show
// Mode on the same tier for the same reason.
//
// Nothing here dirties the show file and nothing here reaches the audio thread:
// no useProjectSettings, no saveProject, and the only writes are to the machine
// store and the view flags the workspace already renders from.
const { t } = useLocalization();
const { mixerView, canDetach, setMixerView } = useMixerView();

function onViewChange(e: Event) {
  setMixerView((e.target as HTMLSelectElement).value as 'side' | 'full' | 'window');
}
</script>
