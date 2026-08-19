<template>
  <!--
    The channel column in the channel view: the same job as a rail strip, but
    given the full height of the window, so it carries the controls a strip has
    no room for — filters and a larger fader.

    Top to bottom: name with channel stepping either side, meters and fader
    taking the dominant space, mute/PFL, high- and low-pass, pan.

    It is not MixerStrip. The rail strip is a dense summary sized to sit twenty
    across; this is one channel with room to work. They share every control
    component (StereoMeter, MeterScale, CanvasFader, Knob), so the parts stay
    identical even though the arrangement does not.
  -->
  <section class="cf">
    <header class="cf__head">
      <button class="cf__step" :disabled="!prevId" :title="t('mixer.prevChannel')" @click="$emit('select', prevId)">
        <span class="material-symbols-rounded">chevron_left</span>
      </button>
      <!-- No colour chip here. Between two stepping arrows the name field wants
           every pixel it can get, and the chip is already carried by the view
           header and by every tile in the select row. -->
      <div class="cf__name">
        <input
          :value="bus.name"
          :disabled="bus.system"
          @change="$emit('patch', bus.id, { name: ($event.target as HTMLInputElement).value })"
        />
      </div>
      <button class="cf__step" :disabled="!nextId" :title="t('mixer.nextChannel')" @click="$emit('select', nextId)">
        <span class="material-symbols-rounded">chevron_right</span>
      </button>
    </header>

    <!-- Meter, shared scale, fader — the same geometry as a rail strip, so
         0 dBFS still lands on the scale's 0 tick. -->
    <div class="cf__meterfader">
      <div class="cf__meters" :style="{ height: METER_TRACK_PCT + '%' }">
        <StereoMeter
          v-if="bus.mixerId"
          :mixer-id="bus.mixerId"
          :mono="bus.width < 2"
          bare
          :show-scale="false"
          :min-db="FADER_MIN_DB"
          :max-db="METER_MAX_DB"
        />
        <div v-else class="cf__nometer" :title="t('mixer.noStrip')"></div>
      </div>
      <MeterScale :min-db="FADER_MIN_DB" :max-db="FADER_MAX_DB" />
      <CanvasFader
        :db="gainDb"
        :min-db="FADER_MIN_DB"
        :max-db="FADER_MAX_DB"
        :width="36"
        @input="onFader"
        @reset="onFader(0)"
      />
    </div>

    <div class="cf__readout">{{ meterLabel }}</div>
    <div class="cf__gain">{{ gainLabel }} dB</div>

    <div class="cf__pair">
      <button class="cf__btn" :class="{ 'cf__btn--mute': bus.mute }" @click="onMute">
        {{ t('mixer.mute') }}
      </button>
      <button
        class="cf__btn"
        :class="{ 'cf__btn--pfl': bus.pfl }"
        :title="t('mixer.pflHint')"
        @click="onPfl"
      >{{ t('mixer.pfl') }}</button>
    </div>

    <!-- Filters. Real: each knob parks at the end of its travel to go out of
         circuit, which is what the origin already meant on the surface, so
         there is no separate in/out switch to disagree with the knob beside
         it. Double-click parks it. The label lights when it is in circuit. -->
    <div class="cf__filters">
      <div class="cf__filterrow">
        <!-- Logarithmic, as every frequency control is: 40 to 80 Hz is the
             same musical move as 200 to 400, so both should take the same
             arc. Linear here would bury the whole useful bottom end in the
             first few degrees of travel. -->
        <KnobField
          :value="hpfHz" :min="HPF_PARKED_HZ" :max="800" :origin="HPF_PARKED_HZ"
          taper="log"
          :decimals="0" unit="Hz" :label="t('mixer.hpf')" :size="34"
          :class="{ 'cf__filter--in': hpfIn }"
          :title="hpfIn ? t('mixer.hpf') : t('mixer.filterParked')"
          @input="onHpf"
        />
        <KnobField
          :value="lpfHz" :min="1000" :max="LPF_PARKED_HZ" :origin="LPF_PARKED_HZ"
          taper="log"
          :decimals="0" unit="Hz" :label="t('mixer.lpf')" :size="34"
          :class="{ 'cf__filter--in': lpfIn }"
          :title="lpfIn ? t('mixer.lpf') : t('mixer.filterParked')"
          @input="onLpf"
        />
      </div>
    </div>

    <!-- Where the channel sits, and how wide it is.

         The big knob is one control with two laws behind it: PAN on a mono bus,
         placing its single lane between the destination's two, and BALANCE on a
         stereo bus, trimming its own pair against each other. Both come out as
         the same two send gains, so neither costs the render thread anything.

         Width and bass-mono only exist on a stereo bus — there is no image to
         widen otherwise — and are held disabled rather than hidden so the
         column does not change height as you step through channels. -->
    <div class="cf__image">
      <div class="cf__pan">
        <Knob
          :value="pan"
          :min="-1"
          :max="1"
          :origin="0"
          :size="44"
          :title="isStereo ? t('mixer.balanceHint') : t('mixer.panHint')"
          @input="onPan"
          @reset="onPan(0)"
        />
        <span class="cf__panlabel">{{ panLabel }}</span>
      </div>

      <div class="cf__widthrow">
        <!-- Linear, unlike the frequency knobs beside it: width is a ratio
             either side of 1, and half the travel belongs below unity. -->
        <KnobField
          :value="widthVal" :min="0" :max="2" :origin="1"
          :decimals="2" unit="" :label="t('mixer.width')" :size="34"
          :class="{ 'cf__filter--in': widthIn }"
          :disabled="!isStereo"
          :title="isStereo ? t('mixer.widthHint') : t('mixer.widthMonoOnly')"
          @input="onWidth"
        />
        <KnobField
          :value="bassHz" :min="BASS_MONO_PARKED_HZ" :max="500"
          :origin="BASS_MONO_PARKED_HZ"
          taper="log"
          :decimals="0" unit="Hz" :label="t('mixer.bassMono')" :size="34"
          :class="{ 'cf__filter--in': bassIn }"
          :disabled="!isStereo"
          :title="isStereo ? t('mixer.bassMonoHint') : t('mixer.widthMonoOnly')"
          @input="onBassMono"
        />
      </div>

      <!-- Correlation, which is the thing to watch while widening: +1 is
           mono-safe, 0 is wide, and anything below zero is material a mono sum
           will start cancelling. The bar runs from the centre so the eye reads
           distance-from-mono rather than an absolute quantity. -->
      <div v-if="isStereo" class="cf__corr" :title="t('mixer.correlationHint')">
        <div class="cf__corrtrack">
          <div class="cf__corrzero"></div>
          <div
            class="cf__corrfill"
            :class="{ 'cf__corrfill--warn': correlation < 0 }"
            :style="corrStyle"
          ></div>
        </div>
        <span class="cf__corrlabel" :class="{ 'cf__corrlabel--warn': correlation < 0 }">
          {{ correlation.toFixed(2) }}
        </span>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import type { Bus, BusDsp } from '~/types/project';
