<template>
  <!--
    Parametric EQ, graph first.

    The graph is the instrument: every band is a numbered handle on the curve.
    Drag sideways for frequency, up and down for gain, wheel for Q (or a
    shelf's slope). Double-click empty graph to add a band there; Alt-click,
    right-click or Delete removes one. Up to 32 bands — the engine runs only
    the ones doing something, so there is no cost to having room.

    Below it, one row of knobs and typeable boxes for the SELECTED band — an EQ
    set by ear and one set from a spec sheet are both real jobs — plus its
    type. Only one band's controls at a time, so the panel never has to scroll
    however many bands there are.

    Behind the curve, when the analyser is on, the bus's live spectrum: after
    the EQ (filled) and optionally before it (line), so a cut can be seen
    doing its work. The curve includes the strip's HPF/LPF, which live on the
    fader column but shape this same signal.
  -->
  <section class="eq det__panel" :class="{ 'eq--bypassed': !eqIn }">
    <h4 class="det__h">
      {{ t('mixer.tabEq') }}
      <span class="eq__hdr-tools">
        <!-- Analyser: off / after the EQ / before and after. -->
        <span class="eq__seg" role="group" :aria-label="t('mixer.eq.analyser')">
          <button
            v-for="m in ANALYSER_MODES" :key="m"
            class="eq__seg-btn"
            :class="{ 'eq__seg-btn--on': analyserMode === m }"
            :title="t('mixer.eq.analyserHint')"
            @click="setAnalyserMode(m)"
          >{{ t(`mixer.eq.analyser_${m}`) }}</button>
        </span>
        <button
          class="det__byp"
          :class="{ 'det__byp--on': !eqIn }"
          :disabled="!bus"
          :title="t('mixer.bypassHint')"
          @click="toggleEq"
        >{{ t('mixer.bypass') }}</button>
      </span>
    </h4>

    <div
      ref="graphRef"
      class="eq__graph"
      tabindex="0"
      :title="t('mixer.eq.graphHint')"
      @dblclick="onGraphDblClick"
      @keydown.delete.prevent="removeSelected"
      @keydown.backspace.prevent="removeSelected"
    >
      <canvas ref="specRef" class="eq__spec"></canvas>
      <svg viewBox="0 0 400 120" preserveAspectRatio="none" class="eq__svg">
        <line
          v-for="f in GRID_HZ" :key="'v' + f"
          class="eq__grid" :x1="xFor(f)" :x2="xFor(f)" y1="0" y2="120"
        />
        <line
          v-for="g in GRID_DB" :key="'g' + g"
          class="eq__grid" x1="0" x2="400" :y1="yFor(g)" :y2="yFor(g)"
        />
        <line class="eq__zero" x1="0" x2="400" :y1="yFor(0)" :y2="yFor(0)" />
        <!-- The selected band's own contribution, faintly, so you can see
             which part of the total curve it is responsible for. -->
        <polyline v-if="selectedCurve" class="eq__band-curve" :points="selectedCurve"
                  :style="{ stroke: bandColor(selected!) }" />
        <polyline class="eq__curve" :points="curvePoints" />
      </svg>
      <span
        v-for="f in LABEL_HZ" :key="'l' + f"
        class="eq__axis eq__axis--x" :style="{ left: xPctFor(f) + '%' }"
      >{{ f >= 1000 ? `${f / 1000}k` : f }}</span>
      <span
        v-for="g in LABEL_DB" :key="'d' + g"
        class="eq__axis eq__axis--y" :style="{ top: (yFor(g) / 120 * 100) + '%' }"
      >{{ g > 0 ? `+${g}` : g }}</span>

      <span
        v-for="i in liveIndexes" :key="'h' + i"
        class="eq__handle"
        :class="{
          'eq__handle--sel': selected === i,
          'eq__handle--idle': !bandDoesSomething(bands[i]!),
          'eq__handle--held': dragBand === i,
        }"
        :style="{
          left: xPctFor(bands[i]!.freq) + '%',
          top: (yFor(handleDb(bands[i]!)) / 120 * 100) + '%',
          background: bandColor(i),
        }"
        :title="handleTitle(i)"
        @pointerdown.stop="onHandleDown(i, $event)"
        @wheel.prevent.stop="onHandleWheel(i, $event)"
        @contextmenu.prevent.stop="removeBand(i)"
        @dblclick.stop
      >{{ i + 1 }}</span>
      <span v-if="hpfIn" class="eq__marker" :style="{ left: xPctFor(bus!.dsp.hpf.freq) + '%' }">{{ t('mixer.hpf') }}</span>
      <span v-if="lpfIn" class="eq__marker" :style="{ left: xPctFor(bus!.dsp.lpf.freq) + '%' }">{{ t('mixer.lpf') }}</span>
      <p v-if="!liveIndexes.length" class="eq__empty">{{ t('mixer.eq.empty') }}</p>
    </div>

    <!-- The selected band. -->
    <div class="eq__bandbar">
      <div class="eq__chips">
        <button
          v-for="i in liveIndexes" :key="'c' + i"
          class="eq__chip"
          :class="{ 'eq__chip--sel': selected === i, 'eq__chip--idle': !bandDoesSomething(bands[i]!) }"
          :style="{ '--chip': bandColor(i) }"
          :title="handleTitle(i)"
          @click="selected = i"
        >{{ i + 1 }}</button>
        <button
          class="eq__chip eq__chip--add"
          :disabled="!bus || liveIndexes.length >= EQ_MAX_BANDS"
          :title="t('mixer.eq.addHint')"
          @click="addBand()"
        >+</button>
      </div>

      <div v-if="selected !== null && bands[selected]" class="eq__controls">
        <label class="eq__type">
          <span class="eq__lbl">{{ t('mixer.eq.type') }}</span>
          <select :value="typeOf(bands[selected]!, selected)" :disabled="!bus"
                  @change="onType(($event.target as HTMLSelectElement).value as BusEqType)">
            <option v-for="ty in EQ_TYPES" :key="ty" :value="ty">{{ t(`mixer.eq.type_${ty}`) }}</option>
          </select>
        </label>
        <KnobField
          :value="bands[selected]!.freq" :min="20" :max="20000" :origin="1000"
          taper="log" :decimals="0" unit="Hz" :size="32"
          :label="t('mixer.freq')" :disabled="!bus"
          @input="(v: number) => pushBand(selected!, { freq: v })"
        />
        <KnobField
          :value="bands[selected]!.gain" :min="-18" :max="18" :origin="0"
          :decimals="1" unit="dB" :size="32"
          :label="t('mixer.gain')" :disabled="!bus || !usesGain(bands[selected]!, selected)"
          @input="(v: number) => pushBand(selected!, { gain: v })"
        />
        <KnobField
          v-if="isShelf(bands[selected]!, selected)"
          :value="bands[selected]!.slope" :min="0.1" :max="2" :origin="1"
          taper="log" :decimals="2" :size="32"
          :label="t('mixer.slope')" :title="t('mixer.slopeHint')" :disabled="!bus"
          @input="(v: number) => pushBand(selected!, { slope: v })"
        />
        <KnobField
          v-else
          :value="bands[selected]!.q" :min="0.1" :max="10" :origin="1"
          taper="log" :decimals="2" :size="32"
          :label="t('mixer.q')" :title="t('mixer.qHint')" :disabled="!bus"
          @input="(v: number) => pushBand(selected!, { q: v })"
        />
        <button class="eq__remove" :disabled="!bus" :title="t('mixer.eq.removeHint')" @click="removeBand(selected!)">
          <span class="material-symbols-rounded">delete</span>
        </button>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import KnobField from './KnobField.vue';
