<template>
  <!--
    The mixer, in miniature, under the channel view — so while one channel's
    EQ or dynamics has the screen, the rest of the desk is still in view:
    every bus's level, what its processing is doing, where its fader and pan
    sit, whether it is muted or in PFL. An overview, not a second set of
    controls: clicking a strip brings that channel into the view above.
  -->
  <div class="mini">
    <button
      v-for="(b, i) in buses"
      :key="b.id"
      class="mini__strip"
      :class="{ 'mini__strip--active': b.id === activeId, 'mini__strip--muted': b.mute }"
      :style="{ '--bus': b.color || 'var(--color-accent)' }"
      :title="b.name"
      @click="$emit('select', b.id)"
    >
      <span class="mini__bar"></span>
      <span class="mini__head">
        <span class="mini__num">{{ i + 1 }}</span>
        <span class="mini__lamps">
          <span class="mini__lamp" :class="{ 'mini__lamp--on': eqIsActive(b.dsp) }">{{ t('mixer.proc.eq') }}</span>
          <span class="mini__lamp" :class="{ 'mini__lamp--on': gateIsActive(b.dsp) }">{{ t('mixer.proc.gateShort') }}</span>
          <span class="mini__lamp" :class="{ 'mini__lamp--on': compIsActive(b.dsp) }">{{ t('mixer.proc.compShort') }}</span>
        </span>
      </span>
      <span class="mini__meter">
        <StereoMeter
          v-if="b.mixerId"
          :mixer-id="b.mixerId"
          :mono="b.width < 2"
          bare
          :show-scale="false"
          :min-db="FADER_MIN_DB"
          :max-db="METER_MAX_DB"
        />
      </span>
      <span class="mini__info">
        <span class="mini__gain">{{ gainLabel(b.gainDb) }}</span>
        <span class="mini__pan">{{ panLabel(b.pan ?? 0) }}</span>
      </span>
      <span class="mini__flags">
        <span class="mini__flag" :class="{ 'mini__flag--mute': b.mute }">M</span>
        <span v-if="!b.preview" class="mini__flag" :class="{ 'mini__flag--pfl': b.pfl }">{{ t('mixer.pfl') }}</span>
      </span>
      <span class="mini__name">{{ b.name }}</span>
    </button>
  </div>
</template>

<script setup lang="ts">
import type { Bus } from '~/types/project';
import StereoMeter from './StereoMeter.vue';
import { FADER_MIN_DB, METER_MAX_DB } from '~/utils/meterScale';
import { eqIsActive, gateIsActive, compIsActive } from '~/utils/dspCurves';

defineProps<{ buses: Bus[]; activeId: string | null }>();
defineEmits<{ (e: 'select', id: string): void }>();
const { t } = useLocalization();

const gainLabel = (db: number) => (db <= -60 ? '-∞' : (db > 0 ? '+' : '') + db.toFixed(1));
const panLabel = (p: number) => {
  const v = Math.round(p * 100);
  return v === 0 ? 'C' : (v < 0 ? 'L' : 'R') + Math.abs(v);
};
</script>

<style scoped>
.mini {
  display: flex;
  gap: 3px;
  flex: 1;
  min-width: 0;
  height: 100%;
  overflow-x: auto;
  overflow-y: hidden;
  padding-bottom: 2px;
}
.mini__strip {
  position: relative;
  display: flex;
  flex-direction: column;
  align-items: stretch;
  gap: 2px;
  flex: 0 0 74px;
  padding: 0 3px 3px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-primary);
  cursor: pointer;
  overflow: hidden;
  text-align: center;
}
.mini__strip:hover { border-color: var(--color-text-secondary); }
.mini__strip--active {
  background: var(--color-surface-hover, var(--color-surface));
  border-color: var(--bus);
  box-shadow: inset 0 0 0 1px var(--bus);
}
/* The bus colour across the top, as a console's scribble strip carries it. */
.mini__bar { display: block; height: 4px; margin: 0 -3px 1px; background: var(--bus); }
.mini__head { display: flex; align-items: center; justify-content: space-between; gap: 2px; }
.mini__num { font-size: 11px; font-weight: 700; font-family: var(--font-mono); }
.mini__lamps { display: flex; gap: 1px; }
.mini__lamp {
  font-size: 7px;
  font-weight: 700;
  font-family: var(--font-mono);
  padding: 0 2px;
  border-radius: 2px;
  color: var(--color-text-disabled);
  background: var(--color-background);
}
.mini__lamp--on { color: #1a1a1a; background: #d4a017; }
.mini__meter { flex: 1 1 auto; min-height: 30px; display: flex; justify-content: center; }
.mini__meter > * { height: 100%; }
.mini__info { display: flex; justify-content: space-between; font-size: 9px; font-family: var(--font-mono); color: var(--color-text-secondary); }
.mini__flags { display: flex; gap: 2px; justify-content: center; }
.mini__flag {
  flex: 1 1 0;
  font-size: 8px;
  font-weight: 700;
  font-family: var(--font-mono);
  border-radius: 2px;
  color: var(--color-text-disabled);
  background: var(--color-background);
}
.mini__flag--mute { color: #fff; background: var(--color-danger); }
.mini__flag--pfl { color: #1a1a1a; background: var(--color-warning); }
.mini__name {
  font-size: 10px;
  font-weight: 600;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.mini__strip--muted .mini__meter { opacity: 0.55; }
</style>
