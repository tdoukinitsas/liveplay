/**
 * Display-only mirror of the RBJ Audio EQ Cookbook coefficient formulas in
 * `server/include/liveplay/audio/biquad.hpp`. This is not the filter that
 * runs on the render thread — it exists purely so the UI can draw the same
 * curve the C++ engine would produce, for the HPF/LPF and EQ band controls.
 *
 * Because this is a second model of the same maths rather than a call into
 * the authoritative one, THE TWO MUST BE KEPT IN STEP. If the C++ header
 * changes a formula, a clamp or a default Q, mirror the change here too, or
 * the curve drawn on screen will quietly stop matching the audio the desk is
 * actually producing.
 *
 * TypeScript has no float/double distinction, so unlike the C++ side (which
 * stores coefficients as `float` and computes them in `double`) everything
 * here just uses `number`. That is a deliberate simplification, not a
 * rounding-accurate reproduction: this file is for drawing a curve, not for
 * bit-matching the DSP.
 */

/** Normalised second-order section coefficients (a0 already divided out). */
export interface BiquadCoeffs {
  b0: number;
  b1: number;
  b2: number;
  a1: number;
  a2: number;
}

// pi, spelled out rather than taken from Math.PI's cousins, to keep this file
// reading like a direct transliteration of the C++ constant it mirrors.
const PI = 3.14159265358979323846;

// Every coefficient formula below clamps the corner frequency below Nyquist.
// The cookbook formulas divide by tan/sin of omega and blow up as the corner
// approaches fs/2: a 20 kHz low-pass at 44.1 kHz is close enough to matter,
// and a UI slider that lets the knob reach 20 kHz will get there. 0.49 * fs
// leaves a little room below Nyquist rather than sitting exactly on it.
function clampFreq(freqHz: number, sampleRate: number): number {
  const maxHz = 0.49 * sampleRate;
  if (freqHz < 1.0) return 1.0;
  if (freqHz > maxHz) return maxHz;
  return freqHz;
}

// Below about 0.1 the section is so broad it is doing nothing; above 40 it
// rings hard enough to be a fault rather than a setting.
function clampQ(q: number): number {
  if (q < 0.1) return 0.1;
  if (q > 40.0) return 40.0;
  return q;
}

function normalise(
  b0: number,
  b1: number,
  b2: number,
  a0: number,
  a1: number,
  a2: number
): BiquadCoeffs {
  return {
    b0: b0 / a0,
    b1: b1 / a0,
    b2: b2 / a0,
    a1: a1 / a0,
    a2: a2 / a0,
  };
}

/** A section that does nothing — the bypassed state of a filter block. */
export function biquadPassthrough(): BiquadCoeffs {
  return { b0: 1.0, b1: 0.0, b2: 0.0, a1: 0.0, a2: 0.0 };
}

export function biquadHighpass(
  freqHz: number,
  sampleRate: number,
  q = 0.70710678
): BiquadCoeffs {
  const w0 = (2.0 * PI * clampFreq(freqHz, sampleRate)) / sampleRate;
  const cosw = Math.cos(w0);
  const alpha = Math.sin(w0) / (2.0 * clampQ(q));
  return normalise(
    (1.0 + cosw) / 2.0,
    -(1.0 + cosw),
    (1.0 + cosw) / 2.0,
    1.0 + alpha,
    -2.0 * cosw,
    1.0 - alpha
  );
}

export function biquadLowpass(
  freqHz: number,
  sampleRate: number,
  q = 0.70710678
): BiquadCoeffs {
  const w0 = (2.0 * PI * clampFreq(freqHz, sampleRate)) / sampleRate;
  const cosw = Math.cos(w0);
  const alpha = Math.sin(w0) / (2.0 * clampQ(q));
  return normalise(
    (1.0 - cosw) / 2.0,
    1.0 - cosw,
    (1.0 - cosw) / 2.0,
    1.0 + alpha,
    -2.0 * cosw,
    1.0 - alpha
  );
}

export function biquadPeaking(
  freqHz: number,
  sampleRate: number,
  gainDb: number,
  q: number
): BiquadCoeffs {
  const A = Math.pow(10.0, gainDb / 40.0);
  const w0 = (2.0 * PI * clampFreq(freqHz, sampleRate)) / sampleRate;
  const cosw = Math.cos(w0);
  const alpha = Math.sin(w0) / (2.0 * clampQ(q));
  return normalise(
    1.0 + alpha * A,
    -2.0 * cosw,
    1.0 - alpha * A,
    1.0 + alpha / A,
    -2.0 * cosw,
    1.0 - alpha / A
  );
}

// A shelf's steepness, clamped the same way the C++ side clamps it. Past 2 the
// transition overshoots into a resonant peak at the corner, which is not what
// the control means; below 0.1 it is so gradual it stops being a shelf.
function clampSlope(slope: number): number {
  if (slope < 0.1) return 0.1;
  if (slope > 2.0) return 2.0;
  return slope;
}

