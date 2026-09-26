<template>
  <!--
    Dynamics: an expander/gate and a compressor/limiter.

    Left to right: the transfer graph at full height, then a gain-reduction
    meter for each processor, then their controls. The graph is shared because
    both act on the same axis — input level in, output level out — and one
    curve is how you see what the two together do to a signal; two graphs would
    show two halves of one answer. The GR meters are not shared, because how
    much each one is pulling is exactly what you need to tell them apart.
  -->
  <section ref="sectionRef" class="dyn det__panel" :class="{ 'dyn--bypassed': !dynIn }">
    <h4 class="det__h">
      {{ t('mixer.tabDynamics') }}
      <button
        class="det__byp"
        :class="{ 'det__byp--on': !dynIn }"
        :disabled="!bus"
        :title="t('mixer.bypassHint')"
        @click="toggleSection"
      >{{ t('mixer.bypass') }}</button>
    </h4>

    <div class="dyn__body">
      <div class="dyn__inner">
      <div class="dyn__viz">
      <!-- Transfer curve: input level across, output level down. Unity is the
           diagonal; the gate pulls the bottom-left down and the compressor
           flattens the top-right. Flat-unity until there is something to
           plot. -->
      <div class="dyn__graph">
        <svg viewBox="0 0 120 120" preserveAspectRatio="none" class="dyn__svg">
          <defs>
            <!-- The level meter's colours, left to right along the input axis:
                 the same zones every other meter in the app uses. -->
            <linearGradient :id="gradId" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="120" y2="0">
              <stop v-for="s in gradientStops" :key="s.offset" :offset="s.offset" :stop-color="s.color" />
            </linearGradient>
          </defs>
          <!-- The live input level, as the area under the curve up to where the
               signal is now: its width is how loud the input is, its top edge is
               what comes out. You see the signal meet the threshold, which is
               what a threshold is set against. -->
          <polygon v-if="levelPoints" class="dyn__level" :points="levelPoints" :fill="`url(#${gradId})`" />
          <line v-for="g in [30, 60, 90]" :key="'v' + g" class="dyn__grid" :x1="g" :x2="g" y1="0" y2="120" />
          <line v-for="g in [30, 60, 90]" :key="'h' + g" class="dyn__grid" x1="0" x2="120" :y1="g" :y2="g" />
          <!-- Unity, as a dashed guide: bottom-left is quiet in and quiet out,
               top-right is loud in and loud out. The curve below sits on this
               wherever nothing is being done to the signal. -->
          <line class="dyn__unity" x1="0" y1="120" x2="120" y2="0" />
          <!-- ONE curve for both processors, which is the point of sharing the
               graph: the gate bends the bottom-left corner down and the
               compressor flattens the top-right, and what you want to see is
               the single shape the two of them together impose. It tracks the
               knobs, so the effect of a ratio, a range or a knee is visible
               while it is being set. -->
          <polyline class="dyn__curve" :points="curvePoints" />
          <circle v-if="levelPoints" class="dyn__dot" :cx="xFor(shownIn)" :cy="yFor(outputFor(shownIn))" r="2.2" />
          <!-- Where each processor starts working. -->
          <line
            v-if="gateActive"
            class="dyn__thresh"
            :x1="xFor(gateValues.threshold)" :x2="xFor(gateValues.threshold)"
            y1="0" y2="120"
          />
          <line
            v-if="compActive"
            class="dyn__thresh"
            :x1="xFor(compValues.threshold)" :x2="xFor(compValues.threshold)"
            y1="0" y2="120"
          />
        </svg>
      </div>

      <!-- Gain reduction, one per processor. Deliberately not StereoMeter:
           that measures signal level against the project's output target,
           and this measures how far a processor is pulling down — a different
           quantity on a different scale.

           Each fills downward from the top by how far its processor is
           pulling, which is the direction gain reduction actually moves. Just
           meters — the in/out switches live beside the processors' names,
           where they can be found; having them here as well would be two
           controls for one thing. -->
      <div class="dyn__grmeters">
        <div class="dyn__gr">
          <div class="dyn__grtrack">
            <div class="dyn__grfill" :style="{ height: gateGrPct + '%' }"></div>
          </div>
          <span class="dyn__grlabel" :class="{ 'dyn__grlabel--on': gateOn }">
            {{ t('mixer.gateShort') }}
          </span>
        </div>
        <div class="dyn__gr">
          <div class="dyn__grtrack">
            <div class="dyn__grfill" :style="{ height: compGrPct + '%' }"></div>
          </div>
          <span class="dyn__grlabel" :class="{ 'dyn__grlabel--on': compOn }">
            {{ t('mixer.compShort') }}
          </span>
        </div>
      </div>
      </div>

      <div class="dyn__controls">
        <div class="dyn__group" :class="{ 'dyn__group--out': !gateOn }">
          <h5 class="dyn__h">
            {{ t('mixer.gate') }}
            <!-- The processor's in/out, next to its name where it can be
                 found. It was only on the meter label beside the graph, as a
                 sideways two-letter tag, which is not a switch anyone would
                 spot — so the section looked greyed out and inert with no
                 obvious way to bring it in. -->
            <button
              class="dyn__in"
              :class="{ 'dyn__in--on': gateOn }"
              :disabled="!bus"
              :title="t('mixer.gateToggle')"
              @click="toggleGate"
            >{{ gateOn ? t('mixer.inCircuit') : t('mixer.outOfCircuit') }}</button>
          </h5>
          <div class="dyn__row">
            <KnobField
              v-for="p in gateParams" :key="p.field"
              :value="gateValues[p.field]" :min="p.min" :max="p.max" :origin="p.origin"
              :taper="p.taper" :decimals="p.decimals" :unit="p.unit" :label="t(p.key)"
              :size="knobSize" :disabled="!bus"
              @input="(v: number) => onGate(p.field, v)"
            />
          </div>
        </div>

        <div class="dyn__group" :class="{ 'dyn__group--out': !compOn }">
          <h5 class="dyn__h">
            {{ t('mixer.compressor') }}
            <button
              class="dyn__in"
              :class="{ 'dyn__in--on': compOn }"
              :disabled="!bus"
              :title="t('mixer.compToggle')"
              @click="toggleComp"
            >{{ compOn ? t('mixer.inCircuit') : t('mixer.outOfCircuit') }}</button>
          </h5>
          <div class="dyn__row">
            <KnobField
              v-for="p in compParams" :key="p.field"
              :value="compValues[p.field]" :min="p.min" :max="p.max" :origin="p.origin"
              :taper="p.taper" :decimals="p.decimals" :unit="p.unit" :label="t(p.key)"
              :size="knobSize" :disabled="!bus"
              @input="(v: number) => onComp(p.field, v)"
            />
          </div>
        </div>
      </div>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch, type Ref } from 'vue';
