<template>
  <div ref="headerRef" class="project-header">
    <div ref="leftRef" class="header-left">
      <img
        ref="logoRef"
        :src="isDark ? './assets/icons/SVG/liveplay-icon-darkmode@web.svg' : './assets/icons/SVG/liveplay-icon-lightmode@web.svg'"
        alt="LivePlay"
        class="header-logo"
      />
      <h2 class="project-name" :class="{ 'project-name--hidden': hideTitle }">{{ currentProject?.name || t('project.noProject') }}</h2>
    </div>

    <div
      v-if="silenceWarning"
      ref="warningRef"
      class="silence-warning"
      :class="[silenceWarningClass, { 'silence-warning--left': warningMode === 'left' }]"
      :style="warningStyle"
    >
      {{ t('project.silenceWarning') }} {{ Math.ceil(silenceWarning) }} {{ t('project.seconds') }}
    </div>

    <div ref="rightRef" class="header-right">
      <Btn icon="tune" :text="t('settings.title')" @click="showProjectSettings = true" />
      <Btn icon="keyboard" :text="t('controls.shortcutBtn')" @click="showControlConfig = true" />

      <!-- Clock pair: wall clock + LTC timecode, always both visible -->
      <div class="clock-pair">
        <div class="digital-clock clock--active">
          <span class="clock-label">{{ t('project.clock') }}</span>
          <span class="clock-value">{{ currentTime }}</span>
        </div>
        <div class="digital-clock" :class="ltcTimecode ? 'clock--active' : 'clock--inactive'">
          <span class="clock-label">LTC</span>
          <span class="clock-value">{{ ltcTimecode ?? '--:--:--:--' }}</span>
        </div>
      </div>
    </div>
  </div>

  <ControlConfigModal
    v-if="showControlConfig"
    @close="showControlConfig = false"
  />
  <ProjectSettingsModal
    :open="showProjectSettings"
    @close="showProjectSettings = false"
  />
</template>

<script setup lang="ts">
import ProjectSettingsModal from './ProjectSettingsModal.vue';
import Btn from './Btn.vue';
import type { AudioItem } from '~/types/project';

const { currentProject, findItemByUuid, findItemByIndex } = useProject();
const { t } = useLocalization();
const { activeCues } = useAudioEngine();

const showControlConfig = ref(false);
const showProjectSettings = useState('showProjectSettings', () => false);

const isDark = computed(() => currentProject.value?.theme.mode === 'dark');
const currentTime = ref('00:00:00');

// ---- Silence warning -------------------------------------------------------

const silenceWarning = ref<number | null>(null);

const silenceWarningClass = computed(() => {
  if (!silenceWarning.value) return '';
  const seconds = silenceWarning.value;
  if (seconds <= 5) return 'flash-fast';
  if (seconds <= 10) return 'flash-medium';
  if (seconds <= 30) return 'flash-slow';
  return 'warning-yellow';
});

// ---- Silence-warning placement --------------------------------------------
// The warning sits centred over the header, but the header's right side now
// carries buttons + clocks. We adapt:
//   1. center : enough room → keep it centred over the whole header.
//   2. gap    : centred banner would overlap the left/right blocks → centre it
//               in the free gap between the title area and the buttons/clocks.
//   3. left   : it still won't fit in the gap → align it left and let it take
//               the project title's place (the logo stays put).
const headerRef  = ref<HTMLElement | null>(null);
const leftRef    = ref<HTMLElement | null>(null);
const logoRef    = ref<HTMLElement | null>(null);
const rightRef   = ref<HTMLElement | null>(null);
const warningRef = ref<HTMLElement | null>(null);

const warningMode = ref<'center' | 'gap' | 'left'>('center');
const warningLeftPx = ref(0);
const hideTitle = computed(() => !!silenceWarning.value && warningMode.value === 'left');

const warningStyle = computed(() => ({
  left: `${warningLeftPx.value}px`,
  transform: warningMode.value === 'left' ? 'translateX(0)' : 'translateX(-50%)',
}));

const PLACEMENT_MARGIN = 12; // breathing room kept from neighbouring blocks

function recomputeWarningPlacement() {
  const header = headerRef.value;
  const warning = warningRef.value;
  const left = leftRef.value;
  const logo = logoRef.value;
  const right = rightRef.value;
  if (!header || !warning || !left || !logo || !right) return;

  const headerRect = header.getBoundingClientRect();
  // Geometry is measured with the title always occupying space, so the chosen
  // mode never oscillates: in "left" mode the title is only made invisible, it
  // keeps its layout box.
  const leftEdge  = left.getBoundingClientRect().right - headerRect.left;
  const rightEdge = right.getBoundingClientRect().left - headerRect.left;
  const logoEdge  = logo.getBoundingClientRect().right - headerRect.left;
  const w = warning.offsetWidth;
  const center = headerRect.width / 2;

  // 1. Centred over the whole header without touching either block?
  if (center - w / 2 >= leftEdge + PLACEMENT_MARGIN &&
      center + w / 2 <= rightEdge - PLACEMENT_MARGIN) {
    warningMode.value = 'center';
    warningLeftPx.value = center;
    return;
  }

  // 2. Centred within the free gap between the two blocks?
  const gapAvail = (rightEdge - leftEdge) - 2 * PLACEMENT_MARGIN;
  if (w <= gapAvail) {
    warningMode.value = 'gap';
    warningLeftPx.value = (leftEdge + rightEdge) / 2;
    return;
  }

  // 3. Fall back to left-aligned, taking the title's place (logo stays).
  warningMode.value = 'left';
  warningLeftPx.value = logoEdge + PLACEMENT_MARGIN;
}

