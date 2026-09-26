<template>
  <!--
    One bus, as a channel strip. Ordered the way a desk is, top to bottom:
    header (grip, width, menu), inserts, output assignment, channel settings,
    pan, mute/PFL, then fader and meter taking the dominant vertical space,
    with the scribble strip — colour, name, role badge — at the bottom.

    Every bus renders through this, the Master and Preview buses included
    (D31). They are ordinary buses that happen to hold a role: the strip
    reads `bus.master` / `bus.preview` and changes only what the role
    changes — PFL becomes MONO on the preview strip, the role badge appears,
    the rail grip goes (they are pinned, not ordered) — and everything else,
    fader, meter, pan, mute, output, menu, is the same row at the same height,
    so the faders line up across the whole rail.
  -->
  <div
    class="strip"
    :style="{ '--bus': bus.color || 'var(--color-accent)' }"
    :class="{
      'strip--selected': selected,
      'strip--muted': bus.mute,
      'strip--touch': touch,
      'strip--role': bus.master || bus.preview,
      'strip--dragging': dragging,
    }"
    @click="$emit('select', bus.id)"
    @contextmenu.prevent="openMenuAt($event.clientX, $event.clientY)"
  >
    <!-- Header row: the drag grip for rail reordering (a rail strip only —
         the role strips are pinned), the width readout, and the ⋮ menu. The
         grip is the only part of the strip a drag starts from: a pointer-down
         anywhere else is a fader, a knob or a button, and must stay one. -->
    <div class="strip__head">
      <span
        class="strip__grip"
        :class="{ 'strip__grip--fixed': bus.master || bus.preview }"
        :title="bus.master || bus.preview ? '' : t('mixer.dragToReorder')"
        @pointerdown="onGripDown"
      >
        <span v-if="!bus.master && !bus.preview" class="material-symbols-rounded">drag_indicator</span>
        <span class="strip__width">{{ widthLabel }}</span>
      </span>
      <button
        ref="menuBtn"
        class="strip__menu"
        :title="t('mixer.busMenu')"
        @click.stop="openMenuFromButton"
        @mousedown.stop
      >
        <span class="material-symbols-rounded">more_vert</span>
      </button>
    </div>

    <!-- What the channel's processing is doing: EQ / GATE / COMP lamps, the
         EQ response and the dynamics curve in miniature, with the live input
         level and gain reduction — readable across the whole rail without
         opening a channel. It replaced two "—" insert placeholders that did
         nothing. Clicking it opens the channel view. -->
    <MixerStripProcessing :bus="bus" @open="$emit('open', bus.id)" />

    <!-- Output assignment: the most consequential control on the strip. The
         picker (BusOutputSelect.vue) is the same one the channel view uses;
         the unbound warning beside it is a click affordance straight into the
         output map, since the picker can choose a name but only the map can
         bind one to hardware. -->
    <div class="strip__row">
      <BusOutputSelect
        class="strip__output"
        :bus="bus"
        :buses="buses"
        :outputs="outputs"
        @open-output-map="$emit('open-output-map')"
      />
      <button
        v-if="outputUnmapped"
        class="strip__outputwarn"
        :title="bus.preview ? t('mixer.previewUnmapped') : t('mixer.outputMissingHint')"
        @click.stop="$emit('open-output-map')"
      >
        <span class="material-symbols-rounded">warning</span>
      </button>
    </div>

    <!-- Channel settings: the channel view, for every strip. The word is
         there as well as the icon because a lone tune glyph on a 96px strip
         was the control newcomers never found. -->
    <div class="strip__row">
      <button
        class="strip__fx"
        :title="t('mixer.channelDetails')"
        @click.stop="$emit('open', bus.id)"
      >
        <span class="material-symbols-rounded">tune</span>
        <span class="strip__fxlabel">{{ t('mixer.editChannel') }}</span>
      </button>
    </div>

    <!-- Pan on a mono bus, balance on a stereo one — one knob, and the server
         picks the law from the bus's width. Width itself lives in the channel
         view: it needs two more controls and a correlation readout to be
         usable, and none of that fits a strip sized to sit twenty across. -->
    <div class="strip__pan" @click.stop>
      <Knob
        :value="pan"
        :min="-1"
        :max="1"
        :origin="0"
        :size="touch ? 40 : 30"
        :title="bus.width >= 2 ? t('mixer.balanceHint') : t('mixer.panHint')"
        @input="onPan"
        @reset="onPan(0)"
      />
      <span class="strip__panlabel">{{ panLabel }}</span>
    </div>

    <!-- Mute / PFL.
         PFL adds a pre-fader, pre-mute tap into the preview bus and changes
         nothing about what the room hears — several can be up at once. Every
         strip has it, the master included (PFL on the house is how you check
         the mix in the phones); the preview strip, being the destination,
         gets MONO in that slot instead — the console arrangement: one button
         on the monitoring path that folds the phones to mono. PFL whichever
         buses you want to check, then press it — anything that cancels when
         summed drops out. It only ever touches the phones. -->
    <div class="strip__row strip__row--split">
      <button
        class="strip__btn"
        :class="{ 'strip__btn--mute': bus.mute }"
        @click.stop="onMute"
      >{{ t('mixer.mute') }}</button>
      <button
        v-if="bus.preview"
        class="strip__btn"
        :class="{ 'strip__btn--mono': bus.monoCheck }"
        :title="t('mixer.monoCheckHint')"
        @click.stop="onMonoCheck"
      >{{ t('mixer.monoCheck') }}</button>
      <button
        v-else
        class="strip__btn"
        :class="{ 'strip__btn--pfl': bus.pfl }"
        :title="t('mixer.pflHint')"
        @click.stop="onPfl"
      >{{ t('mixer.pfl') }}</button>
    </div>

    <!-- Meter, shared scale, fader.
         The scale and fader span the full -60..+12. The meter is dBFS and
         tops out at 0, so its track is only as tall as that part of the range
         — 0 dBFS lands exactly on the 0 tick, the meter keeps full resolution
         across its own range, and there is no dead strip above it that a
         signal could never reach. Every strip meters its own engine lanes —
         the master's are the house pair, but it reads them through its own
         mixerId like every other bus, so there is one meter path, not two. -->
    <div class="strip__meterfader">
      <div class="strip__meters" :style="{ height: METER_TRACK_PCT + '%' }">
        <StereoMeter
          v-if="bus.mixerId"
          :mixer-id="bus.mixerId"
          :mono="bus.width < 2"
          bare
          :show-scale="false"
          show-gr
          :min-db="FADER_MIN_DB"
          :max-db="METER_MAX_DB"
        />
        <div v-else class="strip__nometer" :title="t('mixer.noStrip')"></div>
      </div>
      <MeterScale :min-db="FADER_MIN_DB" :max-db="FADER_MAX_DB" />
      <CanvasFader
        :db="gainDb"
        :min-db="FADER_MIN_DB"
        :max-db="FADER_MAX_DB"
        :width="touch ? 32 : 20"
        @input="onFader"
        @reset="onFader(0)"
      />
    </div>

    <!-- What the meter reads, in the project's meter unit, so the number
         under the strip and the bars above it cannot disagree. -->
    <div class="strip__readout">{{ meterLabel }}</div>
    <div class="strip__gain">{{ gainLabel }}</div>

    <!-- Scribble strip: colour chip (click to recolour), name (double-click
         to rename), and under it the role badge and the item count. -->
    <div
      class="strip__name"
      :title="bus.preview ? t('mixer.previewHint')
                          : (renaming ? '' : bus.name + ' — ' + t('mixer.renameHint'))"
    >
      <BusColorPicker :color="bus.color" @pick="onPickColor" />
      <input
        v-if="renaming"
        ref="nameInput"
        class="strip__nameinput"
        :value="bus.name"
        @click.stop
        @mousedown.stop
        @keyup.enter="commitRename"
        @keyup.esc="renaming = false"
        @blur="commitRename"
      />
      <span v-else class="strip__nametext" @dblclick.stop="startRename">{{ bus.name }}</span>
    </div>
    <!-- Reserved height, not natural: a collapsed row on one strip puts its
         fader and name a row lower than every other one's. The preview bus
         feeds from PFL, not cues, so its count would always read 0 and is
         left off. -->
    <div class="strip__count">
      <span v-if="bus.master" class="strip__badge strip__badge--master">{{ t('mixer.roleMaster') }}</span>
      <span v-else-if="bus.preview" class="strip__badge strip__badge--preview">{{ t('mixer.rolePreview') }}</span>
      <span v-if="!bus.preview">{{ t('mixer.itemCount', { count: bus.itemUuids.length }) }}</span>
    </div>

    <BusMenu
      :bus="bus"
      :open="menuOpen"
      :x="menuPos.x"
      :y="menuPos.y"
      @close="menuOpen = false"
      @rename="startRename"
      @color="onPickColor"
      @open="$emit('open', bus.id)"
      @set-master="$emit('set-role', bus.id, 'master')"
      @set-preview="$emit('set-role', bus.id, 'preview')"
      @delete="$emit('delete', bus.id)"
    />
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue';
import type { Bus } from '~/types/project';
import type { OutputMap } from '~/composables/useLiveplayServer';
import CanvasFader from './CanvasFader.vue';
import Knob from './Knob.vue';
import StereoMeter from './StereoMeter.vue';
import MeterScale from './MeterScale.vue';
import BusOutputSelect from './BusOutputSelect.vue';
import BusColorPicker from './BusColorPicker.vue';
import BusMenu from './BusMenu.vue';
import MixerStripProcessing from './MixerStripProcessing.vue';
import { useMixerMeter, lufsFromKwMs } from '~/composables/useLiveMeters';
import { useOutputTarget } from '~/composables/useOutputTarget';
import {
  FADER_MIN_DB, FADER_MAX_DB, METER_MAX_DB, METER_TRACK_PCT, formatMeterLabel,
} from '~/utils/meterScale';

