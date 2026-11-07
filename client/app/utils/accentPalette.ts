// =====================================================================
// accentPalette.ts — the accent colour swatches, and how an accent is drawn
// ---------------------------------------------------------------------
// Pure (no Nuxt, no DOM) so client/tests/accent-palette-tests.mjs can import it
// as it ships.
//
// WHAT IS STORED vs WHAT IS DRAWN. The preference holds one of ACCENT_COLORS
// (or anything an older build wrote). What reaches the stylesheet is
// accentForTheme(stored, mode): the same hue, moved just far enough in
// lightness to read against that theme's surfaces. No single vivid colour can
// do that on both a #161616 and a #f3f3f3 ground — 3:1 against both needs a
// relative luminance between ~0.16 and ~0.26, which rules out every yellow,
// lime, orange and pastel — so a neon lime is drawn as itself in dark mode and
// as a deep olive-lime in light mode. The mid-tone swatches (the blue, red
// and purple that shipped first among them) already sit in that band and are
// drawn unchanged in both; the original green is a hair short on #f3f3f3 and
// moves by a couple of percent.
//
// The bar is WCAG 1.4.11 non-text contrast, 3:1: the accent is a border, a
// fill, a focus ring, a playing-row marker and a label colour, and 4.5:1 for
// body text against both grounds is impossible for any colour at all.
//
// TEXT ON AN ACCENT FILL uses onAccent — white, or near-black when the fill
// is light (a yellow button in dark mode). Hard-coded white on a yellow fill
// was 1.7:1.
// =====================================================================

/** 24 swatches in hue order, six to a row in the picker. The six that shipped
 *  first and read best are kept exactly so an existing preference is
 *  untouched: #da1e28, #f1c21b, #24a148, #0f62fe, #8a3ffc, #ff7eb6. */
export const ACCENT_COLORS: readonly string[] = [
  // red, coral, vermilion, orange, amber, yellow
  '#da1e28', '#ff9a8a', '#ff5022', '#ff7a12', '#ffa000', '#f1c21b',
  // lemon-lime, neon lime, green, emerald, teal, cyan
  '#d4ea15', '#9cf01c', '#24a148', '#00c98a', '#00d2c0', '#08b8e6',
  // sky, periwinkle, blue, indigo, purple, lavender
  '#4aa8ff', '#8fb4ff', '#0f62fe', '#6b4dff', '#8a3ffc', '#c4a2ff',
  // violet, magenta, fuchsia, pink, hot pink, raspberry
  '#a73af2', '#dc3cf0', '#ff3dcc', '#ff7eb6', '#ff4f95', '#f2266a',
];

export const DEFAULT_ACCENT = '#da1e28';

/** The darker two columns of the 2.5 palette, each mapped to the brighter
 *  swatch of its hue that survived. Anything else is kept as written. */
const RETIRED_ACCENTS: Record<string, string> = {
  '#0353e9': '#0f62fe', '#002d9c': '#0f62fe',   // blues
  '#a2191f': '#da1e28', '#750e13': '#da1e28',   // reds
  '#198038': '#24a148', '#0e6027': '#24a148',   // greens
  '#d2a106': '#f1c21b', '#b28600': '#f1c21b',   // yellows
  '#6929c4': '#8a3ffc', '#491d8b': '#8a3ffc',   // purples
  '#ee5396': '#ff7eb6', '#d02670': '#ff7eb6',   // pinks
};

/** Grounds the accent must read against, worst case first: the accent sits on
 *  both --color-background and --color-surface, so it has to clear the one it
 *  contrasts with least. Mirrors assets/styles/main.scss. */
export const THEME_GROUNDS = {
  dark:  ['#262626', '#161616'],   // surface, background
  light: ['#f3f3f3', '#ebebeb'],   // surface, background
} as const;

export const MIN_CONTRAST = 3;
// shade() returns a rounded #rrggbb, so the bisection already tests what ships.
const TARGET_CONTRAST = MIN_CONTRAST;

