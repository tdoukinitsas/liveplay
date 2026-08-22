<template>
  <!--
    The mixer: a bank of channel strips, one per bus, with the master pinned
    to the right.

    Strips are buses, not cues. A cue is transient — it starts and stops
    constantly during a show — so a mixer whose strips appeared and vanished
    mid-show would be unusable. What feeds a bus is shown in the channel
    details view instead.
  -->
  <div class="mixer">
    <!-- The same header as the playlist and the cart player: title left,
         labelled buttons right, identical padding and height so the three
         views line up across the workspace. It sits above both the rail and
         the channel view, which keeps its own bus header below this one. -->
    <header class="mixer-header">
      <h2>{{ t('mixer.title') }}</h2>
      <MixerActions
        class="mixer-header-actions"
        :mode="mode"
        :detached="detached"
        :can-detach="canDetach"
        :pfl-count="pflCount"
        @add="addBus"
        @detach="detach"
        @mode="$emit('mode', $event)"
        @close="$emit('close')"
        @clear-pfl="clearPfl"
        @output-map="outputMapOpen = true"
      />
    </header>

    <!-- The channel view replaces the rail rather than sharing the window with
         it. Splitting the height between the two left the strips half-height
         in the one mode that has room for them, and put the fader you were
         adjusting somewhere different from where you grabbed it. Here the rail
         is always full height, and opening a channel swaps to a view whose own
         left column is that channel. -->
    <MixerChannelDetails
      v-if="detailsBus"
      :bus="detailsBus"
      :buses="userBuses"
      :output-names="outputNames"
      @patch="onPatch"
      @delete="onDelete"
      @select="showChannel"
      @close="detailsId = ''"
      @open-output-map="outputMapOpen = true"
    />

    <div v-else class="mixer__body">
      <div class="mixer__strips">
        <MixerStrip
          v-for="bus in userBuses"
          :key="bus.id"
          :bus="bus"
          :selected="bus.id === selectedId"
          :touch="touch"
          :output-names="outputNames"
          @select="selectedId = $event"
          @open="openDetails"
          @patch="onPatch"
          @open-output-map="outputMapOpen = true"
        />
        <p v-if="userBuses.length === 0" class="mixer__empty">{{ t('mixer.empty') }}</p>
      </div>

      <!-- Monitor and master, pinned right — the same component as every
           other strip, so they have the same rows at the same heights and
           their faders line up with the rail's. The master was bespoke
           markup here, which is exactly why the one strip that matters most
           looked unlike all the others.

           Monitor is a real bus with a real fader — that fader is the
           headphone level — but it is where PFL lands rather than a channel
           anything can be assigned to, so it sits beside the master instead
           of in the assignable rail. -->
      <div class="mixer__master">
        <MixerStrip
          v-if="monitorBus"
          :bus="monitorBus"
          :touch="touch"
          :output-names="outputNames"
          monitor
          @patch="onPatch"
          @open-output-map="outputMapOpen = true"
        />
        <MixerStrip :bus="masterBus" :touch="touch" :output-names="[]" master />
      </div>
    </div>

    <!-- Outside the rail/details v-if branch so it works from either mode,
         and in the detached mixer window, which has its own socket but the
         same component tree. -->
    <OutputMapModal :open="outputMapOpen" @close="outputMapOpen = false" />
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import type { Bus } from '~/types/project';
import MixerStrip from './MixerStrip.vue';
import MixerChannelDetails from './MixerChannelDetails.vue';
import MixerActions from './MixerActions.vue';
import OutputMapModal from './OutputMapModal.vue';

const props = withDefaults(
  defineProps<{ mode?: 'side' | 'full'; detached?: boolean }>(),
  { mode: 'side', detached: false },
);
const emit = defineEmits<{ (e: 'close'): void; (e: 'mode', mode: 'side' | 'full'): void }>();

// Detaching is an Electron affordance — in a browser there is no second
// window to open, so the button simply isn't offered there.
const canDetach = computed(() =>
  import.meta.client && !!(window as any).electronAPI?.openMixerWindow);

// The main window flips its own panel off when it hears `mixer-window-opened`,
// so we don't touch mixerOpen here — that keeps the detach path identical
// whether the window is spawned from this button or reopened later.
function detach() {
  void (window as any).electronAPI?.openMixerWindow?.();
}

