<template>
  <!--
    Vertical canvas-based volume fader.

    Why canvas instead of <input type="range" rotate(-90deg)>: the rotated
    range input was unreliable across Chromium builds — pointer math broke
    in containers with transforms, the thumb hit-box drifted, and dark-mode
    label colours leaked from the UA stylesheet. Canvas gives us pixel-exact
    control of the track, thumb, drag math, and theming.

    Interaction:
      • Click + vertical drag       → set value (coarse)
      • Shift + drag                → fine adjust (¼ sensitivity)
      • Wheel                       → ±0.5 dB step
      • Double-click                → emit('reset')
  -->
  <div
    ref="hostRef"
    class="canvas-fader"
    @mousedown="onMouseDown"
    @dblclick="$emit('reset')"
    @wheel.prevent="onWheel"
  >
    <canvas ref="canvasRef" class="canvas-fader__canvas" />
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue';

const props = withDefaults(defineProps<{
  db: number;
  minDb?: number;
  maxDb?: number;
}>(), {
  minDb: -60,
  maxDb: 6,
});

const emit = defineEmits<{
  (e: 'input', db: number): void;
  (e: 'reset'): void;
}>();

const hostRef   = ref<HTMLElement | null>(null);
const canvasRef = ref<HTMLCanvasElement | null>(null);

const range = computed(() => props.maxDb - props.minDb);

// Map a dB value to a 0..1 vertical position (1 = top = maxDb).
const dbToNorm = (db: number) =>
  Math.max(0, Math.min(1, (db - props.minDb) / range.value));
const normToDb = (n: number) =>
  Math.max(props.minDb, Math.min(props.maxDb,
    props.minDb + Math.max(0, Math.min(1, n)) * range.value));

function readCssVar(name: string, fallback: string): string {
  const host = hostRef.value;
  if (!host) return fallback;
  const v = getComputedStyle(host).getPropertyValue(name).trim();
  return v || fallback;
}

function draw() {
  const cv = canvasRef.value;
  const host = hostRef.value;
  if (!cv || !host) return;

  const dpr = window.devicePixelRatio || 1;
  const w = host.clientWidth;
  const h = host.clientHeight;
  if (w === 0 || h === 0) return;
  if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
    cv.width  = Math.round(w * dpr);
    cv.height = Math.round(h * dpr);
  }
  const ctx = cv.getContext('2d');
  if (!ctx) return;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const trackW = 4;
  const trackX = (w - trackW) / 2;
  const padTop    = 6;
  const padBottom = 6;
  const trackTop    = padTop;
  const trackBottom = h - padBottom;
  const trackH = trackBottom - trackTop;

  const border = readCssVar('--color-border', '#444');
  const accent = readCssVar('--color-accent', '#0f62fe');
  const surface = readCssVar('--color-surface', '#1f2226');

  // Track
  ctx.fillStyle = border;
  ctx.beginPath();
  if (typeof (ctx as any).roundRect === 'function') {
    (ctx as any).roundRect(trackX, trackTop, trackW, trackH, trackW / 2);
  } else {
    ctx.rect(trackX, trackTop, trackW, trackH);
  }
  ctx.fill();

  // Fill from 0 dB up to current value (or current value up to 0 dB if negative).
  const zeroY = trackTop + (1 - dbToNorm(0)) * trackH;
  const valY  = trackTop + (1 - dbToNorm(props.db)) * trackH;
  ctx.fillStyle = accent;
  ctx.beginPath();
  const top = Math.min(zeroY, valY);
  const bot = Math.max(zeroY, valY);
  if (typeof (ctx as any).roundRect === 'function') {
    (ctx as any).roundRect(trackX, top, trackW, bot - top, trackW / 2);
  } else {
    ctx.rect(trackX, top, trackW, bot - top);
  }
  ctx.fill();

  // 0 dB unity tick
  ctx.strokeStyle = readCssVar('--color-text-secondary', '#888');
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(trackX - 3, zeroY);
  ctx.lineTo(trackX + trackW + 3, zeroY);
  ctx.stroke();

  // Thumb
  const thumbR = 6;
  ctx.fillStyle = accent;
  ctx.strokeStyle = surface;
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(trackX + trackW / 2, valY, thumbR, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();
}

watch(() => [props.db, props.minDb, props.maxDb], () => draw());

let resizeObserver: ResizeObserver | null = null;
onMounted(() => {
  draw();
  if (hostRef.value) {
    resizeObserver = new ResizeObserver(() => draw());
    resizeObserver.observe(hostRef.value);
  }
});
onUnmounted(() => {
  if (resizeObserver) resizeObserver.disconnect();
  resizeObserver = null;
});

// ---- Pointer interaction ---------------------------------------------------
// Drag math is anchored: at mouse-down we record the current dB AND mouse
// position. Subsequent moves translate the *delta* into a dB delta over the
// available track height. This avoids the "jumps to cursor" feel of an
// absolute-position fader, and lets shift-drag scale sensitivity cleanly.
let dragging   = false;
let dragStartY = 0;
let dragStartDb = 0;

function trackHeightPx(): number {
  const host = hostRef.value;
  if (!host) return 1;
  return Math.max(1, host.clientHeight - 12); // matches padTop + padBottom
}

function onMouseDown(e: MouseEvent) {
  if (e.button !== 0) return;
  dragging = true;
  dragStartY = e.clientY;
  dragStartDb = props.db;
  window.addEventListener('mousemove', onMouseMove);
  window.addEventListener('mouseup', onMouseUp);
  // For a single click without drag, snap to clicked position. We use a
  // tiny threshold: only snap if the click is far from the thumb.
  const host = hostRef.value;
  if (host) {
    const rect = host.getBoundingClientRect();
    const yInside = e.clientY - rect.top;
    const norm = 1 - (yInside - 6) / trackHeightPx();
    const targetDb = normToDb(norm);
    if (Math.abs(targetDb - props.db) > 3) {
      // Re-anchor the drag to the snapped value so further drag is relative.
      dragStartDb = targetDb;
      emit('input', clampToStep(targetDb, /*fine*/ false));
    }
  }
}

function onMouseMove(e: MouseEvent) {
  if (!dragging) return;
  const dy = dragStartY - e.clientY; // dragging up = positive
  const sens = e.shiftKey ? 0.25 : 1;
  const dbPerPx = (range.value / trackHeightPx()) * sens;
  const nextDb = clampToStep(dragStartDb + dy * dbPerPx, e.shiftKey);
  emit('input', nextDb);
}

function onMouseUp() {
  dragging = false;
  window.removeEventListener('mousemove', onMouseMove);
  window.removeEventListener('mouseup', onMouseUp);
}

function onWheel(e: WheelEvent) {
  const step = e.shiftKey ? 0.1 : 0.5;
  const dir  = e.deltaY < 0 ? 1 : -1;
  emit('input', clampToStep(props.db + dir * step, e.shiftKey));
}

function clampToStep(db: number, fine: boolean): number {
  const step = fine ? 0.1 : 0.5;
  const snapped = Math.round(db / step) * step;
  const v = Math.max(props.minDb, Math.min(props.maxDb, snapped));
  // Avoid -0 noise
  return Object.is(v, -0) ? 0 : Number(v.toFixed(2));
}
</script>

<style scoped>
.canvas-fader {
  flex: 1;
  width: 20px;
  min-height: 40px;
  cursor: pointer;
  user-select: none;
  touch-action: none;
}
.canvas-fader__canvas {
  display: block;
  width: 100%;
  height: 100%;
}
</style>
