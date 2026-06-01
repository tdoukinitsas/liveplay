<template>
  <div class="playback-controls">
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
      </button>
      <button class="control-btn panic-btn" @click="handlePanic" :disabled="activeCues.size === 0" :title="stopAllTooltip">
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
             "Preview" pill at the start of the name. Meter reads master
             channels 30/31 (the preview output bus). Seek + time come from
             the per-cue meter stream (playhead_seconds). -->
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
            <StereoMeter :left-index="30" :right-index="31" :min-db="-60" :max-db="0" />
          </div>
        </div>

        <ActiveCueItem
          v-for="[uuid, cue] in Array.from(activeCues.entries())"
          :key="uuid"
          :cue="cue"
        />
      </div>
    </div>
    
    <!-- Per-output meters — one StereoMeter + volume fader per active audio output pair.
         Main output (masters 0/1) is always shown. Preview (30/31) and
         device-override pairs (2+) appear when they carry signal. -->
    <div class="output-meters">
      <div v-for="pair in outputPairs" :key="pair.key" class="output-pair">
        <StereoMeter
          :left-index="pair.leftIndex"
          :right-index="pair.rightIndex"
          :label="pair.label"
          :show-peak-value="true"
        />
        <VolumeSlider
          :db="getOutputGainDb(pair.leftIndex)"
          :min-db="-60"
          :max-db="40"
          :title="pair.label"
          @input="(db: number) => onOutputGainInput(pair.leftIndex, pair.rightIndex, db)"
          @reset="resetOutputGain(pair.leftIndex, pair.rightIndex)"
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
//   * The master mix meter is rendered by <LiveMeterBar source="master"
//     :index="0" />, which consumes the live WebSocket meter stream from
//     the engine — replacing the static-waveform "cheat" levels.
import { formatKeyLabel } from '~/composables/useCartHotkeys';
import type { AudioItem } from '~/types/project';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import { useCueMeters } from '~/composables/useLiveMeters';
import VolumeSlider from './VolumeSlider.vue';

const { activeCues, panicStop, nextItemOverrideUuid, autoNextItemUuid, setNextItem, playCue, triggerGroup } = useAudioEngine();
const { findItemByUuid, previewItemUuid, previewCueId, stopPreview, currentProject } = useProject();
const { playbackMappings } = useCartHotkeys();
const { t } = useLocalization();
const server = useLiveplayServer();

// ---- Preview seek / time --------------------------------------------------
// Subscribe to the preview cue's per-item meter stream so we can display an
// accurate playhead, elapsed time, and remaining time in the preview card.
const previewMeter = useCueMeters(() => previewCueId.value || null);
const previewCurrentTime = computed(() => previewMeter.playhead.value);
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

// Dynamic per-output meters. Main (0/1) is always shown. Preview (30/31)
// and per-device overrides (2+) appear only when they carry signal so we
// don't flood the UI with silent meters.
const outputPairs = computed(() => {
  const m = server.meters;
  const activeIdx = new Set((m?.master_channels ?? []).map((mc: any) => mc.index as number));

  const configuredId = (currentProject.value as any)?.settings?.defaultOutputDevice;
  const mainLabel = configuredId
    ? (server.devices.find((d: any) => d.id === configuredId)?.display_name ?? 'Main')
    : (server.devices.find((d: any) => d.is_default)?.display_name ?? 'Main');
  const pairs: Array<{ key: string; leftIndex: number; rightIndex: number; label: string }> = [];

  // Main output — always visible
  pairs.push({ key: 'main', leftIndex: 0, rightIndex: 1, label: mainLabel });

  // Per-device override pairs (allocated at 2+, step 2)
  for (let i = 2; i < 30; i += 2) {
    if (activeIdx.has(i) || activeIdx.has(i + 1)) {
      pairs.push({ key: `out-${i}`, leftIndex: i, rightIndex: i + 1, label: `Out ${i / 2}` });
    }
  }

  // Preview output (master 30/31) — only when active
  if (activeIdx.has(30) || activeIdx.has(31)) {
    pairs.push({ key: 'preview-out', leftIndex: 30, rightIndex: 31, label: 'Preview' });
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
  // Tell the server to stop everything immediately (no fade); also stop the
  // legacy in-process engine while it's still in use.
  server.stopAll(0);
  panicStop();
};

// ---- Per-output gain faders -----------------------------------------------
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
</style>