import { useOutputTarget } from '~/composables/useOutputTarget';
import { gateOutput, compOutput } from '~/utils/dspCurves';
import KnobField from './KnobField.vue';
import type { Bus, BusComp, BusDsp, BusGate } from '~/types/project';
import { useMixerMeter } from '~/composables/useLiveMeters';

// Optional so the panel still renders before the first bus fetch lands.
const props = defineProps<{ bus?: Bus | null }>();

const emit = defineEmits<{
  (e: 'patch', id: string, patch: Partial<Bus>): void;
  (e: 'dsp-live', dsp: Partial<BusDsp>): void;
}>();

const { t } = useLocalization();
const server = useLiveplayServer();

// Whether the section is in circuit.
const dynIn = computed(() => props.bus?.dsp?.dynEnabled ?? true);

// Ranges are the conventional ones for each control. Times and the ratio are
// logarithmic: 1 ms to 2 ms is the same change of feel as 100 to 200, and a
// linear attack knob would spend nine tenths of its travel above 10 ms where
// almost nothing about a gate changes. Levels stay linear — decibels are
// already perceptually even.
//
// Hold starts at zero, where a logarithm has nowhere to go, so it is declared
// log and the knob falls back to linear on its own rather than being
// special-cased here.
type GateField = 'threshold' | 'ratio' | 'range' | 'attack' | 'hold' | 'release';
interface GateCtl {
  key: string; field: GateField; min: number; max: number; origin: number;
  decimals: number; unit: string; taper: 'linear' | 'log';
}
const gateParams: GateCtl[] = [
  { key: 'mixer.threshold', field: 'threshold', min: -80, max: 0,    origin: -40, decimals: 1, unit: 'dB', taper: 'linear' },
  { key: 'mixer.ratio',     field: 'ratio',     min: 1,   max: 20,   origin: 2,   decimals: 1, unit: ':1', taper: 'log' },
  { key: 'mixer.range',     field: 'range',     min: -80, max: 0,    origin: -20, decimals: 1, unit: 'dB', taper: 'linear' },
  { key: 'mixer.attack',    field: 'attack',    min: 0.1, max: 100,  origin: 1,   decimals: 1, unit: 'ms', taper: 'log' },
  { key: 'mixer.hold',      field: 'hold',      min: 0,   max: 1000, origin: 10,  decimals: 0, unit: 'ms', taper: 'log' },
  { key: 'mixer.release',   field: 'release',   min: 5,   max: 5000, origin: 100, decimals: 0, unit: 'ms', taper: 'log' },
];

