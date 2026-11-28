// =====================================================================
// useTimelineView.ts
// ---------------------------------------------------------------------
// The visible window of a cue's file, shared by every timeline editor in the
// Properties panel (the Playback and Events tabs). Both used to carry their
// own: Playback zoomed 1-20x on a linear slider with a 0-100 % scrollbar,
// Events zoomed to a quarter of a second with a log slider and a scroll in
// seconds. Same file, same gestures, two behaviours. One model now:
//
//   * the view is [viewStart, viewStart + visibleDuration] of the file;
//   * zoom runs from the whole file (1) down to a quarter of a second across;
//   * the slider is logarithmic, so each step zooms by the same factor;
//   * the buttons zoom around the anchor (the edit cursor when it is on
//     screen, else the middle); the wheel zooms around the pointer;
//   * Shift+wheel, a sideways wheel or trackpad, and a middle-button drag pan.
//
// TimelineNav.vue draws the controls for it.
// =====================================================================
import type { Ref } from 'vue';

export const TIMELINE_ZOOM_STEPS = 100;
/** Each zoom button press, and the factor the slider moves in. */
export const TIMELINE_ZOOM_FACTOR = 1.5;
/** Each wheel notch. */
const WHEEL_ZOOM_FACTOR = 1.25;
/** The closest view: this many seconds across the timeline. */
const MIN_VISIBLE_SECONDS = 0.25;
const ZOOM_CEILING = 500;

export function useTimelineView(
  duration: Ref<number>,
  /** Where the buttons zoom around when it is on screen (the edit cursor). */
  anchor: () => number | null = () => null,
) {
  const zoom = ref(1);
  const viewStart = ref(0);

  const maxZoom = computed(() =>
    Math.max(1, Math.min(ZOOM_CEILING, duration.value / MIN_VISIBLE_SECONDS)));
  const visibleDuration = computed(() => (duration.value > 0 ? duration.value / zoom.value : 0));
  const maxViewStart = computed(() => Math.max(0, duration.value - visibleDuration.value));
  const viewEnd = computed(() => viewStart.value + visibleDuration.value);

  const inView = (s: number) => s >= viewStart.value - 1e-6 && s <= viewEnd.value + 1e-6;

  function setView(nextZoom: number, nextStart: number) {
    zoom.value = Math.min(maxZoom.value, Math.max(1, nextZoom));
    const vis = duration.value > 0 ? duration.value / zoom.value : 0;
    viewStart.value = Math.min(Math.max(0, nextStart), Math.max(0, duration.value - vis));
  }

  /** Zoom keeping `at` (a file time) where it is on screen. */
  function zoomAround(factor: number, at: number) {
    if (!(duration.value > 0)) return;
    const frac = visibleDuration.value > 0 ? (at - viewStart.value) / visibleDuration.value : 0.5;
    const nextZoom = Math.min(maxZoom.value, Math.max(1, zoom.value * factor));
    setView(nextZoom, at - frac * (duration.value / nextZoom));
  }

  /** The buttons: around the anchor when it is on screen, else the middle. */
  function zoomBy(factor: number) {
    const a = anchor();
    zoomAround(factor, a !== null && inView(a) ? a : viewStart.value + visibleDuration.value / 2);
  }

  function fit() { setView(1, 0); }

  function scrollTo(start: number) { setView(zoom.value, start); }

  /** Pan by a distance in pixels of a timeline `width` px wide. */
  function panByPixels(px: number, width: number) {
    setView(zoom.value, viewStart.value + (px / Math.max(1, width)) * visibleDuration.value);
  }

  const zoomSlider = computed(() =>
    maxZoom.value > 1
      ? Math.round((Math.log(zoom.value) / Math.log(maxZoom.value)) * TIMELINE_ZOOM_STEPS)
      : 0);

  function setZoomSlider(step: number) {
    zoomBy(Math.pow(maxZoom.value, step / TIMELINE_ZOOM_STEPS) / zoom.value);
  }

  /**
   * Wheel over the timeline. `timeAtPointer` is the file time under the
   * pointer; `width` the timeline's width in px.
   */
  function onWheel(e: WheelEvent, timeAtPointer: number, width: number) {
    if (!(duration.value > 0)) return;
    const sideways = e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY);
    if (sideways) {
      panByPixels(Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.deltaY, width);
      return;
    }
    zoomAround(e.deltaY < 0 ? WHEEL_ZOOM_FACTOR : 1 / WHEEL_ZOOM_FACTOR, timeAtPointer);
  }

  // A new length (another file, or the waveform arriving) keeps the view legal.
  watch([duration, maxZoom], () => setView(zoom.value, viewStart.value));

  return reactive({
    zoom, viewStart, maxZoom, visibleDuration, maxViewStart, viewEnd,
    inView, setView, zoomAround, zoomBy, fit, scrollTo, panByPixels,
    zoomSlider, setZoomSlider, onWheel,
  });
}

export type TimelineView = ReturnType<typeof useTimelineView>;