import type { Bus, BusDsp, BusEqBand, BusEqType } from '~/types/project';
import { EQ_MAX_BANDS, EQ_TYPES, HPF_PARKED_HZ, LPF_PARKED_HZ } from '~/types/project';
import type { BiquadCoeffs } from '~/utils/filterResponse';
import {
  biquadHighpass, biquadHighShelf, biquadLowpass, biquadLowShelf, biquadNotch, biquadPeaking,
  combinedMagnitudeDb,
} from '~/utils/filterResponse';

const props = defineProps<{ bus?: Bus | null }>();
const emit = defineEmits<{
  (e: 'patch', id: string, patch: Partial<Bus>): void;
  /** In-flight band values, so the curve tracks the knob. See the fader. */
  (e: 'dsp-live', dsp: Partial<BusDsp>): void;
}>();

const { t } = useLocalization();
const server = useLiveplayServer();

// ---- Bands -----------------------------------------------------------------
// Slots, not a list: a band keeps its index for life and a deleted one is
// `on: false`, so deleting band 2 never turns band 3's filter into band 2's
// (the engine would ramp between them — an audible morph). See server README.
const DEFAULT_BANDS: BusEqBand[] = [
  { freq: 100,   gain: 0, q: 0.7, shelf: false, slope: 1, type: 'bell', on: true },
  { freq: 500,   gain: 0, q: 1.0, shelf: false, slope: 1, type: 'bell', on: true },
  { freq: 2500,  gain: 0, q: 1.0, shelf: false, slope: 1, type: 'bell', on: true },
  { freq: 10000, gain: 0, q: 0.7, shelf: false, slope: 1, type: 'bell', on: true },
];

