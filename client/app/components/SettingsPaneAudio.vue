<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionAudio') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionAudioHelp') }}</p>

    <!-- Routing does not live here. A show says which bus each cue plays
         through and what each bus sends to; that is the mixer's job (D30), and
         a project that names a sound card cannot travel between venues. This
         section only points the way. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">tune</span>
        {{ t('settings.routing') }}
      </label>
      <p class="settings-help">{{ t('settings.routingInMixer') }}</p>
      <div class="settings-actions">
        <Btn icon="tune" :text="t('settings.openMixer')" @click="openMixer" />
      </div>
    </section>

    <!-- LTC device (timecode output). Still a device name in the document
         (D21) — the one that has not moved to a logical output yet. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">schedule</span>
        {{ t('settings.ltcDevice') }}
      </label>
      <select class="settings-select" :value="ltcDeviceId" @change="onLtcDeviceChange">
        <option value="">{{ t('settings.noneSelected') }}</option>
        <option v-for="d in devices" :key="d.name" :value="d.name">{{ d.name }}</option>
      </select>
      <p class="settings-help">{{ t('settings.ltcDeviceHelp') }}</p>
    </section>

    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">speed</span>
        {{ t('settings.outputTarget') }}
      </label>
      <select class="settings-select" :value="outputTarget" @change="onOutputTargetChange">
        <option value="ebu-r128">{{ t('settings.outputTargetEbuR128') }}</option>
        <option value="streaming">{{ t('settings.outputTargetStreaming') }}</option>
        <option value="radio">{{ t('settings.outputTargetRadio') }}</option>
        <option value="netflix">{{ t('settings.outputTargetNetflix') }}</option>
        <option value="live">{{ t('settings.outputTargetLive') }}</option>
      </select>
      <p class="settings-help">{{ t('settings.outputTargetHelp') }}</p>
    </section>

    <section class="settings-field">
      <label class="settings-label settings-label--checkbox">
        <input type="checkbox" :checked="disableLimiter" @change="onDisableLimiterChange" />
        {{ t('settings.disableLimiter') }}
      </label>
      <p class="settings-help">{{ t('settings.disableLimiterHelp') }}</p>
    </section>

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

    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">monitoring</span>
        {{ t('settings.meterBallistics') }}
      </label>
      <select class="settings-select" :value="meterBallistics" @change="onMeterBallisticsChange">
        <option value="digital-ppm">{{ t('settings.meterBallisticsDigitalPpm') }}</option>
        <option value="ppm-i">{{ t('settings.meterBallisticsPpmI') }}</option>
        <option value="ppm-ii">{{ t('settings.meterBallisticsPpmII') }}</option>
        <option value="vu">{{ t('settings.meterBallisticsVu') }}</option>
        <option value="instant">{{ t('settings.meterBallisticsInstant') }}</option>
      </select>
      <p class="settings-help">{{ t('settings.meterBallisticsHelp') }}</p>
    </section>
  </div>
</template>

<script setup lang="ts">
import { useOutputTarget } from '~/composables/useOutputTarget';
import { useProjectSettings } from '~/composables/useProjectSettings';
import Btn from './Btn.vue';

const { t } = useLocalization();
const server = useLiveplayServer();
const { settings, applyPatch } = useProjectSettings();
const { close } = useSettingsPage();

const devices = computed(() => server.devices ?? []);

const ltcDeviceId = computed(() => settings.value.ltcDevice || '');
const outputTarget = computed(() => settings.value.outputTarget || 'ebu-r128');
const disableLimiter = computed(() => !!settings.value.disableLimiter);
const { meterMode: currentMeterMode } = useOutputTarget();
const meterMode = computed(() => settings.value.meterMode || currentMeterMode.value);
const meterBallistics = computed(() => settings.value.meterBallistics || 'digital-ppm');

// The device list backs the LTC select, so make sure it is loaded whenever
// this pane is shown rather than only at app start.
onMounted(async () => {
  try { await server.fetchDevices(); } catch { /* connection may not be ready yet */ }
});

function onLtcDeviceChange(e: Event) {
  applyPatch({ ltcDevice: (e.target as HTMLSelectElement).value || null });
}
function onOutputTargetChange(e: Event) {
  applyPatch({ outputTarget: (e.target as HTMLSelectElement).value });
}
function onDisableLimiterChange(e: Event) {
  applyPatch({ disableLimiter: (e.target as HTMLInputElement).checked });
}
function onMeterModeChange(e: Event) {
  applyPatch({ meterMode: (e.target as HTMLSelectElement).value });
}
function onMeterBallisticsChange(e: Event) {
  applyPatch({ meterBallistics: (e.target as HTMLSelectElement).value });
}

// The mixer is a workspace pane, not part of Settings: flip the shared flag
// ProjectHeader / MainWorkspace already watch, and get out of its way.
const mixerOpen = useState<boolean>('liveplay:mixerOpen', () => false);
function openMixer() {
  mixerOpen.value = true;
  close();
}
</script>
