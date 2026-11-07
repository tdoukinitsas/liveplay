// =====================================================================
// accent-palette-tests.mjs  —  theme accent swatches (2.6)
// ---------------------------------------------------------------------
// usage:  node client/tests/accent-palette-tests.mjs      (from the repo root)
//
// Guards the promises made by client/app/utils/accentPalette.ts:
//
//   * every swatch, as drawn, clears 3:1 against both grounds of both themes;
//   * text on an accent fill clears 4.5:1;
//   * the six swatches that shipped first are still there and drawn unchanged
//     wherever they already passed, so upgrading does not repaint anyone;
//   * every retired 2.5 swatch maps to the surviving swatch of its hue;
//   * the stylesheet grounds the util measures against are the real ones.
//
// The util is TypeScript in a package that is not "type": "module", so Node
// would load it as CommonJS. It is copied verbatim to a temp .mts and imported
// from there — the assertions run against the shipped text, not a copy.
// =====================================================================
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const src = path.join(here, '..', 'app', 'utils', 'accentPalette.ts');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'accent-'));
const mts = path.join(tmp, 'accentPalette.mts');
fs.copyFileSync(src, mts);
const A = await import(pathToFileURL(mts).href);

let failures = 0, passes = 0;
const check = (ok, msg) => { if (ok) passes++; else { failures++; console.error('FAIL', msg); } };

// ---- palette shape ---------------------------------------------------
check(A.ACCENT_COLORS.length === 24, `24 swatches (got ${A.ACCENT_COLORS.length})`);
check(new Set(A.ACCENT_COLORS).size === A.ACCENT_COLORS.length, 'swatches are unique');
for (const c of A.ACCENT_COLORS) check(/^#[0-9a-f]{6}$/.test(c), `${c} is lower-case #rrggbb`);

const KEPT = ['#da1e28', '#f1c21b', '#24a148', '#0f62fe', '#8a3ffc', '#ff7eb6'];
for (const c of KEPT) check(A.ACCENT_COLORS.includes(c), `original swatch ${c} kept`);
check(A.ACCENT_COLORS.includes(A.DEFAULT_ACCENT), 'default accent is a swatch');

// ---- contrast --------------------------------------------------------
for (const mode of ['dark', 'light']) {
  for (const c of A.ACCENT_COLORS) {
    const t = A.accentForTheme(c, mode);
    for (const g of A.THEME_GROUNDS[mode]) {
      const r = A.contrast(t.accent, g);
      check(r >= 3, `${c} in ${mode}: ${t.accent} vs ${g} = ${r.toFixed(2)} (< 3)`);
    }
    const ot = A.contrast(t.accent, t.onAccent);
    check(ot >= 4.5, `${c} in ${mode}: text ${t.onAccent} on ${t.accent} = ${ot.toFixed(2)} (< 4.5)`);
    const oh = A.contrast(t.hover, t.onAccent);
    check(oh >= 4.5, `${c} in ${mode}: text ${t.onAccent} on hover ${t.hover} = ${oh.toFixed(2)} (< 4.5)`);
  }
}

// A colour that already passes is drawn exactly as stored. All six originals
// pass on dark; on light the red, blue and purple pass as they are, the green
// (2.9:1 on #f3f3f3) moves a hair, and the yellow and pink are deepened.
for (const c of KEPT) {
  check(A.accentForTheme(c, 'dark').accent === c, `${c} unchanged in dark`);
}
for (const c of ['#da1e28', '#0f62fe', '#8a3ffc']) {
  check(A.accentForTheme(c, 'light').accent === c, `${c} unchanged in light`);
}
// Minimum move: a near-pass barely changes (each channel within 4%).
{
  const a = A.parseHex('#24a148'), b = A.parseHex(A.accentForTheme('#24a148', 'light').accent);
  check(a.every((v, i) => Math.abs(v - b[i]) <= 0.04), '#24a148 nudged, not repainted, in light');
}

// ---- migration -------------------------------------------------------
const RETIRED = {
  '#0353e9': '#0f62fe', '#002d9c': '#0f62fe',
  '#a2191f': '#da1e28', '#750e13': '#da1e28',
  '#198038': '#24a148', '#0e6027': '#24a148',
  '#d2a106': '#f1c21b', '#b28600': '#f1c21b',
  '#6929c4': '#8a3ffc', '#491d8b': '#8a3ffc',
  '#ee5396': '#ff7eb6', '#d02670': '#ff7eb6',
};
for (const [from, to] of Object.entries(RETIRED)) {
  check(A.normalizeAccent(from) === to, `${from} → ${to}`);
  check(A.normalizeAccent(from.toUpperCase()) === to, `${from.toUpperCase()} → ${to} (case)`);
}
check(A.normalizeAccent('#DA1E28') === '#da1e28', 'default as stored by DEFAULT_THEME');
check(A.normalizeAccent('#abc') === '#aabbcc', '#rgb expands');
check(A.normalizeAccent('#123456') === '#123456', 'unknown valid colour kept');
for (const bad of [null, undefined, '', 'red', '#12345', 'url(x)']) {
  check(A.normalizeAccent(bad) === A.DEFAULT_ACCENT, `malformed ${JSON.stringify(bad)} → default`);
}
// A custom colour still gets drawn legibly.
for (const mode of ['dark', 'light']) {
  const t = A.accentForTheme('#123456', mode);
  for (const g of A.THEME_GROUNDS[mode]) check(A.contrast(t.accent, g) >= 3, `custom #123456 legible in ${mode}`);
}

// ---- grounds match the stylesheet ------------------------------------
const scss = fs.readFileSync(path.join(here, '..', 'assets', 'styles', 'main.scss'), 'utf8');
const block = (sel) => {
  const i = scss.indexOf(`[data-theme='${sel}'] {`);
  return scss.slice(i, scss.indexOf('}', i));
};
const varOf = (b, name) => (new RegExp(`--color-${name}:\\s*(#[0-9a-fA-F]{6})`).exec(b) || [])[1]?.toLowerCase();
for (const mode of ['dark', 'light']) {
  const b = block(mode);
  const grounds = [varOf(b, 'surface'), varOf(b, 'background')];
  check(JSON.stringify(grounds) === JSON.stringify([...A.THEME_GROUNDS[mode]]),
    `THEME_GROUNDS.${mode} ${JSON.stringify(A.THEME_GROUNDS[mode])} matches main.scss ${JSON.stringify(grounds)}`);
}

fs.rmSync(tmp, { recursive: true, force: true });
console.log(`${passes} passed, ${failures} failed`);
process.exit(failures ? 1 : 0);
