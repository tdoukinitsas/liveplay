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
      <div v-if="activeCues.size === 0" class="no-cues">
        {{ t('playback.noActiveCues') }}
      </div>
      
      <div v-else class="cue-list">
        <ActiveCueItem
          v-for="[uuid, cue] in Array.from(activeCues.entries())"
          :key="uuid"
          :cue="cue"
        />
      </div>
    </div>
    
    <!-- Master Mix Meter — live, from the C++ server's master channel 0/1.
         Replaces the legacy static-waveform "cheat" meter. -->
    <div class="master-meter">
      <div class="master-label">MIX</div>
      <div class="master-meter-wrapper">
        <LiveMeterBar source="master" :index="0" :vertical="true" :show-label="true" />
        <LiveMeterBar source="master" :index="1" :vertical="true" :show-label="false" />
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

const { activeCues, panicStop, nextItemOverrideUuid, autoNextItemUuid, setNextItem, playCue, triggerGroup } = useAudioEngine();
const { findItemByUuid } = useProject();
const { playbackMappings } = useCartHotkeys();
const { t } = useLocalization();
const server = useLiveplayServer();

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

.master-meter {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacing-xs);
  padding-left: var(--spacing-md);
  border-left: 2px solid var(--color-border);
  height: 100%;
  justify-content: center;
}

.master-label {
  font-size: 11px;
  font-weight: 600;
  color: var(--color-text-secondary);
  letter-spacing: 0.5px;
  margin-bottom: var(--spacing-xs);
}

.master-meter-wrapper {
  flex: 1;
  display: flex;
  gap: var(--spacing-xs);
  max-height: calc(var(--playback-controls-height) - 40px);
}
</style>
