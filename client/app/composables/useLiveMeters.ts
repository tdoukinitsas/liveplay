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
import { onScopeDispose, ref } from 'vue';
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

const SILENT: MeterSnapshot = { peak_db: -120, rms_db: -120, peak_max_db: -120 };

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

export function useMasterMeter(index: () => MasterChannelIndex | null | undefined) {
  const server = useLiveplayServer();
  const peak    = ref(SILENT.peak_db);
  const rms     = ref(SILENT.rms_db);
  const peakMax = ref(SILENT.peak_max_db);
  const gainReduction = ref(0);

  const unsubscribe = server.onMeters((m) => {
    const i = index();
    if (i == null) { peak.value = SILENT.peak_db; rms.value = SILENT.rms_db; peakMax.value = SILENT.peak_max_db; gainReduction.value = 0; return; }
    const frame: MasterMeterFrame | undefined =
      m.master_channels.find(x => x.index === i);
    peak.value    = frame?.peak_db     ?? SILENT.peak_db;
    rms.value     = frame?.rms_db      ?? SILENT.rms_db;
    peakMax.value = frame?.peak_max_db ?? SILENT.peak_max_db;
    gainReduction.value = frame?.gain_reduction_db ?? 0;
  });
  onScopeDispose(() => unsubscribe());

  return { peak, rms, peakMax, gainReduction };
}
