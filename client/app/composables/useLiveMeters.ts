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
import { useOutputTarget } from '~/composables/useOutputTarget';
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

/**
 * Meter stream for one mixer strip.
 *
 * `lane` selects a single lane (0 = L, 1 = R) so a stereo bus can show two
 * meters; omitting it gives the combined reading across lanes, which is what a
 * mono bus and any compact single-bar display want. Servers predating per-lane
 * frames simply have no `lanes` array, and fall back to the combined values.
 */
export function useMixerMeter(mixerId: () => MixerChannelId | null | undefined,
                              lane?: () => number | null | undefined) {
  const server = useLiveplayServer();
  const peak    = ref(SILENT.peak_db);
  const rms     = ref(SILENT.rms_db);
  const peakMax = ref(SILENT.peak_max_db);
  // The rest of the snapshot, so a bus strip can drive the same meter widget
  // as the master: true peak for dBTP, K-weighted mean squares for the
  // momentary and short-term loudness readouts. The server has always sent
  // these per lane; nothing read them.
  const truePeak    = ref(SILENT.true_peak_db);
  const truePeakMax = ref(SILENT.true_peak_max_db);
  const kwMs        = ref(0);
  const kwMsS       = ref(0);
  // How far each dynamics processor is pulling down. Per strip rather than per
  // lane: both detectors are linked, so there is one figure per processor for
  // the whole channel.
  const gateGr      = ref(0);
  const compGr      = ref(0);
  // Correlation between the strip's lanes: +1 mono-compatible, 0 wide,
  // negative means a mono sum will cancel part of it. Mono strips read +1.
  const correlation = ref(1);
  // Peak level into and out of the dynamics since the last frame (dBFS),
  // for the transfer-curve meter. -120 when silent or on an older server.
  const dynIn       = ref(-120);
  const dynOut      = ref(-120);

  const silence = () => {
    peak.value = SILENT.peak_db; rms.value = SILENT.rms_db;
    peakMax.value = SILENT.peak_max_db;
    truePeak.value = SILENT.true_peak_db; truePeakMax.value = SILENT.true_peak_max_db;
    kwMs.value = 0; kwMsS.value = 0;
    gateGr.value = 0; compGr.value = 0; correlation.value = 1;
    dynIn.value = -120; dynOut.value = -120;
  };

  const unsubscribe = server.onMeters((m) => {
    const id = mixerId();
    if (!id) { silence(); return; }
    const frame: MixerMeterFrame | undefined =
      m.mixer_channels.find(x => x.mixer_id === id);
    if (!frame) { silence(); return; }

    const which = lane?.();
    const src = (which != null && (frame as any)?.lanes?.[which])
      ? (frame as any).lanes[which]
      : frame;

    peak.value        = src?.peak_db          ?? SILENT.peak_db;
    rms.value         = src?.rms_db           ?? SILENT.rms_db;
    peakMax.value     = src?.peak_max_db      ?? SILENT.peak_max_db;
    truePeak.value    = src?.true_peak_db     ?? src?.peak_db     ?? SILENT.true_peak_db;
    truePeakMax.value = src?.true_peak_max_db ?? src?.peak_max_db ?? SILENT.true_peak_max_db;
    kwMs.value        = src?.kw_ms            ?? 0;
    kwMsS.value       = src?.kw_ms_s          ?? 0;
    // Read off the strip frame, not the lane: gain reduction is one number for
    // the channel because both detectors are linked across its lanes.
    gateGr.value      = (frame as any)?.gate_gr_db ?? 0;
    compGr.value      = (frame as any)?.comp_gr_db ?? 0;
    correlation.value = (frame as any)?.correlation ?? 1;
    dynIn.value       = (frame as any)?.dyn_in_db ?? -120;
    dynOut.value      = (frame as any)?.dyn_out_db ?? -120;
  });
  onScopeDispose(() => unsubscribe());

  return { peak, rms, peakMax, truePeak, truePeakMax, kwMs, kwMsS,
           gateGr, compGr, correlation, dynIn, dynOut };
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

/**
 * The output limiter a bus passes through, if any.
 *
 * There is no per-bus limiter. The brickwall limiter sits on every hardware
 * output channel, after all the buses feeding it have been summed, with one
 * on/off switch (the project's `disableLimiter`) and one ceiling (the Output
 * Target's). So a bus "has" a limiter exactly when it sends to a hardware
 * output — `masters` is the output pair it lands on — and the gain reduction
 * shown for it is that output pair's. A bus that feeds another bus reaches a
 * limiter only through that bus, and shows none of its own.
 */
export function useBusLimiter(bus: () => { masters?: [number, number] | null } | null | undefined) {
  const left  = useMasterMeter(() => bus()?.masters?.[0] ?? null);
  const right = useMasterMeter(() => bus()?.masters?.[1] ?? null);
  const { levels } = useOutputTarget();
  const { currentProject } = useProject();

  const present = computed(() => !!bus()?.masters);
  // The detached mixer window may have no project loaded; the limiter is on
  // unless a project says otherwise, so that is what it shows there.
  const enabled = computed(() =>
    present.value && !(currentProject.value as any)?.settings?.disableLimiter);
  const grDb = computed(() => enabled.value
    ? Math.min(left.gainReduction.value, right.gainReduction.value) : 0);
  const ceilingDb = computed(() => levels.value.limiterCeilingDb);
  // "Working" = pulling the signal down by more than a hair right now.
  const working = computed(() => grDb.value < -0.5);

  return { present, enabled, grDb, ceilingDb, working };
}
