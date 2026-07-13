// =====================================================================
// useLiveMeters.ts
// ---------------------------------------------------------------------
// Convenience layer over useLiveplayServer().onMeters for components that
// only care about a specific cue / mixer channel / master channel.
//
// Returns reactive refs that update every WS frame (~60 Hz). Falls back to
// silent values when the server is disconnected so meter widgets render
// at -∞ dB instead of stale data.
// =====================================================================
import { computed, onScopeDispose, ref, watch } from 'vue';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import type {
  CueId,
  ItemMeterFrame,
  MasterChannelIndex,
  MasterMeterFrame,
  MixerChannelId,
  MixerMeterFrame,
  MeterSnapshot,
} from '~/types/server';

const SILENT: MeterSnapshot = {
  peak_db: -120, rms_db: -120, peak_max_db: -120,
  true_peak_db: -120, true_peak_max_db: -120,
  kw_ms: 0, kw_ms_s: 0,
};

// BS.1770 loudness of a channel group from per-channel K-weighted mean
// squares: LUFS = -0.691 + 10·log10(Σ kw_ms). Returns -120 for silence /
// disabled loudness metering.
export function lufsFromKwMs(values: Array<number | undefined>): number {
  let sum = 0;
  for (const v of values) sum += v ?? 0;
  if (sum <= 1e-12) return -120;
  return -0.691 + 10 * Math.log10(sum);
}

export function useCueMeters(cueId: () => CueId | null | undefined) {
  const server = useLiveplayServer();
  const sources = ref<MeterSnapshot[]>([]);
  const transport = ref<number>(0);
  const playhead  = ref<number>(0);

  const unsubscribe = server.onMeters((m) => {
    const id = cueId();
    if (!id) { sources.value = []; return; }
    const frame: ItemMeterFrame | undefined = m.items.find(i => i.cue_id === id);
    if (!frame) { sources.value = []; return; }
    sources.value  = frame.sources;
    transport.value = frame.transport;
    playhead.value  = frame.playhead_seconds;
  });
  onScopeDispose(() => unsubscribe());

  return { sources, transport, playhead };
}

export function useMixerMeter(mixerId: () => MixerChannelId | null | undefined) {
  const server = useLiveplayServer();
  const peak    = ref(SILENT.peak_db);
  const rms     = ref(SILENT.rms_db);
  const peakMax = ref(SILENT.peak_max_db);

  const unsubscribe = server.onMeters((m) => {
    const id = mixerId();
    if (!id) { peak.value = SILENT.peak_db; rms.value = SILENT.rms_db; peakMax.value = SILENT.peak_max_db; return; }
    const frame: MixerMeterFrame | undefined =
      m.mixer_channels.find(x => x.mixer_id === id);
    peak.value    = frame?.peak_db     ?? SILENT.peak_db;
    rms.value     = frame?.rms_db      ?? SILENT.rms_db;
    peakMax.value = frame?.peak_max_db ?? SILENT.peak_max_db;
  });
  onScopeDispose(() => unsubscribe());

  return { peak, rms, peakMax };
}

// ---------------------------------------------------------------------
// Peak hold + clip latch, derived from the lossless peak_max_db stream.
// `source` should return the latest peak_max_db (already reactive).
//  - `held`: the peak line — holds the highest recent value for `holdMs`,
//    then releases to the current value.
//  - `clipped`: latches true once the raw max crosses `clipThresholdDb`;
//    stays latched until resetClip() (operator acknowledges).
// ---------------------------------------------------------------------
export function usePeakHold(
  source: () => number,
  opts?: { holdMs?: number; clipThresholdDb?: number },
) {
  const holdMs  = opts?.holdMs ?? 1500;
  const clipDb  = opts?.clipThresholdDb ?? -0.1;
  const held    = ref(-120);
  const clipped = ref(false);
  let heldAt = 0;
  let latest = -120;

  watch(computed(source), (v) => {
    latest = v;
    const now = performance.now();
    if (v >= held.value || now - heldAt >= holdMs) {
      held.value = v;
      heldAt = now;
    }
    if (v >= clipDb) clipped.value = true;
  });

  // Watch only fires on value *changes*; a signal that goes silent stops
  // producing changes, which would freeze the hold line forever. This timer
  // enforces expiry regardless.
  const timer = setInterval(() => {
    if (performance.now() - heldAt >= holdMs && held.value !== latest) {
      held.value = latest;
      heldAt = performance.now();
    }
  }, 250);
  onScopeDispose(() => clearInterval(timer));

  const resetClip = () => { clipped.value = false; };
  return { held, clipped, resetClip };
}

export function useMasterMeter(index: () => MasterChannelIndex | null | undefined) {
  const server = useLiveplayServer();
  const peak     = ref(SILENT.peak_db);
  const rms      = ref(SILENT.rms_db);
  const peakMax  = ref(SILENT.peak_max_db);
  const truePeak    = ref(SILENT.true_peak_db);
  const truePeakMax = ref(SILENT.true_peak_max_db);
  const kwMs     = ref(0);
  const kwMsS    = ref(0);
  const gainReduction = ref(0);

  const unsubscribe = server.onMeters((m) => {
    const i = index();
    if (i == null) {
      peak.value = SILENT.peak_db; rms.value = SILENT.rms_db;
      peakMax.value = SILENT.peak_max_db;
      truePeak.value = SILENT.true_peak_db; truePeakMax.value = SILENT.true_peak_max_db;
      kwMs.value = 0; kwMsS.value = 0;
      gainReduction.value = 0;
      return;
    }
    const frame: MasterMeterFrame | undefined =
      m.master_channels.find(x => x.index === i);
    peak.value        = frame?.peak_db          ?? SILENT.peak_db;
    rms.value         = frame?.rms_db           ?? SILENT.rms_db;
    peakMax.value     = frame?.peak_max_db      ?? SILENT.peak_max_db;
    truePeak.value    = frame?.true_peak_db     ?? frame?.peak_db     ?? SILENT.true_peak_db;
    truePeakMax.value = frame?.true_peak_max_db ?? frame?.peak_max_db ?? SILENT.true_peak_max_db;
    kwMs.value        = frame?.kw_ms            ?? 0;
    kwMsS.value       = frame?.kw_ms_s          ?? 0;
    gainReduction.value = frame?.gain_reduction_db ?? 0;
  });
  onScopeDispose(() => unsubscribe());

  return { peak, rms, peakMax, truePeak, truePeakMax, kwMs, kwMsS, gainReduction };
}