// Live-then-persist, like every other control on the strip: coefficients go
// to the engine on every drag event over a call that writes no document, and
// the bus is written once the gesture settles (250 ms).
const localBands = ref<BusEqBand[] | null>(null);
let   bandHold   = false;
let   bandSettle: ReturnType<typeof setTimeout> | null = null;

const bands = computed<BusEqBand[]>(() => localBands.value ?? props.bus?.dsp?.eq ?? DEFAULT_BANDS);
watch(() => props.bus?.dsp?.eq, () => { if (!bandHold) localBands.value = null; }, { deep: true });
watch(() => props.bus?.id, () => { localBands.value = null; selected.value = null; });
onBeforeUnmount(() => { if (bandSettle) clearTimeout(bandSettle); });

const liveIndexes = computed(() =>
  bands.value.map((b, i) => (b.on === false ? -1 : i)).filter(i => i >= 0));

// A pre-2.5 server has no `type`; its outer-band `shelf` means a shelf.
function typeOf(b: BusEqBand, i: number): BusEqType {
  if (b.type) return b.type;
  if (b.shelf) return i === 0 ? 'lowShelf' : 'highShelf';
  return 'bell';
}
const isShelf  = (b: BusEqBand, i: number) => { const ty = typeOf(b, i); return ty === 'lowShelf' || ty === 'highShelf'; };
const usesGain = (b: BusEqBand, i: number) => { const ty = typeOf(b, i); return ty === 'bell' || ty === 'lowShelf' || ty === 'highShelf'; };
// Same rule as the engine: a flat bell or shelf is an identity; a cut or a
// notch always works.
const bandDoesSomething = (b: BusEqBand) => {
  const i = bands.value.indexOf(b);
  return usesGain(b, i) ? b.gain !== 0 : true;
};

const selected = ref<number | null>(null);
watch(liveIndexes, (idx) => {
  if (selected.value === null || !idx.includes(selected.value)) selected.value = idx[0] ?? null;
}, { immediate: true });

// Always send `shelf` consistent with `type`, so the server never falls back
// on the legacy positional reading.
function normalised(b: BusEqBand, i: number): BusEqBand {
  const type = typeOf(b, i);
  return { ...b, type, shelf: type === 'lowShelf' || type === 'highShelf', on: b.on !== false };
}

function sendBands(next: BusEqBand[], persistNow = false) {
  if (!props.bus) return;
  localBands.value = next;
  bandHold = true;
  emit('dsp-live', { eq: next });
  void server.setBusDsp(props.bus.id, { eq: next }).catch(() => {});
  if (bandSettle) clearTimeout(bandSettle);
  if (persistNow) {
    bandHold = false;
    emit('patch', props.bus.id, { dsp: { eq: next } } as Partial<Bus>);
    return;
  }
  bandSettle = setTimeout(() => {
    bandSettle = null;
    bandHold = false;
    emit('patch', props.bus!.id, { dsp: { eq: localBands.value ?? next } } as Partial<Bus>);
  }, 250);
}

