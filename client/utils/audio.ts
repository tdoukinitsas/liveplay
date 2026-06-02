/**
 * Audio Utilities for dB and Volume Conversion
 * 
 * Key concepts:
 * - 10 dB increase = 10x sound pressure level (perceived as ~2x louder)
 * - 6 dB increase = doubles amplitude (2x voltage)
 * - 3 dB increase = doubles power (2x energy)
 * 
 * Howler.js uses linear volume 0.0 to 1.0 (can go above 1.0 with Web Audio API)
 * We use -60 dB to +10 dB range in UI (-∞ to 0 dB in Howler, with +10dB headroom)
 */

/**
 * Convert decibels to linear volume
 * Formula: linear = 10^(dB/20)
 * 
 * @param db - Volume in decibels (-60 to +10)
 * @returns Linear volume (0.0 to ~3.16)
 */
export function dbToLinear(db: number): number {
  if (db <= -60) return 0;
  return Math.pow(10, db / 20);
}

/**
 * Convert linear volume to decibels
 * Formula: dB = 20 * log10(linear)
 * 
 * @param linear - Linear volume (0.0 to ~3.16)
 * @returns Volume in decibels (-60 to +10)
 */
export function linearToDb(linear: number): number {
  if (linear <= 0) return -60;
  return 20 * Math.log10(linear);
}

/**
 * Apply -10dB headroom offset for Howler.js playback
 * UI shows 0dB as "normal" while actually playing at -10dB
 * This gives +10dB headroom for louder playback
 * 
 * @param uiVolume - Volume from UI (0.0 to ~3.16 linear)
 * @returns Howler volume (0.0 to 1.0 for -10dB offset, can exceed 1.0)
 */
export function applyVolumeOffset(uiVolume: number): number {
  // Convert UI volume to dB
  const uiDB = linearToDb(uiVolume);
  
  // Apply -10dB offset
  const actualDB = uiDB - 10;
  
  // Convert back to linear for Howler
  const actualVolume = dbToLinear(actualDB);
  
  // Allow values above 1.0 for Web Audio API
  return Math.max(0, actualVolume);
}

/**
 * Calculate perceived loudness (RMS) from waveform peaks
 * RMS = Root Mean Square = sqrt(average of squared samples)
 * 
 * @param peaks - Waveform peak data (normalized 0-1)
 * @param startIndex - Start index in peaks array
 * @param endIndex - End index in peaks array
 * @returns RMS value (0-1)
 */
export function calculateRMS(peaks: number[], startIndex: number = 0, endIndex?: number): number {
  if (!peaks || peaks.length === 0) return 0;
  
  const end = endIndex ?? peaks.length;
  const count = end - startIndex;
  if (count <= 0) return 0;
  
  let sumSquares = 0;
  for (let i = startIndex; i < end; i++) {
    const value = peaks[i];
    sumSquares += value * value;
  }
  
  return Math.sqrt(sumSquares / count);
}

/**
 * Calculate perceived loudness in dB (LUFS approximation)
 * Uses RMS as a simple approximation of perceived loudness
 * 
 * @param peaks - Waveform peak data
 * @param startIndex - Start index
 * @param endIndex - End index
 * @returns Perceived loudness in dB (-60 to 0)
 */
export function calculatePerceivedLoudness(peaks: number[], startIndex?: number, endIndex?: number): number {
  const rms = calculateRMS(peaks, startIndex, endIndex);
  return linearToDb(rms);
}

/**
 * Calculate normalization gain to reach target loudness
 * 
 * @param currentLoudnessDb - Current perceived loudness in dB
 * @param targetLoudnessDb - Target loudness in dB (e.g., -10 for our 0dB UI)
 * @returns Gain multiplier (linear)
 */
export function calculateNormalizationGain(currentLoudnessDb: number, targetLoudnessDb: number): number {
  if (currentLoudnessDb <= -60) return 1; // Can't normalize silence
  
  const gainDb = targetLoudnessDb - currentLoudnessDb;
  return dbToLinear(gainDb);
}

/**
 * Estimate audio level at current playback position
 * Combines volume setting with waveform data
 *
 * @param volume - Base volume (linear)
 * @param waveformValue - Waveform peak at current position (0-1)
 * @returns Current level in dB (-60 to +10)
 */
export function estimateCurrentLevel(volume: number, waveformValue: number): number {
  if (volume <= 0 || waveformValue <= 0) return -60;

  // Convert both to dB and add
  const volumeDb = linearToDb(volume);
  const waveformDb = linearToDb(waveformValue);
  const totalDb = volumeDb + waveformDb;

  return Math.max(-60, Math.min(10, totalDb));
}

// ---------------------------------------------------------------------------
// Auto-process: trim silence + normalise to target loudness.
// Applied on first waveform load when disableAutoVolumeAndTrim is false.
// ---------------------------------------------------------------------------

interface AutoProcessableItem {
  duration: number;
  inPoint: number;
  outPoint: number;
  volume: number;
  waveform?: { peaks: number[]; length: number; duration: number };
}

/**
 * Apply trim-silence and loudness normalisation to an audio item in-place.
 * Returns true when any property was modified.
 *
 * @param item            - Audio item (mutated directly)
 * @param targetLoudnessDb - LUFS/dBFS target from the active output target
 */
export function applyAutoProcessing(item: AutoProcessableItem, targetLoudnessDb: number): boolean {
  if (!item.waveform || !item.waveform.peaks || item.waveform.peaks.length === 0) return false;

  const peaks = item.waveform.peaks;
  const duration = item.duration || item.waveform.duration || 0;
  if (duration <= 0) return false;

  let changed = false;

  // --- 1. Trim silence ---
  const maxPeak = peaks.reduce((m, v) => (v > m ? v : m), 0);
  if (maxPeak > 0) {
    const threshold = maxPeak * 0.05; // 5% of peak
    const padding = 0.1;

    let startIdx = 0;
    for (let i = 0; i < peaks.length; i++) {
      if (peaks[i] > threshold) { startIdx = i; break; }
    }
    let endIdx = peaks.length - 1;
    for (let i = peaks.length - 1; i >= 0; i--) {
      if (peaks[i] > threshold) { endIdx = i; break; }
    }

    const newInPoint  = Math.max(0, (startIdx / peaks.length) * duration - padding);
    const newOutPoint = Math.min(duration, ((endIdx + 1) / peaks.length) * duration + padding);

    if (Math.abs(newInPoint - item.inPoint) > 0.05 || Math.abs(newOutPoint - item.outPoint) > 0.05) {
      item.inPoint  = newInPoint;
      item.outPoint = newOutPoint;
      changed = true;
    }
  }

  // --- 2. Normalise to target loudness ---
  const trimmedStart = Math.floor((item.inPoint / duration) * peaks.length);
  const trimmedEnd   = Math.ceil((item.outPoint / duration) * peaks.length);
  const trimmedPeaks = peaks.slice(trimmedStart, trimmedEnd);

  const intrinsicLoudness = calculatePerceivedLoudness(trimmedPeaks);
  if (intrinsicLoudness > -60) {
    const gainDb    = targetLoudnessDb - intrinsicLoudness;
    const newVolume = Math.pow(10, gainDb / 20);
    const maxVolume = Math.pow(10, 10 / 20); // +10 dB ceiling
    const clamped   = Math.min(Math.max(newVolume, 0.001), maxVolume);
    if (Math.abs(clamped - item.volume) > 0.001) {
      item.volume = clamped;
      changed = true;
    }
  }

  return changed;
}