// Opening the channel view from a docked pane switches to full width first —
// it needs the whole window, and silently rendering it crushed was worse than
// not showing it.
function openDetails(id: string) {
  selectedId.value = id;
  detailsId.value  = id;
  if (props.mode !== 'full') emit('mode', 'full');
}

// The select row and the arrows move the channel view without leaving it.
function showChannel(id: string) {
  if (!id) return;
  selectedId.value = id;
  detailsId.value  = id;
}

const server = useLiveplayServer();
const { t } = useLocalization();
const { uiMode } = useUiMode();
const touch = computed(() => uiMode.value === 'playback');

// Shared rather than local: switching between docked and full swaps which
// MixerPanel instance is mounted, so a local ref would be destroyed with the
// old one and the selection would be lost exactly when opening details forces
// that switch — which looked like needing to click twice.
const selectedId  = useState<string>('liveplay:mixerSelectedBus', () => '');
// Which channel the channel view is showing; empty means the rail.
const detailsId   = useState<string>('liveplay:mixerDetailsBus', () => '');
const outputNames = ref<string[]>([]);
// Pure view state (invariant 1's one allowance): whether the output-map
// editor is open. Everything the modal shows and saves comes from the
// server, never from anything held here.
const outputMapOpen = ref(false);

// The master strip drives the *same* parameter as the transport bar's Main
// fader — the output-channel gain on masters 0/1 — rather than the engine's
// global master gain. Two faders both labelled master that moved independently
// was just confusing. outputChannelGains is reactive and kept live by the
// output_channel_gain_changed broadcast, so the two track each other in both
// directions and across clients.
const masterGainDb = computed(() => server.outputChannelGains[0] ?? 0);

// The master presented as a bus, so it can go through MixerStrip. Nothing on
// the server backs this — MixerStrip reads `master` and takes the output-pair
// path for the fader and the meter — but shaping it as a Bus keeps one strip
// component instead of two that drift apart.
const masterBus = computed<Bus>(() => ({
  id: '__master__',
  name: t('mixer.master'),
  color: '',
  order: Number.MAX_SAFE_INTEGER,
  width: 2,
  gainDb: masterGainDb.value,
  mute: false,
  pan: 0,
  pfl: false,
  monoCheck: false,
  dsp: {
    eqEnabled: true, dynEnabled: true,
    hpf: { freq: 20, q: 0.7071 }, lpf: { freq: 20000, q: 0.7071 },
    eq: [], gate: { on: false, threshold: -40, ratio: 2, range: -20,
                    attack: 1, hold: 10, release: 100 },
    comp: { on: false, threshold: -18, ratio: 4, makeup: 0,
            attack: 10, knee: 6, release: 200 },
    width: { width: 1, bassMonoHz: 20, bassMonoQ: 0.7071 },
  },
  bound: true,
  system: true,
  output: { type: 'output', target: '' },
  mixerId: '',
  itemUuids: [],
}));

const buses = computed<Bus[]>(() => server.buses ?? []);

// Only user buses get a strip. Main is where unassigned cues already land and
// Monitor is the PFL destination, which nothing can be assigned to until PFL
// exists — drawing either as a fader that does nothing would be a lie, and
// Monitor showing up as the lone strip on a fresh project reads as "the mixer
// is already set up" when it isn't.
//
// Deliberately NOT filtered on mixerId. A bus is a bus whether or not the
// engine currently has a strip for it; MixerStrip already renders the
// strip-less state. Hiding them meant a single fetch that caught the server
// mid-rebuild emptied the whole rail, and nothing refetched until the panel
// was remounted — which is why expanding or undocking appeared to "fix" it.
const userBuses = computed(() => buses.value.filter(b => !b.system));

// Monitor gets a strip of its own next to the master, because since PFL landed
// it has something to carry and a level worth reaching for. Absent only while
// the first fetch is in flight.
const monitorBus = computed(() => buses.value.find(b => b.id === 'monitor') ?? null);

// Anything currently in the phones. Drives the clear control, which exists
// because PFL is additive and silent about it: three channels tapped from
// three different windows sound like one muddled headphone mix with no single
// button lit to explain it.
const pflCount = computed(() => buses.value.reduce((n, b) => n + (b.pfl ? 1 : 0), 0));

async function clearPfl() {
  await server.clearAllPfl();
}

