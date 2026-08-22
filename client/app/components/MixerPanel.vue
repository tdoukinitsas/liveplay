<template>
  <!--
    The mixer: a bank of channel strips, one per bus, with the Preview and
    Master buses pinned to the right.

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
      :buses="orderedBuses"
      :outputs="outputMap"
      @patch="onPatch"
      @delete="onDelete"
      @set-role="onSetRole"
      @select="showChannel"
      @close="detailsId = ''"
      @open-output-map="outputMapOpen = true"
    />

    <div v-else class="mixer__body">
      <!-- The rail: every bus without a role, in `order`. Strips are dragged
           by their grip to reorder; the marker shows where the drop lands. -->
      <div ref="railEl" class="mixer__strips">
        <MixerStrip
          v-for="bus in railBuses"
          :key="bus.id"
          :bus="bus"
          :buses="buses"
          :outputs="outputMap"
          :selected="bus.id === selectedId"
          :touch="touch"
          :dragging="drag?.id === bus.id"
          @select="selectedId = $event"
          @open="openDetails"
          @patch="onPatch"
          @delete="onDelete"
          @set-role="onSetRole"
          @drag-start="onDragStart"
          @open-output-map="outputMapOpen = true"
        />
        <p v-if="railBuses.length === 0" class="mixer__empty">{{ t('mixer.empty') }}</p>
        <div
          v-if="drag && dropMarkerLeft !== null"
          class="mixer__dropmark"
          :style="{ left: dropMarkerLeft + 'px' }"
        ></div>
      </div>

      <!-- Preview and Master, pinned right — the same component as every
           other strip, so they have the same rows at the same heights and
           their faders line up with the rail's. They are ordinary buses
           that hold a role (D24): Preview is where PFL and cue pre-listen
           land, and its fader is the headphone level; Master is the house
           and where every unassigned cue plays. A role can be moved to any
           rail strip from its ⋮ menu, and the strips swap places here. -->
      <div class="mixer__master">
        <MixerStrip
          v-for="bus in pinnedBuses"
          :key="bus.id"
          :bus="bus"
          :buses="buses"
          :outputs="outputMap"
          :selected="bus.id === selectedId"
          :touch="touch"
          @select="selectedId = $event"
          @open="openDetails"
          @patch="onPatch"
          @delete="onDelete"
          @set-role="onSetRole"
          @open-output-map="outputMapOpen = true"
        />
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
import type { OutputMap } from '~/composables/useLiveplayServer';
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
// The machine's output map, for the strips' pickers; null until fetched.
const outputMap   = ref<OutputMap | null>(null);
// Pure view state (invariant 1's one allowance): whether the output-map
// editor is open. Everything the modal shows and saves comes from the
// server, never from anything held here.
const outputMapOpen = ref(false);

const buses = computed<Bus[]>(() => server.buses ?? []);

// The rail is every bus without a role, in order. Deliberately NOT filtered
// on mixerId: a bus is a bus whether or not the engine currently has a strip
// for it, and MixerStrip already renders the strip-less state. Hiding them
// meant a single fetch that caught the server mid-rebuild emptied the whole
// rail, and nothing refetched until the panel was remounted — which is why
// expanding or undocking appeared to "fix" it.
const railBuses = computed(() =>
  buses.value.filter(b => !b.master && !b.preview).sort((a, b) => a.order - b.order));

// The pinned pair: Preview, then Master, at the right-hand end — as a desk
// puts its monitor section beside the main fader. Either is absent only
// while the first fetch is in flight; the server guarantees one of each.
const pinnedBuses = computed(() => {
  const out: Bus[] = [];
  const preview = buses.value.find(b => b.preview);
  const master  = buses.value.find(b => b.master);
  if (preview) out.push(preview);
  if (master)  out.push(master);
  return out;
});

// Rail then pinned: the order the channel view steps through and the order
// of its select row, matching what the eye sees left to right.
const orderedBuses = computed(() => [...railBuses.value, ...pinnedBuses.value]);

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
    ? buses.value.find(b => b.id === detailsId.value) ?? null
    : null);

// Refetched here, on the outputs_changed broadcast and on every WS reconnect,
// so a remap from any client (this window's own modal included) reaches every
// strip's picker; the modal owns its own draft of the map and is not the
// thing this depends on. Devices too: the pickers list them (D29), and the
// server's `bound` for a device-named target follows the same enumeration.
async function refreshOutputs() {
  try {
    outputMap.value = await server.fetchOutputs();
  } catch { /* offline; the next reconnect or outputs_changed retries */ }
  try { await server.fetchDevices(); } catch { /* same */ }
}

const unsubDocPatch = server.onDocPatch((payload: any) => {
  if (payload?.op === 'outputs_changed') void refreshOutputs();
});
const unsubReconnected = server.onReconnected(() => { void refreshOutputs(); });
onBeforeUnmount(() => {
  unsubDocPatch();
  unsubReconnected();
  endDrag();
});

onMounted(async () => {
  await server.fetchBuses();
  await refreshOutputs();
});

async function onPatch(id: string, patch: Partial<Bus>) {
  await server.patchBus(id, patch);
}
async function onDelete(id: string) {
  // A role holder cannot be deleted (D24); the menu already says so and
  // disables the item, and the server 409s regardless.
  const b = buses.value.find(x => x.id === id);
  if (!b || b.master || b.preview) return;
  await server.deleteBus(id);
  if (selectedId.value === id) selectedId.value = '';
  // Deleting the channel you are looking at drops you back to the rail rather
  // than leaving the view pointed at something that no longer exists.
  if (detailsId.value === id) detailsId.value = '';
}
async function onSetRole(id: string, role: 'master' | 'preview') {
  try { await server.setBusRole(id, role); } catch { /* refused; the list refetches on the next broadcast */ }
}
async function addBus() {
  const id = await server.createBus({ name: t('mixer.newBusName'), width: 2 });
  selectedId.value = id;
  // Back to the rail, where the new strip actually is.
  detailsId.value = '';
}

