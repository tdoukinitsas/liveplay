<template>
  <!-- The row under every timeline in the Properties panel: zoom out, the
       zoom slider, zoom in, the level, Fit, then the scrollbar while zoomed in
       (or how to navigate, while the whole file is showing). -->
  <div class="tl-nav">
    <button type="button" class="tl-nav-btn" :title="t('events.zoomOut')" :aria-label="t('events.zoomOut')"
            :disabled="view.zoom <= 1" @click="view.zoomBy(1 / TIMELINE_ZOOM_FACTOR)">
      <span class="material-symbols-rounded" aria-hidden="true">zoom_out</span>
    </button>
    <input
      class="tl-zoom-slider"
      type="range"
      min="0"
      :max="TIMELINE_ZOOM_STEPS"
      step="1"
      :value="view.zoomSlider"
      :aria-label="t('events.zoom')"
      @input="view.setZoomSlider(Number(($event.target as HTMLInputElement).value))"
    />
    <button type="button" class="tl-nav-btn" :title="t('events.zoomIn')" :aria-label="t('events.zoomIn')"
            :disabled="view.zoom >= view.maxZoom" @click="view.zoomBy(TIMELINE_ZOOM_FACTOR)">
      <span class="material-symbols-rounded" aria-hidden="true">zoom_in</span>
    </button>
    <span class="tl-zoom-text">{{ Math.round(view.zoom * 100) }}%</span>
    <button type="button" class="tl-nav-btn" :title="t('events.zoomFit')" :aria-label="t('events.zoomFit')"
            :disabled="view.zoom <= 1" @click="view.fit()">
      <span class="material-symbols-rounded" aria-hidden="true">fit_screen</span>
    </button>
    <input
      v-if="view.zoom > 1"
      class="tl-scroll-slider"
      type="range"
      min="0"
      :max="view.maxViewStart"
      step="0.001"
      :value="view.viewStart"
      :aria-label="t('events.scroll')"
      @input="view.scrollTo(Number(($event.target as HTMLInputElement).value))"
    />
    <span v-else class="tl-nav-hint">{{ t('events.navHint') }}</span>
  </div>
</template>

<script setup lang="ts">
import { TIMELINE_ZOOM_FACTOR, TIMELINE_ZOOM_STEPS, type TimelineView } from '~/composables/useTimelineView';

defineProps<{ view: TimelineView }>();
const { t } = useLocalization();
</script>

<style scoped>
.tl-nav {
  display: flex;
  align-items: center;
  gap: 6px;
  flex: 0 0 auto;
  min-height: 26px;
  min-width: 0;
}

.tl-nav-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 24px;
  height: 24px;
  flex: 0 0 auto;
  background: none;
  border: 1px solid transparent;
  border-radius: 4px;
  color: var(--color-text-secondary);
  cursor: pointer;

  .material-symbols-rounded { font-size: 18px; }

  &:hover:not(:disabled) {
    color: var(--color-text-primary);
    background: var(--color-surface-hover);
  }

  &:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }
}

.tl-zoom-slider {
  width: 120px;
  flex: 0 1 120px;
  min-width: 60px;
  padding: 0;
  accent-color: var(--color-accent);
}

.tl-zoom-text {
  min-width: 44px;
  font-size: 11px;
  color: var(--color-text-secondary);
  font-variant-numeric: tabular-nums;
}

.tl-scroll-slider {
  flex: 1;
  min-width: 80px;
  padding: 0;
  accent-color: var(--color-accent);
}

.tl-nav-hint {
  flex: 1;
  min-width: 0;
  font-size: 11px;
  color: var(--color-text-secondary);
  text-align: right;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
</style>
