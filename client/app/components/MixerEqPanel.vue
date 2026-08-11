<template>
  <!--
    Four-band EQ.

    The grid runs bands across and parameters down: one column per band, rows
    for frequency, gain and Q. That way a column is a band — the thing you
    actually reach for — and comparing the same parameter across bands is a
    glance along a row. The transpose (parameters across, bands down) reads
    worse for both.

    Every cell is a knob and a typeable box, because an EQ set by ear and an EQ
    set from a spec sheet are both real jobs.

    The curve is live and includes the channel's high- and low-pass, which live
    on the fader column but shape this same signal — an EQ display that ignored
    them would be drawing a lie.

    The handles on the curve are draggable: sideways sets frequency, up and
    down sets gain, and the wheel over a handle sets Q (or a shelf's slope).
    That is the same set of parameters the knobs below reach, addressed the
    other way round — by pointing at the shape you want rather than by naming
    the numbers that make it.
  -->
  <section class="eq det__panel" :class="{ 'eq--bypassed': !eqIn }">
    <h4 class="det__h">
      {{ t('mixer.tabEq') }}
      <!-- Lights when the section is OUT, as a bypass button does on a desk:
           the lamp means "this is not in circuit", which is the state worth
           spotting from across a room. -->
      <button
        class="det__byp"
        :class="{ 'det__byp--on': !eqIn }"
        :disabled="!bus"
        :title="t('mixer.bypassHint')"
        @click="toggleEq"
      >{{ t('mixer.bypass') }}</button>
    </h4>

    <!-- Response curve. Flat, with a handle per band at its frequency —
         the shape the real curve will take, so the panel does not change
         layout when it starts working. This is the part that grows into
         spare height; the band controls below it are never squeezed. -->
    <div ref="graphRef" class="eq__graph">
      <svg viewBox="0 0 400 120" preserveAspectRatio="none" class="eq__svg">
        <line
          v-for="g in gridDb" :key="'g' + g"
          class="eq__grid" x1="0" x2="400" :y1="yFor(g)" :y2="yFor(g)"
        />
        <line class="eq__zero" x1="0" x2="400" :y1="yFor(0)" :y2="yFor(0)" />
        <!-- The grid lines are stretched with the box too; pinning their width
             the same way stops them thickening as the panel grows. -->
        <polyline class="eq__curve" :points="curvePoints" />
      </svg>
      <!-- A handle sits at its band's frequency and rides its gain, so the
           marker is on the part of the curve it made — and dragging it is how
           you set both at once.

           Pointer events rather than the mouse events Knob uses: they cover
           pen and touch for free, and the window's resize handles already work
           this way. Knob is not worth converting on its own account, but there
           is no reason to add a second mouse-only control. -->
      <span
        v-for="(b, i) in bands" :key="'h' + i"
        class="eq__handle eq__handle--band"
        :class="{ 'eq__handle--out': b.gain === 0, 'eq__handle--held': dragBand === i }"
        :style="{
          left: xPctFor(b.freq) + '%',
          top: (yFor(Math.max(-GRAPH_DB, Math.min(GRAPH_DB, b.gain))) / 120 * 100) + '%',
          background: handleColor(i),
        }"
        :title="handleTitle(i)"
        @pointerdown.stop="onHandleDown(i, $event)"
        @wheel.prevent.stop="onHandleWheel(i, $event)"
        @dblclick.stop="onHandleReset(i)"
      >{{ EQ_BAND_NAMES[i] }}</span>
      <!-- The filters get markers too, so a corner in the curve can be traced
           to the knob that put it there rather than looking like an EQ band
           nobody moved. Only shown when the filter is in circuit. -->
      <span
        v-if="hpfIn"
        class="eq__handle eq__handle--filter"
        :style="{ left: xPctFor(bus!.dsp.hpf.freq) + '%' }"
      >{{ t('mixer.hpf') }}</span>
      <span
        v-if="lpfIn"
        class="eq__handle eq__handle--filter"
        :style="{ left: xPctFor(bus!.dsp.lpf.freq) + '%' }"
      >{{ t('mixer.lpf') }}</span>
    </div>

    <div class="eq__grid-controls">
      <!-- Header row: the bands. A band's name lights while it is in circuit,
           which for a bell means its gain is off zero — the same rule the
           engine uses to decide whether to run the section at all. -->
      <span class="eq__rowlabel"></span>
      <span
        v-for="(b, i) in bands" :key="'n' + i"
        class="eq__bandname"
        :class="{ 'eq__bandname--in': b.gain !== 0 }"
      >{{ EQ_BAND_NAMES[i] }}</span>

      <!-- Frequency is logarithmic: what the ear hears is the ratio between
           two frequencies, so an octave should take the same arc wherever it
           sits. On a linear taper 1 kHz would land at 5% of travel. -->
      <span class="eq__rowlabel">{{ t('mixer.freq') }}</span>
      <KnobField
        v-for="(b, i) in bands" :key="'f' + i"
        :value="b.freq" :min="20" :max="20000" :origin="bandDefaults[i]!.freq"
        taper="log"
        :decimals="0" unit="Hz" :size="30" :show-label="false"
        :disabled="!bus"
        @input="(v: number) => onBand(i, 'freq', v)"
      />

      <span class="eq__rowlabel">{{ t('mixer.gain') }}</span>
      <KnobField
        v-for="(b, i) in bands" :key="'g' + i"
        :value="b.gain" :min="-18" :max="18" :origin="0"
        :decimals="1" unit="dB" :size="30" :show-label="false"
        :disabled="!bus"
        @input="(v: number) => onBand(i, 'gain', v)"
      />

      <!-- Q is logarithmic too: 0.5 to 1 is the same change of shape as 4 to
           8, and a linear taper would waste most of the dial above Q 3 where
           the differences stop being audible.

           On a shelved band this knob drives SLOPE instead, which is a
           different quantity on a different range — so the row label changes
           with it, and the two values are stored separately. Switching a band
           to a shelf and back leaves the bell exactly as it was. -->
      <span class="eq__rowlabel">{{ anyShelf ? t('mixer.qOrSlope') : t('mixer.q') }}</span>
      <KnobField
        v-for="(b, i) in bands" :key="'q' + i"
        :value="b.shelf ? b.slope : b.q"
        :min="0.1" :max="b.shelf ? 2 : 10"
        :origin="b.shelf ? 1 : bandDefaults[i]!.q"
        taper="log"
        :decimals="2" :size="30" :show-label="false"
        :disabled="!bus"
        :title="b.shelf ? t('mixer.slopeHint') : t('mixer.qHint')"
        @input="(v: number) => onBand(i, b.shelf ? 'slope' : 'q', v)"
      />

      <!-- Bell or shelf, on the outer bands only. The middle two get a blank
           cell rather than a disabled button: there is no decision to make
           there, and an inert control invites the question of why it will not
           press. -->
      <span class="eq__rowlabel">{{ t('mixer.shape') }}</span>
      <template v-for="(b, i) in bands" :key="'s' + i">
        <button
          v-if="canShelve(i)"
          class="eq__shape"
          :class="{ 'eq__shape--shelf': b.shelf }"
          :disabled="!bus"
          :title="t('mixer.shapeHint')"
          @click="onShape(i, !b.shelf)"
        >{{ b.shelf ? (i === 0 ? t('mixer.lowShelf') : t('mixer.highShelf')) : t('mixer.bell') }}</button>
        <span v-else class="eq__shapeblank">{{ t('mixer.bell') }}</span>
      </template>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import KnobField from './KnobField.vue';