function pushBand(index: number, patch: Partial<BusEqBand>, persistNow = false) {
  const next = bands.value.map((b, i) => normalised(i === index ? { ...b, ...patch } : b, i));
  sendBands(next, persistNow);
}

function onType(type: BusEqType) {
  if (selected.value === null) return;
  pushBand(selected.value, { type, shelf: type === 'lowShelf' || type === 'highShelf' }, true);
}

// Reuses the first deleted slot, else appends — slots are identity.
function addBand(freq = 1000, gain = 0) {
  if (!props.bus || liveIndexes.value.length >= EQ_MAX_BANDS) return;
  const fresh: BusEqBand = { freq: Math.round(freq), gain: Math.round(gain * 10) / 10, q: 1,
                             shelf: false, slope: 1, type: 'bell', on: true };
  const next = bands.value.map((b, i) => normalised(b, i));
  const free = next.findIndex(b => b.on === false);
  const at = free >= 0 ? free : next.length;
  if (free >= 0) next[free] = fresh; else next.push(fresh);
  selected.value = at;
  sendBands(next, true);
}

function removeBand(index: number) {
  if (!props.bus) return;
  const next = bands.value.map((b, i) => normalised(b, i));
  if (!next[index]) return;
  // Flatten as well as switch off, so the curve drops it at once even before
  // the server trims or the round trip lands.
  next[index] = { ...next[index]!, on: false, gain: 0 };
  sendBands(next, true);
}
function removeSelected() { if (selected.value !== null) removeBand(selected.value); }

// ---- Section bypass ----------------------------------------------------------
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
  emit('patch', props.bus.id, { dsp: { eqEnabled: next } } as Partial<Bus>);
}

// ---- Geometry ------------------------------------------------------------------
const GRAPH_DB = 18;
const GRID_DB  = [12, 6, -6, -12];
const LABEL_DB = [12, 6, 0, -6, -12];
const GRID_HZ  = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
const LABEL_HZ = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
const LO = Math.log10(20);
const HI = Math.log10(20000);
const xPctFor = (hz: number) => ((Math.log10(hz) - LO) / (HI - LO)) * 100;
const xFor = (hz: number) => xPctFor(hz) * 4;
const yFor = (db: number) => 60 - (db / GRAPH_DB) * 60;
const clampDb = (db: number) => Math.max(-GRAPH_DB, Math.min(GRAPH_DB, db));
// Cuts and notches have no gain: their handle rides the 0 dB line.
const handleDb = (b: BusEqBand) => {
  const i = bands.value.indexOf(b);
  return usesGain(b, i) ? clampDb(b.gain) : 0;
};

// Eight colours, cycled: enough to tell neighbours apart.
const BAND_COLORS = ['#3b82f6', '#22c55e', '#eab308', '#ef4444', '#a855f7', '#06b6d4', '#f97316', '#ec4899'];
const bandColor = (i: number) => BAND_COLORS[i % BAND_COLORS.length]!;