import { HPF_PARKED_HZ, LPF_PARKED_HZ, BASS_MONO_PARKED_HZ } from '~/types/project';
import CanvasFader from './CanvasFader.vue';
import StereoMeter from './StereoMeter.vue';
import MeterScale from './MeterScale.vue';
import Knob from './Knob.vue';
import KnobField from './KnobField.vue';
import { useMixerMeter, lufsFromKwMs } from '~/composables/useLiveMeters';
import { useOutputTarget } from '~/composables/useOutputTarget';
import {
  FADER_MIN_DB, FADER_MAX_DB, METER_MAX_DB, METER_TRACK_PCT, formatMeterLabel,
} from '~/utils/meterScale';

const props = defineProps<{
  bus: Bus;
  prevId: string;
  nextId: string;
}>();

const emit = defineEmits<{
  (e: 'patch', id: string, patch: Partial<Bus>): void;
  (e: 'select', id: string): void;
  /**
   * The in-flight filter values, on every drag event rather than on settle.
   *
   * The EQ curve draws these filters, but it reads them off the bus, and the
   * bus is only rewritten when the gesture settles 250 ms later. Without this
   * the curve sat still while the knob moved and then jumped once it was let
   * go, which reads as a broken display rather than a deliberate delay.
   */
  (e: 'dsp-live', dsp: BusDsp): void;
}>();

const { t } = useLocalization();
const server = useLiveplayServer();

