<template>
  <!--
    The output limiter this bus goes through, in the channel view — after
    Dynamics and before "Feeding this bus", which is where it sits in the
    signal path.

    It is not the bus's own processor, and the panel says so by what it lets
    you touch: nothing here but a way to Settings. There is one brickwall
    limiter per hardware output channel, after every bus feeding that output
    has been summed, with one on/off switch for the project and a ceiling set
    by the Output Target. What belongs on the bus is the answer to "is it
    holding this bus back right now, and by how much?", so that is what this
    shows: gain reduction on the output pair this bus lands on, and the ceiling
    it is holding to.

    Only rendered for a bus that feeds hardware (`masters` set). A bus that
    feeds another bus reaches a limiter only through that bus.
  -->
  <section class="lim det__panel" :class="{ 'lim--off': !limiter.enabled.value }">
    <h4 class="det__h" :title="t('mixer.limiter.hint')">
      {{ t('mixer.limiter.title') }}
      <span class="det__byp" :class="{ 'det__byp--on': !limiter.enabled.value }">
        {{ limiter.enabled.value ? t('mixer.limiter.on') : t('mixer.limiter.off') }}
      </span>
    </h4>

    <div class="lim__row" :title="t('mixer.limiter.grHint')">
      <span class="lim__label">{{ t('mixer.limiter.gr') }}</span>
      <div class="lim__meter">
        <div class="lim__fill" :style="{ width: grPct + '%' }"></div>
        <span v-for="db in TICKS" :key="db" class="lim__tick" :style="{ left: (db / -GR_FULL_DB) * 100 + '%' }"></span>
      </div>
      <span class="lim__value">{{ grText }}</span>
    </div>

    <div class="lim__row">
      <span class="lim__label">{{ t('mixer.limiter.ceiling') }}</span>
      <span class="lim__value lim__value--grow">{{ limiter.ceilingDb.value.toFixed(1) }} dB</span>
      <button type="button" class="lim__settings" :title="t('mixer.limiter.settingsHint')" @click="openSettings('audio')">
        <span class="material-symbols-rounded">settings</span>
        {{ t('settings.title') }}
      </button>
    </div>
  </section>
</template>

<script setup lang="ts">
import type { Bus } from '~/types/project';
import { useBusLimiter } from '~/composables/useLiveMeters';

const props = defineProps<{ bus: Bus }>();
const { t } = useLocalization();
const { open: openSettings } = useSettingsPage();

const limiter = useBusLimiter(() => props.bus);

// 12 dB of limiting is a lot; the meter stops there so a few dB still reads.
const GR_FULL_DB = 12;
const TICKS = [-3, -6, -9];
const grPct = computed(() =>
  Math.min(100, (Math.abs(Math.min(0, limiter.grDb.value)) / GR_FULL_DB) * 100));
const grText = computed(() =>
  limiter.grDb.value < -0.05 ? `${limiter.grDb.value.toFixed(1)} dB` : '0.0 dB');
</script>

<style scoped lang="scss">
.lim {
  flex: 0 0 auto;
  min-width: 0;
}

.lim__row {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  font-size: 11px;
  min-width: 0;
}

.lim__label {
  flex: 0 0 auto;
  min-width: 52px;
  color: var(--color-text-secondary);
  white-space: nowrap;
}

.lim__meter {
  position: relative;
  flex: 1 1 auto;
  min-width: 40px;
  height: 8px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 2px;
  overflow: hidden;
}

.lim__fill {
  position: absolute;
  top: 0;
  bottom: 0;
  left: 0;
  background: var(--color-accent);
  transition: width 60ms linear;
}

.lim__tick {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 1px;
  background: var(--color-border);
}

.lim__value {
  flex: 0 0 auto;
  min-width: 56px;
  text-align: right;
  font-family: var(--font-mono);
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}

.lim__value--grow {
  flex: 1 1 auto;
  text-align: left;
}

.lim__settings {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 6px;
  font-size: 11px;
  font-family: inherit;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;

  .material-symbols-rounded { font-size: 14px; }

  &:hover { border-color: var(--color-accent); }
}

/* Switched off for the project: say so without hiding where it would act. */
.lim--off .lim__meter,
.lim--off .lim__value { opacity: 0.45; }
</style>
