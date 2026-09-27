<template>
  <div class="playback-controls" :class="{ 'show-mode': showMode }">
    <div class="controls-left">
      <button
        class="control-btn play-next-btn"
        :class="{ 'has-next': !!effectiveNextUuid }"
        @click="handlePlayNext"
        :disabled="!effectiveNextUuid"
        :title="playNextTooltip"
      >
        <span class="material-symbols-rounded">fast_forward</span>
        <span>{{ t('controls.playNext') }}</span>
        <span v-if="nextInSeconds !== null" class="next-in-badge">
          {{ t('controls.nextIn', { seconds: nextInSeconds }) }}
        </span>
      </button>
      <button class="control-btn panic-btn" @click="handlePanic" :disabled="activeCues.size === 0 && !previewingItem" :title="stopAllTooltip">
        <span class="icon">⚠</span>
        <span>{{ t('playback.panic') }}</span>
      </button>
    </div>
    
    <div class="active-cues">
      <div v-if="activeCues.size === 0 && !previewingItem" class="no-cues">
        {{ t('playback.noActiveCues') }}
      </div>

      <div v-else class="cue-list">
        <!-- Preview card: styled identically to ActiveCueItem, with a green
             "Preview" pill at the start of the name. Meter reads the preview
             output bus, which is the top pair of the master bus (30/31 on a
             default 32-wide bus). Seek + time come from the per-cue meter
             stream (playhead_seconds). -->
        <div v-if="previewingItem" class="preview-cue-card">
          <div class="preview-cue-content">
            <div class="preview-cue-header">
              <span class="preview-cue-name">
                <span class="preview-status-pill">{{ t('status.previewing') }}</span>
                {{ previewingItem.displayName }}
              </span>
              <div class="preview-cue-actions">
                <button class="preview-stop-btn" @click="stopPreview" :title="t('actions.stopPreview')">
                  <span class="material-symbols-rounded">stop</span>
                </button>
              </div>
            </div>

            <div class="preview-cue-progress">
              <div class="preview-time-info">
                <span>{{ formatPreviewTime(previewCurrentTime) }}</span>
                <span>-{{ formatPreviewTime(previewDuration - previewCurrentTime) }}</span>
              </div>
              <div class="preview-progress-bar" @click="handlePreviewSeek">
                <div class="preview-progress-fill" :style="{ width: previewProgressPct + '%' }"></div>
                <div class="preview-progress-handle" :style="{ left: previewProgressPct + '%' }"></div>
              </div>
            </div>
          </div>
          <div class="preview-cue-meter">
            <StereoMeter
              :left-index="server.masterBus.previewL"
              :right-index="server.masterBus.previewR"
              :min-db="-60"
              :max-db="0"
            />
          </div>
        </div>

        <ActiveCueItem
          v-for="[uuid, cue] in Array.from(activeCues.entries())"
          :key="uuid"
          :cue="cue"
        />
      </div>
    </div>
    
    <!-- Per-output meters — one StereoMeter + fader per hardware output pair
         (D32). Each row is a bus that reaches hardware: the Master bus first,
         then every other bound bus, then Preview — the Preview row only while
         it carries signal or a pre-listen is running, so the headphone pair
         does not take transport-bar space during a show. The fader is that
         bus's own fader, the same control the mixer strip drives. -->
    <div class="output-meters">
      <div v-for="pair in outputPairs" :key="pair.key" class="output-pair">
        <StereoMeter
          :left-index="pair.leftIndex"
          :right-index="pair.rightIndex"
          :label="pair.label"
          :show-peak-value="true"
        />
        <VolumeSlider
          :db="pair.bus ? busFaderDb(pair.bus) : getOutputGainDb(pair.leftIndex)"
          :min-db="FADER_MIN_DB"
          :max-db="FADER_MAX_DB"
          :title="pair.label"
          @input="(db: number) => pair.bus ? onBusFader(pair.bus, db) : onOutputGainInput(pair.leftIndex, pair.rightIndex, db)"
          @reset="pair.bus ? onBusFader(pair.bus, 0) : resetOutputGain(pair.leftIndex, pair.rightIndex)"
        />
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
// PlaybackControls migration (Milestone 5):
//   * Panic / stop-all now fans out to BOTH the legacy useAudioEngine
//     (until every component is migrated away from it) AND the new C++
//     server via useLiveplayServer().stopAll(). Removing the legacy call
//     is safe once all play paths route through the server.
//   * The output meters are <StereoMeter> pairs driven by the live WebSocket
//     meter stream from the engine — replacing the static-waveform "cheat"
//     levels. Every meter in the app is now that one component.
import { formatKeyLabel } from '~/composables/useCartHotkeys';
import type { AudioItem, Bus } from '~/types/project';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import { useCueMeters } from '~/composables/useLiveMeters';
import { FADER_MIN_DB, FADER_MAX_DB } from '~/utils/meterScale';
import VolumeSlider from './VolumeSlider.vue';