const props = defineProps<{
  bus: Bus;
  /** Every bus in the project — the output picker needs the whole list. */
  buses: Bus[];
  /** The machine's output map, null until fetched. */
  outputs: OutputMap | null;
  selected?: boolean;
  touch?: boolean;
  /** Set by MixerPanel while this strip is the one being dragged. */
  dragging?: boolean;
}>();

const emit = defineEmits<{
  (e: 'select', id: string): void;
  (e: 'open', id: string): void;
  (e: 'patch', id: string, patch: Partial<Bus>): void;
  (e: 'delete', id: string): void;
  (e: 'set-role', id: string, role: 'master' | 'preview'): void;
  (e: 'open-output-map'): void;
  /** Pointer-down on the grip: MixerPanel runs the drag from here. */
  (e: 'drag-start', id: string, ev: PointerEvent): void;
}>();

const { t } = useLocalization();
const server = useLiveplayServer();

// The fader is driven locally while it moves. Binding it straight to
// bus.gainDb meant every drag event did a PATCH plus a full bus refetch, and
// the knob snapped back to the stale value until the round-trip landed —
// unusable, and brutal on the server.
//
// So: the engine gets the level immediately (a strip-only call that writes no
// document and triggers no refetch) and the bus is persisted once the gesture
// settles. The same path for every strip — the master's fader is its bus
// gain like any other's, and the transport bar's output fader drives the
// very same value (D32), so the two cannot disagree.
const gainDb  = ref(props.bus.gainDb);
let   holding = false;
let   settle: ReturnType<typeof setTimeout> | null = null;

