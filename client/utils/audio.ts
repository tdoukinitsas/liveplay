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
