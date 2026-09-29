// The waveform drawn behind a cue row — the playlist row, the cart slot, and
// the mixer's "Feeding this bus" list all paint the same picture, so it lives
// here once.
//
// It shows the TRIMMED region only (the peaks between in- and out-point), in
// the item's own colour, as bars centred on the row with gamma-2 shaping:
// dynamics stay visible without loud tracks blowing out to a solid block. The
// strength of the tint is left to the canvas's CSS opacity, which is why each
// caller can dim it differently without touching this code.
//
// Pure drawing: no observers, no reactivity. Callers decide WHEN to draw (the
// rows do it lazily, once visible, and again on resize).
import type { AudioItem } from '~/types/project';

export interface RowWaveformOptions {
  /** Bar colour. Defaults to the item's own colour, then white. */
  color?: string;
}

type WaveformSource = Pick<AudioItem, 'duration' | 'inPoint' | 'outPoint' | 'color' | 'waveform'>;

/**
 * Size `canvas` to its laid-out box (at device pixel ratio) and draw the
 * item's trimmed waveform into it. A no-op when the item has no peaks yet —
 * the caller redraws once they arrive.
 */
export function drawRowWaveform(
  canvas: HTMLCanvasElement,
  item: WaveformSource,
  opts: RowWaveformOptions = {},
): void {
  const peaks = item.waveform?.peaks;
  if (!peaks || peaks.length === 0) return;

  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  // Set canvas size to match element size (use actual pixels for clarity)
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  ctx.scale(dpr, dpr);

  ctx.clearRect(0, 0, rect.width, rect.height);

  // The item's own colour, so the waveform tints to match the row; the
  // canvas's CSS opacity gives the naturally dark tint.
  ctx.fillStyle = opts.color || item.color || '#ffffff';

  // Calculate which peaks to show (slice based on in/out ratios)
  const totalDuration = item.duration;
  const inPoint = item.inPoint || 0;
  const outPoint = item.outPoint || totalDuration;
  const startIndex = Math.floor((inPoint / totalDuration) * peaks.length);
  const endIndex = Math.ceil((outPoint / totalDuration) * peaks.length);
  const trimmedPeaks = peaks.slice(startIndex, endIndex);

  const barWidth = rect.width / trimmedPeaks.length;
  const centerY = rect.height / 2;

  trimmedPeaks.forEach((value, i) => {
    // Gamma 2 expansion: shows dynamics without blowing up loud tracks.
    const clamped = Math.min(1, Math.max(0, value));
    const shaped = clamped * clamped;
    const barHeight = shaped * rect.height * 0.8;
    const x = i * barWidth;
    const y = centerY - barHeight / 2;

    ctx.fillRect(x, y, Math.max(barWidth, 1), barHeight);
  });
}