const GATE_DEFAULTS: BusGate = {
  on: false, threshold: -40, ratio: 2, range: -20,
  attack: 1, hold: 10, release: 100,
};

// The compressor's ranges. Ratio runs to 60:1 rather than the gate's 20 because
// the top of this control is meant to be a limiter setting; attack goes to
// 300 ms because a slow-attack bus compressor letting transients through is a
// real way to use one, where a gate that took 300 ms to open would be broken.
//
// Knee is linear like the other level controls, and its origin is 6 dB rather
// than 0: a hard knee is the special case here, not the starting point.
type CompField = 'threshold' | 'ratio' | 'makeup' | 'attack' | 'knee' | 'release';
interface CompCtl {
  key: string; field: CompField; min: number; max: number; origin: number;
  decimals: number; unit: string; taper: 'linear' | 'log';
}
const compParams: CompCtl[] = [
  { key: 'mixer.threshold', field: 'threshold', min: -60, max: 0,    origin: -18, decimals: 1, unit: 'dB', taper: 'linear' },
  { key: 'mixer.ratio',     field: 'ratio',     min: 1,   max: 60,   origin: 4,   decimals: 1, unit: ':1', taper: 'log' },
  { key: 'mixer.makeup',    field: 'makeup',    min: -12, max: 24,   origin: 0,   decimals: 1, unit: 'dB', taper: 'linear' },
  { key: 'mixer.attack',    field: 'attack',    min: 0.1, max: 300,  origin: 10,  decimals: 1, unit: 'ms', taper: 'log' },
  { key: 'mixer.knee',      field: 'knee',      min: 0,   max: 24,   origin: 6,   decimals: 1, unit: 'dB', taper: 'linear' },
  { key: 'mixer.release',   field: 'release',   min: 5,   max: 5000, origin: 200, decimals: 0, unit: 'ms', taper: 'log' },
];

const COMP_DEFAULTS: BusComp = {
  on: false, threshold: -18, ratio: 4, makeup: 0,
  attack: 10, knee: 6, release: 200,
};