const { activeCues, panicStop, nextItemOverrideUuid, autoNextItemUuid, setNextItem, playCue, triggerGroup } = useAudioEngine();
const { findItemByUuid, previewItemUuid, previewCueId, stopPreview, advancePending } = useProject();

// "Wait before next" countdown (#8). The server owns the wait and announces
// it; this only ticks a clock while one is running.
const nowMs = ref(Date.now());
let nowTimer: ReturnType<typeof setInterval> | null = null;
watch(advancePending, (p) => {
  if (p && !nowTimer) nowTimer = setInterval(() => { nowMs.value = Date.now(); }, 200);
  if (!p && nowTimer) { clearInterval(nowTimer); nowTimer = null; }
}, { immediate: true });
onBeforeUnmount(() => { if (nowTimer) clearInterval(nowTimer); });
const nextInSeconds = computed(() => {
  const p = advancePending.value;
  if (!p) return null;
  return Math.max(0, Math.ceil((p.dueAt - nowMs.value) / 1000));
});
const { playbackMappings } = useCartHotkeys();
const { t } = useLocalization();
const server = useLiveplayServer();
const { uiMode } = useUiMode();
// Show Mode enlarges the GO / Stop-All buttons for touch; the active-cue cards
// and meters are already the right size and stay as-is.
const showMode = computed(() => uiMode.value === 'playback');

// ---- Preview seek / time --------------------------------------------------
// Subscribe to the preview cue's per-item meter stream so we can display an
// accurate playhead, elapsed time, and remaining time in the preview card.
const previewMeter = useCueMeters(() => previewCueId.value || null);
// The meter reports the absolute file playhead; the card counts from the
// in-point, as the duration below does.
const previewCurrentTime = computed(() => {
  const inPoint = (previewingItem.value as any)?.inPoint ?? 0;
  return Math.max(0, previewMeter.playhead.value - inPoint);
});
const previewDuration = computed(() => {
  if (!previewingItem.value) return 0;
  const item = previewingItem.value as any;
  const inPoint  = item.inPoint  ?? 0;
  const outPoint = item.outPoint ?? item.duration ?? 0;
  return Math.max(0, outPoint - inPoint);
});
const previewProgressPct = computed(() => {
  if (!previewDuration.value) return 0;
  return Math.min(100, (previewCurrentTime.value / previewDuration.value) * 100);
});

function formatPreviewTime(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  const m = Math.floor(s / 60);
  return `${m}:${(s % 60).toString().padStart(2, '0')}`;
}

function handlePreviewSeek(e: MouseEvent) {
  if (!previewCueId.value || !previewDuration.value) return;
  const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
  const pct = (e.clientX - rect.left) / rect.width;
  const seekTo = pct * previewDuration.value;
  const item = previewingItem.value as any;
  const inPoint = item?.inPoint ?? 0;
  server.seekCueId(previewCueId.value, Math.max(0, seekTo + inPoint));
}