// Same live-then-persist pattern as the rail strip: the engine gets the move
// immediately over a strip-only call, the bus is written once the gesture
// settles. Binding straight to bus.gainDb meant a PATCH plus a refetch per drag
// event, with the fader snapping back to the stale value in between.
const gainDb  = ref(props.bus.gainDb);
let   holding = false;
let   settle: ReturnType<typeof setTimeout> | null = null;

watch(() => props.bus.gainDb, v => { if (!holding) gainDb.value = v; });
watch(() => props.bus.id, () => { gainDb.value = props.bus.gainDb; pan.value = props.bus.pan ?? 0; });

function onFader(db: number) {
  gainDb.value = db;
  holding = true;
  if (props.bus.mixerId) void server.setMixerGainDb(props.bus.mixerId, db).catch(() => {});
  if (settle) clearTimeout(settle);
  settle = setTimeout(() => {
    settle = null; holding = false;
    emit('patch', props.bus.id, { gainDb: gainDb.value });
  }, 250);
}

const isStereo = computed(() => props.bus.width >= 2);

const pan       = ref(props.bus.pan ?? 0);
let   panHold   = false;
let   panSettle: ReturnType<typeof setTimeout> | null = null;

watch(() => props.bus.pan, v => { if (!panHold) pan.value = v ?? 0; });

// One handler for both laws. The server decides which to apply from the bus's
// width, so there is nothing to branch on here — and a mono bus is no longer
// the only one allowed to move, which is what this used to refuse.
function onPan(v: number) {
  pan.value = v;
  panHold = true;
  void server.setBusPan(props.bus.id, v).catch(() => {});
  if (panSettle) clearTimeout(panSettle);
  panSettle = setTimeout(() => {
    panSettle = null; panHold = false;
    emit('patch', props.bus.id, { pan: pan.value });
  }, 250);
}

onBeforeUnmount(() => {
  if (settle) clearTimeout(settle);
  if (panSettle) clearTimeout(panSettle);
  if (filtSettle) clearTimeout(filtSettle);
  if (widthSettle) clearTimeout(widthSettle);
});

function onMute() {
  const next = !props.bus.mute;
  if (props.bus.mixerId) void server.setMixerMute(props.bus.mixerId, next).catch(() => {});
  emit('patch', props.bus.id, { mute: next });
}

// Pre-fade listen: a tap into Monitor, taken before this fader and before the
// mute above it. Engine-only state, so nothing is persisted and there is no
// document round-trip to wait for.
function onPfl() {
  void server.setBusPfl(props.bus.id, !props.bus.pfl).catch(() => {});
}

// ---- Filters -------------------------------------------------------------
// Same live-then-persist shape as the fader and the pan knob: the strip gets
// new coefficients on every drag event over a strip-only call, and the bus is
// written once the gesture settles. Binding straight to the bus would PATCH
// and refetch per event, and the knob would fight the round-trip.
const hpfHz    = ref(props.bus.dsp?.hpf?.freq ?? HPF_PARKED_HZ);
const lpfHz    = ref(props.bus.dsp?.lpf?.freq ?? LPF_PARKED_HZ);
let   filtHold = false;
let   filtSettle: ReturnType<typeof setTimeout> | null = null;

watch(() => props.bus.dsp, v => {
  if (filtHold) return;
  hpfHz.value = v?.hpf?.freq ?? HPF_PARKED_HZ;
  lpfHz.value = v?.lpf?.freq ?? LPF_PARKED_HZ;
}, { deep: true });

// Parked at the end of its travel means out of circuit — the same rule the
// server applies, so the lamp and the audio cannot disagree.
const hpfIn = computed(() => hpfHz.value > HPF_PARKED_HZ);
const lpfIn = computed(() => lpfHz.value < LPF_PARKED_HZ);

function currentDsp(): Partial<BusDsp> {
  return {
    hpf: { freq: hpfHz.value, q: props.bus.dsp?.hpf?.q ?? 0.7071 },
    lpf: { freq: lpfHz.value, q: props.bus.dsp?.lpf?.q ?? 0.7071 },
  };
}