import { METER_COLORS } from '~/composables/useOutputTarget';
import type { Bus, BusDsp, BusEqBand } from '~/types/project';
import {
  EQ_BAND_NAMES, EQ_SHELVABLE_BANDS, HPF_PARKED_HZ, LPF_PARKED_HZ,
} from '~/types/project';
import type { BiquadCoeffs } from '~/utils/filterResponse';
import {
  biquadHighpass, biquadHighShelf, biquadLowpass, biquadLowShelf, biquadPeaking,
  combinedMagnitudeDb,
} from '~/utils/filterResponse';

// The bus whose curve this is. Optional so the panel still renders (flat)
// before the first bus fetch lands.
const props = defineProps<{ bus?: Bus | null }>();

const emit = defineEmits<{
  (e: 'patch', id: string, patch: Partial<Bus>): void;
  /** In-flight band values, so the curve tracks the knob. See the fader. */
  (e: 'dsp-live', dsp: Partial<BusDsp>): void;
}>();

const { t } = useLocalization();
const server = useLiveplayServer();

// The conventional four-band starting layout. Doubles as each knob's origin,
// so a double-click puts a band back where it started rather than at zero
// Hertz — and it must match the server's defaults or a fresh bus would appear
// to have been moved already.
const bandDefaults: BusEqBand[] = [
  { freq: 100,   gain: 0, q: 0.7, shelf: false, slope: 1 },
  { freq: 500,   gain: 0, q: 1.0, shelf: false, slope: 1 },
  { freq: 2500,  gain: 0, q: 1.0, shelf: false, slope: 1 },
  { freq: 10000, gain: 0, q: 0.7, shelf: false, slope: 1 },
];