watch(() => props.bus.gainDb, v => { if (!holding) gainDb.value = v; });

function onFader(db: number) {
  gainDb.value = db;
  holding = true;
  if (props.bus.mixerId) {
    void server.setMixerGainDb(props.bus.mixerId, db).catch(() => {});
  }
  if (settle) clearTimeout(settle);
  settle = setTimeout(() => {
    settle  = null;
    holding = false;
    emit('patch', props.bus.id, { gainDb: gainDb.value });
  }, 250);
}

// Pan follows the fader's pattern for the same reason: the engine gets the
// position immediately over a strip-only call, and the bus is persisted once
// the gesture settles. Unlike gain, pan lives in the bus's send gains rather
// than on the strip, so the live call is /api/buses/<id>/pan.
const pan       = ref(props.bus.pan ?? 0);
let   panHold   = false;
let   panSettle: ReturnType<typeof setTimeout> | null = null;

watch(() => props.bus.pan, v => { if (!panHold) pan.value = v ?? 0; });

function onPan(v: number) {
  pan.value = v;
  panHold = true;
  void server.setBusPan(props.bus.id, v).catch(() => {});
  if (panSettle) clearTimeout(panSettle);
  panSettle = setTimeout(() => {
    panSettle = null;
    panHold   = false;
    emit('patch', props.bus.id, { pan: pan.value });
  }, 250);
}