// ---- Rail drag-reorder ----------------------------------------------------
// A pointer drag from a strip's grip. The strip reports the grab; the rail
// owns the gesture, since where a drop lands is a question about every
// strip's position, not the one being moved. The marker is the only visual
// — strips do not shuffle live, because the server owns the order and the
// rail re-sorts from its broadcast after the PATCH. Nothing here is state
// another client could see.
const railEl = ref<HTMLElement | null>(null);
const drag = ref<{ id: string; dropIndex: number } | null>(null);
const dropMarkerLeft = ref<number | null>(null);

function railStripEls(): HTMLElement[] {
  return Array.from(railEl.value?.querySelectorAll<HTMLElement>(':scope > .strip') ?? []);
}

function onDragStart(id: string, e: PointerEvent) {
  drag.value = { id, dropIndex: railBuses.value.findIndex(b => b.id === id) };
  // Capture on the grip element so the move/up keep arriving even when the
  // pointer leaves it — or the window.
  try { (e.target as HTMLElement).setPointerCapture?.(e.pointerId); } catch { /* not capturable */ }
  window.addEventListener('pointermove', onDragMove);
  window.addEventListener('pointerup', onDragEnd);
  window.addEventListener('pointercancel', endDrag);
  updateDrop(e.clientX);
}

// The insertion index is the count of strips whose centre the pointer has
// passed; the marker sits at that strip's left edge, or past the last one.
function updateDrop(clientX: number) {
  if (!drag.value) return;
  const els = railStripEls();
  let idx = els.length;
  for (let i = 0; i < els.length; i++) {
    const r = els[i]!.getBoundingClientRect();
    if (clientX < r.left + r.width / 2) { idx = i; break; }
  }
  drag.value.dropIndex = idx;
  const rail = railEl.value;
  if (!rail) { dropMarkerLeft.value = null; return; }
  if (els.length === 0) { dropMarkerLeft.value = null; return; }
  // offsetLeft is relative to the rail (it is positioned), and scrolls with
  // the strips, so the marker stays beside the strip it marks while the rail
  // is scrolled.
  if (idx < els.length) {
    dropMarkerLeft.value = els[idx]!.offsetLeft - 2;
  } else {
    const last = els[els.length - 1]!;
    dropMarkerLeft.value = last.offsetLeft + last.offsetWidth;
  }
}

function onDragMove(e: PointerEvent) {
  updateDrop(e.clientX);
  // Scroll the rail when dragging against its edges, so a long rail can be
  // crossed in one gesture.
  const rail = railEl.value;
  if (rail) {
    const r = rail.getBoundingClientRect();
    if (e.clientX > r.right - 24) rail.scrollLeft += 8;
    else if (e.clientX < r.left + 24) rail.scrollLeft -= 8;
  }
}

function endDrag() {
  window.removeEventListener('pointermove', onDragMove);
  window.removeEventListener('pointerup', onDragEnd);
  window.removeEventListener('pointercancel', endDrag);
  drag.value = null;
  dropMarkerLeft.value = null;
}

async function onDragEnd() {
  const d = drag.value;
  endDrag();
  if (!d) return;
  await dropBus(d.id, d.dropIndex);
}

// Land `id` at `dropIndex` in the rail. One PATCH {order} on the moved bus
// with an integer between its new neighbours' orders is the preferred path
// (D31): the server stores it and broadcasts, and the rail re-sorts. Only
// when the neighbours are consecutive and no integer fits between them is
// the rail renumbered 0..n-1, one PATCH per bus that moves — idempotent
// whatever the server does to the order field on its side.
async function dropBus(id: string, dropIndex: number) {
  const rail = railBuses.value;
  const from = rail.findIndex(b => b.id === id);
  if (from < 0) return;
  // Dropping a strip on either side of itself is a no-op.
  if (dropIndex === from || dropIndex === from + 1) return;
  const seq = rail.filter(b => b.id !== id);
  const at  = dropIndex > from ? dropIndex - 1 : dropIndex;
  const moved = rail[from]!;
  seq.splice(at, 0, moved);
  const prev = seq[at - 1];
  const next = seq[at + 1];
  let order: number | null = null;
  if (prev && next) {
    if (next.order - prev.order >= 2) order = Math.floor((prev.order + next.order) / 2);
  } else if (prev) {
    order = prev.order + 1;
  } else if (next) {
    order = next.order - 1;
  }
  try {
    if (order !== null) {
      await server.reorderBus(id, order);
    } else {
      for (let i = 0; i < seq.length; i++) {
        if (seq[i]!.order !== i) await server.reorderBus(seq[i]!.id, i);
      }
    }
  } catch { /* refused or offline; the rail stays as the server has it */ }
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
  /* Positioned so the drop marker's offsetLeft is measured against it. */
  position: relative;
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
/* The live insertion marker: a bar in the gap the drop will land in. */
.mixer__dropmark {
  position: absolute;
  top: var(--spacing-sm);
  bottom: var(--spacing-sm);
  width: 3px;
  border-radius: 2px;
  background: var(--color-accent);
  pointer-events: none;
}

/* The pinned strips are just strips in a divider; the padding matches
   .mixer__strips so their rows sit at exactly the same heights as the rail's. */
.mixer__master {
  display: flex;
  gap: var(--spacing-xs);
  flex: 0 0 auto;
  min-height: 0;
  padding: var(--spacing-sm);
  border-left: 1px solid var(--color-border);
  background: var(--color-surface);
}
</style>
