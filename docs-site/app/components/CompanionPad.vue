<template>
  <!-- A 15-key Companion surface running the LivePlay module during the demo
       show (The Tempest, Act 1). Drawn the way the module draws its presets:
       text only, cue keys as progress gauges in the cue's colour. -->
  <div ref="root" class="pad" role="img" :aria-label="t('companion.padAlt')" dir="ltr">
    <div class="pad__keys" aria-hidden="true">
      <span
        v-for="(key, i) in keys"
        :key="i"
        class="pad__key"
        :style="{ '--track': key.track, '--fill': key.fill, '--ink': key.ink }"
      >
        <span v-if="key.progress" class="pad__fill" :style="{ width: `${key.progress * 100}%` }"></span>
        <span class="pad__text" dir="auto" :style="{ '--fs': fitSize(key.label) }">{{ key.label }}</span>
      </span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { useI18n } from '../composables/useI18n';

const { t, locale } = useI18n();

// ---- The module's own drawing rules (companion-module-liveplay src/colors.ts) ----
type Rgb = [number, number, number];
const hex = (h: string): Rgb => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16)) as Rgb;
const css = ([r, g, b]: Rgb) => `rgb(${r} ${g} ${b})`;
const dim = (c: Rgb, f: number): Rgb => c.map(v => Math.round(v * f)) as Rgb;
const blend = (a: Rgb, b: Rgb, f: number): Rgb => a.map((v, i) => Math.round(v + (b[i] - v) * f)) as Rgb;
const WHITE: Rgb = [255, 255, 255];
const BLACK: Rgb = [0, 0, 0];
const NEUTRAL: Rgb = [28, 28, 30];
/** Black or white by WCAG luminance, crossing over at 0.179 as the module does. */
const contrast = (c: Rgb): Rgb => {
  const lin = (v: number) => (v / 255 <= 0.04045 ? v / 255 / 12.92 : ((v / 255 + 0.055) / 1.055) ** 2.4);
  return 0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2]) > 0.179 ? BLACK : WHITE;
};
/** An idle pad is all track: the cue colour at 60% brightness. */
const TRACK = 0.6;

// "Stop All" as the module labels it in each language (it copies LivePlay's
// own wording); GO, PANIC and Master stay in English there too.
const STOP_ALL: Record<string, string> = {
  en: 'Stop All', ar: 'إيقاف الكل', bn: 'সব বন্ধ করুন', de: 'Alles stoppen', el: 'Διακοπή όλων',
  es: 'Detener todo', fa: 'توقف همه', fr: 'Tout arrêter', hi: 'सभी रोकें', it: 'Ferma tutto',
  ja: 'すべて停止', ko: '모두 중지', no: 'Stopp alle', pt: 'Parar tudo', ro: 'Oprire tot',
  ru: 'Остановить всё', sq: 'Ndalo të gjitha', sv: 'Stoppa alla', tr: 'Tümünü durdur',
  ur: 'سب بند کریں', zh: '全部停止',
};