// Same live-then-persist shape as everything else on this channel: the strip
// gets the value on every drag event over a call that writes no document, and
// the bus is written once the gesture settles.
//
// Written once and used by both processors. The two need identical machinery —
// a local override, a hold flag so an in-flight echo does not stamp on the
// gesture, and a settle timer — and the second copy of it is where the two
// would drift apart.
function liveSection<T extends object>(
  key: 'gate' | 'comp',
  stored: () => T | undefined,
  defaults: T,
) {
  const local = ref<T | null>(null) as Ref<T | null>;
  let hold = false;
  let settle: ReturnType<typeof setTimeout> | null = null;

  const values = computed<T>(() => local.value ?? stored() ?? defaults);

  watch(stored, () => { if (!hold) local.value = null; }, { deep: true });
  watch(() => props.bus?.id, () => { local.value = null; });
  onBeforeUnmount(() => { if (settle) clearTimeout(settle); });

  function push(next: T, persistNow: boolean) {
    local.value = next;
    hold = true;
    // The panel draws its own curve from `local`, but the parent holds the
    // merged copy every other panel sees, so it is told too. Without this the
    // parent's view of the bus goes stale for the length of the gesture.
    emit('dsp-live', { [key]: next } as Partial<BusDsp>);
    void server.setBusDsp(props.bus!.id, { [key]: next } as Partial<BusDsp>).catch(() => {});
    if (settle) clearTimeout(settle);
    if (persistNow) {
      // A switch is a discrete press, not a gesture: there is no later event to
      // re-arm a settle timer, so a settled write would never arrive.
      hold = false;
      emit('patch', props.bus!.id, { dsp: { [key]: next } } as Partial<Bus>);
      return;
    }
    settle = setTimeout(() => {
      settle = null;
      hold   = false;
      emit('patch', props.bus!.id,
           { dsp: { [key]: local.value ?? next } } as Partial<Bus>);
    }, 250);
  }

  return { values, push };
}

const gate = liveSection<BusGate>('gate', () => props.bus?.dsp?.gate, GATE_DEFAULTS);
const comp = liveSection<BusComp>('comp', () => props.bus?.dsp?.comp, COMP_DEFAULTS);

const gateValues = gate.values;
const compValues = comp.values;
const gateOn = computed(() => gateValues.value.on);
const compOn = computed(() => compValues.value.on);

function onGate(field: GateField, v: number) {
  if (!props.bus) return;
  gate.push({ ...gateValues.value, [field]: v }, false);
}
function onComp(field: CompField, v: number) {
  if (!props.bus) return;
  comp.push({ ...compValues.value, [field]: v }, false);
}

function toggleGate() {
  if (!props.bus) return;
  gate.push({ ...gateValues.value, on: !gateOn.value }, true);
}
function toggleComp() {
  if (!props.bus) return;
  comp.push({ ...compValues.value, on: !compOn.value }, true);
}

// The section bypass. Takes out everything in the panel at once and gives it
// back untouched, which is the difference between it and switching each
// processor off individually.
function toggleSection() {
  if (!props.bus) return;
  const next = !dynIn.value;
  emit('dsp-live', { dynEnabled: next });
  void server.setBusDsp(props.bus.id, {
    // Both processors are carried alongside, because /dsp merges onto the
    // STORED bus: a lone dynEnabled would re-enable the section using whatever
    // the document last saved rather than what is on the surface right now.
    dynEnabled: next, gate: gateValues.value, comp: compValues.value,
  }).catch(() => {});
  emit('patch', props.bus.id, { dsp: { dynEnabled: next } } as Partial<Bus>);
}

// ---- Transfer curve ------------------------------------------------------
// Input level across, output level down, over a 60 dB window. The diagonal is
// unity; the gate bends the bottom-left corner downward and the compressor
// flattens the top-right.
//
// The graph is the only place the shape of a ratio, a range or a knee setting
// is visible — the numbers alone do not tell you what a 10:1 expander with a
// -6 dB range will actually do — so it has to track the knobs rather than
// waiting for anything to settle.
const GRAPH_MIN_DB = -60;
const GRAPH_MAX_DB = 0;
const xFor = (db: number) =>
  ((Math.max(GRAPH_MIN_DB, Math.min(GRAPH_MAX_DB, db)) - GRAPH_MIN_DB) /
   (GRAPH_MAX_DB - GRAPH_MIN_DB)) * 120;
const yFor = (db: number) =>
  120 - ((Math.max(GRAPH_MIN_DB, Math.min(GRAPH_MAX_DB, db)) - GRAPH_MIN_DB) /
         (GRAPH_MAX_DB - GRAPH_MIN_DB)) * 120;

// Both switches have to be in for a processor to be doing anything, so the
// curve falls back to unity when either is out — the picture should agree with
// the audio, not with the knob positions.
const gateActive = computed(() => gateOn.value && dynIn.value);
const compActive = computed(() => compOn.value && dynIn.value);