// L/R offset in the usual console notation: C at centre, L50/R50 halfway.
const panLabel = computed(() => {
  const v = Math.round(pan.value * 100);
  if (v === 0) return 'C';
  return (v < 0 ? 'L' : 'R') + Math.abs(v);
});

onBeforeUnmount(() => {
  if (settle) clearTimeout(settle);
  if (panSettle) clearTimeout(panSettle);
});

// Inline rename: double-click on the name, or Rename in the menu. Every bus
// can be renamed, role holders included — a role is a job, not a name.
const renaming  = ref(false);
const nameInput = ref<HTMLInputElement | null>(null);

async function startRename() {
  renaming.value = true;
  await nextTick();
  nameInput.value?.select();
}
function commitRename() {
  if (!renaming.value) return;
  renaming.value = false;
  const next = nameInput.value?.value?.trim();
  if (next && next !== props.bus.name) emit('patch', props.bus.id, { name: next });
}

function onPickColor(color: string) {
  emit('patch', props.bus.id, { color });
}

// Mute goes to the engine first so it takes effect on the click rather than
// after the document round-trip, then persists.
function onMute() {
  const next = !props.bus.mute;
  if (props.bus.mixerId) void server.setMixerMute(props.bus.mixerId, next).catch(() => {});
  emit('patch', props.bus.id, { mute: next });
}

// PFL is engine-only state — no document write, nothing to persist, so it goes
// straight to the server rather than through the patch path the fader uses.
function onPfl() {
  if (props.bus.preview) return;
  void server.setBusPfl(props.bus.id, !props.bus.pfl).catch(() => {});
}

// The mono-sum audition, and engine-only for the same reason PFL is: it is
// what the operator is checking right now, not part of the show.
function onMonoCheck() {
  if (!props.bus.preview) return;
  void server.setMonitorMono(!props.bus.monoCheck).catch(() => {});
}

// Width is set in the channel view; the strip just reports it in the header,
// beside the menu that leads there.
const widthLabel = computed(() => props.bus.width >= 2 ? 'ST' : 'MONO');

// The meter's own reading, in the project's unit. Subscribes to the same
// streams StereoMeter does — cheap, since every subscriber shares one WS frame
// — so the number under the strip and the bars above it can't disagree.
const { meterMode } = useOutputTarget();
const meterL = useMixerMeter(() => props.bus.mixerId, () => 0);
const meterR = useMixerMeter(() => props.bus.mixerId, () => (props.bus.width >= 2 ? 1 : 0));

const meterLabel = computed(() => {
  const mono = props.bus.width < 2;
  if (meterMode.value === 'LUFS') {
    return formatMeterLabel(
      mono ? lufsFromKwMs([meterL.kwMs.value])
           : lufsFromKwMs([meterL.kwMs.value, meterR.kwMs.value]),
      meterMode.value);
  }
  const pick = (s: typeof meterL) =>
    meterMode.value === 'RMS'  ? s.rms.value
    : meterMode.value === 'dBTP' ? s.truePeak.value
    : s.peak.value;
  return formatMeterLabel(mono ? pick(meterL) : Math.max(pick(meterL), pick(meterR)), meterMode.value);
});