// ---- The curve -----------------------------------------------------------------
// Mirrors the engine (utils/filterResponse.ts). Display-only.
const SAMPLE_RATE = 48000;
function bandSection(b: BusEqBand, i: number): BiquadCoeffs | null {
  if (b.on === false || !bandDoesSomething(b)) return null;
  switch (typeOf(b, i)) {
    case 'lowShelf':  return biquadLowShelf(b.freq, SAMPLE_RATE, b.gain, b.slope);
    case 'highShelf': return biquadHighShelf(b.freq, SAMPLE_RATE, b.gain, b.slope);
    case 'lowCut':    return biquadHighpass(b.freq, SAMPLE_RATE, b.q);
    case 'highCut':   return biquadLowpass(b.freq, SAMPLE_RATE, b.q);
    case 'notch':     return biquadNotch(b.freq, SAMPLE_RATE, b.q);
    default:          return biquadPeaking(b.freq, SAMPLE_RATE, b.gain, b.q);
  }
}
const sections = computed<BiquadCoeffs[]>(() => {
  const out: BiquadCoeffs[] = [];
  const hpf = props.bus?.dsp?.hpf;
  const lpf = props.bus?.dsp?.lpf;
  if (hpf && hpf.freq > HPF_PARKED_HZ) out.push(biquadHighpass(hpf.freq, SAMPLE_RATE, hpf.q || 0.7071));
  if (lpf && lpf.freq < LPF_PARKED_HZ) out.push(biquadLowpass(lpf.freq, SAMPLE_RATE, lpf.q || 0.7071));
  if (eqIn.value) bands.value.forEach((b, i) => { const s = bandSection(b, i); if (s) out.push(s); });
  return out;
});
const CURVE_POINTS = 161;
function polyline(secs: BiquadCoeffs[]) {
  return Array.from({ length: CURVE_POINTS }, (_, k) => {
    const x  = (k / (CURVE_POINTS - 1)) * 400;
    const hz = Math.pow(10, LO + (k / (CURVE_POINTS - 1)) * (HI - LO));
    const db = secs.length ? combinedMagnitudeDb(secs, hz, SAMPLE_RATE) : 0;
    return `${x.toFixed(1)},${yFor(clampDb(db)).toFixed(1)}`;
  }).join(' ');
}
const curvePoints = computed(() => polyline(sections.value));
const selectedCurve = computed(() => {
  if (selected.value === null || !eqIn.value) return '';
  const b = bands.value[selected.value];
  const s = b ? bandSection(b, selected.value) : null;
  return s ? polyline([s]) : '';
});

const hpfIn = computed(() => !!props.bus?.dsp?.hpf && props.bus.dsp.hpf.freq > HPF_PARKED_HZ);
const lpfIn = computed(() => !!props.bus?.dsp?.lpf && props.bus.dsp.lpf.freq < LPF_PARKED_HZ);

const handleTitle = (i: number) => {
  const b = bands.value[i]!;
  const ty = typeOf(b, i);
  const width = isShelf(b, i) ? `${t('mixer.slope')} ${b.slope.toFixed(2)}` : `Q ${b.q.toFixed(2)}`;
  const gain = usesGain(b, i) ? `, ${b.gain > 0 ? '+' : ''}${b.gain.toFixed(1)} dB` : '';
  return `${i + 1} · ${t(`mixer.eq.type_${ty}`)} — ${Math.round(b.freq)} Hz${gain}, ${width}\n${t('mixer.eq.handleHint')}`;
};

// ---- Pointer ---------------------------------------------------------------------
const graphRef = ref<HTMLElement | null>(null);
const dragBand = ref<number | null>(null);

function pointToFreqGain(e: MouseEvent) {
  const box = graphRef.value!.getBoundingClientRect();
  const pct = Math.max(0, Math.min(1, (e.clientX - box.left) / box.width));
  const yPct = Math.max(0, Math.min(1, (e.clientY - box.top) / box.height));
  return { freq: Math.pow(10, LO + pct * (HI - LO)), gain: clampDb(GRAPH_DB - yPct * GRAPH_DB * 2) };
}

function onGraphDblClick(e: MouseEvent) {
  if (!props.bus || !graphRef.value) return;
  const { freq, gain } = pointToFreqGain(e);
  addBand(freq, gain);
}