export function biquadLowShelf(
  freqHz: number,
  sampleRate: number,
  gainDb: number,
  slope = 1.0
): BiquadCoeffs {
  const A = Math.pow(10.0, gainDb / 40.0);
  const w0 = (2.0 * PI * clampFreq(freqHz, sampleRate)) / sampleRate;
  const cosw = Math.cos(w0);
  const s = clampSlope(slope);
  const alpha = (Math.sin(w0) / 2.0) * Math.sqrt((A + 1.0 / A) * (1.0 / s - 1.0) + 2.0);
  const tsa = 2.0 * Math.sqrt(A) * alpha;
  return normalise(
    A * (A + 1.0 - (A - 1.0) * cosw + tsa),
    2.0 * A * (A - 1.0 - (A + 1.0) * cosw),
    A * (A + 1.0 - (A - 1.0) * cosw - tsa),
    A + 1.0 + (A - 1.0) * cosw + tsa,
    -2.0 * (A - 1.0 + (A + 1.0) * cosw),
    A + 1.0 + (A - 1.0) * cosw - tsa
  );
}

export function biquadHighShelf(
  freqHz: number,
  sampleRate: number,
  gainDb: number,
  slope = 1.0
): BiquadCoeffs {
  const A = Math.pow(10.0, gainDb / 40.0);
  const w0 = (2.0 * PI * clampFreq(freqHz, sampleRate)) / sampleRate;
  const cosw = Math.cos(w0);
  const s = clampSlope(slope);
  const alpha = (Math.sin(w0) / 2.0) * Math.sqrt((A + 1.0 / A) * (1.0 / s - 1.0) + 2.0);
  const tsa = 2.0 * Math.sqrt(A) * alpha;
  return normalise(
    A * (A + 1.0 + (A - 1.0) * cosw + tsa),
    -2.0 * A * (A - 1.0 + (A + 1.0) * cosw),
    A * (A + 1.0 + (A - 1.0) * cosw - tsa),
    A + 1.0 - (A - 1.0) * cosw + tsa,
    2.0 * (A - 1.0 - (A + 1.0) * cosw),
    A + 1.0 - (A - 1.0) * cosw - tsa
  );
}

/** Full cut at the centre, unity either side (mirrors biquad_notch). */
export function biquadNotch(freqHz: number, sampleRate: number, q: number): BiquadCoeffs {
  const w0 = (2.0 * PI * clampFreq(freqHz, sampleRate)) / sampleRate;
  const cosw = Math.cos(w0);
  const alpha = Math.sin(w0) / (2.0 * clampQ(q));
  return normalise(1.0, -2.0 * cosw, 1.0, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
}

/**
 * Magnitude response at one frequency, for drawing the EQ curve. Evaluates
 * |H(e^jw)| directly from the coefficients, matching the C++ side's approach
 * of describing the filter that is actually running rather than a second,
 * further-removed model of it.
 */
export function biquadMagnitudeDb(
  coeffs: BiquadCoeffs,
  freqHz: number,
  sampleRate: number
): number {
  const w = (2.0 * PI * freqHz) / sampleRate;
  const cw = Math.cos(w);
  const sw = Math.sin(w);
  const c2w = Math.cos(2.0 * w);
  const s2w = Math.sin(2.0 * w);
  const numRe = coeffs.b0 + coeffs.b1 * cw + coeffs.b2 * c2w;
  const numIm = -(coeffs.b1 * sw + coeffs.b2 * s2w);
  const denRe = 1.0 + coeffs.a1 * cw + coeffs.a2 * c2w;
  const denIm = -(coeffs.a1 * sw + coeffs.a2 * s2w);
  const num = Math.sqrt(numRe * numRe + numIm * numIm);
  const den = Math.sqrt(denRe * denRe + denIm * denIm);
  if (den <= 0.0) return -200.0;
  const mag = num / den;
  return mag <= 0.0 ? -200.0 : 20.0 * Math.log10(mag);
}

/** Floor for a reported magnitude. Shared with the single-section case. */
const MIN_DB = -200.0;

/**
 * Combined magnitude, in dB, of several cascaded sections at one frequency.
 * Cascaded sections multiply in the linear domain, so their dB figures add,
 * and this is that sum.
 *
 * Clamped at the end, because the floor does not survive addition: four
 * sections each reporting the `-200 dB` degenerate floor would total -800,
 * and a curve renderer scaling to its own extremes would then draw every real
 * band as a flat line at the top. Silence is silence however many sections
 * produced it.
 */
export function combinedMagnitudeDb(
  sections: BiquadCoeffs[],
  freqHz: number,
  sampleRate: number
): number {
  let totalDb = 0.0;
  for (const section of sections) {
    totalDb += biquadMagnitudeDb(section, freqHz, sampleRate);
  }
  return totalDb < MIN_DB ? MIN_DB : totalDb;
}