// The gate's half. Mirrors the engine's static curve: below the threshold every
// decibel down costs (ratio - 1) more, until the range floor stops it going
// further.
// The static curves live in utils/dspCurves.ts, shared with the mixer strip's
// miniature so the two can never draw different shapes for the same settings
// (both mirror dynamics.hpp).
const gated      = (db: number) => gateOutput(gateValues.value, db);
const compressed = (db: number) => compOutput(compValues.value, db);

function outputFor(inputDb: number): number {
  let db = inputDb;
  if (gateActive.value) db = gated(db);
  if (compActive.value) db = compressed(db);
  return db;
}

// Half-decibel steps. The knee spans as little as a couple of decibels, and at
// the 1 dB steps this used to use it was drawn as a corner rather than a curve
// — which is precisely the thing the knee control is for.
const CURVE_POINTS = 121;
const curvePoints = computed(() =>
  Array.from({ length: CURVE_POINTS }, (_, i) => {
    const inDb = GRAPH_MIN_DB + (i / (CURVE_POINTS - 1)) * (GRAPH_MAX_DB - GRAPH_MIN_DB);
    return `${xFor(inDb).toFixed(2)},${yFor(outputFor(inDb)).toFixed(2)}`;
  }).join(' '));

// ---- Gain reduction ------------------------------------------------------
// Reported per strip rather than per lane, because both detectors are linked
// across the lanes and so there is one figure per processor for the channel.
const meter = useMixerMeter(() => props.bus?.mixerId);
// The track's full height, in decibels of reduction. Different per processor
// because they work over different depths: a gate's range runs to 80 dB, while
// a compressor pulling more than 20 is already an unusual amount, and scaling
// both to the deeper one would leave the compressor's bar barely moving.
const GATE_FULL_DB = 40;
const COMP_FULL_DB = 20;
const grPct = (gr: number, full: number) =>
  Math.min(100, (Math.abs(Math.min(0, gr)) / full) * 100);
const gateGrPct = computed(() => grPct(meter.gateGr.value, GATE_FULL_DB));
const compGrPct = computed(() => grPct(meter.compGr.value, COMP_FULL_DB));

// ---- Input level on the curve --------------------------------------------
// The server's peak-since-last-frame, with meter ballistics: rises at once,
// falls at 20 dB/s, so it reads like the meters beside it instead of flicker.
const FALL_DB_PER_S = 20;
const shownIn = ref(-120);
let lastAt = 0;
watch(() => meter.dynIn.value, (db) => {
  const now = performance.now();
  const dt = lastAt ? (now - lastAt) / 1000 : 0;
  lastAt = now;
  shownIn.value = Math.max(db, shownIn.value - FALL_DB_PER_S * dt);
});
const levelPoints = computed(() => {
  const inDb = shownIn.value;
  if (inDb <= GRAPH_MIN_DB) return '';
  const pts: string[] = [`0,120`];
  const steps = 60;
  for (let i = 0; i <= steps; i++) {
    const db = GRAPH_MIN_DB + (i / steps) * (inDb - GRAPH_MIN_DB);
    pts.push(`${xFor(db).toFixed(2)},${yFor(outputFor(db)).toFixed(2)}`);
  }
  pts.push(`${xFor(inDb).toFixed(2)},120`);
  return pts.join(' ');
});
// The app's meter colours, sampled every 3 dB along the input axis.
const { colorForLevel } = useOutputTarget();
const gradId = `dynlvl-${Math.random().toString(36).slice(2, 9)}`;
const gradientStops = computed(() => {
  const out: { offset: string; color: string }[] = [];
  for (let db = GRAPH_MIN_DB; db <= GRAPH_MAX_DB; db += 3) {
    out.push({ offset: `${((db - GRAPH_MIN_DB) / (GRAPH_MAX_DB - GRAPH_MIN_DB) * 100).toFixed(1)}%`, color: colorForLevel(db) });
  }
  return out;
});

// Knobs grow with the panel, so a big screen gets bigger targets rather than
// more empty space.
const sectionRef = ref<HTMLElement | null>(null);
const knobSize = ref(28);
let knobRo: ResizeObserver | null = null;
onMounted(() => {
  if (!sectionRef.value) return;
  knobRo = new ResizeObserver(([entry]) => {
    const w = entry?.contentRect.width ?? 0;
    const h = entry?.contentRect.height ?? 0;
    knobSize.value = w > 760 && h > 420 ? 40 : w > 560 && h > 320 ? 34 : 28;
  });
  knobRo.observe(sectionRef.value);
});
onBeforeUnmount(() => knobRo?.disconnect());
</script>