function pushFilters() {
  filtHold = true;
  const dsp = currentDsp();
  // The curve is told first, so it tracks the knob rather than the round trip.
  emit('dsp-live', dsp);
  void server.setBusDsp(props.bus.id, dsp).catch(() => {});
  if (filtSettle) clearTimeout(filtSettle);
  filtSettle = setTimeout(() => {
    filtSettle = null;
    filtHold   = false;
    emit('patch', props.bus.id, { dsp: currentDsp() } as Partial<Bus>);
  }, 250);
}

function onHpf(v: number) { hpfHz.value = v; pushFilters(); }
function onLpf(v: number) { lpfHz.value = v; pushFilters(); }

// ---- Stereo image --------------------------------------------------------
// Width lives in the strip's DSP rather than in its sends, because the M/S
// matrix needs a negative cross-term above unity and send gains are decibels.
// So it travels the same live-then-persist path the filters do.
const widthVal  = ref(props.bus.dsp?.width?.width ?? 1);
const bassHz    = ref(props.bus.dsp?.width?.bassMonoHz ?? BASS_MONO_PARKED_HZ);
let   widthHold = false;
let   widthSettle: ReturnType<typeof setTimeout> | null = null;

watch(() => props.bus.dsp?.width, v => {
  if (widthHold) return;
  widthVal.value = v?.width ?? 1;
  bassHz.value   = v?.bassMonoHz ?? BASS_MONO_PARKED_HZ;
}, { deep: true });

watch(() => props.bus.id, () => {
  widthVal.value = props.bus.dsp?.width?.width ?? 1;
  bassHz.value   = props.bus.dsp?.width?.bassMonoHz ?? BASS_MONO_PARKED_HZ;
});

// In circuit, by the same rule the filters use: unity width and a parked
// bass-mono knob are both genuine no-ops, so neither needs a switch.
const widthIn = computed(() => widthVal.value !== 1);
const bassIn  = computed(() => bassHz.value > BASS_MONO_PARKED_HZ);

function pushWidth() {
  widthHold = true;
  const width = {
    width: widthVal.value,
    bassMonoHz: bassHz.value,
    bassMonoQ: props.bus.dsp?.width?.bassMonoQ ?? 0.7071,
  };
  emit('dsp-live', { width });
  void server.setBusDsp(props.bus.id, { width }).catch(() => {});
  if (widthSettle) clearTimeout(widthSettle);
  widthSettle = setTimeout(() => {
    widthSettle = null;
    widthHold   = false;
    emit('patch', props.bus.id, { dsp: { width } } as Partial<Bus>);
  }, 250);
}

function onWidth(v: number)    { widthVal.value = v; pushWidth(); }
function onBassMono(v: number) { bassHz.value   = v; pushWidth(); }

const gainLabel = computed(() =>
  gainDb.value <= -60 ? '-∞' : (gainDb.value > 0 ? '+' : '') + gainDb.value.toFixed(1));

const panLabel = computed(() => {
  const v = Math.round(pan.value * 100);
  if (v === 0) return 'C';
  return (v < 0 ? 'L' : 'R') + Math.abs(v);
});

// Correlation, drawn as a bar growing out of the centre of its track: right
// toward +1 (mono-compatible) and left toward -1 (cancelling). Centre-out
// rather than left-filling because what the eye needs is distance from mono,
// and the sign of that distance.
const correlation = computed(() => (isStereo.value ? mL.correlation.value : 1));
const corrStyle = computed(() => {
  const c = Math.max(-1, Math.min(1, correlation.value));
  const half = Math.abs(c) * 50;
  return c >= 0
    ? { left: '50%', width: half + '%' }
    : { left: (50 - half) + '%', width: half + '%' };
});

// Reads the same streams the meter above does, formatted by the shared helper,
// so this number and the rail strip's cannot disagree.
const { meterMode } = useOutputTarget();
const mL = useMixerMeter(() => props.bus.mixerId, () => 0);
const mR = useMixerMeter(() => props.bus.mixerId, () => (props.bus.width >= 2 ? 1 : 0));

const meterLabel = computed(() => {
  const mono = props.bus.width < 2;
  if (meterMode.value === 'LUFS') {
    return formatMeterLabel(
      mono ? lufsFromKwMs([mL.kwMs.value]) : lufsFromKwMs([mL.kwMs.value, mR.kwMs.value]),
      meterMode.value);
  }
  const pick = (s: typeof mL) =>
    meterMode.value === 'RMS'  ? s.rms.value
    : meterMode.value === 'dBTP' ? s.truePeak.value
    : s.peak.value;
  return formatMeterLabel(mono ? pick(mL) : Math.max(pick(mL), pick(mR)), meterMode.value);
});
</script>

