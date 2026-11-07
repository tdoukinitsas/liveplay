// =====================================================================
// waveformRender.ts
// ---------------------------------------------------------------------
// The playback waveform's drawing (the Properties panel's Playback tab),
// shared so every view of a cue's audio reads the same: one lane per source
// channel (stereo draws L above R), the loudness-referenced scale, the level
// after the cue's volume over a grey pre-volume reference, the perceived-
// loudness line, and the playback markers (fades, crossfade, Start Next).
// What each view chooses is the bar colour and how strongly the markers show.
// =====================================================================
import { calculatePerceivedLoudness } from '~/utils/audio';

export type Rgb = [number, number, number];

/** The visible window of the file, and the canvas area it fills (CSS px). */
export interface WaveformView {
  duration: number;   // the whole file, seconds
  start: number;      // first visible second
  end: number;        // last visible second
  width: number;
  height: number;
}

/**
 * Lanes to draw: one per source channel when the server gave per-channel data
 * (stereo renders L above R), otherwise a single combined lane. Legacy
 * waveforms (ffmpeg mono downmix, pre-channelPeaks projects) keep one lane.
 */
export function waveformLanes(
  wf: { peaks?: number[] | null; channelPeaks?: number[][] | null } | null | undefined,
): number[][] {
  if (!wf) return [];
  const perChannel = wf.channelPeaks;
  if (Array.isArray(perChannel) && perChannel.length > 1 &&
      perChannel.every(lane => Array.isArray(lane) && lane.length > 0)) {
    return perChannel;
  }
  return wf.peaks && wf.peaks.length > 0 ? [wf.peaks] : [];
}

/**
 * The lanes, as the Playback tab draws them. `volume` is the cue's linear
 * volume (bars show the audible level); `targetDb` the project's auto-volume
 * target, which sets the vertical scale; `rgbFor` the colour of a bar at a
 * level in dB (the Playback tab uses the output target's zones).
 */
