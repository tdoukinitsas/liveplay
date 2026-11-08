// =====================================================================
// volumeAutomation.ts
// ---------------------------------------------------------------------
// A cue's volume automation (2.6): keyframes of (source-file seconds, dB)
// applied on top of the item's own volume. The rules mirror the engine's
// (server/include/liveplay/audio/gain_envelope.hpp), so the waveform the
// client draws is the level the server plays:
//
//   * `t` is source-file time, not time since the in-point.
//   * Between keyframes the level moves linearly in dB.
//   * Before the first / after the last keyframe it holds that keyframe's
//     level. No keyframes = 0 dB everywhere.
//   * ENVELOPE_MIN_DB (-60) and below is silence; ramps to it interpolate
//     against -60, so only the keyframe itself is exactly silent.
//   * ENVELOPE_MAX_DB (+10) is the top, the same as the volume slider.
//
// Everything here is pure: the editing helpers return new arrays, which is
// what the item diff wants (it PATCHes whole fields).
// =====================================================================
import type { AudioItem, VolumeAutomationPoint } from '~/types/project';

export const ENVELOPE_MIN_DB = -60;
export const ENVELOPE_MAX_DB = 10;
/** The default ramp a range edit puts inside each edge of the selection. */
export const RANGE_RAMP_SECONDS = 0.5;

export const clampEnvelopeDb = (db: number) =>
  Math.max(ENVELOPE_MIN_DB, Math.min(ENVELOPE_MAX_DB, Number.isFinite(db) ? db : ENVELOPE_MIN_DB));

export const sortPoints = (points: VolumeAutomationPoint[]) =>
  [...points].sort((a, b) => a.t - b.t);

/** Whether the item's automation is switched on and has anything to say. */
export function automationActive(item: Pick<AudioItem, 'volumeAutomation'> | null | undefined): boolean {
  const va = item?.volumeAutomation;
  return !!va && va.enabled && Array.isArray(va.points) && va.points.length > 0;
}

/** Level at `t` (seconds, source-file time) in dB. `points` must be sorted. */
export function envelopeDbAt(points: VolumeAutomationPoint[], t: number): number {
  const n = points.length;
  if (n === 0) return 0;
  if (t <= points[0]!.t) return points[0]!.db;
  if (t >= points[n - 1]!.t) return points[n - 1]!.db;
  // Binary search for the segment [a, b] containing t.
  let lo = 0, hi = n - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (points[mid]!.t <= t) lo = mid; else hi = mid;
  }
  const a = points[lo]!, b = points[hi]!;
  const span = b.t - a.t;
  if (span <= 0) return b.db;
  return a.db + (b.db - a.db) * ((t - a.t) / span);
}

export const dbToGain = (db: number) => (db <= ENVELOPE_MIN_DB ? 0 : Math.pow(10, db / 20));

export const envelopeGainAt = (points: VolumeAutomationPoint[], t: number) =>
  dbToGain(envelopeDbAt(points, t));

type FadeFields = Pick<AudioItem,
  'inPoint' | 'outPoint' | 'duration' | 'playFade' | 'stopFade' | 'crossFade' |
  'startNextEnabled' | 'startNextTime' | 'startNextFadeOut' | 'fadeOutDuration' |
  'volumeAutomation'>;

/**
 * Everything between the file and the item's fader, as one gain curve:
 * the trim (silent outside in/out), the play fade, the fade-outs that end at
 * the out-point (stop fade, crossfade, a Start Next fade-out) and the
 * automation envelope. The item's volume is left to the caller — the Playback
 * tab multiplies it in, the compact row waveforms keep their own scale.
 *
 * Fades are linear in gain, as the engine runs them. Overlapping fade-outs
 * take the lowest, since whichever starts first is the one you hear.
 */
