<template>
  <div class="routing-panel">
    <header>
      <h3>Routing — {{ cue?.display_name || cueId }}</h3>
      <small v-if="cue?.source_channels != null">
        {{ cue.source_channels }} source channel{{ cue.source_channels === 1 ? '' : 's' }}
        <span v-if="cue.ltc?.enabled"> (incl. LTC)</span>
      </small>
    </header>

    <!--
      Three-stage routing UI:
        1. For each source channel of this cue, pick destination mixer
           channels (with per-route gain).
        2. For each mixer channel referenced above, pick master-bus
           destinations (with per-route gain).
        3. For each master-bus destination, pick (device, hardware channel).
      LTC controls are at the bottom — toggle, frame rate, offset.
    -->

    <!-- Step 1: source channels → mixer channels --------------------- -->
    <section>
      <h4>Source channels → Mixer channels</h4>
      <table>
        <thead>
          <tr>
            <th>Source ch.</th>
            <th>Send to mixer</th>
            <th>Gain (dB)</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="i in cue?.source_channels ?? 0" :key="`src-${i - 1}`">
            <td><code>{{ i - 1 }}{{ isLtcChannel(i - 1) ? ' (LTC)' : '' }}</code></td>
            <td>
              <select v-model="addRouteSelection[i - 1]">
                <option :value="''" disabled>— choose —</option>
                <option v-for="mc in server.mixerChannels" :key="mc.id" :value="mc.id">
                  {{ mc.display_name }}
                </option>
              </select>
            </td>
            <td>
              <input v-model.number="addRouteGain[i - 1]" type="number" step="0.5" value="0" />
            </td>
            <td>
              <button class="btn small primary"
                      :disabled="!addRouteSelection[i - 1]"
                      @click="addItemToMixer(i - 1)">Add</button>
            </td>
          </tr>
        </tbody>
      </table>
    </section>

    <!-- Step 2: mixer channel → master channel ----------------------- -->
    <section>
      <h4>Mixer channels → Master bus</h4>
      <div v-for="mc in server.mixerChannels" :key="mc.id" class="mixer-row">
        <div class="mixer-row__head">
          <strong>{{ mc.display_name }}</strong>
          <small class="dim">{{ mc.id }}</small>
        </div>
        <div class="mixer-row__action">
          Master channel:
          <input v-model.number="mixerToMasterDraft[mc.id]" type="number" min="0" max="63" />
          Gain (dB):
          <input v-model.number="mixerToMasterGainDraft[mc.id]" type="number" step="0.5" />
          <button class="btn small primary" @click="addMixerToMaster(mc.id)">Wire</button>
        </div>
      </div>
    </section>

    <!-- Step 3: master channel → device + hw channel ----------------- -->
    <section>
      <h4>Master bus → Hardware</h4>
      <table>
        <thead>
          <tr>
            <th>Master ch.</th>
            <th>Device</th>
            <th>Hardware ch.</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="m in 8" :key="`m-${m - 1}`">
            <td><code>{{ m - 1 }}</code></td>
            <td>
              <select v-model="masterAssignDevice[m - 1]">
                <option :value="''" disabled>— choose —</option>
                <option v-for="d in server.devices" :key="d.id" :value="d.id">
                  {{ d.display_name }} ({{ d.channel_count }} ch)
                </option>
              </select>
            </td>
            <td>
              <input v-model.number="masterAssignHw[m - 1]" type="number" min="0" max="63" value="0" />
            </td>
            <td>
              <button class="btn small primary"
                      :disabled="!masterAssignDevice[m - 1]"
                      @click="assignMaster(m - 1)">Assign</button>
            </td>
          </tr>
        </tbody>
      </table>
    </section>

    <!-- Step 4: LTC ------------------------------------------------- -->
    <section>
      <h4>LTC (SMPTE Linear Timecode)</h4>
      <label>
        <input type="checkbox" :checked="ltcEnabled" @change="toggleLtc(($event.target as HTMLInputElement).checked)" />
        Generate LTC for this cue
      </label>
      <div class="ltc-grid">
        <label>
          Frame rate
          <select v-model.number="ltcFps" :disabled="!ltcEnabled" @change="commitLtc()">
            <option :value="0">24 fps</option>
            <option :value="1">25 fps</option>
            <option :value="2">29.97 NDF</option>
            <option :value="3">29.97 DF</option>
            <option :value="4">30 fps</option>
          </select>
        </label>
        <label>
          Offset (ms)
          <input v-model.number="ltcOffsetMs" :disabled="!ltcEnabled" type="number" @change="commitLtc()" />
        </label>
      </div>
      <p class="hint">
        LTC is generated as an extra source channel appended after the file's audio
        tracks. Route it to a master output above to send it to a hardware channel.
      </p>
    </section>
  </div>
</template>

<!--
  RoutingMatrixPanel.vue
  -----------------------------------------------------------------------
  Per-cue routing UI. Backs onto the C++ server's /api/routing/* endpoints.
  Replaces the legacy "stereo speaker pair" assumption in the 1.x cue
  properties panel.
-->
<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { useLiveplayServer } from '~/composables/useLiveplayServer';