const ON_ACCENT_LIGHT = '#ffffff';
const ON_ACCENT_DARK  = '#161616';

// ---- colour maths ----------------------------------------------------

type RGB = [number, number, number];   // 0..1, sRGB-encoded

export function parseHex(hex: string): RGB | null {
  const m = /^#?([0-9a-f]{3}|[0-9a-f]{6})$/i.exec((hex || '').trim());
  if (!m) return null;
  let h = m[1]!;
  if (h.length === 3) h = h.split('').map(c => c + c).join('');
  return [0, 2, 4].map(i => parseInt(h.slice(i, i + 2), 16) / 255) as RGB;
}

export function toHex(rgb: RGB): string {
  return '#' + rgb.map(c =>
    Math.round(Math.min(1, Math.max(0, c)) * 255).toString(16).padStart(2, '0')).join('');
}

const toLinear = (c: number) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
const toEncoded = (c: number) => (c <= 0.0031308 ? c * 12.92 : 1.055 * c ** (1 / 2.4) - 0.055);

export function luminance(hex: string): number {
  const rgb = parseHex(hex);
  if (!rgb) return 0;
  const [r, g, b] = rgb.map(toLinear) as RGB;
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrast(a: string, b: string): number {
  const la = luminance(a), lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

/** Mix toward black (t<0) or white (t>0) in linear light, which keeps the hue
 *  rather than drifting it the way an sRGB-space mix does. */
function shade(hex: string, t: number): string {
  const rgb = parseHex(hex)!;
  const target = t < 0 ? 0 : 1, k = Math.abs(t);
  return toHex(rgb.map(c => toEncoded(toLinear(c) + (target - toLinear(c)) * k)) as RGB);
}

const worstContrast = (hex: string, grounds: readonly string[]) =>
  Math.min(...grounds.map(g => contrast(hex, g)));

// ---- public ----------------------------------------------------------

/** What a stored preference means today: retired swatches move to their hue's
 *  survivor, malformed values fall back to the default, anything else valid
 *  (a hand-edited profile, a colour from a future build) is kept. Lower-case
 *  #rrggbb either way. */
export function normalizeAccent(stored: string | null | undefined): string {
  const rgb = parseHex(stored ?? '');
  if (!rgb) return DEFAULT_ACCENT;
  const hex = toHex(rgb);
  return RETIRED_ACCENTS[hex] ?? hex;
}

export interface ThemedAccent {
  accent: string;     // --color-accent: borders, rings, labels, fills
  hover: string;      // --color-accent-hover: a pressed/hovered fill
  onAccent: string;   // --color-on-accent: text and icons on an accent fill
}

/** The stored accent as it should be drawn on `mode`'s surfaces. */
export function accentForTheme(stored: string | null | undefined,
                               mode: 'light' | 'dark'): ThemedAccent {
  const base = normalizeAccent(stored);
  const grounds = THEME_GROUNDS[mode];
  let accent = base;
  if (worstContrast(base, grounds) < TARGET_CONTRAST) {
    // Light grounds: darken. Dark grounds: lighten. Bisect the smallest move
    // that clears the bar, so a colour that nearly passes barely changes.
    const dir = mode === 'light' ? -1 : 1;
    let lo = 0, hi = 1;
    for (let i = 0; i < 24; i++) {
      const mid = (lo + hi) / 2;
      if (worstContrast(shade(base, dir * mid), grounds) >= TARGET_CONTRAST) hi = mid;
      else lo = mid;
    }
    accent = shade(base, dir * hi);
  }
  const onAccent = contrast(accent, ON_ACCENT_LIGHT) >= contrast(accent, ON_ACCENT_DARK)
    ? ON_ACCENT_LIGHT : ON_ACCENT_DARK;
  // A hovered fill steps away from its text colour, so a hover never costs the
  // label contrast: darker under white text, lighter under dark text.
  const hover = shade(accent, onAccent === ON_ACCENT_LIGHT ? -0.18 : 0.18);
  return { accent, hover, onAccent };
}