const gainLabel = computed(() => {
  const v = gainDb.value;
  if (v <= -60) return '-∞';
  return (v > 0 ? '+' : '') + v.toFixed(1);
});

// Whether this bus's hardware route is unbound — answered by the server
// (D26), not inferred from the name list: a device-named target is bound
// while the device is present, and Main Out is bound even unmapped, neither
// of which the names alone would tell you.
const outputUnmapped = computed(() =>
  props.bus.output.type === 'output' && props.bus.bound === false);

// ---- ⋮ menu ---------------------------------------------------------------
const menuOpen = ref(false);
const menuPos  = ref({ x: 0, y: 0 });
const menuBtn  = ref<HTMLButtonElement | null>(null);

function openMenuAt(x: number, y: number) {
  menuPos.value = { x, y };
  menuOpen.value = true;
}
// From the button, the menu hangs under it rather than opening at the
// pointer, so it reads as belonging to the button.
function openMenuFromButton() {
  const r = menuBtn.value?.getBoundingClientRect();
  if (r) openMenuAt(r.left, r.bottom + 2);
}

// ---- Rail reordering ------------------------------------------------------
// The strip only reports the grab; MixerPanel owns the drag, because the
// drop position is a question about the whole rail, not this strip.
function onGripDown(e: PointerEvent) {
  if (props.bus.master || props.bus.preview) return;
  if (e.button !== 0) return;
  emit('drag-start', props.bus.id, e);
}
</script>

<style scoped>
.strip {
  /* The bus colour across the top, as a console's scribble strip carries it,
     so a channel can be found by colour from across the room. */
  border-top: 4px solid var(--bus) !important;
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  /* Two lane meters with the gain-reduction sub-track, the shared scale and
     the fader, side by side; every strip the same width so the rail stays a
     uniform grid. */
  width: 96px;
  flex: 0 0 auto;
  padding: var(--spacing-xs);
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  cursor: pointer;
  transition: border-color var(--transition-fast);
  /* Everything except the meter/fader block is fixed height; min-height:0 is
     what lets that one block absorb the difference instead of the strip
     overflowing its container. */
  min-height: 0;
  overflow: hidden;
}
.strip--touch { width: 140px; }
.strip:hover { border-color: var(--color-text-disabled); }
.strip--selected { border-color: var(--color-accent); }
.strip--muted .strip__meterfader { opacity: 0.45; }
/* Lifted while it is being dragged, so the eye tracks the one in motion. */
.strip--dragging { opacity: 0.5; border-style: dashed; }

/* Every row but the fader keeps its natural height when the pane is resized. */
.strip > *:not(.strip__meterfader) { flex: 0 0 auto; }