export function drawWaveformLanes(
  ctx: CanvasRenderingContext2D,
  lanes: number[][],
  view: WaveformView,
  opts: { volume: number; targetDb: number; rgbFor: (db: number) => Rgb },
): void {
  if (lanes.length === 0 || view.duration <= 0) return;
  const laneHeight = view.height / lanes.length;
  const volumeMultiplier = opts.volume;

  // Loudness-referenced vertical scale.
  // ------------------------------------
  // The display is calibrated so that a signal sitting AT the project's
  // target optimal loudness fills ~3/4 of the lane height, leaving the
  // top 1/4 as headroom for louder transient peaks. Without this, an
  // auto-normalised track (whose volume is pulled down to hit the target)
  // rendered as a tiny sliver. We map linear amplitude → height fraction
  // with a fixed gain so the target maps to 0.75 and clamp at 1.0.
  const targetLinear = Math.pow(10, opts.targetDb / 20);
  const HEIGHT_AT_TARGET = 0.75;
  const loudnessScale = HEIGHT_AT_TARGET / Math.max(targetLinear, 1e-4);
  const heightFraction = (linear: number) =>
    Math.min(Math.max(linear, 0) * loudnessScale, 1);

  lanes.forEach((peaks, laneIndex) => {
    const laneCenter = laneIndex * laneHeight + laneHeight / 2;
    const totalPeaks = peaks.length;

    // Calculate visible peak range
    const startPeak = Math.floor((view.start / view.duration) * totalPeaks);
    const endPeak = Math.floor((view.end / view.duration) * totalPeaks);
    const visiblePeaksArray = peaks.slice(startPeak, endPeak);
    if (visiblePeaksArray.length === 0) return;

    const barWidth = view.width / visiblePeaksArray.length;

    // Draw each bar with individual coloring
    visiblePeaksArray.forEach((value, i) => {
      const normalizedPeak = value; // Already normalized 0-1
      const x = i * barWidth;

      // Base waveform bar height (pre-volume, subtle gray reference)
      const baseBarHeight = heightFraction(normalizedPeak) * laneHeight;
      const baseY = laneCenter - baseBarHeight / 2;
      ctx.fillStyle = 'rgba(128, 128, 128, 0.15)';
      ctx.fillRect(x, baseY, Math.max(barWidth, 1), baseBarHeight);

      // Bar height after volume multiplication (the audible level).
      const linearAmplitude = normalizedPeak * volumeMultiplier;
      const amplifiedBarHeight = heightFraction(linearAmplitude) * laneHeight;
      const amplifiedY = laneCenter - amplifiedBarHeight / 2;

      // Convert this bar's amplitude to dB for color selection
      const barDB = linearAmplitude <= 0 ? -60 : 20 * Math.log10(linearAmplitude);

      // Get color for this specific bar's level
      const [r, g, b] = opts.rgbFor(barDB);

      // Draw colored bar with opacity based on whether it's clipping
      const alpha = linearAmplitude > 1 ? 0.8 : 0.5; // More opaque if clipping
      ctx.fillStyle = `rgba(${r}, ${g}, ${b}, ${alpha})`;
      ctx.fillRect(x, amplifiedY, Math.max(barWidth, 1), amplifiedBarHeight);
    });

    // Draw perceived loudness line (RMS level) for this lane
    const perceivedLoudness = calculatePerceivedLoudness(visiblePeaksArray);
    const rmsLinear = perceivedLoudness <= -60 ? 0 : Math.pow(10, perceivedLoudness / 20);
    const rmsHeight = heightFraction(rmsLinear * volumeMultiplier) * laneHeight;

    // Draw horizontal line at RMS level (on both sides of the lane centre)
    ctx.strokeStyle = 'rgba(255, 165, 0, 0.5)'; // Orange with transparency
    ctx.lineWidth = 1;

    // Top line
    const topY = laneCenter - rmsHeight / 2;
    ctx.beginPath();
    ctx.moveTo(0, topY);
    ctx.lineTo(view.width, topY);
    ctx.stroke();

    // Bottom line
    const bottomY = laneCenter + rmsHeight / 2;
    ctx.beginPath();
    ctx.moveTo(0, bottomY);
    ctx.lineTo(view.width, bottomY);
    ctx.stroke();

    // Reset line dash
    ctx.setLineDash([]);

    // Lane zero line + divider between lanes, so L/R read as two strips.
    if (lanes.length > 1) {
      ctx.strokeStyle = 'rgba(128, 128, 128, 0.25)';
      ctx.beginPath();
      ctx.moveTo(0, laneCenter);
      ctx.lineTo(view.width, laneCenter);
      ctx.stroke();
      if (laneIndex > 0) {
        ctx.strokeStyle = 'rgba(128, 128, 128, 0.15)';
        ctx.beginPath();
        ctx.moveTo(0, laneIndex * laneHeight);
        ctx.lineTo(view.width, laneIndex * laneHeight);
        ctx.stroke();
      }
    }
  });
}

/** What the playback markers are drawn from: the cue's own fields. */
export interface PlaybackMarkers {
  inPoint: number;
  outPoint: number;
  playFade?: number;
  stopFade?: number;
  crossFade?: number;
  startNextEnabled?: boolean;
  startNextTime: number;
  startNextFadeOut?: boolean;
  fadeOutDuration?: number;
}

/**
 * The fades as the Playback tab draws them: play fade and stop fade red
 * diagonals, a crossfade a yellow X, Start Next a green line (and its fade-out
 * when it has one). Drawn at the context's current globalAlpha, so a view that
 * only shows them (the Events tab) can ghost them.
 */