function onHandleDown(index: number, e: PointerEvent) {
  selected.value = index;
  graphRef.value?.focus({ preventScroll: true });
  if (!props.bus || e.button !== 0) return;
  if (e.altKey) { removeBand(index); return; }
  const box = graphRef.value?.getBoundingClientRect();
  if (!box || box.width <= 0 || box.height <= 0) return;
  const start = bands.value[index]!;
  const startX = e.clientX, startY = e.clientY;
  const startPct = xPctFor(start.freq) / 100;
  const startDb = clampDb(start.gain);
  const gainful = usesGain(start, index);
  dragBand.value = index;
  const el = e.currentTarget as HTMLElement;
  try { el.setPointerCapture(e.pointerId); } catch { /* best effort */ }
  const move = (ev: PointerEvent) => {
    // Delta from the grab point, so shift can make it fine.
    const fine = ev.shiftKey ? 0.25 : 1;
    const pct  = Math.max(0, Math.min(1, startPct + ((ev.clientX - startX) / box.width) * fine));
    const freq = Math.round(Math.max(20, Math.min(20000, Math.pow(10, LO + pct * (HI - LO)))));
    const patch: Partial<BusEqBand> = { freq };
    if (gainful) {
      const gain = clampDb(startDb - ((ev.clientY - startY) / box.height) * GRAPH_DB * 2 * fine);
      patch.gain = Math.round(gain * 10) / 10;
    }
    pushBand(index, patch);
  };
  const up = (ev: PointerEvent) => {
    try { el.releasePointerCapture(ev.pointerId); } catch { /* gone */ }
    el.removeEventListener('pointermove', move);
    el.removeEventListener('pointerup', up);
    el.removeEventListener('pointercancel', up);
    dragBand.value = null;
  };
  el.addEventListener('pointermove', move);
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
}

// Wheel = Q (or slope on a shelf), multiplicative: both are ratios.
function onHandleWheel(index: number, e: WheelEvent) {
  if (!props.bus) return;
  selected.value = index;
  const b = bands.value[index]!;
  const f = e.deltaY < 0 ? 1.15 : 1 / 1.15;
  const clampRound = (v: number, lo: number, hi: number) => Math.round(Math.max(lo, Math.min(hi, v)) * 100) / 100;
  if (isShelf(b, index)) pushBand(index, { slope: clampRound(b.slope * f, 0.1, 2) });
  else pushBand(index, { q: clampRound(b.q * f, 0.1, 10) });
}

// ---- Analyser --------------------------------------------------------------------
// Off, after the EQ, or before and after. Remembered per viewer (it is how
// this operator likes to look, not a property of the show).
type AnalyserMode = 'off' | 'post' | 'both';
const ANALYSER_MODES: AnalyserMode[] = ['off', 'post', 'both'];
const MODE_KEY = 'liveplay.eqAnalyser';
const analyserMode = ref<AnalyserMode>('post');
try { const m = localStorage.getItem(MODE_KEY); if (m === 'off' || m === 'post' || m === 'both') analyserMode.value = m; } catch { /* no storage */ }
function setAnalyserMode(m: AnalyserMode) {
  analyserMode.value = m;
  try { localStorage.setItem(MODE_KEY, m); } catch { /* no storage */ }
}

const specRef = ref<HTMLCanvasElement | null>(null);
// Displayed spectra with peak-fall ballistics, so the picture reads as a
// shape rather than a flicker: rises at once, falls at FALL_DB_PER_S.
const FALL_DB_PER_S = 36;
const SPEC_FLOOR = -90;
let dispPre: number[] = [];
let dispPost: number[] = [];
let lastFrameAt = 0;
let lastDataAt = 0;
let raf = 0;

function subscribe() {
  const want = analyserMode.value !== 'off' && props.bus ? props.bus.id : null;
  server.setAnalyser(want);
  if (!want) { dispPre = []; dispPost = []; drawSpectrum(); }
}
watch([analyserMode, () => props.bus?.id], subscribe);

let offFrames: (() => void) | null = null;
let ro: ResizeObserver | null = null;
onMounted(() => {
  subscribe();
  offFrames = server.onAnalyser((f) => {
    if (!props.bus || f.busId !== props.bus.id) return;
    const now = performance.now();
    const dt = lastFrameAt ? (now - lastFrameAt) / 1000 : 0;
    lastFrameAt = now;
    lastDataAt = now;
    const fall = FALL_DB_PER_S * dt;
    const step = (disp: number[], fresh: number[]) =>
      fresh.map((v, k) => Math.max(v, (disp[k] ?? SPEC_FLOOR) - fall));
    dispPre = step(dispPre, f.pre);
    dispPost = step(dispPost, f.post);
    if (!raf) raf = requestAnimationFrame(() => { raf = 0; drawSpectrum(); });
  });
  if (specRef.value) {
    ro = new ResizeObserver(() => drawSpectrum());
    ro.observe(specRef.value);
  }
});
onBeforeUnmount(() => {
  offFrames?.();
  ro?.disconnect();
  if (raf) cancelAnimationFrame(raf);
  server.setAnalyser(null);
});