.strip__head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  height: 16px;
}
.strip__grip {
  display: flex;
  align-items: center;
  gap: 2px;
  flex: 1;
  min-width: 0;
  height: 100%;
  color: var(--color-text-disabled);
  cursor: grab;
  /* A pointer drag, not a scroll: without this a touch drag on the grip would
     pan the rail instead. */
  touch-action: none;
  user-select: none;
}
.strip__grip--fixed { cursor: default; }
.strip__grip .material-symbols-rounded { font-size: 14px; }
.strip__width {
  font-family: var(--font-mono);
  font-size: 9px;
  letter-spacing: 0.04em;
  color: var(--color-text-secondary);
}
.strip__menu {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 16px;
  height: 16px;
  padding: 0;
  color: var(--color-text-secondary);
  background: none;
  border: none;
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.strip__menu:hover { color: var(--color-text-primary); background: var(--color-background); }
.strip__menu .material-symbols-rounded { font-size: 14px; }

.strip__inserts { display: flex; flex-direction: column; gap: 2px; }
.strip__insert {
  height: 16px;
  font-size: 10px;
  color: var(--color-text-disabled);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: not-allowed;
}

.strip__row { display: flex; gap: 2px; }
.strip__row--split > * { flex: 1; }

.strip__output {
  width: 100%;
  min-width: 0;
  flex: 1 1 auto;
  font-size: 10px;
  padding: 2px;
}
/* The click affordance that replaces the old dead-end tooltip: a small red
   flag beside the select, sized to fit in a 96px strip without crowding the
   picker it sits next to. */
.strip__outputwarn {
  display: flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  width: 16px;
  padding: 0;
  color: #fff;
  background: var(--color-warning);
  border: 1px solid var(--color-warning);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.strip__outputwarn .material-symbols-rounded { font-size: 11px; }

.strip__fx,
.strip__btn {
  font-size: 10px;
  padding: 3px 0;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.strip__fx {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 4px;
  width: 100%;
}
.strip__fx:hover { color: var(--color-text-primary); }
.strip__fx .material-symbols-rounded { font-size: 14px; }
.strip__fxlabel { font-family: var(--font-mono); letter-spacing: 0.04em; }
.strip__btn--mute {
  background: var(--color-danger);
  border-color: var(--color-danger);
  color: #fff;
}
.strip__btn--pfl { background: var(--color-success); border-color: var(--color-success); color: #fff; }
/* Lit in the accent rather than in PFL's green: it is a different kind of
   state — a temporary way of listening, not a thing being listened to — and
   two greens side by side on the same row would read as the same control. */
.strip__btn--mono {
  background: var(--color-accent);
  border-color: var(--color-accent);
  color: #fff;
}

.strip__pan {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 4px;
}
.strip__panlabel {
  font-family: var(--font-mono);
  font-size: 10px;
  min-width: 26px;
  color: var(--color-text-secondary);
}

.strip__meterfader {
  display: flex;
  gap: 3px;
  justify-content: center;
  /* Default stretch: the scale and fader must span the full range. Only the
     meter block is shorter, and it aligns to the bottom (below) so its
     0 dBFS top edge lands on the scale's 0 tick. */
  /* flex-basis 0 + min-height 0: this is the one part of the strip that gives
     way when the pane gets shorter. With a min-height it refused to shrink and
     the strip overflowed instead, so a short mixer rendered with the fader
     spilling past the bottom rows. A floor small enough to still be grabbable
     is enough — below that the pane itself should scroll. */
  flex: 1 1 0;
  min-height: 48px;
  /* Breathing room under the mute/PFL row: the fader reads as its own zone
     rather than the next button in a stack. */
  margin-top: var(--spacing-xs);
}
.strip__meters {
  display: flex;
  gap: 2px;
  align-self: flex-end;
  min-height: 0;
}
.strip__nometer { width: 6px; height: 100%; background: var(--color-background); border-radius: 2px; }

.strip__readout,
.strip__gain {
  text-align: center;
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--color-text-primary);
}
/* The meter's reading is secondary to the fader's, which is the one the
   operator is setting. */
.strip__readout {
  font-size: 10px;
  color: var(--color-text-secondary);
}

.strip__name {
  display: flex;
  align-items: center;
  gap: 4px;
  padding: 3px;
  background: var(--color-background);
  border-radius: var(--border-radius-sm);
  overflow: hidden;
}
.strip__nametext {
  font-size: 11px;
  color: var(--color-text-primary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  cursor: text;
}
.strip__nameinput {
  width: 100%;
  min-width: 0;
  font-size: 11px;
  color: var(--color-text-primary);
  background: var(--color-surface);
  border: 1px solid var(--color-accent);
  border-radius: 2px;
  padding: 0 2px;
}
.strip__count {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 4px;
  font-size: 9px;
  line-height: 12px;
  /* Reserved, not natural: see the template comment. */
  height: 12px;
  color: var(--color-text-disabled);
  white-space: nowrap;
  overflow: hidden;
}
/* The role badge: small, uppercase, and in the token colours so it reads as
   a label rather than a lit button. Master in the accent — it is the house;
   Preview in the success green PFL already uses, since that is where PFL
   lands. */
.strip__badge {
  padding: 0 3px;
  font-family: var(--font-mono);
  font-size: 8px;
  font-weight: 600;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  line-height: 11px;
  border-radius: 2px;
  color: #fff;
}
.strip__badge--master  { background: var(--color-accent); }
.strip__badge--preview { background: var(--color-success); }
</style>
