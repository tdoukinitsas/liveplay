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

    <!-- LTC output (timecode). A logical output name like any bus target
         (D38): the show says where its timecode goes, the machine's output
         map says what that means here. Picking a device names it directly,
         which works on this machine and is the thing to replace with a mapped
         name before the show travels. -->
    <section class="settings-field">
      <label class="settings-label">
        <span class="material-symbols-rounded">schedule</span>
        {{ t('settings.ltcOutput') }}
      </label>
      <select class="settings-select" :value="ltcOutput" @change="onLtcOutputChange">
        <option value="">{{ t('settings.noneSelected') }}</option>
        <optgroup v-if="outputNames.length" :label="t('settings.ltcOutputGroupOutputs')">
          <option v-for="n in outputNames" :key="`o:${n}`" :value="n">{{ n }}</option>
        </optgroup>
        <optgroup v-if="devices.length" :label="t('settings.ltcOutputGroupDevices')">
          <option v-for="d in devices" :key="`d:${d.name}`" :value="d.name">{{ d.name }}</option>
        </optgroup>
        <!-- What the project asks for, when this machine offers neither a
             mapping nor the hardware. Kept on the list so simply opening the
             settings page cannot quietly repoint someone else's show. -->
        <optgroup v-if="ltcOutputMissing" :label="t('settings.ltcOutputGroupUnavailable')">
          <option :value="ltcOutput">{{ ltcOutput }}</option>
        </optgroup>
      </select>
      <p class="settings-help">
        {{ ltcOutputMissing ? t('settings.ltcOutputUnavailableHelp', { name: ltcOutput })
                            : t('settings.ltcOutputHelp') }}
      </p>
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

// The machine's logical outputs, built-ins included — the portable half of
// the LTC picker. Fetched rather than watched: the output map only changes
// from the Hardware Outputs pane, and this list is re-read on mount.
const outputNames = ref<string[]>([]);

const ltcOutput = computed(() => settings.value.ltcOutput || '');
// The project names an output this machine can neither map nor match to
// present hardware, so timecode is silent until one of those is true.
const ltcOutputMissing = computed(() =>
  !!ltcOutput.value &&
  !outputNames.value.includes(ltcOutput.value) &&
  !devices.value.some(d => d.name === ltcOutput.value));
const outputTarget = computed(() => settings.value.outputTarget || 'ebu-r128');
const disableLimiter = computed(() => !!settings.value.disableLimiter);
const { meterMode: currentMeterMode } = useOutputTarget();
const meterMode = computed(() => settings.value.meterMode || currentMeterMode.value);
const meterBallistics = computed(() => settings.value.meterBallistics || 'digital-ppm');

// Both lists back the LTC select, so load them whenever this pane is shown
// rather than only at app start.
onMounted(async () => {
  try { await server.fetchDevices(); } catch { /* connection may not be ready yet */ }
  try {
    const map = await server.fetchOutputs();
    const named = (map?.outputs ?? []).map(o => o.name);
    // Built-ins first, matching the order the strip's output picker uses, and
    // without repeating a built-in that also has a real mapping.
    const builtin = (map?.builtin ?? []).filter(n => !named.includes(n));
    outputNames.value = [...builtin, ...named];
  } catch { /* an unreachable server just leaves the device list */ }
});

function onLtcOutputChange(e: Event) {
  applyPatch({ ltcOutput: (e.target as HTMLSelectElement).value || null });
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
