// =====================================================================
// window-bounds-tests.js  —  P4, Electron half
// ---------------------------------------------------------------------
// usage:  node client/tests/window-bounds-tests.js        (from the repo root)
//
// Covers the two pure functions behind window-geometry persistence in
// client/electron/main.js:
//
//   savedWindowBounds()  — what to open a window WITH, given what was stored
//   windowBoundsEntry()  — what to store, given what the window reports
//
// Both decide whether an operator can reach their own window, so both are worth
// assertions rather than a careful read. What they guard against:
//
//   * a position on a monitor that has since been unplugged — a title bar at
//     x=2400 with one display left cannot be dragged back into view;
//   * a size saved on a 4K panel opening taller than a laptop's work area;
//   * the `useContentSize` trap. The main and mixer windows are created with it,
//     so their width/height mean the CONTENT box while getBounds() reports the
//     OUTER frame. Store one and feed back the other and the window grows by a
//     frame every launch.
//
// HOW THIS RUNS AGAINST THE REAL CODE: main.js cannot be require()d — it pulls
// in electron and starts an app — so each function's source is lifted out of the
// file verbatim and evaluated with `screen` and the store reader injected. That
// means these assertions are about the shipped text, not a copy of it, which a
// copy could never be: it would keep passing while main.js was broken.
//
// The lifting is by name. Rename either function and lift() throws with the name
// it could not find, which is the right way for this to fail — loudly, rather
// than by quietly testing nothing.
// =====================================================================
const fs = require('fs');
const path = require('path');

const MAIN_JS = path.join(__dirname, '..', 'electron', 'main.js');
const src = fs.readFileSync(MAIN_JS, 'utf8');

function lift(name) {
  const start = src.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`${name}() not found in electron/main.js`);
  let depth = 0, i = src.indexOf('{', start);
  for (; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}') { depth--; if (depth === 0) break; }
  }
  if (depth !== 0) throw new Error(`${name}(): unbalanced braces`);
  return src.slice(start, i + 1);
}

function liftConst(name) {
  const m = src.match(new RegExp(`const ${name} = [^;]+;`));
  if (!m) throw new Error(`${name} not found in electron/main.js`);
  return m[0];
}

const SRC_SAVED = lift('savedWindowBounds');
const SRC_ENTRY = lift('windowBoundsEntry');
const SRC_ISPOS = liftConst('isPositiveInt');

// One display: 1920x1080 less a taskbar. Two: the second to the right, larger.
const ONE = [{ workArea: { x: 0, y: 0, width: 1920, height: 1040 } }];
const TWO = [
  { workArea: { x: 0, y: 0, width: 1920, height: 1040 } },
  { workArea: { x: 1920, y: 0, width: 2560, height: 1400 } },
];

function savedWindowBoundsWith(displays, stored) {
  const screen = {
    getPrimaryDisplay: () => displays[0],
    // Electron's returns the display overlapping the rect most, and the CLOSEST
    // display when none overlaps — which is why savedWindowBounds cannot trust
    // it alone and tests for overlap itself.
    getDisplayMatching: (rect) => {
      let best = displays[0], bestArea = -1;
      for (const d of displays) {
        const a = d.workArea;
        const w = Math.max(0, Math.min(rect.x + rect.width,  a.x + a.width)  - Math.max(rect.x, a.x));
        const h = Math.max(0, Math.min(rect.y + rect.height, a.y + a.height) - Math.max(rect.y, a.y));
        if (w * h > bestArea) { bestArea = w * h; best = d; }
      }
      return best;
    },
  };
  const readWindowBoundsStore = () => (stored === undefined ? {} : { win: stored });
  return new Function('screen', 'readWindowBoundsStore',
    `${SRC_ISPOS}\n${SRC_SAVED}\nreturn savedWindowBounds;`)(screen, readWindowBoundsStore);
}

const windowBoundsEntry = new Function(`${SRC_ENTRY}\nreturn windowBoundsEntry;`)();

