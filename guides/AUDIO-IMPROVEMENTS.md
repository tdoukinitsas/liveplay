# Audio System Improvements

This document describes the comprehensive audio improvements implemented in LivePlay.

## Summary of Changes

### 1. Audio Utility Functions (NEW)
**File:** `utils/audio.ts`

Created a centralized audio mathematics library with proper logarithmic conversions:

- `dbToLinear(db)` - Convert dB to linear amplitude using 10^(dB/20)
- `linearToDb(linear)` - Convert linear to dB using 20*log10(linear)
- `applyVolumeOffset(uiVolume)` - Apply -10dB headroom offset for Howler.js
- `calculateRMS(peaks, start?, end?)` - Calculate Root Mean Square for loudness
- `calculatePerceivedLoudness(peaks)` - Convert RMS to dB (perceived loudness)
- `calculateNormalizationGain(current, target)` - Calculate gain needed for normalization
- `estimateCurrentLevel(volume, waveform)` - Estimate real-time playback level

**Audio Theory Documentation:**
- 10 dB increase = 10x sound pressure level (~2x perceived loudness)
- 6 dB increase = 2x amplitude (doubles volume)
- 3 dB increase = 2x power
- Howler.js: 0-1 volume range (>1 possible with Web Audio API)

### 2. Waveform Generation (10 samples/second)
**File:** `electron/main.js` (lines ~628-700)

**Changed From:** Fixed ~1000 samples regardless of duration
**Changed To:** 10 samples per second (duration-based sampling)

**Implementation:**
```javascript
// Get audio duration using FFprobe
const duration = metadata.format.duration;

// Calculate samples: 10 per second
const targetSamples = Math.ceil(duration * 10);

// Calculate sample interval
const sampleInterval = Math.floor(buffer.length / (targetSamples * 2));
```

**Benefits:**
- 5-second audio: 50 samples (was 1000) - more efficient
- 5-minute audio: 3000 samples (was 1000) - more detail
- Consistent temporal resolution across all files
- Saved with metadata: `{ peaks, duration, sampleRate: 10 }`

### 3. Volume-Based Waveform Display
**Files:** `components/PlaylistItem.vue`, `components/CartSlot.vue`

**Added:** Volume scaling to waveform display

**Implementation:**
```typescript
const volumeMultiplier = audioItem.volume || 1.0;
const barHeight = value * rect.height * 0.8 * volumeMultiplier;
```

**Benefits:**
- Waveform height reflects item volume
- Visual feedback matches actual playback level
- Consistent with WaveformTrimmer behavior

### 4. Perceived Loudness Visualization
**File:** `components/WaveformTrimmer.vue`

**Added:** Horizontal RMS line showing perceived loudness

**Implementation:**
```typescript
// Calculate RMS of visible peaks
const perceivedLoudness = calculatePerceivedLoudness(visiblePeaksArray);

// Convert to linear for display
const rmsLinear = Math.pow(10, perceivedLoudness / 20);
const rmsAmplified = rmsLinear * volumeMultiplier;
const rmsHeight = rmsAmplified * canvasHeight * 0.8;

// Draw dashed orange line at RMS level (top and bottom)
ctx.strokeStyle = 'rgba(255, 165, 0, 0.4)';
ctx.setLineDash([5, 5]);
```

**Benefits:**
- Shows average loudness of trimmed region
- Orange dashed line on both sides of center
- Helps identify quiet/loud sections
- Reference for normalization

### 5. Auto-Normalize Button
**File:** `components/WaveformTrimmer.vue`

**Added:** "Normalize" button next to "Trim Silence"

**Implementation:**
```typescript
const normalizeAudio = () => {
  // Calculate current perceived loudness
  const currentLoudness = calculatePerceivedLoudness(trimmedPeaks);
  
  // Target: -10dB (our "0dB" with headroom)
  const targetLoudness = -10;
  
  // Calculate and apply gain
  const gain = calculateNormalizationGain(currentLoudness, targetLoudness);
  const newVolume = Math.min(Math.max(currentVolume * gain, 0.001), 4.0);
  
  emit('update:volume', newVolume);
};
```

**Benefits:**
- One-click loudness normalization
- Targets -10dB (safe level with headroom)
- Respects trim points (only measures trimmed region)
- Clamps to reasonable range (0.001 to 4.0)

### 6. Updated Audio Engine
**File:** `composables/useAudioEngine.ts`

**Changes:**
- Import audio utilities: `linearToDb`, `dbToLinear`, `applyVolumeOffset`
- Replace all inline `20 * Math.log10()` with `linearToDb()`
- Replace all inline `Math.pow(10, db / 20)` with `dbToLinear()`
- Use centralized `applyVolumeOffset()` function

**Benefits:**
- Consistent conversions across codebase
- Proper logarithmic mathematics
- Maintainable single source of truth
- Better code readability

### 7. Translation Keys
**File:** `locales/en.json`

**Added:**
```json
"normalize": "Normalize"
```

**Note:** Other languages will need translation updates.

## Testing Checklist

### Waveform Generation
- [ ] Test with 5-second audio (should have ~50 samples)
- [ ] Test with 1-minute audio (should have ~600 samples)
- [ ] Test with 5-minute audio (should have ~3000 samples)
- [ ] Test with 1-hour audio (should have ~36000 samples)
- [ ] Verify waveform displays correctly in all views

### Volume-Based Display
- [ ] Load audio file and adjust volume slider
- [ ] Verify waveform height changes in playlist view
- [ ] Verify waveform height changes in cart slot
- [ ] Test with volume at 0dB, +10dB, -10dB, -60dB