const canShelve = (i: number) => (EQ_SHELVABLE_BANDS as readonly number[]).includes(i);
const anyShelf  = computed(() => bands.value.some(b => b.shelf));

// The bands as displayed: the bus's, held locally while a knob is moving.
//
// Same live-then-persist shape as the filters and the fader. The strip gets
// new coefficients on every drag event over a call that writes no document,
// and the bus is written once the gesture settles. Binding straight to the bus
// would PATCH and refetch per event, and the knob would fight the round trip.
const localBands = ref<BusEqBand[] | null>(null);
let   bandHold   = false;
let   bandSettle: ReturnType<typeof setTimeout> | null = null;

const bands = computed<BusEqBand[]>(() =>
  localBands.value
  ?? props.bus?.dsp?.eq
  ?? bandDefaults);

watch(() => props.bus?.dsp?.eq, () => { if (!bandHold) localBands.value = null; },
      { deep: true });
watch(() => props.bus?.id, () => { localBands.value = null; });

onBeforeUnmount(() => { if (bandSettle) clearTimeout(bandSettle); });

// Whether the section is in circuit. Held locally through the round trip for
// the same reason the bands are, so the button responds to the press.
const localEqIn = ref<boolean | null>(null);
const eqIn = computed(() => localEqIn.value ?? props.bus?.dsp?.eqEnabled ?? true);
watch(() => props.bus?.dsp?.eqEnabled, () => { localEqIn.value = null; });
watch(() => props.bus?.id, () => { localEqIn.value = null; });

function toggleEq() {
  if (!props.bus) return;
  const next = !eqIn.value;
  localEqIn.value = next;
  emit('dsp-live', { eqEnabled: next });
  void server.setBusDsp(props.bus.id, { eqEnabled: next }).catch(() => {});
  // Bypass is a discrete press rather than a gesture, so it persists straight
  // away instead of waiting for a settle timer that will never be re-armed.
  emit('patch', props.bus.id, { dsp: { eqEnabled: next } } as Partial<Bus>);
}

// One band, several fields at once. Dragging a handle moves frequency and gain
// together, so a single-key setter would have sent two calls per pointer move
// and let the two settle timers race each other.
function pushBand(index: number, patch: Partial<BusEqBand>, persistNow = false) {
  if (!props.bus) return;
  const next = bands.value.map((b, i) => (i === index ? { ...b, ...patch } : { ...b }));
  localBands.value = next;
  bandHold = true;
  // The curve is told first, so it tracks the knob rather than the round trip.
  emit('dsp-live', { eq: next });
  void server.setBusDsp(props.bus.id, { eq: next }).catch(() => {});
  if (bandSettle) clearTimeout(bandSettle);
  if (persistNow) {
    // A switch is a discrete press, not a gesture: nothing later will re-arm a
    // settle timer, so a settled write would never arrive.
    bandHold = false;
    emit('patch', props.bus.id, { dsp: { eq: next } } as Partial<Bus>);
    return;
  }
  bandSettle = setTimeout(() => {
    bandSettle = null;
    bandHold   = false;
    emit('patch', props.bus!.id, { dsp: { eq: localBands.value ?? next } } as Partial<Bus>);
  }, 250);
}

function onBand(index: number, key: keyof BusEqBand, value: number) {
  pushBand(index, { [key]: value } as Partial<BusEqBand>);
}

function onShape(index: number, shelf: boolean) {
  if (!canShelve(index)) return;
  pushBand(index, { shelf }, true);
}

const GRAPH_DB = 18;               // curve spans +/- this
const gridDb = [12, 6, -6, -12];

// Frequency axis is logarithmic, as an EQ display always is: an octave takes
// the same width everywhere, so 100-200 Hz reads as wide as 1-2 kHz.
const LO = Math.log10(20);
const HI = Math.log10(20000);
const xPctFor = (hz: number) => ((Math.log10(hz) - LO) / (HI - LO)) * 100;
const yFor = (db: number) => 60 - (db / GRAPH_DB) * 60;

