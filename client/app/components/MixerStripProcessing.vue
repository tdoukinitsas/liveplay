<template>
  <!--
    What a bus's processing is doing, at strip size. Lamps first — EQ, GATE,
    COMP, lit when that section is actually in circuit, and LIM on a bus that
    feeds a hardware output, since that output's limiter is what it goes
    through last — then the EQ response
    and the dynamics transfer curve as miniatures of the channel view's
    panels, drawn from the same maths (utils/dspCurves.ts), with the live input
    level on the curve and a gain-reduction bar per processor beside it.

    The console idea it borrows: you should be able to read a channel's state
    across a whole rail without opening each one. The whole block opens the
    channel view.
  -->
  <button class="proc" :title="t('mixer.proc.openHint')" @click.stop="$emit('open')">
    <span class="proc__lamps">
      <span class="proc__lamp" :class="{ 'proc__lamp--on': eqOn }">{{ t('mixer.proc.eq') }}</span>
      <span class="proc__lamp" :class="{ 'proc__lamp--on': gateOn, 'proc__lamp--work': gateWorking }">{{ t('mixer.proc.gate') }}</span>
      <span class="proc__lamp" :class="{ 'proc__lamp--on': compOn, 'proc__lamp--work': compWorking }">{{ t('mixer.proc.comp') }}</span>
      <span
        v-if="limiter.present.value"
        class="proc__lamp"
        :class="{ 'proc__lamp--on': limiter.enabled.value, 'proc__lamp--work': limiter.working.value }"
        :title="t('mixer.proc.limHint')"
      >{{ t('mixer.proc.lim') }}</span>
    </span>

    <span class="proc__eq" :class="{ 'proc__eq--off': !eqOn }">
      <svg viewBox="0 0 100 28" preserveAspectRatio="none" class="proc__svg">
        <line class="proc__zero" x1="0" x2="100" y1="14" y2="14" />
        <polyline class="proc__eqcurve" :points="eqPoints" />
      </svg>
    </span>

    <span class="proc__dyn" :class="{ 'proc__dyn--off': !gateOn && !compOn }">
      <svg viewBox="0 0 40 40" class="proc__dynsvg">
        <defs>
          <linearGradient :id="gradId" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="40" y2="0">
            <stop v-for="s in gradientStops" :key="s.offset" :offset="s.offset" :stop-color="s.color" />
          </linearGradient>
        </defs>
        <line class="proc__unity" x1="0" y1="40" x2="40" y2="0" />
        <polygon v-if="levelPoints" :points="levelPoints" :fill="`url(#${gradId})`" class="proc__level" />
        <polyline class="proc__dyncurve" :points="dynPoints" />
      </svg>
      <span class="proc__gr" :title="t('mixer.proc.gateGr')">
        <span class="proc__grfill" :style="{ height: gateGrPct + '%' }"></span>
      </span>
      <span class="proc__gr" :title="t('mixer.proc.compGr')">
        <span class="proc__grfill" :style="{ height: compGrPct + '%' }"></span>
      </span>
      <span v-if="limiter.present.value" class="proc__gr proc__gr--lim" :title="t('mixer.proc.limGr')">
        <span class="proc__grfill" :style="{ height: limGrPct + '%' }"></span>
      </span>
    </span>
  </button>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import type { Bus } from '~/types/project';
import { useBusLimiter, useMixerMeter } from '~/composables/useLiveMeters';
import { useOutputTarget } from '~/composables/useOutputTarget';
import {
  eqSections, eqCurvePoints, eqIsActive, dynOutput, gateIsActive, compIsActive,
} from '~/utils/dspCurves';

const props = defineProps<{ bus: Bus }>();
defineEmits<{ (e: 'open'): void }>();
const { t } = useLocalization();

const dsp = computed(() => props.bus.dsp);
const eqOn   = computed(() => eqIsActive(dsp.value));
const gateOn = computed(() => gateIsActive(dsp.value));
const compOn = computed(() => compIsActive(dsp.value));

// ±18 dB, the same range as the channel view's graph.
const eqPoints = computed(() => eqCurvePoints(eqSections(dsp.value), 100, 28, 18, 49));

// Transfer curve over -60..0 dB into a 40x40 box.
const MIN_DB = -60;
const xy = (db: number) => ((Math.max(MIN_DB, Math.min(0, db)) - MIN_DB) / -MIN_DB) * 40;
const dynPoints = computed(() => Array.from({ length: 41 }, (_, i) => {
  const inDb = MIN_DB + (i / 40) * -MIN_DB;
  return `${xy(inDb).toFixed(1)},${(40 - xy(dynOutput(dsp.value, inDb))).toFixed(1)}`;
}).join(' '));