// The output rows come from the buses (D32). A bus with `masters` occupies a
// hardware pair on the engine's master bus; that pair is what the meter reads
// and the bus's own fader is what the slider moves. Order: the master-role bus
// (the house, masters 0/1), then every other hardware-bound bus by `order`,
// then the preview-role bus. Bound buses are shown whether or not they carry
// signal — a silent Master is still an output — except Preview, which keeps
// the old "only while active" rule so a headphone pair does not take space
// during a show.
//
// `server.buses` is empty until the composable's first-connect fetch lands
// (it refetches on every buses_patched / outputs_changed), so nothing is
// fetched from here.
type OutputPair = { key: string; leftIndex: number; rightIndex: number; label: string; bus: Bus | null };

const outputPairs = computed<OutputPair[]>(() => {
  const m = server.meters;
  const activeIdx = new Set((m?.master_channels ?? []).map((mc: any) => mc.index as number));
  const previewL = server.masterBus.previewL;
  const previewR = server.masterBus.previewR;

  const bound = server.buses.filter(b => b.bound && b.masters !== null);
  const ordered = [
    ...bound.filter(b => b.master),
    ...bound.filter(b => !b.master && !b.preview).sort((a, b) => a.order - b.order),
    ...bound.filter(b => b.preview && !b.master),
  ];

  const pairs: OutputPair[] = [];
  const claimed = new Set<number>();
  for (const bus of ordered) {
    const [l, r] = bus.masters!;
    claimed.add(l); claimed.add(r);
    if (bus.preview && !(activeIdx.has(l) || activeIdx.has(r) || previewItemUuid.value)) continue;
    pairs.push({ key: bus.id, leftIndex: l, rightIndex: r, label: bus.name, bus });
  }

  // Diagnostic fallback: a pair that carries signal but no bus claims, and
  // is not the reserved preview pair, still shows as "Out N" with the old
  // per-output-channel gain fader. With every hardware-bound bus reporting
  // `masters` this is expected never to appear; if it does, the engine has
  // routed something the bus list does not account for, and hiding it would
  // hide exactly the thing worth seeing.
  for (let i = 0; i + 1 < server.masterBus.channels; i += 2) {
    if (claimed.has(i) || claimed.has(i + 1)) continue;
    if (i === previewL || i === previewR) continue;
    if (!(activeIdx.has(i) || activeIdx.has(i + 1))) continue;
    pairs.push({ key: `out-${i}`, leftIndex: i, rightIndex: i + 1, label: t('playback.outputPair', { n: i / 2 }), bus: null });
  }

  return pairs;
});

// Preview pill data: when an item is being pre-listened on the headphone bus,
// this resolves to the item record so we can render its display name.
const previewingItem = computed(() => {
  const uuid = previewItemUuid.value;
  if (!uuid) return null;
  return findItemByUuid(uuid);
});

const effectiveNextUuid = computed(() => nextItemOverrideUuid.value ?? autoNextItemUuid.value);

const playNextTooltip = computed(() => {
  const binding = playbackMappings.value['play-next'];
  const shortcut = binding ? formatKeyLabel(binding) : '';
  return shortcut ? `${t('controls.playNext')} (${shortcut})` : t('controls.playNext');
});

const stopAllTooltip = computed(() => {
  const binding = playbackMappings.value['stop-all'];
  const shortcut = binding ? formatKeyLabel(binding) : '';
  return shortcut ? `${t('playback.panic')} (${shortcut})` : t('playback.panic');
});

const handlePanic = () => {
  // Stop everything, fading over the project-wide Stop All time
  // (settings.stopAllFadeMs, default 1 s; set to 0 for an instant panic).
  // panicStop() forwards to the server with no explicit fade so the server
  // applies that project setting.
  panicStop();
};

// ---- Bus faders ------------------------------------------------------------
// The same pattern as MixerStrip: while a fader moves the level goes straight
// to the engine strip (no document write, no refetch) and the local value is
// held, because binding to bus.gainDb meant every drag event did a PATCH plus
// a full bus refetch and the knob snapped back to the stale value until the
// round-trip landed. Once the gesture settles the bus is persisted with
// patchBus {gainDb}, and the held value is released so the fader follows the
// bus again — including changes made from the mixer or another client.
const heldDb = reactive<Record<string, number>>({});
const settleTimers: Record<string, ReturnType<typeof setTimeout>> = {};