const props = defineProps<{
  cueId: string;
}>();

const server = useLiveplayServer();
const cue = computed(() => server.cues.find(c => c.id === props.cueId));

// Initial fetch when the panel mounts (and on cue change).
function refresh() {
  server.fetchMixerChannels();
  server.fetchDevices();
  server.fetchCues();
}
watch(() => props.cueId, refresh, { immediate: true });

const addRouteSelection = reactive<Record<number, string>>({});
const addRouteGain      = reactive<Record<number, number>>({});
const mixerToMasterDraft     = reactive<Record<string, number>>({});
const mixerToMasterGainDraft = reactive<Record<string, number>>({});
const masterAssignDevice = reactive<Record<number, string>>({});
const masterAssignHw     = reactive<Record<number, number>>({});

function isLtcChannel(idx: number): boolean {
  const c = cue.value;
  if (!c?.ltc?.enabled) return false;
  // The server appends LTC after the file's channels.
  return idx === (c.source_channels ?? 0) - 1;
}

async function addItemToMixer(sourceCh: number) {
  const mixerId = addRouteSelection[sourceCh];
  if (!mixerId) return;
  await server.routeItemToMixer(props.cueId, sourceCh,
                                 mixerId, addRouteGain[sourceCh] ?? 0);
  addRouteSelection[sourceCh] = '';
}

async function addMixerToMaster(mixerId: string) {
  const master = mixerToMasterDraft[mixerId];
  if (master == null || master < 0) return;
  await server.routeMixerToMaster(mixerId, master,
                                   mixerToMasterGainDraft[mixerId] ?? 0);
}

async function assignMaster(masterCh: number) {
  const device = masterAssignDevice[masterCh];
  if (!device) return;
  await server.assignMasterToDevice(masterCh, device, masterAssignHw[masterCh] ?? 0);
}

// LTC ----------------------------------------------------------------
const ltcEnabled = computed(() => cue.value?.ltc?.enabled ?? false);
const ltcFps     = ref<number>(cue.value?.ltc?.fps ?? 4);
const ltcOffsetMs = ref<number>(
  Math.round((cue.value?.ltc?.offset_ns ?? 0) / 1_000_000));

watch(() => cue.value?.ltc, l => {
  if (!l) return;
  ltcFps.value      = l.fps;
  ltcOffsetMs.value = Math.round(l.offset_ns / 1_000_000);
}, { immediate: true });

async function toggleLtc(enabled: boolean) {
  await server.setCueLtc(props.cueId, enabled, ltcFps.value,
                          ltcOffsetMs.value * 1_000_000);
  await server.fetchCues();
}
async function commitLtc() {
  if (!ltcEnabled.value) return;
  await server.setCueLtc(props.cueId, true, ltcFps.value,
                          ltcOffsetMs.value * 1_000_000);
}
</script>

<style lang="scss" scoped>
.routing-panel {
  display: flex;
  flex-direction: column;
  gap: 16px;
  color: #ddd;
  font-size: 13px;

  header { display: flex; flex-direction: column; gap: 2px; }
  h3 { margin: 0; font-size: 16px; }
  h4 { margin: 0 0 4px; font-size: 13px; color: #9ec5ff; }
  small.dim { color: #888; font-family: var(--font-mono); }

  section {
    background: #181818;
    border: 1px solid #2a2a2a;
    border-radius: 6px;
    padding: 10px 12px;
  }

  table {
    width: 100%;
    border-collapse: collapse;
    th, td {
      text-align: left;
      padding: 4px 6px;
      border-bottom: 1px solid #222;
      font-size: 12px;
    }
    th { color: #888; font-weight: 500; }
    code { font-family: var(--font-mono); color: #ffd58a; }
    input, select {
      width: 100%;
      background: #1d1d1d;
      border: 1px solid #333;
      color: #eee;
      padding: 3px 6px;
      border-radius: 3px;
    }
  }

  .mixer-row {
    border-top: 1px solid #2a2a2a;
    padding: 6px 0;
    &:first-child { border-top: none; }
    &__head { display: flex; justify-content: space-between; }
    &__action {
      display: flex;
      gap: 6px;
      align-items: center;
      margin-top: 4px;
      input { width: 60px; }
    }
  }

  .ltc-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 8px;
    margin-top: 6px;
    label { display: flex; flex-direction: column; gap: 2px; font-size: 12px; color: #aaa; }
    select, input {
      background: #1d1d1d; border: 1px solid #333; color: #eee;
      padding: 4px 6px; border-radius: 3px;
    }
  }
  .hint { color: #888; font-size: 11px; margin: 4px 0 0; }

  .btn {
    background: #2a2a2a;
    border: 1px solid #3a3a3a;
    border-radius: 4px;
    padding: 4px 10px;
    color: #ddd;
    cursor: pointer;
    &:hover:not(:disabled) { background: #353535; }
    &:disabled { opacity: 0.5; cursor: not-allowed; }
    &.primary  { background: #2a5e9a; border-color: #2a5e9a; }
    &.small    { padding: 2px 8px; font-size: 12px; }
  }
}
</style>