// The curve, including the channel's high- and low-pass.
//
// The filters live on the fader column, not in this panel, but they shape the
// same signal and an EQ display that ignored them would be drawing a lie: turn
// a 400 Hz high-pass in and the bottom of the band goes with it, whatever the
// LF band says. Every section that touches the audio belongs in the picture.
//
// The maths mirrors the C++ engine (see utils/filterResponse.ts). It is a
// second model of the same filters, so it is display-only and the two have to
// be kept in step.
const SAMPLE_RATE = 48000;

const sections = computed<BiquadCoeffs[]>(() => {
  const out: BiquadCoeffs[] = [];
  const hpf = props.bus?.dsp?.hpf;
  const lpf = props.bus?.dsp?.lpf;
  // Parked at the end of its travel is out of circuit — the same rule the
  // server applies when it decides whether to run the section at all.
  if (hpf && hpf.freq > HPF_PARKED_HZ) {
    out.push(biquadHighpass(hpf.freq, SAMPLE_RATE, hpf.q || 0.7071));
  }
  if (lpf && lpf.freq < LPF_PARKED_HZ) {
    out.push(biquadLowpass(lpf.freq, SAMPLE_RATE, lpf.q || 0.7071));
  }
  // A bypassed section contributes nothing to the curve. The filters still do:
  // they are separate controls on the fader column and this button does not
  // reach them. Without this the curve would keep drawing a shape the audio
  // no longer has, which is worse than not drawing it at all.
  if (eqIn.value) {
    bands.value.forEach((b, i) => {
      // 0 dB is an identity for a shelf as much as for a bell, so a flat band
      // contributes nothing to draw — the same rule the engine uses to decide
      // whether to run the section at all.
      if (b.gain === 0) return;
      if (b.shelf && canShelve(i)) {
        out.push(i === 0
          ? biquadLowShelf(b.freq, SAMPLE_RATE, b.gain, b.slope)
          : biquadHighShelf(b.freq, SAMPLE_RATE, b.gain, b.slope));
      } else {
        out.push(biquadPeaking(b.freq, SAMPLE_RATE, b.gain, b.q));
      }
    });
  }
  return out;
});

// Sampled along the same log axis the handles sit on, so a corner lands under
// its knob rather than a few pixels off it.
const CURVE_POINTS = 81;
const curvePoints = computed(() => {
  const secs = sections.value;
  return Array.from({ length: CURVE_POINTS }, (_, i) => {
    const x  = (i / (CURVE_POINTS - 1)) * 400;
    const hz = Math.pow(10, LO + (i / (CURVE_POINTS - 1)) * (HI - LO));
    const db = secs.length ? combinedMagnitudeDb(secs, hz, SAMPLE_RATE) : 0;
    // Clamped to the drawn range: a 24 dB/octave skirt heads for -80 dB and
    // would otherwise draw a vertical spike off the bottom of the box.
    const clamped = Math.max(-GRAPH_DB, Math.min(GRAPH_DB, db));
    return `${x.toFixed(1)},${yFor(clamped).toFixed(1)}`;
  }).join(' ');
});

// ---- Dragging a handle ---------------------------------------------------
// Sideways is frequency, up and down is gain, and the wheel is Q — the three
// things a band has, reached by pointing at the shape instead of naming the
// numbers. The knobs below stay: setting an EQ by ear and setting one from a
// spec sheet are both real jobs, and this only serves the first.
//
// Movement is measured as a DELTA from where the handle was grabbed rather
// than by putting the handle under the pointer. Both feel identical at normal
// sensitivity, but only the delta form has anywhere to put shift-for-fine —
// which matters most here, where a whole decade of frequency can be a
// centimetre of travel.
const FREQ_MIN = 20;
const FREQ_MAX = 20000;
const graphRef  = ref<HTMLElement | null>(null);
const dragBand  = ref<number | null>(null);