<style scoped>
.cf {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  width: 148px;
  flex: 0 0 auto;
  min-height: 0;
  padding: var(--spacing-sm);
  background: var(--color-surface);
  border-right: 1px solid var(--color-border);
}
/* Only the meter/fader block gives way when the window shortens. */
.cf > *:not(.cf__meterfader) { flex: 0 0 auto; }

.cf__head { display: flex; align-items: center; gap: 2px; }
.cf__step {
  display: flex;
  padding: 2px;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.cf__step:disabled { opacity: 0.3; cursor: default; }
.cf__name { display: flex; align-items: center; flex: 1; min-width: 0; }
.cf__name input {
  width: 100%;
  min-width: 0;
  font-size: 12px;
  padding: 3px 4px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
}

.cf__meterfader {
  display: flex;
  gap: 4px;
  justify-content: center;
  flex: 1 1 0;
  min-height: 60px;
}
.cf__meters { display: flex; gap: 2px; align-self: flex-end; min-height: 0; }
.cf__nometer { width: 6px; height: 100%; background: var(--color-background); border-radius: 2px; }

.cf__readout {
  text-align: center;
  font-family: var(--font-mono);
  font-size: 10px;
  color: var(--color-text-secondary);
}
.cf__gain {
  text-align: center;
  font-family: var(--font-mono);
  font-size: 12px;
  color: var(--color-text-primary);
}

.cf__pair { display: flex; gap: 4px; }
.cf__btn {
  flex: 1;
  font-size: 11px;
  padding: 5px 0;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.cf__btn--mute { background: var(--color-danger); border-color: var(--color-danger); color: #fff; }
.cf__btn--pfl  { background: var(--color-success); border-color: var(--color-success); color: #fff; }
.cf__btn:disabled { opacity: 0.4; cursor: not-allowed; }

.cf__filters {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: var(--spacing-xs);
  border: 1px dashed var(--color-border);
  border-radius: var(--border-radius-sm);
}
.cf__filterrow { display: flex; justify-content: space-around; gap: 4px; }
/* A filter in circuit says so. Parked at the end of its travel it is out, and
   the only way to tell at a glance is the label — there is no in/out switch
   to look at. */
.cf__filter--in :deep(.kf__label) { color: var(--color-accent); }

/* Placement and image, as one block: where the channel sits and how wide it
   is are the same question asked twice. */
.cf__image {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 4px;
}
.cf__pan { display: flex; flex-direction: column; align-items: center; gap: 2px; }
.cf__panlabel {
  font-family: var(--font-mono);
  font-size: 10px;
  color: var(--color-text-secondary);
}
.cf__widthrow { display: flex; justify-content: center; gap: 4px; }

/* Correlation. Deliberately not StereoMeter: that reads level against the
   project's output target, and this is a signed ratio on a fixed -1..+1 scale
   that has nothing to do with how loud anything is. */
.cf__corr {
  display: flex;
  align-items: center;
  gap: 4px;
  width: 100%;
}
.cf__corrtrack {
  position: relative;
  flex: 1 1 auto;
  height: 4px;
  min-width: 40px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 2px;
  overflow: hidden;
}
/* Mono sits at the right-hand end, so the bar shortens as the image opens up
   and the tick marks where correlation crosses into cancellation. */
.cf__corrzero {
  position: absolute;
  top: 0;
  bottom: 0;
  left: 50%;
  width: 1px;
  background: var(--color-border);
}
.cf__corrfill {
  position: absolute;
  top: 0;
  bottom: 0;
  background: var(--color-accent);
  transition: left 80ms linear, width 80ms linear;
}
/* Negative correlation is the one state worth colouring: it is material that
   will partly vanish the moment anything sums this strip to mono. */
.cf__corrfill--warn { background: var(--color-danger, #e5484d); }
.cf__corrlabel {
  font-family: var(--font-mono);
  font-size: 9px;
  min-width: 30px;
  text-align: right;
  color: var(--color-text-secondary);
}
.cf__corrlabel--warn { color: var(--color-danger, #e5484d); }
</style>