function busFaderDb(bus: Bus): number {
  return heldDb[bus.id] ?? bus.gainDb;
}

function onBusFader(bus: Bus, db: number) {
  heldDb[bus.id] = db;
  if (bus.mixerId) void server.setMixerGainDb(bus.mixerId, db).catch(() => {});
  if (settleTimers[bus.id]) clearTimeout(settleTimers[bus.id]);
  settleTimers[bus.id] = setTimeout(() => {
    delete settleTimers[bus.id];
    const id = bus.id;
    // patchBus refetches the bus list before resolving, so by the time the
    // held value is released bus.gainDb already carries what we sent.
    void server.patchBus(id, { gainDb: db })
      // Already settled behind a 250 ms timer, so this is one save per gesture
      // rather than one per fader frame — and saveProject debounces again on
      // top of that. Without it a level set from the transport bar lived only
      // in the server's memory.
      .then(() => { void useProject().saveProject(); })
      .catch(() => {})
      .finally(() => { if (!settleTimers[id]) delete heldDb[id]; });
  }, 250);
}

// ---- Fallback per-output-channel faders ------------------------------------
// Only for the unclaimed "Out N" rows above. outputChannelGains stays at unity
// in the bus world; these drive it directly, as the transport bar always did.
function getOutputGainDb(leftIndex: number): number {
  return server.outputChannelGains[leftIndex] ?? 0;
}

function onOutputGainInput(leftIndex: number, rightIndex: number, db: number) {
  // Update both channels of the stereo pair together.
  server.setOutputChannelGainDb(leftIndex, db);
  server.setOutputChannelGainDb(rightIndex, db);
}

function resetOutputGain(leftIndex: number, rightIndex: number) {
  server.setOutputChannelGainDb(leftIndex, 0);
  server.setOutputChannelGainDb(rightIndex, 0);
}

const handlePlayNext = () => {
  const uuid = effectiveNextUuid.value;
  if (!uuid) return;
  const item = findItemByUuid(uuid);
  if (!item) return;
  if (nextItemOverrideUuid.value) setNextItem(null);
  if (item.type === 'audio') playCue(item as AudioItem);
  else if (item.type === 'group') triggerGroup(item);
};
</script>

<style scoped>
.playback-controls {
  height: var(--playback-controls-height);
  border-bottom: 1px solid var(--color-border);
  display: flex;
  align-items: center;
  gap: var(--spacing-lg);
  padding: 0 var(--spacing-lg);
  background-color: var(--color-surface);
}

.controls-left {
  display: flex;
  gap: var(--spacing-sm);
}

/* Show Mode — bigger GO / Stop-All buttons. The controls bar grows a little
   taller to fit them; preview card and stop button are also enlarged. */
.playback-controls.show-mode {
  min-height: calc(var(--playback-controls-height) + 20px);

  .control-btn {
    padding: var(--spacing-lg) var(--spacing-xl);
    font-size: 17px;

    .material-symbols-rounded,
    .icon {
      font-size: 26px;
    }
  }

  .preview-cue-card {
    min-width: 500px;
    max-width: 500px;
    padding: var(--spacing-md);
  }

  .preview-cue-header {
    font-size: 16px;
  }

  .preview-stop-btn {
    width: 32px;
    height: 32px;
    font-size: 24px;
  }
}

.control-btn {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  padding: var(--spacing-md) var(--spacing-lg);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  font-weight: 500;
  
  &:hover:not(:disabled) {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
  }
  
  &:disabled {
    opacity: 0.5;
  }
}