// Recompute whenever the displayed text changes (digit count shifts width) or
// the header is resized.
watch(() => [silenceWarning.value, isDark.value], () => {
  nextTick(recomputeWarningPlacement);
});

const checkForSilence = () => {
  if (!currentProject.value || activeCues.value.size === 0) {
    silenceWarning.value = null;
    return;
  }

  const cueEndTimes = new Map<string, { time: number; hasValidBehavior: boolean }>();

  for (const [uuid, cue] of activeCues.value) {
    const item = findItemByUuid(uuid);
    if (!item || item.type !== 'audio') continue;
    const audioItem = item as any;
    const timeRemaining = cue.duration - cue.currentTime;
    const hasValidEndBehavior = validateEndBehavior(audioItem);
    cueEndTimes.set(uuid, { time: timeRemaining, hasValidBehavior: hasValidEndBehavior });
  }

  if (cueEndTimes.size === 1) {
    const [, { time, hasValidBehavior }] = Array.from(cueEndTimes.entries())[0];
    silenceWarning.value = (!hasValidBehavior && time <= 60) ? time : null;
    return;
  }

  let minTimeToActualSilence = Infinity;
  const sortedCues = Array.from(cueEndTimes.entries()).sort((a, b) => a[1].time - b[1].time);

  for (let i = 0; i < sortedCues.length; i++) {
    const [, { time, hasValidBehavior }] = sortedCues[i];
    let cuesStillPlaying = 0;
    for (let j = 0; j < sortedCues.length; j++) {
      if (i === j) continue;
      if (sortedCues[j][1].time > time) cuesStillPlaying++;
    }
    if (cuesStillPlaying === 0 && !hasValidBehavior) {
      minTimeToActualSilence = Math.min(minTimeToActualSilence, time);
      break;
    }
  }

  silenceWarning.value = (minTimeToActualSilence <= 60 && minTimeToActualSilence !== Infinity)
    ? minTimeToActualSilence
    : null;
};

const validateEndBehavior = (audioItem: any): boolean => {
  if (!audioItem.endBehavior || audioItem.endBehavior.action === 'nothing') return false;
  const action = audioItem.endBehavior.action;
  if (action === 'next' || action === 'play-next') {
    const currentIndex = audioItem.index;
    if (!currentIndex || !currentProject.value) return false;
    const parentIndex = currentIndex.slice(0, -1);
    const currentPosition = currentIndex[currentIndex.length - 1];
    if (parentIndex.length === 0) {
      return !!currentProject.value.items[currentPosition + 1];
    } else {
      const parent = findItemByIndex(parentIndex);
      return !!(parent && parent.type === 'group' && (parent as any).children[currentPosition + 1]);
    }
  }
  if (action === 'goto-item') {
    return !!(audioItem.endBehavior.targetUuid && findItemByUuid(audioItem.endBehavior.targetUuid));
  }
  if (action === 'goto-index') {
    const ti = audioItem.endBehavior.targetIndex;
    return !!(ti && Array.isArray(ti) && findItemByIndex(ti));
  }
  if (action === 'loop') return true;
  return false;
};

// ---- Wall clock ------------------------------------------------------------

const updateClock = () => {
  const now = new Date();
  currentTime.value = [
    now.getHours(),
    now.getMinutes(),
    now.getSeconds(),
  ].map(n => String(n).padStart(2, '0')).join(':');
};

// ---- LTC timecode ----------------------------------------------------------

// Frame-rate lookup: index matches the ltcFrameRateIndex values in project.ts
const FPS_TABLE = [24, 25, 29.97, 29.97, 30] as const;

function parseTcToFrames(tc: string, fps: number): number {
  const parts = tc.replace(';', ':').split(':');
  if (parts.length !== 4) return 0;
  const [h, m, s, f] = parts.map(Number);
  return ((h * 3600 + m * 60 + s) * Math.round(fps)) + f;
}

function framesToTc(totalFrames: number, fps: number): string {
  const fpsInt = Math.round(fps);
  const f = totalFrames % fpsInt;
  const totalSecs = Math.floor(totalFrames / fpsInt);
  const s = totalSecs % 60;
  const m = Math.floor(totalSecs / 60) % 60;
  const h = Math.floor(totalSecs / 3600);
  return [h, m, s, f].map(n => String(n).padStart(2, '0')).join(':');
}