function cssVar(el: Element, name: string, fallback: string) {
  const v = getComputedStyle(el).getPropertyValue(name).trim();
  return v || fallback;
}

function drawSpectrum() {
  const c = specRef.value;
  if (!c) return;
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  if (!w || !h) return;
  if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
    c.width = Math.round(w * dpr);
    c.height = Math.round(h * dpr);
  }
  const ctx = c.getContext('2d');
  if (!ctx) return;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  if (analyserMode.value === 'off' || !dispPost.length) return;
  // Stale data (playback stopped, nothing arriving) fades out instead of
  // freezing on the last frame.
  if (performance.now() - lastDataAt > 1500) return;
  const n = dispPost.length;
  const yOf = (db: number) => h * (1 - (Math.max(SPEC_FLOOR, Math.min(0, db)) - SPEC_FLOOR) / -SPEC_FLOOR);
  const xOf = (k: number) => ((k + 0.5) / n) * w;
  const accent = cssVar(c, '--color-accent', '#e11d48');
  // After the EQ: a filled shape.
  ctx.beginPath();
  ctx.moveTo(0, h);
  for (let k = 0; k < n; k++) ctx.lineTo(xOf(k), yOf(dispPost[k]!));
  ctx.lineTo(w, h);
  ctx.closePath();
  ctx.globalAlpha = 0.22;
  ctx.fillStyle = accent;
  ctx.fill();
  ctx.globalAlpha = 0.55;
  ctx.strokeStyle = accent;
  ctx.lineWidth = 1;
  ctx.stroke();
  // Before the EQ: a line, so the difference between the two is the EQ.
  if (analyserMode.value === 'both' && dispPre.length === n) {
    ctx.beginPath();
    for (let k = 0; k < n; k++) {
      const x = xOf(k), y = yOf(dispPre[k]!);
      if (k === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.globalAlpha = 0.7;
    ctx.strokeStyle = cssVar(c, '--color-text-secondary', '#9ca3af');
    ctx.setLineDash([3, 3]);
    ctx.stroke();
    ctx.setLineDash([]);
  }
  ctx.globalAlpha = 1;
}
// Keep fading while frames have stopped arriving.
let fadeTimer: ReturnType<typeof setInterval> | null = setInterval(() => {
  if (dispPost.length && performance.now() - lastDataAt > 1500) { dispPre = []; dispPost = []; drawSpectrum(); }
}, 500);
onBeforeUnmount(() => { if (fadeTimer) clearInterval(fadeTimer); fadeTimer = null; });
</script>

<style scoped>
.eq { min-height: 0; }
.eq > *:not(.eq__graph) { flex: 0 0 auto; }
.eq--bypassed .eq__graph,
.eq--bypassed .eq__bandbar { opacity: 0.45; }

.eq__hdr-tools { display: inline-flex; align-items: center; gap: 6px; }
.eq__seg { display: inline-flex; border: 1px solid var(--color-border); border-radius: var(--border-radius-sm); overflow: hidden; }
.eq__seg-btn {
  padding: 1px 6px;
  font-size: 9px;
  font-family: var(--font-mono);
  letter-spacing: 0.04em;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: none;
  cursor: pointer;
}
.eq__seg-btn + .eq__seg-btn { border-left: 1px solid var(--color-border); }
.eq__seg-btn--on { color: #fff; background: var(--color-accent); }

/* The graph takes every pixel the band bar does not: a curve and a spectrum
   genuinely keep improving with size. */
.eq__graph {
  position: relative;
  flex: 1 1 auto;
  min-height: 160px;
  background: var(--color-background);
  border-radius: var(--border-radius-sm);
  overflow: hidden;
  outline: none;
  cursor: crosshair;
}
.eq__graph:focus-visible { box-shadow: 0 0 0 1px var(--color-accent); }
.eq__spec, .eq__svg { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
.eq__spec { pointer-events: none; }
.eq__svg { pointer-events: none; }
.eq__grid { stroke: var(--color-border); stroke-width: 1; vector-effect: non-scaling-stroke; opacity: 0.45; }
.eq__zero { stroke: var(--color-text-disabled); stroke-width: 1; vector-effect: non-scaling-stroke; }
.eq__curve {
  fill: none;
  stroke: var(--color-text-primary);
  stroke-width: 1.75;
  vector-effect: non-scaling-stroke;
  stroke-linejoin: round;
}
.eq__band-curve {
  fill: none;
  stroke-width: 1.25;
  vector-effect: non-scaling-stroke;
  stroke-dasharray: 4 3;
  opacity: 0.8;
}
.eq__axis {
  position: absolute;
  font-size: 9px;
  font-family: var(--font-mono);
  color: var(--color-text-disabled);
  pointer-events: none;
  user-select: none;
}
.eq__axis--x { bottom: 2px; transform: translateX(-50%); }
.eq__axis--y { left: 4px; transform: translateY(-50%); }

.eq__handle {
  position: absolute;
  transform: translate(-50%, -50%);
  width: 18px;
  height: 18px;
  display: grid;
  place-items: center;
  border-radius: 50%;
  font-size: 10px;
  font-weight: 700;
  font-family: var(--font-mono);
  color: #000;
  border: 2px solid rgba(0, 0, 0, 0.35);
  cursor: grab;
  touch-action: none;
  user-select: none;
  opacity: 0.9;
  z-index: 2;
}
.eq__handle:hover { opacity: 1; }
.eq__handle--idle { opacity: 0.45; }
.eq__handle--sel { border-color: #fff; opacity: 1; z-index: 3; box-shadow: 0 0 0 2px rgba(0, 0, 0, 0.5); }
.eq__handle--held { cursor: grabbing; }
.eq__marker {
  position: absolute;
  top: 6px;
  transform: translateX(-50%);
  font-size: 8px;
  font-family: var(--font-mono);
  padding: 1px 3px;
  border-radius: 2px;
  background: var(--color-text-disabled);
  color: #000;
  pointer-events: none;
}
.eq__empty {
  position: absolute;
  inset: 0;
  margin: 0;
  display: grid;
  place-items: center;
  font-size: 11px;
  color: var(--color-text-secondary);
  pointer-events: none;
}

.eq__bandbar { display: flex; flex-direction: column; gap: 6px; padding-top: 6px; }
.eq__chips { display: flex; flex-wrap: wrap; gap: 4px; }
.eq__chip {
  min-width: 22px;
  height: 20px;
  padding: 0 5px;
  border-radius: 10px;
  font-size: 10px;
  font-weight: 700;
  font-family: var(--font-mono);
  color: #000;
  background: var(--chip, var(--color-surface));
  border: 2px solid transparent;
  cursor: pointer;
  opacity: 0.8;
}
.eq__chip--idle { opacity: 0.4; }
.eq__chip--sel { border-color: var(--color-text-primary); opacity: 1; }
.eq__chip--add {
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px dashed var(--color-border);
  opacity: 1;
}
.eq__chip--add:disabled { opacity: 0.4; cursor: not-allowed; }
.eq__controls { display: flex; align-items: flex-end; gap: 10px; flex-wrap: wrap; }
.eq__type { display: flex; flex-direction: column; gap: 3px; }
.eq__lbl {
  font-size: 9px;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
}
.eq__type select {
  font-size: 11px;
  padding: 3px 4px;
  background: var(--color-background);
  color: var(--color-text-primary);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
}
.eq__remove {
  margin-left: auto;
  display: grid;
  place-items: center;
  width: 28px;
  height: 28px;
  background: var(--color-background);
  color: var(--color-text-secondary);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.eq__remove:hover:not(:disabled) { color: var(--color-danger); border-color: var(--color-danger); }
.eq__remove .material-symbols-rounded { font-size: 16px; }
</style>