.play-next-btn {
  color: var(--color-text-secondary);

  &.has-next {
    background-color: var(--color-warning);
    border-color: var(--color-warning);
    color: black;
    font-weight: 600;

    &:hover:not(:disabled) {
      background-color: var(--color-warning);
      border-color: var(--color-warning);
      filter: brightness(0.88);
    }
  }
}

.panic-btn {
  background-color: var(--color-danger);
  border-color: var(--color-danger);
  color: white;
  font-weight: 600;

  &:hover:not(:disabled) {
    background-color: var(--color-danger);
    border-color: var(--color-danger);
    filter: brightness(0.85);
  }
}

.icon {
  font-size: 20px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.active-cues {
  flex: 1;
  min-width: 0;
  overflow-x: auto;
  overflow-y: hidden;
  padding: var(--spacing-sm) 0;
}

.no-cues {
  color: var(--color-text-secondary);
  font-style: italic;
  padding: var(--spacing-md);
}

.cue-list {
  display: flex;
  flex-direction: row;
  gap: var(--spacing-sm);
}

/* Preview card — same card dimensions and visual structure as ActiveCueItem,
   with a green "Preview" pill prefixing the name. */
.preview-cue-card {
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  padding: var(--spacing-sm) var(--spacing-md);
  min-width: 400px;
  max-width: 400px;
  display: flex;
  gap: var(--spacing-sm);
}

.preview-cue-content {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
}

.preview-cue-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacing-sm);
}

.preview-cue-name {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  font-weight: 500;
  flex: 1;
  min-width: 0;
  color: var(--color-text-primary);
  overflow: hidden;
  white-space: nowrap;
  mask-image: linear-gradient(to right, black 80%, transparent 100%);
  -webkit-mask-image: linear-gradient(to right, black 80%, transparent 100%);
}

.preview-status-pill {
  display: inline-flex;
  align-items: center;
  padding: 2px 8px;
  border-radius: 2px;
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.04em;
  background-color: var(--color-success);
  color: black;
  white-space: nowrap;
  flex-shrink: 0;
}

.preview-cue-actions {
  display: flex;
  gap: 4px;
  flex-shrink: 0;
}

.preview-stop-btn {
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background-color: var(--color-danger);
  color: white;
  font-size: 20px;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  border: none;

  &:hover {
    opacity: 0.8;
  }
}

.output-meters {
  display: flex;
  flex-direction: row;
  align-items: stretch;
  gap: var(--spacing-sm);
  padding-left: var(--spacing-md);
  border-left: 2px solid var(--color-border);
  height: calc(var(--playback-controls-height) - 16px);
  flex-shrink: 0;
}

.output-pair {
  display: flex;
  flex-direction: row;
  align-items: stretch;
  gap: 4px;
}


.preview-cue-meter {
  display: flex;
  align-items: stretch;
  padding-left: var(--spacing-sm);
  border-left: 1px solid var(--color-border);
}

.preview-cue-progress {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
}

.preview-time-info {
  display: flex;
  justify-content: space-between;
  font-size: 12px;
  color: var(--color-text-secondary);
}

.preview-progress-bar {
  height: 8px;
  background-color: var(--color-surface);
  border-radius: var(--border-radius-sm);
  position: relative;
  cursor: pointer;
  direction: ltr;

  &:hover .preview-progress-handle {
    opacity: 1;
  }
}

.preview-progress-fill {
  height: 100%;
  background-color: var(--color-success);
  border-radius: var(--border-radius-sm);
  transition: width 100ms linear;
}

.preview-progress-handle {
  position: absolute;
  top: 50%;
  transform: translate(-50%, -50%);
  width: 16px;
  height: 16px;
  background-color: white;
  border: 2px solid var(--color-success);
  border-radius: 50%;
  opacity: 0;
  transition: opacity var(--transition-fast);
  pointer-events: none;
}
.next-in-badge {
  margin-left: 4px;
  padding: 1px 6px;
  border-radius: 999px;
  background: var(--color-accent);
  color: var(--color-background);
  font-size: 11px;
  font-weight: 600;
  font-variant-numeric: tabular-nums;
}
</style>