let failures = 0;
const ok = (name, pass, detail) => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}${detail ? '   ' + detail : ''}`);
  if (!pass) failures++;
};

const DEF = { width: 1400, height: 900 };

// =====================================================================
// savedWindowBounds — what a window opens with
// =====================================================================
let r = savedWindowBoundsWith(ONE, undefined)('win', DEF);
ok('no stored bounds gives the defaults, untouched',
   r.width === 1400 && r.height === 900 && r.x === undefined && r.y === undefined,
   JSON.stringify(r));

r = savedWindowBoundsWith(ONE, { x: 120, y: 60, width: 1000, height: 700 })('win', DEF);
ok('an on-screen entry is restored whole',
   r.x === 120 && r.y === 60 && r.width === 1000 && r.height === 700, JSON.stringify(r));

r = savedWindowBoundsWith(ONE, { x: 2400, y: 100, width: 1000, height: 700 })('win', DEF);
ok('a position on a display that no longer exists is DROPPED',
   r.x === undefined && r.y === undefined,
   `${JSON.stringify(r)} — an unreachable title bar cannot be dragged back`);
ok('...while its SIZE is kept',
   r.width === 1000 && r.height === 700, JSON.stringify(r));

r = savedWindowBoundsWith(ONE, { x: 200, y: -900, width: 1000, height: 700 })('win', DEF);
ok('a position above the top of the screen is dropped too',
   r.x === undefined && r.y === undefined, JSON.stringify(r));

r = savedWindowBoundsWith(ONE, { x: 1850, y: 900, width: 1000, height: 700 })('win', DEF);
ok('a window hanging off the edge but still touching is KEPT',
   r.x === 1850 && r.y === 900,
   `${JSON.stringify(r)} — partly off screen is a layout, not a fault`);

r = savedWindowBoundsWith(TWO, { x: 2400, y: 100, width: 1000, height: 700 })('win', DEF);
ok('the same position IS kept when that display is still attached',
   r.x === 2400 && r.y === 100,
   `${JSON.stringify(r)} — the drop above must be about the display, not the number`);

r = savedWindowBoundsWith(ONE, { x: 0, y: 0, width: 3400, height: 1800 })('win', DEF);
ok('a size larger than the work area is clamped to it',
   r.width === 1920 && r.height === 1040,
   `${JSON.stringify(r)} — a 4K layout must not open taller than a laptop`);

for (const [label, bad] of [
  ['width missing',  { x: 0, y: 0, height: 700 }],
  ['width NaN',      { x: 0, y: 0, width: NaN, height: 700 }],
  ['width negative', { x: 0, y: 0, width: -1000, height: 700 }],
  ['width a string', { x: 0, y: 0, width: '1000', height: 700 }],
  ['height zero',    { x: 0, y: 0, width: 1000, height: 0 }],
]) {
  r = savedWindowBoundsWith(ONE, bad)('win', DEF);
  ok(`a malformed size is rejected WHOLE (${label})`,
     r.width === 1400 && r.height === 900 && r.x === undefined,
     `${JSON.stringify(r)} — "width: NaNpx" is a dead rule, not a small window`);
}

r = savedWindowBoundsWith(ONE, { x: 12.5, y: 60, width: 1000, height: 700 })('win', DEF);
ok('a non-integer position is dropped rather than passed on',
   r.x === undefined && r.y === undefined,
   `${JSON.stringify(r)} — size still kept: ${r.width}x${r.height}`);

r = savedWindowBoundsWith(ONE, { x: 0, y: 0, width: 1000, height: 700, maximized: true })('win', DEF);
ok('maximized is carried through for the caller to apply', r.maximized === true, JSON.stringify(r));
r = savedWindowBoundsWith(ONE, { x: 0, y: 0, width: 1000, height: 700 })('win', DEF);
ok('...absent otherwise, so nothing maximizes by accident', !('maximized' in r), JSON.stringify(r));
r = savedWindowBoundsWith(ONE, { x: 0, y: 0, width: 1000, height: 700, maximized: 'yes' })('win', DEF);
ok('...and only a real `true` counts', !('maximized' in r), JSON.stringify(r));

const shared = { width: 1400, height: 900 };
savedWindowBoundsWith(ONE, { x: 0, y: 0, width: 1000, height: 700 })('win', shared);
ok('the caller\'s defaults object is not written through',
   shared.width === 1400 && shared.height === 900 && !('x' in shared), JSON.stringify(shared));

// =====================================================================
// windowBoundsEntry — what gets stored
// =====================================================================
// A window at 1000x700 content inside a 1015x764 frame: the ~15x64 Windows adds.
const OUTER   = { x: 100, y: 50, width: 1015, height: 764 };
const CONTENT = { x: 100, y: 50, width: 1000, height: 700 };

let e = windowBoundsEntry(undefined, {
  maximized: false, normalBounds: OUTER, bounds: OUTER, contentBounds: CONTENT,
  contentSize: true,
});
ok('a content-sized window stores its CONTENT size',
   e.width === 1000 && e.height === 700,
   `${JSON.stringify(e)} — storing 1015x764 and feeding it back as a content ` +
   `size is how a window grows by a frame every launch`);
ok('...and its OUTER position, which is what the constructor takes',
   e.x === 100 && e.y === 50, JSON.stringify(e));

e = windowBoundsEntry(undefined, {
  maximized: false, normalBounds: OUTER, bounds: OUTER, contentBounds: CONTENT,
  contentSize: false,
});
ok('a normally-sized window stores its OUTER size',
   e.width === 1015 && e.height === 764, JSON.stringify(e));

// The maximized case, which is the reason this function exists separately.
const NORMAL = { x: 200, y: 120, width: 1015, height: 764 };
const MAXED  = { x: 0, y: 0, width: 1920, height: 1040 };
e = windowBoundsEntry({ x: 200, y: 120, width: 1000, height: 700, maximized: false }, {
  maximized: true, normalBounds: NORMAL, bounds: MAXED, contentBounds: MAXED,
  contentSize: true,
});
ok('maximizing records the flag',
   e.maximized === true, JSON.stringify(e));
ok('...and the position it would RESTORE to, not the maximized one',
   e.x === 200 && e.y === 120,
   `${JSON.stringify(e)} — the maximized rect is 0,0 and storing it would move ` +
   `the window on un-maximize`);
ok('...and KEEPS the previously stored size rather than the maximized rect',
   e.width === 1000 && e.height === 700,
   `${JSON.stringify(e)} — 1920x1040 stored as a normal size gives a window ` +
   `that fills the screen without being maximized, so un-maximizing does nothing`);

e = windowBoundsEntry(undefined, {
  maximized: true, normalBounds: NORMAL, bounds: MAXED, contentBounds: MAXED,
  contentSize: true,
});
ok('maximized with nothing stored yet omits the size entirely',
   e.width === undefined && e.height === undefined && e.maximized === true,
   `${JSON.stringify(e)} — getNormalBounds() is an OUTER rect and there is no ` +
   `getNormalContentBounds(), so the caller's defaults apply underneath instead`);

e = windowBoundsEntry({ x: 1, y: 1, width: 'wide', height: 700 }, {
  maximized: true, normalBounds: NORMAL, bounds: MAXED, contentBounds: MAXED,
  contentSize: true,
});
ok('...and a malformed previous size is not carried forward',
   e.width === undefined && e.height === undefined, JSON.stringify(e));

e = windowBoundsEntry({ x: 9, y: 9, width: 800, height: 600, maximized: true }, {
  maximized: false, normalBounds: NORMAL, bounds: OUTER, contentBounds: CONTENT,
  contentSize: true,
});
ok('un-maximizing clears the flag and records the live size',
   e.maximized === false && e.width === 1000 && e.height === 700,
   `${JSON.stringify(e)} — a stale flag would re-maximize on every launch`);

console.log(failures === 0
  ? '\nAll window-bounds assertions passed.'
  : `\n${failures} assertion(s) FAILED.`);
process.exit(failures === 0 ? 0 : 1);