// The channel view takes the whole window, so it is only ever entered in full
// mode — a docked side pane has nowhere to put the processing panels.
//
// Which channel it shows is deliberately separate from which strip is
// highlighted. When they were the same value, any click on a strip yanked the
// whole window out of the rail and into the channel view; you now have to ask
// for it, via the strip's button or the select row at the bottom.
const detailsBus = computed(() =>
  props.mode === 'full'
    ? userBuses.value.find(b => b.id === detailsId.value) ?? null
    : null);

// Was fetched once at mount and never again — a remap from any client (this
// window's own modal included) left every strip's output <select> pointing
// at a stale name list until the panel was remounted. Refetching here, on
// the outputs_changed broadcast and on every WS reconnect, is what actually
// keeps it live; the modal owns its own copy of the map and is not the thing
// this depends on.
async function refreshOutputNames() {
  try {
    const map = await server.fetchOutputs();
    outputNames.value = (map?.outputs ?? []).map(o => o.name);
  } catch { /* offline; the next reconnect or outputs_changed retries */ }
}

const unsubDocPatch = server.onDocPatch((payload: any) => {
  if (payload?.op === 'outputs_changed') void refreshOutputNames();
});
const unsubReconnected = server.onReconnected(() => { void refreshOutputNames(); });
onBeforeUnmount(() => {
  unsubDocPatch();
  unsubReconnected();
});

onMounted(async () => {
  await server.fetchBuses();
  await refreshOutputNames();
});

async function onPatch(id: string, patch: Partial<Bus>) {
  await server.patchBus(id, patch);
}
async function onDelete(id: string) {
  await server.deleteBus(id);
  if (selectedId.value === id) selectedId.value = '';
  // Deleting the channel you are looking at drops you back to the rail rather
  // than leaving the view pointed at something that no longer exists.
  if (detailsId.value === id) detailsId.value = '';
}
async function addBus() {
  const id = await server.createBus({ name: t('mixer.newBusName'), width: 2 });
  selectedId.value = id;
  // Back to the rail, where the new strip actually is.
  detailsId.value = '';
}
</script>

<style scoped>
.mixer {
  display: flex;
  flex-direction: column;
  /* flex:1 + min-width:0 matter: as a lone item in the row-flex workspace the
     panel would otherwise shrink-wrap its content, leaving the rest of the
     container showing through un-themed. */
  flex: 1;
  min-width: 0;
  height: 100%;
  background: var(--color-background);
  overflow: hidden;
  /* Named container for the header's label-collapsing query (MixerActions.vue):
     the docked side pane can be ~220px wide while the viewport is not, so the
     query has to be against the mixer's own width. inline-size containment
     costs nothing here — flex:1 + min-width:0 already stop the content
     dictating the width. */
  container: mixer / inline-size;
}

/* Copied from .playlist-header / .cart-header on purpose — same padding,
   height and border so the three views' top bars line up. */
.mixer-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacing-sm);
  flex: 0 0 auto;
  padding: var(--spacing-md) var(--spacing-lg);
  min-height: 56px;
  box-sizing: border-box;
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
}
.mixer-header h2 {
  font-size: 18px;
  font-weight: 600;
  white-space: nowrap;
}
/* The action group itself is MixerActions; this only stops it wrapping under
   the title when the docked pane is narrow — the container query in
   MixerActions.vue drops the labels instead. */
.mixer-header-actions { flex: 0 0 auto; }

.mixer__body { display: flex; flex: 1; min-height: 0; min-width: 0; }
.mixer__strips {
  display: flex;
  gap: var(--spacing-xs);
  padding: var(--spacing-sm);
  /* min-width:0 is what actually lets this scroll: without it the flex item
     refuses to shrink below its content and the strips squash instead. */
  flex: 1;
  min-width: 0;
  /* min-height:0 completes the chain that lets a strip's fader shrink with the
     pane instead of the strips overflowing. */
  min-height: 0;
  overflow-x: auto;
  align-items: stretch;
}
.mixer__empty {
  align-self: center;
  color: var(--color-text-disabled);
  font-size: 12px;
}

/* The master is just a strip in a divider; the padding matches .mixer__strips
   so its rows sit at exactly the same heights as the channels'. */
.mixer__master {
  display: flex;
  flex: 0 0 auto;
  min-height: 0;
  padding: var(--spacing-sm);
  border-left: 1px solid var(--color-border);
  background: var(--color-surface);
}
</style>