// Live input level on the curve, with meter ballistics (rise at once, fall
// 20 dB/s) so it reads like the meter below it.
const meter = useMixerMeter(() => props.bus.mixerId);
const shownIn = ref(-120);
let lastAt = 0;
watch(() => meter.dynIn.value, (db) => {
  const now = performance.now();
  const dt = lastAt ? (now - lastAt) / 1000 : 0;
  lastAt = now;
  shownIn.value = Math.max(db, shownIn.value - 20 * dt);
});
const levelPoints = computed(() => {
  const inDb = shownIn.value;
  if (inDb <= MIN_DB) return '';
  const pts = ['0,40'];
  for (let i = 0; i <= 20; i++) {
    const db = MIN_DB + (i / 20) * (inDb - MIN_DB);
    pts.push(`${xy(db).toFixed(1)},${(40 - xy(dynOutput(dsp.value, db))).toFixed(1)}`);
  }
  pts.push(`${xy(inDb).toFixed(1)},40`);
  return pts.join(' ');
});

const { colorForLevel } = useOutputTarget();
const gradId = `stripdyn-${Math.random().toString(36).slice(2, 9)}`;
const gradientStops = computed(() => {
  const out: { offset: string; color: string }[] = [];
  for (let db = MIN_DB; db <= 0; db += 6) {
    out.push({ offset: `${((db - MIN_DB) / -MIN_DB * 100).toFixed(0)}%`, color: colorForLevel(db) });
  }
  return out;
});

// Gain reduction, scaled like the channel view's meters: a gate's range runs
// deep, a compressor pulling 20 dB is already a lot.
const grPct = (gr: number, full: number) => Math.min(100, (Math.abs(Math.min(0, gr)) / full) * 100);
const gateGrPct = computed(() => grPct(meter.gateGr.value, 40));
const compGrPct = computed(() => grPct(meter.compGr.value, 20));
// "Working" = actually pulling the signal down right now.
const gateWorking = computed(() => gateOn.value && meter.gateGr.value < -1);
// The output limiter this bus goes through, if it feeds hardware. A limiter
// holding back 6 dB is already working very hard.
const limiter = useBusLimiter(() => props.bus);
const limGrPct = computed(() => grPct(limiter.grDb.value, 6));
const compWorking = computed(() => compOn.value && meter.compGr.value < -1);
</script>

<style scoped>
.proc {
  display: flex;
  flex-direction: column;
  gap: 3px;
  width: 100%;
  padding: 3px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
  text-align: left;
}
.proc:hover { border-color: var(--color-accent); }

.proc__lamps { display: flex; gap: 2px; }
.proc__lamp {
  flex: 1 1 0;
  text-align: center;
  font-size: 8px;
  font-weight: 700;
  font-family: var(--font-mono);
  letter-spacing: 0.04em;
  padding: 1px 0;
  border-radius: 2px;
  color: var(--color-text-disabled);
  background: var(--color-surface);
}
/* In circuit: the console's lit-insert amber. Working: brighter. */
.proc__lamp--on { color: #1a1a1a; background: #d4a017; }
.proc__lamp--work { background: #f5b82e; box-shadow: 0 0 4px rgba(245, 184, 46, 0.6); }

.proc__eq { display: block; height: 28px; }
.proc__eq--off { opacity: 0.45; }
.proc__svg { display: block; width: 100%; height: 100%; }
.proc__zero { stroke: var(--color-border); stroke-width: 1; vector-effect: non-scaling-stroke; }
.proc__eqcurve {
  fill: none;
  stroke: #22d3ee;
  stroke-width: 1.5;
  vector-effect: non-scaling-stroke;
  stroke-linejoin: round;
}

.proc__dyn { display: flex; align-items: stretch; gap: 3px; height: 40px; }
/* The limiter bar stays at full strength when the bus's own dynamics are off:
   it belongs to the output, not to them. */
.proc__dyn--off > .proc__dynsvg,
.proc__dyn--off > .proc__gr:not(.proc__gr--lim) { opacity: 0.5; }
.proc__dynsvg { flex: 0 0 40px; height: 40px; background: var(--color-surface); border-radius: 2px; }
.proc__unity { stroke: var(--color-text-disabled); stroke-width: 0.5; stroke-dasharray: 2 2; opacity: 0.5; }
.proc__level { opacity: 0.6; }
.proc__dyncurve { fill: none; stroke: #22d3ee; stroke-width: 1.2; stroke-linejoin: round; }
.proc__gr {
  position: relative;
  flex: 1 1 0;
  max-width: 8px;
  background: var(--color-surface);
  border-radius: 2px;
  overflow: hidden;
}
/* Down from the top: gain reduction is what is taken away. */
.proc__grfill {
  position: absolute;
  top: 0; left: 0; right: 0;
  background: #f5b82e;
  transition: height 60ms linear;
}
</style>