<style scoped>
.dyn { min-height: 0; }
.dyn > .det__h { flex: 0 0 auto; }

.dyn__body {
  flex: 1 1 auto;
  min-height: 0;
  /* The graph and the GR meters size themselves against this box, so on a
     large screen the curve grows instead of sitting in a sea of space. */
  container-type: size;
  /* container-type: size takes the box's height from its parent, not its
     content — so in the stacked layouts, where rows size to content, it needs
     a floor or it collapses. */
  min-height: 220px;
}

/* Square, so the transfer curve keeps its 1:1 reading — a stretched dynamics
   graph lies about the slope. Capped, because square plus full panel height
   means it grows without limit on a tall window and swallows the panel; past
   this size it stops telling you anything more. */
/* Sized, not stretched: the box has no content of its own, so stretching was
   the only thing giving it height — and stretching is also what pinned it to
   the top of a panel taller than it. A definite height lets it centre against
   the controls beside it, which is where the eye expects the curve to sit. */
.dyn__inner {
  display: flex;
  align-items: center;
  gap: var(--spacing-md, 12px);
  width: 100%;
  height: 100%;
}
/* `flex: 0 1 auto` and min-width:0, not `0 0 auto`. The graph and GR meters are
   sized from the container (50cqw / 100cqh), and at some widths that sum comes
   out larger than the row has to give. Unshrinkable, the block simply took the
   space and pushed the controls beside it out of the panel — which is the
   overlap, seen from the other end. It may now give way; the graph's own
   max(110px, …) floor stops it collapsing to nothing. */
.dyn__viz {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  flex: 0 1 auto;
  min-width: 0;
}
.dyn__graph {
  flex: 0 0 auto;
  align-self: center;
  /* Square (a stretched transfer curve lies about the slope), as big as the
     box allows: the full height, or half the width, whichever is smaller. */
  width: max(110px, min(50cqw, 100cqh));
  height: max(110px, min(50cqw, 100cqh));
  /* The width above is computed from the container, which does not know what
     the GR meters beside it are taking. Capping at the space actually left
     stops the pair adding up to more than the row has. */
  max-width: 100%;
  background: var(--color-background);
  border-radius: var(--border-radius-sm);
  overflow: hidden;
}
.dyn__svg { display: block; width: 100%; height: 100%; }
/* non-scaling-stroke throughout: the viewBox is stretched to the panel with
   preserveAspectRatio="none", so an ordinary stroke is stretched with it and
   renders heavier — and unevenly — as the panel grows. */
.dyn__grid  {
  stroke: var(--color-border);
  stroke-width: 1;
  vector-effect: non-scaling-stroke;
  opacity: 0.5;
}
.dyn__curve {
  fill: none;
  stroke: var(--color-accent);
  stroke-width: 1.5;
  vector-effect: non-scaling-stroke;
  stroke-linejoin: round;
}
.dyn__level { opacity: 0.55; }
.dyn__dot {
  fill: var(--color-text-primary);
  stroke: var(--color-background);
  stroke-width: 1;
  vector-effect: non-scaling-stroke;
}
.dyn__thresh {
  stroke: var(--color-text-secondary);
  stroke-width: 1;
  stroke-dasharray: 2 3;
  vector-effect: non-scaling-stroke;
  opacity: 0.7;
}

