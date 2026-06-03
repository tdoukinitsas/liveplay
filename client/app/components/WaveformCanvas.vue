<template>
  <div class="waveform-canvas" :style="{ height: height + 'px' }">
    <canvas ref="canvasEl" class="waveform-canvas__draw" />
    <div v-if="loading" class="waveform-canvas__overlay">loading waveform…</div>
    <div v-else-if="error" class="waveform-canvas__overlay error">{{ error }}</div>
  </div>
</template>

<!--
  WaveformCanvas.vue
  -----------------------------------------------------------------------
  Renders a server-downsampled waveform (1000 buckets by default) onto a
  Canvas2D element. Replaces the WaveSurfer.js-based renderer used in the
  1.x client.

  Props:
    cueId       — the cue whose waveform to fetch
    height      — canvas height in pixels (default 96)
    color       — RGB peak colour (default "#5fb5ff")
    rmsColor    — RGB RMS colour overlay (default "rgba(255,255,255,0.35)")
    playheadSec — optional current playhead position; overlays a vertical
                  line on top of the waveform
    durationSec — duration in seconds (provide for an accurate playhead
                  position; falls back to the waveform's own duration_ms)

  Behaviour:
    * Fetches the bucket data via useLiveplayServer().fetchWaveform()
    * Re-renders on resize using ResizeObserver
    * Renders peak as opaque bars, RMS as a brighter inset bar
    * Multichannel files render channels stacked vertically
-->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import type { ServerWaveform } from '~/types/server';

const props = withDefaults(defineProps<{
  cueId: string | null;
  height?: number;
  color?: string;
  rmsColor?: string;
  playheadSec?: number;
  durationSec?: number;
}>(), {
  height: 96,
  color: '#5fb5ff',
  rmsColor: 'rgba(255,255,255,0.35)',
  playheadSec: 0,
  durationSec: 0,
});

const server = useLiveplayServer();
const canvasEl = ref<HTMLCanvasElement | null>(null);
const waveform = ref<ServerWaveform | null>(null);
const loading  = ref(false);
const error    = ref<string | null>(null);

let resizeObserver: ResizeObserver | null = null;

const duration = computed(() => {
  if (props.durationSec > 0) return props.durationSec;
  if (waveform.value)        return waveform.value.duration_ms / 1000;
  return 0;
});

async function load() {
  if (!props.cueId) { waveform.value = null; return; }
  loading.value = true;
  error.value   = null;
  try {
    waveform.value = await server.fetchWaveform(props.cueId, 1000);
    draw();
  } catch (e: any) {
    error.value = String(e.message || e);
    waveform.value = null;
  } finally {
    loading.value = false;
  }
}

function draw() {
  const c = canvasEl.value;
  if (!c) return;
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth;
  const h = c.clientHeight;
  c.width  = Math.max(1, Math.floor(w * dpr));
  c.height = Math.max(1, Math.floor(h * dpr));
  const ctx = c.getContext('2d');
  if (!ctx) return;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  if (!waveform.value) return;

  const channels = waveform.value.channels;
  const chHeight = h / Math.max(1, channels.length);

  for (let ci = 0; ci < channels.length; ++ci) {
    const ch = channels[ci];
    const peak = ch.peak;
    const rms  = ch.rms;
    const yMid = ci * chHeight + chHeight / 2;
    const halfH = chHeight / 2 - 1;

    const buckets = peak.length;
    const bw = w / buckets;

    ctx.fillStyle = props.color;
    for (let i = 0; i < buckets; ++i) {
      const p = peak[i];
      const yTop = yMid - p * halfH;
      const yBot = yMid + p * halfH;
      ctx.fillRect(i * bw, yTop, Math.max(1, bw - 0.5), yBot - yTop);
    }

    ctx.fillStyle = props.rmsColor;
    for (let i = 0; i < buckets; ++i) {
      const r = rms[i];
      const yTop = yMid - r * halfH;
      const yBot = yMid + r * halfH;
      ctx.fillRect(i * bw, yTop, Math.max(1, bw - 0.5), yBot - yTop);
    }

    if (ci > 0) {
      ctx.strokeStyle = 'rgba(255,255,255,0.08)';
      ctx.beginPath();
      ctx.moveTo(0, ci * chHeight);
      ctx.lineTo(w, ci * chHeight);
      ctx.stroke();
    }
  }

  // Playhead line.
  if (duration.value > 0 && props.playheadSec > 0) {
    const x = Math.min(w, (props.playheadSec / duration.value) * w);
    ctx.strokeStyle = '#ff5a5a';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, h);
    ctx.stroke();
  }
}

onMounted(() => {
  load();
  if (canvasEl.value) {
    resizeObserver = new ResizeObserver(() => draw());
    resizeObserver.observe(canvasEl.value);
  }
});

onUnmounted(() => {
  if (resizeObserver && canvasEl.value) resizeObserver.unobserve(canvasEl.value);
  resizeObserver = null;
});

watch(() => props.cueId,       () => load());
watch(() => props.playheadSec, () => draw());
</script>

<style lang="scss" scoped>
.waveform-canvas {
  position: relative;
  width: 100%;
  background: #111;
  border-radius: 4px;
  overflow: hidden;

  &__draw {
    display: block;
    width: 100%;
    height: 100%;
  }

  &__overlay {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #888;
    font-size: 12px;
    pointer-events: none;
    background: rgba(0, 0, 0, 0.25);

    &.error { color: #ff8080; }
  }
}
</style>
