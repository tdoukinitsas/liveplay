<template>
  <!--
    One bus, as a channel strip. Ordered the way a desk is, top to bottom:
    inserts, output assignment, details, pan, mute/PFL, then fader and meter
    taking the dominant vertical space, with the name at the bottom where the
    scribble strip lives.

    The master is rendered by this component too, as a strip whose gain drives
    the output-channel pair instead of a bus. It used to be bespoke markup in
    MixerPanel, which is why it had its own padding and spacing — and why the
    one strip that mattered most looked unlike all the others. Every row here
    is present on both, blank where it does not apply, because the faders only
    line up across the rail if the rows above and below them are equal heights.
  -->
  <div
    class="strip"
    :class="{
      'strip--selected': selected,
      'strip--muted': bus.mute,
      'strip--touch': touch,
      'strip--master': master,
    }"
    @click="$emit('select', bus.id)"
  >
    <!-- Inserts: scaffolding until the DSP chain exists. -->
    <div class="strip__inserts">
      <button
        v-for="n in 2"
        :key="n"
        class="strip__insert"
        :title="t('mixer.insertsComingSoon')"
        disabled
      >—</button>
    </div>

    <!-- Output assignment: the most consequential control on the strip.
         The master's is fixed, so it reads as a plate rather than a picker. -->
    <div class="strip__row">
      <div v-if="master" class="strip__output strip__output--fixed">{{ t('mixer.toHardware') }}</div>
      <select
        v-else
        class="strip__output"
        :value="outputValue"
        :title="outputTitle"
        :class="{ 'strip__output--warn': outputUnmapped }"
        @click.stop
        @change="onOutputChange"
      >
        <!-- Monitor is offered no route to the master. It carries PFL, and
             PFL summed into the house is the accident this whole scheme was
             chosen to make impossible; the server refuses it too. -->
        <option v-if="!monitor" value="master">{{ t('mixer.toMaster') }}</option>
        <option
          v-for="o in outputOptions"
          :key="'out:' + o"
          :value="'out:' + o"
        >{{ o }}</option>
      </select>
    </div>

    <!-- Channel details. The mono/stereo toggle used to sit beside this, but
         a button reading "M" next to one reading "MUTE" is a trap, and width
         is a setup decision rather than a live one — it lives in the details
         view now. -->
    <div class="strip__row">
      <button
        class="strip__fx"
        :title="master ? t('mixer.comingSoon') : t('mixer.channelDetails')"
        :disabled="master"
        @click.stop="$emit('open', bus.id)"
      >
        <span class="material-symbols-rounded">tune</span>
        <span class="strip__fxlabel">{{ widthLabel }}</span>
      </button>
    </div>

    <!-- Pan on a mono bus, balance on a stereo one — one knob, and the server
         picks the law from the bus's width. Pan places a single lane between
         the two lanes of a stereo destination, which is what a mono channel's
         pot does; balance trims a stereo bus's own pair against each other and
         only ever attenuates.

         Width lives in the channel view, not here: it needs two more controls
         and a correlation readout to be usable, and none of that fits a strip
         sized to sit twenty across. -->
    <div class="strip__pan" @click.stop>
      <Knob
        :value="pan"
        :min="-1"
        :max="1"
        :origin="0"
        :size="touch ? 40 : 30"
        :disabled="master"
        :title="bus.width >= 2 ? t('mixer.balanceHint') : t('mixer.panHint')"
        @input="onPan"
        @reset="onPan(0)"
      />
      <span class="strip__panlabel">{{ panLabel }}</span>
    </div>

    <!-- Mute / PFL.
         PFL adds a pre-fader, pre-mute tap into the Monitor bus and changes
         nothing about what the room hears — several can be up at once. The
         master has no PFL (it is what you are already listening to) and
         neither does Monitor (it is the destination). -->
    <div class="strip__row strip__row--split">
      <button
        class="strip__btn"
        :class="{ 'strip__btn--mute': bus.mute }"
        :disabled="master"
        @click.stop="onMute"
      >{{ t('mixer.mute') }}</button>
      <button
        class="strip__btn"
        :class="{ 'strip__btn--pfl': bus.pfl }"
        :disabled="master || monitor"
        :title="master || monitor ? '' : t('mixer.pflHint')"
        @click.stop="onPfl"
      >{{ t('mixer.pfl') }}</button>
    </div>

    <!-- Meter, shared scale, fader.
         The scale and fader span the full -60..+12. The meter is dBFS and
         tops out at 0, so its track is only as tall as that part of the range
         — 0 dBFS lands exactly on the 0 tick, the meter keeps full resolution
         across its own range, and there is no dead strip above it that a
         signal could never reach. One scale stays honest for both because
         both map dB to position linearly. -->
    <div class="strip__meterfader">
      <div class="strip__meters" :style="{ height: METER_TRACK_PCT + '%' }">
        <!-- One meter for every strip, master included: same zone colours,
             peak hold, clip latch, meter mode and — since the rail should be
             one grid rather than three near-misses — the same gain-reduction
             sub-track. The strip supplies the frame and the shared scale, so
             the meter renders bare and without its own tick column.

             The master reads the output pair, a bus reads its strip lanes;
             that is the only difference, and it is two props. -->
        <StereoMeter
          v-if="master || bus.mixerId"
          :left-index="master ? 0 : null"
          :right-index="master ? 1 : null"
          :mixer-id="master ? null : bus.mixerId"
          :mono="!master && bus.width < 2"
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

    <!-- What the meter reads, in the project's meter unit — the master had
         this and the strips didn't, so the same signal appeared to read two
         different things depending on where you looked. -->
    <div class="strip__readout">{{ meterLabel }}</div>
    <div class="strip__gain">{{ gainLabel }}</div>

    <!-- Scribble strip. Double-click to rename, as on a desk. -->
    <div
      class="strip__name"
      :title="monitor ? t('mixer.monitorHint')
                      : (renaming ? '' : bus.name + ' — ' + t('mixer.renameHint'))"
    >
      <span class="strip__chip" :style="{ background: bus.color || 'var(--color-accent)' }"></span>
      <input
        v-if="renaming"
        ref="nameInput"
        class="strip__nameinput"
        :value="bus.name"
        @click.stop
        @keyup.enter="commitRename"
        @keyup.esc="renaming = false"
        @blur="commitRename"
      />
      <span v-else class="strip__nametext" @dblclick.stop="startRename">{{ bus.name }}</span>
    </div>
    <!-- Item count. Rendered on every strip, blank where it has no meaning, and
         with a reserved height — an empty div collapses to nothing, which let
         the master's meter/fader block grow by a row and pushed its readout,
         level and name below everyone else's. -->
    <div class="strip__count">
      {{ master ? '' : t('mixer.itemCount', { count: bus.itemUuids.length }) }}
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue';
import type { Bus } from '~/types/project';
import CanvasFader from './CanvasFader.vue';
import Knob from './Knob.vue';
import StereoMeter from './StereoMeter.vue';
import MeterScale from './MeterScale.vue';
import { useMixerMeter, useMasterMeter, lufsFromKwMs } from '~/composables/useLiveMeters';
import { useOutputTarget } from '~/composables/useOutputTarget';
import {
  FADER_MIN_DB, FADER_MAX_DB, METER_MAX_DB, METER_TRACK_PCT, formatMeterLabel,
} from '~/utils/meterScale';

