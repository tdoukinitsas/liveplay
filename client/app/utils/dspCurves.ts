// The curves a bus's processing draws — shared by the channel view's full-size
// panels and the mixer strip's mini views, so the two can never draw different
// shapes for the same settings.
//
// Display-only mirrors of the engine (see filterResponse.ts and dynamics.hpp):
// keep them in step with the C++ when either changes.
import type { BusDsp, BusEqBand, BusEqType, BusGate, BusComp } from '~/types/project';
import { HPF_PARKED_HZ, LPF_PARKED_HZ } from '~/types/project';
import type { BiquadCoeffs } from '~/utils/filterResponse';
import {
  biquadHighpass, biquadHighShelf, biquadLowpass, biquadLowShelf, biquadNotch, biquadPeaking,
  combinedMagnitudeDb,
} from '~/utils/filterResponse';

export const CURVE_SAMPLE_RATE = 48000;

// ---- EQ --------------------------------------------------------------------
/** A band's shape; a pre-2.5 server sends only `shelf`, which meant low on band 0. */
export function eqTypeOf(b: BusEqBand, index: number): BusEqType {
  if (b.type) return b.type;
  if (b.shelf) return index === 0 ? 'lowShelf' : 'highShelf';
  return 'bell';
}
export function eqUsesGain(b: BusEqBand, index: number): boolean {
  const ty = eqTypeOf(b, index);
  return ty === 'bell' || ty === 'lowShelf' || ty === 'highShelf';
}
/** Same rule as the engine: a flat bell/shelf is an identity; cuts and notches always work. */
export function eqBandActive(b: BusEqBand, index: number): boolean {
  if (b.on === false) return false;
  return eqUsesGain(b, index) ? b.gain !== 0 : true;
}

export function eqBandSection(b: BusEqBand, index: number, fs = CURVE_SAMPLE_RATE): BiquadCoeffs | null {
  if (!eqBandActive(b, index)) return null;
  switch (eqTypeOf(b, index)) {
    case 'lowShelf':  return biquadLowShelf(b.freq, fs, b.gain, b.slope);
    case 'highShelf': return biquadHighShelf(b.freq, fs, b.gain, b.slope);
    case 'lowCut':    return biquadHighpass(b.freq, fs, b.q);
    case 'highCut':   return biquadLowpass(b.freq, fs, b.q);
    case 'notch':     return biquadNotch(b.freq, fs, b.q);
    default:          return biquadPeaking(b.freq, fs, b.gain, b.q);
  }
}

/** Every section shaping the bus: the strip's HPF/LPF (when not parked) and the EQ (when in). */
export function eqSections(dsp: Partial<BusDsp> | undefined, fs = CURVE_SAMPLE_RATE): BiquadCoeffs[] {
  const out: BiquadCoeffs[] = [];
  if (!dsp) return out;
  if (dsp.hpf && dsp.hpf.freq > HPF_PARKED_HZ) out.push(biquadHighpass(dsp.hpf.freq, fs, dsp.hpf.q || 0.7071));
  if (dsp.lpf && dsp.lpf.freq < LPF_PARKED_HZ) out.push(biquadLowpass(dsp.lpf.freq, fs, dsp.lpf.q || 0.7071));
  if (dsp.eqEnabled !== false) {
    (dsp.eq ?? []).forEach((b, i) => { const s = eqBandSection(b, i, fs); if (s) out.push(s); });
  }
  return out;
}

/** Whether the EQ section is doing anything (for a lamp). Filters count: they shape the same signal. */
export function eqIsActive(dsp: Partial<BusDsp> | undefined): boolean {
  if (!dsp) return false;
  if (dsp.hpf && dsp.hpf.freq > HPF_PARKED_HZ) return true;
  if (dsp.lpf && dsp.lpf.freq < LPF_PARKED_HZ) return true;
  return dsp.eqEnabled !== false && (dsp.eq ?? []).some((b, i) => eqBandActive(b, i));
}

const LOG_LO = Math.log10(20);
const LOG_HI = Math.log10(20000);

/**
 * SVG polyline points for a response, in a w x h box, over +/-rangeDb.
 * Log frequency axis 20 Hz..20 kHz, as every EQ display draws it.
 */
export function eqCurvePoints(secs: BiquadCoeffs[], w: number, h: number, rangeDb: number,
                              points = 81, fs = CURVE_SAMPLE_RATE): string {
  const yFor = (db: number) => h / 2 - (Math.max(-rangeDb, Math.min(rangeDb, db)) / rangeDb) * (h / 2);
  return Array.from({ length: points }, (_, k) => {
    const x  = (k / (points - 1)) * w;
    const hz = Math.pow(10, LOG_LO + (k / (points - 1)) * (LOG_HI - LOG_LO));
    const db = secs.length ? combinedMagnitudeDb(secs, hz, fs) : 0;
    return `${x.toFixed(1)},${yFor(db).toFixed(1)}`;
  }).join(' ');
}

// ---- Dynamics --------------------------------------------------------------
/** The gate's static curve: below threshold each dB costs (ratio-1) more, down to the range floor. */
export function gateOutput(g: BusGate, db: number): number {
  if (db >= g.threshold) return db;
  return db - Math.min(Math.abs(g.range), (Math.max(1, g.ratio) - 1) * (g.threshold - db));
}

/** The compressor's static curve, soft knee and makeup included (mirrors dynamics.hpp). */
export function compOutput(c: BusComp, db: number): number {
  const slope = 1 - 1 / Math.max(1, c.ratio);
  const w     = Math.max(0, c.knee);
  const over  = db - c.threshold;
  let reduction = 0;
  if (w > 0 && over > -w / 2 && over < w / 2) {
    const k = over + w / 2;
    reduction = (slope * k * k) / (2 * w);
  } else if (over > 0) {
    reduction = slope * over;
  }
  return db - reduction + c.makeup;
}

/** Gate then compressor, in engine order, each only when it and the section are in. */
export function dynOutput(dsp: Partial<BusDsp> | undefined, db: number): number {
  if (!dsp || dsp.dynEnabled === false) return db;
  let out = db;
  if (dsp.gate?.on) out = gateOutput(dsp.gate, out);
  if (dsp.comp?.on) out = compOutput(dsp.comp, out);
  return out;
}

export function gateIsActive(dsp: Partial<BusDsp> | undefined): boolean {
  return !!dsp && dsp.dynEnabled !== false && !!dsp.gate?.on;
}
export function compIsActive(dsp: Partial<BusDsp> | undefined): boolean {
  return !!dsp && dsp.dynEnabled !== false && !!dsp.comp?.on;
}
