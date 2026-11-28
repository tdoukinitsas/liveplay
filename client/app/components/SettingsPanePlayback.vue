<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionPlayback') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionPlaybackIntro') }}</p>

    <!-- ================================================================
         GO and Up Next: what the transport does between cues.
         ================================================================ -->
    <h4 class="settings-group-title">
      {{ t('settings.groupGoUpNext') }}
      <SettingsScope scope="show" />
    </h4>

    <!-- R2-08: with several cues on air, which one GO and Up Next follow. The
         server's GO (control surfaces, MIDI) uses the same setting. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">skip_next</span>
        {{ t('settings.goContinuesFrom') }}
      </label>
      <select
        class="settings-select"
        :value="goContinuesFrom"
        @change="onGoContinuesFromChange"
      >
        <option value="earliest">{{ t('settings.goContinuesFromEarliest') }}</option>
        <option value="latest">{{ t('settings.goContinuesFromLatest') }}</option>
      </select>
      <p class="settings-help">{{ t('settings.goContinuesFromHelp') }}</p>
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

    <!-- The header's "Silence in" countdown: how long until nothing is on
         air, which is the other half of what Up Next is for. -->
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

    <!-- ================================================================
         Stop All.
         ================================================================ -->
    <h4 class="settings-group-title">
      {{ t('controls.stopAll') }}
      <SettingsScope scope="show" />
    </h4>

    <!-- Stored in ms, shown in seconds. force_fade on the server, so this
         beats every per-track fade (unless the option below is on). -->
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

    <!-- #85: each stopped cue leaves over its own Stop button fade, for Stop
         All and for "stop all others" ducking. Panic still cuts. -->
    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input
          type="checkbox"
          :checked="stopAllUsesItemFade"
          @change="onStopAllUsesItemFadeChange"
        />
        {{ t('settings.stopAllUsesItemFade') }}
      </label>
      <p class="settings-help">{{ t('settings.stopAllUsesItemFadeHelp') }}</p>
    </section>

    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input
          type="checkbox"
          :checked="stopAllStopsPreview"
          @change="onStopAllStopsPreviewChange"
        />
        {{ t('settings.stopAllStopsPreview') }}
      </label>
      <p class="settings-help">{{ t('settings.stopAllStopsPreviewHelp') }}</p>
    </section>

    <!-- #23: the cues Stop All stops fire their "On end" events only when this
         is on; the double-tap panic never fires any (the server decides). -->
    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input
          type="checkbox"
          :checked="stopAllFiresEvents"
          @change="onStopAllFiresEventsChange"
        />
        {{ t('settings.stopAllFiresEvents') }}
      </label>
      <p class="settings-help">{{ t('settings.stopAllFiresEventsHelp') }}</p>
    </section>

    <!-- ================================================================
         New cues: what an imported track starts out as. Every cue can still
         be changed on its own afterwards, in its Properties.
         ================================================================ -->
    <h4 class="settings-group-title">
      {{ t('settings.groupNewCues') }}
      <SettingsScope scope="show" />
    </h4>

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
          :checked="disableAutoVolumeAndTrim"
          @change="onDisableAutoVolumeAndTrimChange"
        />
        {{ t('settings.disableAutoVolumeAndTrim') }}
      </label>
      <p class="settings-help">{{ t('settings.disableAutoVolumeAndTrimHelp') }}</p>
    </section>

    <!-- ================================================================
         YouTube downloads (moved here from Audio in 2.6, beside the other
         import defaults). Unlike everything above this is not the show's: it
         is this machine's yt-dlp doing the download, so it is saved beside the
         server connection in the app's own settings, and only the desktop app
         has it. Saved on change, like the rest of this pane.
         ================================================================ -->
    <template v-if="yt">
      <h4 class="settings-group-title">
        {{ t('settings.youtubeImport') }}
        <SettingsScope scope="computer" />
      </h4>

      <section class="settings-field">
        <label class="settings-label">
          <span class="material-symbols-rounded">download</span>
          {{ t('settings.youtubeFormat') }}
        </label>
        <select class="settings-select" :value="yt.format" @change="onYtChange('format', ($event.target as HTMLSelectElement).value as 'wav' | 'mp3')">
          <option value="wav">{{ t('settings.youtubeFormatWav') }}</option>
          <option value="mp3">{{ t('settings.youtubeFormatMp3') }}</option>
        </select>
        <label class="settings-label">{{ t('settings.youtubeSampleRate') }}</label>
        <select class="settings-select" :value="yt.sampleRate" @change="onYtChange('sampleRate', Number(($event.target as HTMLSelectElement).value))">
          <option v-for="r in ytRates" :key="r" :value="r">{{ formatRate(r) }}</option>
        </select>
        <template v-if="yt.format === 'mp3'">
          <label class="settings-label">{{ t('settings.youtubeBitrate') }}</label>
          <select class="settings-select" :value="yt.mp3Bitrate" @change="onYtChange('mp3Bitrate', Number(($event.target as HTMLSelectElement).value))">
            <option v-for="b in ytBitrates" :key="b" :value="b">{{ b }} kbps</option>
          </select>
        </template>
        <p class="settings-help">{{ t('settings.youtubeImportHelp') }}</p>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