export function makeCompositeGain(item: FadeFields): (t: number) => number {
  const inPoint = item.inPoint || 0;
  const outPoint = item.outPoint || item.duration || 0;
  const playFade = Math.max(0, item.playFade || 0);
  const stopFade = Math.max(0, item.stopFade || 0);
  const crossFade = Math.max(0, item.crossFade || 0);
  const snFadeStart = item.startNextEnabled && item.startNextFadeOut && (item.startNextTime ?? 0) > 0
    ? item.startNextTime! : null;
  const snFadeLen = Math.max(0.001, item.fadeOutDuration || 1);
  const points = automationActive(item) ? sortPoints(item.volumeAutomation!.points) : null;

  return (t: number) => {
    if (t < inPoint || t > outPoint) return 0;
    let g = 1;
    if (playFade > 0 && t < inPoint + playFade) g *= (t - inPoint) / playFade;
    let out = 1;
    if (stopFade > 0 && t > outPoint - stopFade) out = Math.min(out, (outPoint - t) / stopFade);
    if (crossFade > 0 && t > outPoint - crossFade) out = Math.min(out, (outPoint - t) / crossFade);
    if (snFadeStart !== null && t > snFadeStart) out = Math.min(out, Math.max(0, 1 - (t - snFadeStart) / snFadeLen));
    g *= Math.max(0, out);
    if (points) g *= envelopeGainAt(points, t);
    return g;
  };
}

// ---- Editing ----------------------------------------------------------

/** Add (or replace, within `epsilon` s) a keyframe. Returns the new array. */
export function addPoint(points: VolumeAutomationPoint[], t: number, db: number, epsilon = 0.0005): VolumeAutomationPoint[] {
  const p = { t: Math.max(0, t), db: clampEnvelopeDb(db) };
  const rest = points.filter(q => Math.abs(q.t - p.t) > epsilon);
  return sortPoints([...rest, p]);
}

/**
 * A level change over a selection [a, b] (#automation range edit): the level
 * just outside the selection stays put; inside, every keyframe moves by
 * `deltaDb`, with ramps of up to RANGE_RAMP_SECONDS inside each edge.
 *
 * Works from the envelope as it was when the drag began (`base`), so a drag
 * can be re-applied with a growing delta on every mouse move.
 */
export function applyRangeDelta(
  base: VolumeAutomationPoint[], a: number, b: number, deltaDb: number,
  ramp = RANGE_RAMP_SECONDS,
): VolumeAutomationPoint[] {
  if (b < a) [a, b] = [b, a];
  const len = b - a;
  if (len <= 0) return base;
  const r = Math.min(ramp, len / 2);
  const sorted = sortPoints(base);
  const at = (t: number) => envelopeDbAt(sorted, t);
  const shift = (db: number) => db <= ENVELOPE_MIN_DB && deltaDb <= 0
    ? ENVELOPE_MIN_DB : clampEnvelopeDb(db + deltaDb);

  // Anchors at the edges keep the level outside exactly as it was.
  const out: VolumeAutomationPoint[] = sorted.filter(p => p.t < a || p.t > b);
  out.push({ t: a, db: at(a) }, { t: b, db: at(b) });
  // Inner corners, at the end of each ramp, and every keyframe already inside.
  const inner = new Map<number, number>();
  inner.set(a + r, at(a + r));
  inner.set(b - r, at(b - r));
  for (const p of sorted) if (p.t > a + r && p.t < b - r) inner.set(p.t, p.db);
  // Keyframes inside the ramps would bend them; the ramp replaces them.
  for (const [t, db] of inner) out.push({ t, db: shift(db) });
  return dedupe(sortPoints(out));
}

/** Move keyframes `indices` (in a sorted array) by `deltaDb`, from `base`. */
export function shiftPoints(base: VolumeAutomationPoint[], indices: number[], deltaDb: number): VolumeAutomationPoint[] {
  const set = new Set(indices);
  return base.map((p, i) => set.has(i) ? { t: p.t, db: clampEnvelopeDb(p.db + deltaDb) } : p);
}

function dedupe(points: VolumeAutomationPoint[], epsilon = 0.0005): VolumeAutomationPoint[] {
  const out: VolumeAutomationPoint[] = [];
  for (const p of points) {
    const last = out[out.length - 1];
    if (last && Math.abs(last.t - p.t) <= epsilon) out[out.length - 1] = p;
    else out.push(p);
  }
  return out;
}

/** Round for storage: millisecond times, 0.1 dB levels. Keeps the document tidy. */
export const tidyPoints = (points: VolumeAutomationPoint[]): VolumeAutomationPoint[] =>
  dedupe(sortPoints(points.map(p => ({
    t: Math.round(Math.max(0, p.t) * 1000) / 1000,
    db: Math.round(clampEnvelopeDb(p.db) * 10) / 10,
  }))));