### Perceived Loudness Line
- [ ] Open WaveformTrimmer
- [ ] Verify orange dashed line appears
- [ ] Check line position matches visual loudness
- [ ] Test with quiet audio (line should be low)
- [ ] Test with loud audio (line should be high)

### Auto-Normalize
- [ ] Load quiet audio file
- [ ] Open WaveformTrimmer
- [ ] Click "Normalize" button
- [ ] Verify volume increases to -10dB target
- [ ] Load loud audio file
- [ ] Click "Normalize" button
- [ ] Verify volume decreases to -10dB target
- [ ] Test with trimmed region (should only measure visible part)

### Audio Engine Integration
- [ ] Play multiple cues simultaneously
- [ ] Verify master output meter reads correctly
- [ ] Test ducking behavior (stop-all, duck)
- [ ] Verify volume changes reflect immediately
- [ ] Check console for proper dB values

## Technical Notes

### Why 10 Samples Per Second?

1. **Temporal Resolution:** Provides 100ms resolution, perfect for visualizing audio
2. **File Size:** Reasonable JSON size even for long files (3.6KB per hour of audio)
3. **Performance:** Fast to parse and render in canvas
4. **Detail:** Sufficient for waveform visualization without over-sampling
5. **Consistency:** Same resolution regardless of file duration

### dB Scale Reference

| dB     | Linear | Perceived    | Example                     |
|--------|--------|--------------|----------------------------|
| +10dB  | 3.16   | ~2x louder   | Maximum headroom           |
| +6dB   | 2.00   | Noticeably louder | Doubling amplitude    |
| +3dB   | 1.41   | Slightly louder | Doubling power         |
| 0dB    | 1.00   | Unity gain   | Normal/reference level     |
| -3dB   | 0.71   | Slightly quieter | Half power          |
| -6dB   | 0.50   | Noticeably quieter | Half amplitude     |
| -10dB  | 0.32   | ~Half as loud | Howler.js "0dB"           |
| -20dB  | 0.10   | Much quieter | Background music          |
| -60dB  | 0.001  | Near silence | Noise floor               |

### Howler.js Volume Range

- **Default:** 0.0 to 1.0 (with Web Audio API: unlimited)
- **LivePlay "0dB":** Actually -10dB (0.316 in Howler)
- **Headroom:** +10dB available above "0dB" for peaks
- **UI Range:** -60dB to +10dB (0.001 to 3.16 linear)

### RMS vs Peak Loudness

**RMS (Root Mean Square):**
- Measures average energy over time
- Correlates with perceived loudness
- sqrt(mean(squared samples))
- Used for normalization

**Peak:**
- Maximum instantaneous amplitude
- Not representative of loudness
- Used for clipping detection
- Visible as tallest waveform bars

## Future Enhancements

### Possible Additions:
1. **LUFS Metering:** True loudness standard (EBU R128)
2. **Loudness Range:** Show LRA (dynamic range)
3. **Target Presets:** -23 LUFS (broadcast), -16 LUFS (streaming), -10dB (current)
4. **Limiter:** True peak limiting for normalization
5. **Batch Normalize:** Normalize multiple items to same target
6. **Loudness History:** Graph showing loudness over time

### Migration Notes:
- Existing waveform files need regeneration (10 samples/sec format)
- Old format: `{ peaks: [], duration: approx }`
- New format: `{ peaks: [], duration: exact, sampleRate: 10 }`
- System will auto-regenerate when items are loaded

## Files Modified

1. **NEW:** `utils/audio.ts` - Audio mathematics library
2. **MODIFIED:** `electron/main.js` - 10 samples/sec generation
3. **MODIFIED:** `components/PlaylistItem.vue` - Volume-based display
4. **MODIFIED:** `components/CartSlot.vue` - Volume-based display
5. **MODIFIED:** `components/WaveformTrimmer.vue` - RMS line + normalize button
6. **MODIFIED:** `composables/useAudioEngine.ts` - Use audio utilities
7. **MODIFIED:** `locales/en.json` - Added "normalize" translation

## Audio Mathematics Reference

### Decibel Conversions
```typescript
// Linear to dB (amplitude)
dB = 20 * Math.log10(linear)

// dB to Linear
linear = Math.pow(10, dB / 20)
// Or: linear = 10 ** (dB / 20)

// Linear to dB (power)
dB = 10 * Math.log10(power)

// dB to Power
power = Math.pow(10, dB / 10)
```

### RMS Calculation
```typescript
// Root Mean Square
function calculateRMS(samples: number[]): number {
  const sumSquares = samples.reduce((sum, s) => sum + s * s, 0);
  const meanSquare = sumSquares / samples.length;
  return Math.sqrt(meanSquare);
}

// RMS to dB
function rmsToDb(rms: number): number {
  return rms <= 0 ? -60 : 20 * Math.log10(rms);
}
```

### Normalization Gain
```typescript
// Calculate gain needed to reach target
function calculateGain(currentDb: number, targetDb: number): number {
  const gainDb = targetDb - currentDb;
  return Math.pow(10, gainDb / 20);
}

// Example: -20dB -> -10dB
// gainDb = -10 - (-20) = 10dB
// gain = 10^(10/20) = 3.16x
```

## Implementation Complete ✓

All features have been implemented and are ready for testing. The audio system now has:
- Professional-grade dB conversions
- Duration-aware waveform sampling
- Visual loudness feedback
- One-click normalization
- Centralized audio mathematics

---
**Created:** December 2024
**Version:** 1.1.3+
**Author:** LivePlay Development Team