function onHandleDown(index: number, e: PointerEvent) {
  if (!props.bus || e.button !== 0) return;
  const box = graphRef.value?.getBoundingClientRect();
  if (!box || box.width <= 0 || box.height <= 0) return;

  const start = bands.value[index]!;
  const startX = e.clientX;
  const startY = e.clientY;
  // Where the band sits now, in the graph's own normalised coordinates.
  const startFreqPct = xPctFor(start.freq) / 100;
  const startGainDb  = Math.max(-GRAPH_DB, Math.min(GRAPH_DB, start.gain));

  dragBand.value = index;
  const el = e.currentTarget as HTMLElement;
  try { el.setPointerCapture(e.pointerId); } catch { /* capture is best-effort */ }

  const move = (ev: PointerEvent) => {
    const fine = ev.shiftKey ? 0.25 : 1;
    const dxPct = ((ev.clientX - startX) / box.width) * fine;
    const dyDb  = -((ev.clientY - startY) / box.height) * (GRAPH_DB * 2) * fine;

    const pct  = Math.max(0, Math.min(1, startFreqPct + dxPct));
    const freq = Math.pow(10, LO + pct * (HI - LO));
    const gain = Math.max(-GRAPH_DB, Math.min(GRAPH_DB, startGainDb + dyDb));

    pushBand(index, {
      // Whole hertz low down where a fraction is meaningless, and the same
      // resolution the frequency knob reports so the two agree.
      freq: Math.round(Math.max(FREQ_MIN, Math.min(FREQ_MAX, freq))),
      gain: Math.round(gain * 10) / 10,
    });
  };
  const up = (ev: PointerEvent) => {
    try { el.releasePointerCapture(ev.pointerId); } catch { /* already gone */ }
    el.removeEventListener('pointermove', move);
    el.removeEventListener('pointerup', up);
    el.removeEventListener('pointercancel', up);
    dragBand.value = null;
  };
  el.addEventListener('pointermove', move);
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
}

// The wheel sets the band's width — Q on a bell, slope on a shelf. Multiplied
// rather than added, because both are ratios: one notch should be the same
// proportional change at 0.3 as at 8.
//
// No pinch gesture. Nothing else in this app handles multi-touch — Knob and
// CanvasFader are mouse-event only, and the one touch-aware thing in the mixer
// just makes controls bigger — so a pinch here would be the first and only
// gesture of its kind, which is a worse outcome than not having it.
const WHEEL_STEP = 1.15;

function onHandleWheel(index: number, e: WheelEvent) {
  if (!props.bus) return;
  const b = bands.value[index]!;
  const factor = e.deltaY < 0 ? WHEEL_STEP : 1 / WHEEL_STEP;
  if (b.shelf) {
    pushBand(index, { slope: clampRound(b.slope * factor, 0.1, 2) });
  } else {
    pushBand(index, { q: clampRound(b.q * factor, 0.1, 10) });
  }
}

const clampRound = (v: number, lo: number, hi: number) =>
  Math.round(Math.max(lo, Math.min(hi, v)) * 100) / 100;

// Double-click puts the band back where it started, the same gesture the knobs
// use for the same thing. It resets shape too — a band you have reset should
// be the band you started with, not a shelf sitting at its default frequency.
function onHandleReset(index: number) {
  const d = bandDefaults[index]!;
  pushBand(index, { ...d }, true);
}

const handleTitle = (i: number) => {
  const b = bands.value[i]!;
  const width = b.shelf
    ? `${t('mixer.slope')} ${b.slope.toFixed(2)}`
    : `Q ${b.q.toFixed(2)}`;
  return `${EQ_BAND_NAMES[i]} — ${Math.round(b.freq)} Hz, ` +
         `${b.gain > 0 ? '+' : ''}${b.gain.toFixed(1)} dB, ${width}\n` +
         t('mixer.eqHandleHint');
};

const hpfIn = computed(() =>
  !!props.bus?.dsp?.hpf && props.bus.dsp.hpf.freq > HPF_PARKED_HZ);
const lpfIn = computed(() =>
  !!props.bus?.dsp?.lpf && props.bus.dsp.lpf.freq < LPF_PARKED_HZ);

const handleColor = (i: number) =>
  [METER_COLORS.blue, METER_COLORS.green, METER_COLORS.yellow, METER_COLORS.red][i]
  ?? METER_COLORS.green;
</script>

<style scoped>
/* The controls never clip: they keep their natural height and the graph
   absorbs whatever is left. If the panel still cannot fit, the work area
   scrolls rather than the knobs disappearing off the bottom — an EQ you can
   see but not adjust is worse than one you have to scroll to. */
.eq { min-height: 0; }
.eq > *:not(.eq__graph) { flex: 0 0 auto; }

/* A band out of circuit still shows its handle, so you can find it to bring it
   in, but it recedes rather than competing with the bands doing something. */