/* The in/out switch beside each processor's name. */
.dyn__in {
  margin-left: auto;
  padding: 0 4px;
  font-size: 8px;
  font-family: var(--font-mono);
  letter-spacing: 0.06em;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.dyn__in:hover:not(:disabled) { color: var(--color-text-primary); }
.dyn__in:disabled { opacity: 0.4; cursor: not-allowed; }
.dyn__in--on {
  color: #fff;
  background: var(--color-accent);
  border-color: var(--color-accent);
}
/* Unity is a GUIDE, not a second curve, and it has to look like one.

   Drawn solid it was indistinguishable from the transfer curve, which lies
   exactly on top of it everywhere the processors are doing nothing — so the
   two strokes coincided above the threshold and read as a single heavier
   line, then separated below it and looked like two different curves. Dashed
   and faint, the curve reads as sitting on the guide. */
.dyn__unity {
  stroke: var(--color-text-disabled);
  stroke-width: 1;
  stroke-dasharray: 3 3;
  vector-effect: non-scaling-stroke;
  opacity: 0.35;
}

/* Sized and centred to match the graph, so the two read as one block. */
.dyn__grmeters {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  flex: 0 0 auto;
  align-self: center;
  height: max(100px, min(50cqw, 100cqh));
  min-height: 0;
}
.dyn__gr {
  display: flex;
  align-items: center;
  gap: 3px;
  flex: 1 1 0;
  min-height: 0;
}
.dyn__grtrack {
  position: relative;
  width: 8px;
  height: 100%;
  min-height: 24px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 2px;
  overflow: hidden;
}
/* Grows DOWN from the top, because that is the direction gain reduction moves:
   the bar is how much is being taken away, not how much is getting through. */
.dyn__grfill {
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  background: var(--color-accent);
  transition: height 60ms linear;
}
.dyn__grlabel {
  font-family: var(--font-mono);
  font-size: 8px;
  writing-mode: vertical-rl;
  color: var(--color-text-disabled);
}
.dyn__grlabel--on { color: var(--color-accent); }

/* A processor that is switched out keeps its controls readable and adjustable
   — setting a gate up before switching it in is a normal way to work — but
   recedes so the panel says at a glance what is actually running.
   Deliberately light: at 0.5 the knobs read as disabled, and they are not. */
.dyn__group--out { opacity: 0.7; }
.dyn--bypassed .dyn__controls,
.dyn--bypassed .dyn__grmeters { opacity: 0.45; }

/* The two groups sit centred in whatever height the panel has, packed to the
   left rather than stretched across it. */
.dyn__controls {
  display: flex;
  flex-direction: column;
  justify-content: center;
  gap: 8px;
  flex: 1 1 auto;
  min-width: 0;
}
.dyn__group { display: flex; flex-direction: column; gap: 1px; }
/* Flex so the in/out switch can sit at the far end of the heading rather than
   trailing the text. */
.dyn__h {
  display: flex;
  align-items: center;
  gap: var(--spacing-xs);
  margin: 0;
  font-size: 9px;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: var(--color-text-disabled);
}
/* Six controls, always three across, so each processor reads as two tidy rows
   rather than reflowing into a ragged block as the panel resizes.
   Columns are sized to the knobs rather than to the panel: 1fr columns spread
   the six controls across whatever width was going, which left a group reading
   as scattered dots instead of a block you can take in at once.

   minmax(0, auto) rather than a bare auto: an `auto` track will not shrink
   below its min-content, so when the row had less width than its three knobs
   wanted it overflowed the panel rather than tightening up. The max stays
   `auto`, so nothing about the sizing changes while there is room — this only
   decides what happens when there is not. */
.dyn__row {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, auto));
  justify-content: start;
  gap: 2px 10px;
}

/* Last, so it overrides the base sizes above (same specificity). */
/* A panel taller than it is wide (the usual shape of this column on a big
   screen) stacks the curve over the controls and gives it the full width,
   instead of centring a small square in a tall, empty box. */
@container (orientation: portrait) {
  .dyn__inner { flex-direction: column; justify-content: center; }
  .dyn__graph { width: max(110px, min(100cqw - 40px, 58cqh)); height: max(110px, min(100cqw - 40px, 58cqh)); }
  .dyn__grmeters { height: max(100px, min(100cqw - 40px, 58cqh)); }
  .dyn__controls { flex: 0 0 auto; width: 100%; min-width: 0; }
  /* Same minmax(0, …) reasoning as the three-across rule above, and it matters
     more here: six tracks that cannot tighten overflow a good deal sooner. */
  .dyn__row { grid-template-columns: repeat(6, minmax(0, auto)); justify-content: space-between; }
}
</style>