import { useProjectSettings } from '~/composables/useProjectSettings';
import SettingsScope from './SettingsScope.vue';

const { t } = useLocalization();
const { settings, applyPatch } = useProjectSettings();

const defaultTransitionMode = computed(() => settings.value.defaultTransitionMode || 'crossfade');
// Absent = the cue fired first, as 2.5 behaved (R2-08).
const goContinuesFrom = computed(() => settings.value.goContinuesFrom === 'latest' ? 'latest' : 'earliest');
const disableAutoVolumeAndTrim = computed(() => !!settings.value.disableAutoVolumeAndTrim);
const disableSilenceWarning = computed(() => !!settings.value.disableSilenceWarning);
// Defaults ON (undefined → true) so legacy and new projects both arm the next
// item as "Up Next" for cues without an end behaviour. (#28)
const autoCueNextWithoutEndBehavior = computed(
  () => settings.value.autoCueNextWithoutEndBehavior !== false
);
// Defaults ON: Stop All is a panic button. (#60)
const stopAllStopsPreview = computed(() => settings.value.stopAllStopsPreview !== false);
// Defaults OFF: Stop All silences events too.
const stopAllFiresEvents = computed(() => settings.value.stopAllFiresEvents === true);
// Defaults OFF: one Stop All fade for every cue. (#85)
const stopAllUsesItemFade = computed(() => settings.value.stopAllUsesItemFade === true);
const stopAllFadeSeconds = computed(() => {
  const ms = settings.value.stopAllFadeMs;
  return (typeof ms === 'number' ? ms : 1000) / 1000;
});

function onDefaultTransitionModeChange(e: Event) {
  applyPatch({ defaultTransitionMode: (e.target as HTMLSelectElement).value });
}
function onGoContinuesFromChange(e: Event) {
  applyPatch({ goContinuesFrom: (e.target as HTMLSelectElement).value as 'earliest' | 'latest' });
}
function onAutoCueNextChange(e: Event) {
  applyPatch({ autoCueNextWithoutEndBehavior: (e.target as HTMLInputElement).checked });
}
function onStopAllFadeChange(e: Event) {
  const seconds = parseFloat((e.target as HTMLInputElement).value);
  const ms = Number.isFinite(seconds) ? Math.max(0, Math.round(seconds * 1000)) : 1000;
  applyPatch({ stopAllFadeMs: ms });
}
function onStopAllUsesItemFadeChange(e: Event) {
  applyPatch({ stopAllUsesItemFade: (e.target as HTMLInputElement).checked });
}
function onStopAllStopsPreviewChange(e: Event) {
  applyPatch({ stopAllStopsPreview: (e.target as HTMLInputElement).checked });
}
function onStopAllFiresEventsChange(e: Event) {
  applyPatch({ stopAllFiresEvents: (e.target as HTMLInputElement).checked });
}
function onDisableAutoVolumeAndTrimChange(e: Event) {
  applyPatch({ disableAutoVolumeAndTrim: (e.target as HTMLInputElement).checked });
}
function onDisableSilenceWarningChange(e: Event) {
  applyPatch({ disableSilenceWarning: (e.target as HTMLInputElement).checked });
}

// ---- YouTube downloads (desktop only; moved from the Audio pane) ----------
// The main process owns the defaults and the lists of valid values, so this
// pane offers exactly what it will accept.
const yt = ref<YouTubeImportSettings | null>(null);
const ytOptions = ref<{ sampleRates: Record<'wav' | 'mp3', number[]>; mp3Bitrates: number[] } | null>(null);
const ytRates = computed(() => (yt.value && ytOptions.value?.sampleRates[yt.value.format]) || []);
const ytBitrates = computed(() => ytOptions.value?.mp3Bitrates ?? []);
const formatRate = (hz: number) => `${(hz / 1000).toLocaleString(undefined, { maximumFractionDigits: 1 })} kHz`;

onMounted(async () => {
  const api = (globalThis as any).electronAPI;
  if (!api?.getYouTubeSettings) return;
  const { options, ...current } = await api.getYouTubeSettings();
  ytOptions.value = options;
  yt.value = current;
});

async function onYtChange<K extends keyof YouTubeImportSettings>(key: K, value: YouTubeImportSettings[K]) {
  const api = (globalThis as any).electronAPI;
  if (!api?.setYouTubeSettings) return;
  // The reply is what was saved: switching to MP3 can move a 96 kHz choice
  // down to one MP3 can carry.
  yt.value = await api.setYouTubeSettings({ [key]: value });
}
</script>