const props = defineProps<{
  bus: Bus;
  selected?: boolean;
  touch?: boolean;
  /** Logical output names this machine knows about. */
  outputNames: string[];
  /**
   * Render as the master strip: meters the output pair, drives output-channel
   * gain, and blanks the rows that only mean something for a bus.
   */
  master?: boolean;
  /**
   * Render as the Monitor strip: a real bus with a real fader (it is the
   * headphone level), but the destination of PFL rather than a source of it,
   * and never routable to the master.
   */
  monitor?: boolean;
}>();

const emit = defineEmits<{
  (e: 'select', id: string): void;
  (e: 'open', id: string): void;
  (e: 'patch', id: string, patch: Partial<Bus>): void;
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
// settles.
const gainDb  = ref(props.bus.gainDb);
let   holding = false;
let   settle: ReturnType<typeof setTimeout> | null = null;

watch(() => props.bus.gainDb, v => { if (!holding) gainDb.value = v; });

function onFader(db: number) {
  gainDb.value = db;
  // Hold the local value for every strip, master included.
  //
  // The master used to return here without setting `holding`, which left the
  // watch above live during the drag. Its value comes back asynchronously over
  // output_channel_gain_changed — two broadcasts per drag event, one per
  // channel — so the queued echoes kept overwriting the position with older
  // values: the fader jumped, then crawled to where you had put it as the
  // backlog drained. Every other fader held its value and did not.
  holding = true;
  if (props.master) {
    // The output-channel pair, the same parameter the transport bar's Main
    // fader drives. Both channels, so the pair stays level-matched.
    void server.setOutputChannelGainDb(0, db);
    void server.setOutputChannelGainDb(1, db);
  } else if (props.bus.mixerId) {
    void server.setMixerGainDb(props.bus.mixerId, db).catch(() => {});
  }
  if (settle) clearTimeout(settle);
  settle = setTimeout(() => {
    settle  = null;
    holding = false;
    // Nothing to persist for the master: it has no bus, and the engine is
    // already holding the value the broadcast will echo back.
    if (!props.master) emit('patch', props.bus.id, { gainDb: gainDb.value });
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
  // The master has no bus behind it, so there is nothing to send this to.
  if (props.master) return;
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
  if (props.master) return '--';
  const v = Math.round(pan.value * 100);
  if (v === 0) return 'C';
  return (v < 0 ? 'L' : 'R') + Math.abs(v);
});

onBeforeUnmount(() => {
  if (settle) clearTimeout(settle);
  if (panSettle) clearTimeout(panSettle);
});

const renaming  = ref(false);
const nameInput = ref<HTMLInputElement | null>(null);

async function startRename() {
  if (props.master || props.bus.system) return;
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

// Mute goes to the engine first so it takes effect on the click rather than
// after the document round-trip, then persists.
function onMute() {
  if (props.master) return;
  const next = !props.bus.mute;
  if (props.bus.mixerId) void server.setMixerMute(props.bus.mixerId, next).catch(() => {});
  emit('patch', props.bus.id, { mute: next });
}

// PFL is engine-only state — no document write, nothing to persist, so it goes
// straight to the server rather than through the patch path the fader uses.
function onPfl() {
  if (props.master || props.monitor) return;
  void server.setBusPfl(props.bus.id, !props.bus.pfl).catch(() => {});
}

// Width moved to the channel details view; the strip just reports it, next to
// the button that opens the place it can be changed.
const widthLabel = computed(() => props.bus.width >= 2 ? 'ST' : 'MONO');

// The meter's own reading, in the project's unit. Subscribes to the same
// streams StereoMeter does — cheap, since every subscriber shares one WS frame
// — so the number under the strip and the bars above it can't disagree.
const { meterMode } = useOutputTarget();
const meterL = useMixerMeter(() => props.master ? null : props.bus.mixerId, () => 0);
const meterR = useMixerMeter(() => props.master ? null : props.bus.mixerId,
                             () => (props.bus.width >= 2 ? 1 : 0));
const masterL = useMasterMeter(() => props.master ? 0 : null);
const masterR = useMasterMeter(() => props.master ? 1 : null);

const meterLabel = computed(() => {
  const l = props.master ? masterL : meterL;
  const r = props.master ? masterR : meterR;
  const mono = !props.master && props.bus.width < 2;
  if (meterMode.value === 'LUFS') {
    return formatMeterLabel(
      mono ? lufsFromKwMs([l.kwMs.value])
           : lufsFromKwMs([l.kwMs.value, r.kwMs.value]),
      meterMode.value);
  }
  const pick = (s: typeof l) =>
    meterMode.value === 'RMS'  ? s.rms.value
    : meterMode.value === 'dBTP' ? s.truePeak.value
    : s.peak.value;
  return formatMeterLabel(mono ? pick(l) : Math.max(pick(l), pick(r)), meterMode.value);
});

const gainLabel = computed(() => {
  const v = gainDb.value;
  if (v <= -60) return '-∞';
  return (v > 0 ? '+' : '') + v.toFixed(1);
});

// The <select> carries "master" or "out:<logical name>". Bus→bus targets are
// shown as-is but not selectable yet — that routing isn't implemented.
const outputValue = computed(() => {
  const o = props.bus.output;
  return o.type === 'output' ? 'out:' + o.target : 'master';
});

// The names offered, plus whatever this bus is already pointing at.
//
// Without the second part the select can have no option matching its value and
// renders blank: a bus targeting a name this machine does not map, and — since
// Monitor is offered no "to master" entry — the Monitor strip on any machine
// with no logical outputs at all, which is the default. A picker showing
// nothing where a route should be is worse than showing an unmapped one.
const outputOptions = computed(() => {
  const names = [...props.outputNames];
  const target = props.bus.output.target;
  if (props.bus.output.type === 'output' && target && !names.includes(target)) {
    names.push(target);
  }
  return names;
});

// Whether this bus reaches hardware — answered by the server, not inferred
// from the name list. Monitor is usually bound through settings.previewDevice,
// which is not in the output map at all, so inferring it here put a warning on
// the one bus most likely to be working.
const outputUnmapped = computed(() =>
  props.bus.output.type === 'output' && props.bus.bound === false);

const outputTitle = computed(() => {
  if (props.bus.output.type === 'bus') return t('mixer.busToBusUnsupported');
  // Unmapped means something different on Monitor. Every other bus falls back
  // to treating the name as a device and usually still plays; Monitor's
  // default name matches no device, so unmapped means PFL is inaudible — a
  // thing worth saying outright rather than leaving the operator to work out
  // why pressing PFL does nothing.
  if (outputUnmapped.value) {
    return props.monitor
      ? t('mixer.monitorUnmapped')
      : t('mixer.outputUnmapped', { name: props.bus.output.target });
  }
  return t('mixer.output');
});

function onOutputChange(e: Event) {
  const v = (e.target as HTMLSelectElement).value;
  emit('patch', props.bus.id, v === 'master'
    ? { output: { type: 'master', target: '' } }
    : { output: { type: 'output', target: v.slice(4) } });
}
</script>

<style scoped>
.strip {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  /* Two lane meters, the shared scale and the fader, side by side. The master
     runs widest of all — its meter carries the gain-reduction sub-track — and
     every strip matches it so the rail stays a uniform grid. */
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
/* The master is the same strip; it just sits behind a divider so the eye can
   find it at the end of the rail. */
.strip--master { cursor: default; }

/* Every row but the fader keeps its natural height when the pane is resized. */
.strip > *:not(.strip__meterfader) { flex: 0 0 auto; }

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
  font-size: 10px;
  padding: 2px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
}
.strip__output--warn { border-color: var(--color-warning); }
/* The master's output is not a choice; it reads as a plate, but keeps the
   select's box so the row is the same height on every strip. */
.strip__output--fixed {
  text-align: center;
  color: var(--color-text-disabled);
  border-style: dashed;
}

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
.strip__fx .material-symbols-rounded { font-size: 14px; }
.strip__fx:disabled { cursor: not-allowed; opacity: 0.4; }
.strip__fxlabel { font-family: var(--font-mono); letter-spacing: 0.04em; }
.strip__btn--mute {
  background: var(--color-danger);
  border-color: var(--color-danger);
  color: #fff;
}
.strip__btn--pfl { background: var(--color-success); border-color: var(--color-success); color: #fff; }
.strip__btn:disabled { cursor: not-allowed; opacity: 0.4; }

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
.strip__chip { width: 6px; height: 6px; border-radius: 50%; flex: 0 0 auto; }
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
  text-align: center;
  font-size: 9px;
  line-height: 12px;
  /* Reserved, not natural: blank on the master, and a collapsed row there put
     that strip's fader and name a row lower than every other one. */
  height: 12px;
  color: var(--color-text-disabled);
}
</style>