// Returns the current LTC timecode string if any active cue is outputting LTC
// to a configured LTC device, otherwise null (→ box shown grey with dashes).
const ltcTimecode = computed<string | null>(() => {
  const ltcDevice = (currentProject.value as any)?.settings?.ltcDevice;
  if (!ltcDevice) return null;

  for (const [uuid, cue] of activeCues.value) {
    const item = findItemByUuid(uuid);
    if (!item || item.type !== 'audio') continue;
    const ai = item as AudioItem & { ltcEnabled?: boolean; ltcFrameRateIndex?: number; ltcStartTimecode?: string };
    if (!ai.ltcEnabled) continue;

    const fps = FPS_TABLE[ai.ltcFrameRateIndex ?? 4] ?? 30;
    const startTc = ai.ltcStartTimecode ?? '00:00:00:00';
    const startFrames = parseTcToFrames(startTc, fps);
    const elapsedFrames = Math.floor(cue.currentTime * fps);
    return framesToTc(startFrames + elapsedFrames, fps);
  }
  return null;
});

onMounted(() => {
  updateClock();
  const clockInterval = setInterval(updateClock, 1000);
  const silenceInterval = setInterval(checkForSilence, 100);

  // Re-place the silence banner whenever the header geometry changes
  // (window resize, sidebar toggles, clock width shifts, …).
  let resizeObserver: ResizeObserver | null = null;
  if (headerRef.value && typeof ResizeObserver !== 'undefined') {
    resizeObserver = new ResizeObserver(() => recomputeWarningPlacement());
    resizeObserver.observe(headerRef.value);
  }

  onUnmounted(() => {
    clearInterval(clockInterval);
    clearInterval(silenceInterval);
    if (resizeObserver) resizeObserver.disconnect();
  });
});
</script>

<style scoped lang="scss">
.project-header {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-sm) var(--spacing-lg);
  background-color: var(--color-surface);
  border-bottom: 1px solid var(--color-border);
  min-height: 60px;
}

.header-left {
  display: flex;
  align-items: center;
  gap: var(--spacing-md);
}

.header-logo {
  width: 36px;
  height: 36px;
  object-fit: contain;
}

.project-name {
  font-size: 18px;
  font-weight: 600;
  color: var(--color-text-primary);
  margin: 0;
}

.header-right {
  display: flex;
  align-items: center;
  gap: var(--spacing-md);
}

/* Two clocks side-by-side, never re-arrange */
.clock-pair {
  display: flex;
  gap: var(--spacing-sm);
  align-items: stretch;
}

.digital-clock {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 4px var(--spacing-md);
  border: 2px solid currentColor;
  border-radius: var(--border-radius-md);
  background-color: var(--color-surface);
  transition: color var(--transition-base), border-color var(--transition-base);
  min-width: 110px;
}

.clock--active {
  color: var(--color-accent);
}

.clock--inactive {
  color: var(--color-text-secondary);
  opacity: 0.5;
}

.clock-label {
  font-family: var(--font-mono);
  font-size: 9px;
  font-weight: 700;
  letter-spacing: 0.12em;
  text-transform: uppercase;
  line-height: 1;
  margin-bottom: 2px;
}

.clock-value {
  font-family: var(--font-mono);
  font-size: 22px;
  font-weight: 700;
  letter-spacing: 0.05em;
  line-height: 1;
}

.silence-warning {
  position: absolute;
  /* left + transform are set inline (adaptive placement, see script).
     Vertical centring comes from the flex container's static position. */
  padding: var(--spacing-xs) var(--spacing-lg);
  border-radius: var(--border-radius-md);
  font-weight: 700;
  font-size: 16px;
  color: #000;
  white-space: nowrap;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
  z-index: 10;
}

/* Left-aligned fallback sits in the title's place — tighten the horizontal
   padding so it reads like a header label rather than a centred banner. */
.silence-warning--left {
  padding-left: var(--spacing-md);
  padding-right: var(--spacing-md);
}

/* In the left-aligned fallback the project title is hidden but keeps its
   layout box, so placement geometry stays stable (no flip-flopping). */
.project-name--hidden {
  visibility: hidden;
}

.silence-warning.warning-yellow  { background-color: #fbbf24; }
.silence-warning.flash-slow      { background-color: #fbbf24; animation: flash-slow   2s   ease-in-out infinite; }
.silence-warning.flash-medium    { background-color: #f56d1f; animation: flash-medium 1s   ease-in-out infinite; }
.silence-warning.flash-fast      { background-color: #dc2626; color: #fff; animation: flash-fast 0.5s ease-in-out infinite; }

@keyframes flash-slow   { 0%, 100% { opacity: 0; } 50% { opacity: 1; } }
@keyframes flash-medium { 0%, 100% { opacity: 0; } 50% { opacity: 1; } }
@keyframes flash-fast   { 0%, 100% { opacity: 0; } 50% { opacity: 1; } }
</style>