export function drawPlaybackMarkers(
  ctx: CanvasRenderingContext2D,
  m: PlaybackMarkers,
  view: WaveformView,
): void {
  if (view.duration <= 0) return;
  const canvasHeight = view.height;
  const visibleDuration = view.end - view.start;
  if (visibleDuration <= 0) return;
  const pixelsPerSecond = view.width / visibleDuration;

  // Play Fade (fade in at start) — red diagonal
  if (m.playFade && m.playFade > 0) {
    const fadeStartTime = m.inPoint;
    const fadeEndTime = m.inPoint + m.playFade;
    if (fadeEndTime >= view.start && fadeStartTime <= view.end) {
      const fadeStartX = Math.max(0, (fadeStartTime - view.start) * pixelsPerSecond);
      const fadeEndX = Math.min(view.width, (fadeEndTime - view.start) * pixelsPerSecond);
      const fadeWidth = fadeEndX - fadeStartX;
      if (fadeWidth > 0) {
        ctx.fillStyle = 'rgba(220, 38, 38, 0.2)';
        ctx.fillRect(fadeStartX, 0, fadeWidth, canvasHeight);
        ctx.strokeStyle = 'rgba(220, 38, 38, 0.8)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(fadeStartX, canvasHeight);
        ctx.lineTo(fadeEndX, 0);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(fadeEndX, 0);
        ctx.lineTo(fadeEndX, canvasHeight);
        ctx.stroke();
      }
    }
  }

  // Stop Fade (fade out before end) — red diagonal
  if (m.stopFade && m.stopFade > 0) {
    const fadeStartTime = m.outPoint - m.stopFade;
    const fadeEndTime = m.outPoint;
    if (fadeStartTime <= view.end && fadeEndTime >= view.start) {
      const fadeStartX = Math.max(0, (fadeStartTime - view.start) * pixelsPerSecond);
      const fadeEndX = Math.min(view.width, (fadeEndTime - view.start) * pixelsPerSecond);
      const fadeWidth = fadeEndX - fadeStartX;
      if (fadeWidth > 0) {
        ctx.fillStyle = 'rgba(220, 38, 38, 0.2)';
        ctx.fillRect(fadeStartX, 0, fadeWidth, canvasHeight);
        ctx.strokeStyle = 'rgba(220, 38, 38, 0.8)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(fadeStartX, 0);
        ctx.lineTo(fadeEndX, canvasHeight);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(fadeStartX, 0);
        ctx.lineTo(fadeStartX, canvasHeight);
        ctx.stroke();
      }
    }
  }

  // Cross Fade — yellow X
  if (m.crossFade && m.crossFade > 0) {
    const crossFadeStartTime = m.outPoint - m.crossFade;
    const crossFadeEndTime = m.outPoint;
    if (crossFadeStartTime <= view.end && crossFadeEndTime >= view.start) {
      const crossStartX = Math.max(0, (crossFadeStartTime - view.start) * pixelsPerSecond);
      const crossEndX = Math.min(view.width, (crossFadeEndTime - view.start) * pixelsPerSecond);
      const crossWidth = crossEndX - crossStartX;
      if (crossWidth > 0) {
        ctx.fillStyle = 'rgba(234, 179, 8, 0.2)';
        ctx.fillRect(crossStartX, 0, crossWidth, canvasHeight);
        ctx.strokeStyle = 'rgba(234, 179, 8, 0.8)';
        ctx.lineWidth = 2;
        ctx.beginPath(); ctx.moveTo(crossStartX, 0); ctx.lineTo(crossEndX, canvasHeight); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(crossStartX, canvasHeight); ctx.lineTo(crossEndX, 0); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(crossStartX, 0); ctx.lineTo(crossStartX, canvasHeight); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(crossEndX, 0); ctx.lineTo(crossEndX, canvasHeight); ctx.stroke();
      }
    }
  }

  // Start Next marker — green segue line, plus the optional marker fade-out
  if (m.startNextEnabled && m.startNextTime > 0) {
    const markerTime = m.startNextTime;
    if (markerTime >= view.start && markerTime <= view.end) {
      const markerX = (markerTime - view.start) * pixelsPerSecond;
      ctx.strokeStyle = 'rgba(22, 163, 74, 0.9)';
      ctx.lineWidth = 2;
      ctx.beginPath(); ctx.moveTo(markerX, 0); ctx.lineTo(markerX, canvasHeight); ctx.stroke();
    }
    if (m.startNextFadeOut) {
      const fadeDur = m.fadeOutDuration || 1;
      const fadeEndTime = Math.min(markerTime + fadeDur, m.outPoint);
      if (markerTime <= view.end && fadeEndTime >= view.start) {
        const fadeStartX = Math.max(0, (markerTime - view.start) * pixelsPerSecond);
        const fadeEndX = Math.min(view.width, (fadeEndTime - view.start) * pixelsPerSecond);
        const fadeWidth = fadeEndX - fadeStartX;
        if (fadeWidth > 0) {
          ctx.fillStyle = 'rgba(22, 163, 74, 0.15)';
          ctx.fillRect(fadeStartX, 0, fadeWidth, canvasHeight);
          ctx.strokeStyle = 'rgba(22, 163, 74, 0.8)';
          ctx.lineWidth = 2;
          ctx.beginPath(); ctx.moveTo(fadeStartX, 0); ctx.lineTo(fadeEndX, canvasHeight); ctx.stroke();
        }
      }
    }
  }
}