const formatTime = (s: number) => {
  const total = Math.max(0, Math.floor(s));
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`;
};

interface Key { label: string; track: string; fill?: string; ink: string; progress?: number }

const plain = (label: string, bg: Rgb, ink: Rgb = WHITE): Key => ({ label, track: css(bg), ink: css(ink) });
/** A cue key: a full-key gauge, dimmed while it waits and filling as it plays. */
const gauge = (label: string, color: Rgb, progress = 0, playing = false): Key => ({
  label,
  track: css(dim(color, TRACK)),
  fill: css(color),
  ink: css(contrast(playing ? color : dim(color, TRACK))),
  progress,
});

// ---- The demo show, mid Act 1 ------------------------------------------------
const NOW = { name: "Prospero's Theme", color: hex('#8a3ffc'), length: 88 };
const NEXT = { name: 'Ariel Enters - Shimmer', color: hex('#1192e8') };
const CARTS = [
  { name: 'Thunder Crack', color: hex('#8a3ffc'), length: 9 },
  { name: 'Door Slam', color: hex('#f26b1d') },
  { name: 'Doorbell', color: hex('#d9a400') },
  { name: 'Applause', color: hex('#7cb305') },
  { name: 'Clock Chime', color: hex('#f26b1d') },
];

// The playing cue runs its last minute and then starts over; the first cart is
// pressed every 15 seconds. Both start partway in, which is also the still
// frame shown without JavaScript or with reduced motion.
const NOW_LOOP = 60;
const CART_LOOP = 15;
const clock = ref(0);

/** The end-of-cue warning: yellow, orange, red at 30, 10 and 5 s, each on its own beat. */
const warn = (remaining: number): [Rgb, number] | null => {
  if (remaining <= 5) return [[244, 67, 54], 500];
  if (remaining <= 10) return [[255, 152, 0], 1000];
  if (remaining <= 30) return [[255, 193, 7], 2000];
  return null;
};

const keys = computed<Key[]>(() => {
  const now = clock.value;

  const remaining = NOW_LOOP - ((now + 18) % NOW_LOOP);
  const w = warn(remaining);
  let fill = NOW.color;
  if (w) {
    const pos = ((now * 1000) % w[1]) / w[1];
    fill = blend(NOW.color, w[0], pos < 0.5 ? pos * 2 : (1 - pos) * 2);
  }
  const nowPlaying = gauge(`${NOW.name}\n-${formatTime(remaining)}`, fill, (NOW.length - remaining) / NOW.length, true);

  const cartPos = (now + 3.5) % CART_LOOP;
  const cartPlaying = cartPos < CARTS[0].length!;

  return [
    gauge(`GO\n${NEXT.name}`, NEXT.color),
    nowPlaying,
    plain((STOP_ALL[locale.value] ?? STOP_ALL.en).toUpperCase(), [102, 0, 0]),
    plain('PANIC', [204, 0, 0]),
    plain('LivePlay\nThe Tempest - Evening Performance', [0, 102, 0]),
    ...CARTS.map((c, i) => (i === 0 && cartPlaying
      ? gauge(c.name, c.color, cartPos / c.length!, true)
      : gauge(c.name, c.color))),
    plain('Music\n-2.0 dB', NEUTRAL),
    plain('Ambience\n-4.0 dB', [204, 0, 0]),
    plain('PFL\nAnnounce', [255, 193, 7], BLACK),
    plain('Master\n-1 dB', NEUTRAL),
    plain('Master\n+1 dB', NEUTRAL),
  ];
});

// ---- Companion's auto text size ------------------------------------------------
// Companion tries a ladder of sizes (in px on a 72 px key) and keeps the largest
// at which the wrapped label fits. Words stay whole unless nothing else fits.
// Widths come from the browser's own font metrics once mounted; the prerendered
// page uses a deliberately generous estimate instead.
const LADDER = [44, 30, 24, 18, 14, 12, 10, 8, 7].map(px => px / 72);
const FONT = "700 100px Arimo, Arial, 'Helvetica Neue', 'Noto Sans', sans-serif";
const AREA = 0.9;
const LINE = 1.12;
const metrics = ref<CanvasRenderingContext2D | null>(null);
const estimate = (s: string) => [...s].reduce((sum, ch) => {
  if (/[⺀-鿿가-힯＀-￯]/.test(ch)) return sum + 1;
  if (ch === ' ') return sum + 0.28;
  if (/[A-Z]/.test(ch)) return sum + 0.76;
  if (/[il.,:;'|!-]/.test(ch)) return sum + 0.31;
  if (/[mw]/.test(ch)) return sum + 0.9;
  return sum + 0.62;
}, 0);
/** Width of `s` in ems. */
const width = (s: string) => (metrics.value ? metrics.value.measureText(s).width / 100 : estimate(s));
/** Lines a label needs at a size, or Infinity if a word would have to break. */
const lineCount = (line: string, max: number, breakWords: boolean) => {
  const space = width(' ');
  let lines = 1;
  let used = 0;
  for (const word of line.split(' ')) {
    const w = width(word);
    if (w > max) {
      if (!breakWords) return Infinity;
      lines += Math.ceil((used + w) / max) - 1;
      used = (used + w) % max;
      continue;
    }
    const next = used ? used + space + w : w;
    if (next > max) { lines += 1; used = w; } else used = next;
  }
  return lines;
};
const fits = (label: string, size: number, breakWords: boolean) => {
  const lines = label.split('\n').reduce((sum, line) => sum + lineCount(line, AREA / size, breakWords), 0);
  return lines * size * LINE <= AREA;
};
const sizes = new Map<string, number>();
const fitSize = (label: string) => {
  const key = `${metrics.value ? 'm' : 'e'}${label}`;
  let fit = sizes.get(key);
  if (fit === undefined) {
    fit = LADDER.find(size => fits(label, size, false))
      ?? LADDER.find(size => fits(label, size, true))
      ?? LADDER.at(-1)!;
    sizes.set(key, fit);
  }
  return fit;
};

// ---- Animation: only while on screen, never with reduced motion -----------------
const root = ref<HTMLElement | null>(null);
let frame = 0;
let observer: IntersectionObserver | undefined;
onMounted(() => {
  const ctx = document.createElement('canvas').getContext('2d');
  if (ctx) {
    ctx.font = FONT;
    metrics.value = ctx;
  }
  if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;
  const start = performance.now();
  const tick = (time: number) => {
    // About 30 updates a second is plenty for a countdown and a 2 Hz pulse.
    const seconds = (time - start) / 1000;
    if (seconds - clock.value >= 0.033 || seconds < clock.value) clock.value = seconds;
    frame = requestAnimationFrame(tick);
  };
  observer = new IntersectionObserver(([entry]) => {
    cancelAnimationFrame(frame);
    if (entry.isIntersecting) frame = requestAnimationFrame(tick);
  });
  if (root.value) observer.observe(root.value);
});
onBeforeUnmount(() => {
  cancelAnimationFrame(frame);
  observer?.disconnect();
});
</script>