.eq__handle--out { opacity: 0.4; }
.eq__bandname--in { color: var(--color-accent); }

/* Bypassed: the controls dim so the state is unmistakable at a glance, but
   they stay readable and adjustable. Setting an EQ while listening past it and
   then switching it in is a normal way to work. */
.eq--bypassed .eq__grid-controls,
.eq--bypassed .eq__graph { opacity: 0.45; }
/* Filter markers read as fixtures rather than as a fifth and sixth band. */
.eq__handle--filter {
  background: var(--color-text-disabled);
  opacity: 0.85;
}

/* EQ owns a full-height column of its own now, so the graph goes back to
   taking whatever the band controls below it do not. It was pinned to a
   clamped height when EQ shared a row with dynamics — growing it there stole
   from the panels underneath, which is no longer true. A curve display is one
   of the few things that genuinely keeps improving with height, so there is no
   ceiling; the floor is what stops it collapsing on a short window. */
.eq__graph {
  position: relative;
  flex: 1 1 auto;
  min-height: 140px;
  background: var(--color-background);
  border-radius: var(--border-radius-sm);
  overflow: hidden;
}
.eq__svg { display: block; width: 100%; height: 100%; }
.eq__grid  {
  stroke: var(--color-border);
  stroke-width: 1;
  vector-effect: non-scaling-stroke;
  opacity: 0.5;
}
.eq__zero  {
  stroke: var(--color-text-disabled);
  stroke-width: 1;
  vector-effect: non-scaling-stroke;
}
/* non-scaling-stroke matters here. The viewBox is stretched to the panel with
   preserveAspectRatio="none", so an ordinary stroke is scaled with it and
   renders far heavier than the number suggests. This pins the line to a real
   device width, which is also what keeps it even as the panel resizes. */
.eq__curve {
  fill: none;
  stroke: var(--color-accent);
  stroke-width: 1.25;
  vector-effect: non-scaling-stroke;
  stroke-linejoin: round;
}

.eq__handle {
  position: absolute;
  top: 50%;
  transform: translate(-50%, -50%);
  font-size: 8px;
  font-family: var(--font-mono);
  line-height: 1;
  padding: 2px 3px;
  border-radius: 2px;
  color: #000;
  opacity: 0.85;
  pointer-events: none;
}
/* Band handles are the draggable ones; the filter markers stay inert, because
   the knobs that set them live on the fader column and dragging here would be
   reaching into another component's controls. */
.eq__handle--band {
  pointer-events: auto;
  cursor: grab;
  /* Without this the browser claims the drag for panning on touch and pen, and
     the handle only follows every other frame. */
  touch-action: none;
  /* A label-sized target is a hard thing to hit. The padding is invisible and
     roughly doubles the grab area without moving the marker off its point. */
  padding: 4px 5px;
  user-select: none;
}
.eq__handle--band:hover { opacity: 1; }
.eq__handle--held {
  opacity: 1;
  cursor: grabbing;
  box-shadow: 0 0 0 2px var(--color-accent);
}

/* Bands across, parameters down. auto-flow row so each declaration block above
   fills one row left to right. */
.eq__grid-controls {
  display: grid;
  grid-template-columns: auto repeat(4, 1fr);
  align-items: center;
  justify-items: center;
  gap: 4px 2px;
}
.eq__rowlabel {
  justify-self: start;
  font-size: 9px;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
}
.eq__bandname {
  font-family: var(--font-mono);
  font-size: 10px;
  color: var(--color-text-primary);
}

/* Bell / shelf. Sized like the row of knobs above it rather than like a button
   in a dialog — it is one more per-band control, not an action. */
.eq__shape {
  min-width: 34px;
  padding: 1px 4px;
  font-size: 8px;
  font-family: var(--font-mono);
  letter-spacing: 0.04em;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.eq__shape:hover:not(:disabled) { color: var(--color-text-primary); }
.eq__shape:disabled { opacity: 0.4; cursor: not-allowed; }
.eq__shape--shelf {
  color: #fff;
  background: var(--color-accent);
  border-color: var(--color-accent);
}
/* The middle bands have no choice to make. Their cell says what they are and
   stays quiet rather than offering a control that would not press. */
.eq__shapeblank {
  font-size: 8px;
  font-family: var(--font-mono);
  letter-spacing: 0.04em;
  color: var(--color-text-disabled);
}
</style>
