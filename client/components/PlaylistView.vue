<template>
  <div class="playlist-view">
    <div class="playlist-header">
      <h2>{{ t('playlist.title') }}</h2>
      <div class="playlist-actions">
        <button class="action-btn" @click="handleImport" :disabled="!currentProject">
          <span class="material-symbols-rounded">audio_file</span>
          <span>{{ t('playlist.importAudio') }}</span>
        </button>
        <button class="action-btn youtube-btn" @click="showYouTubeModal = true" :disabled="!currentProject">
          <span class="material-symbols-rounded">youtube_activity</span>
          <span>{{ t('youtube.importFromYouTube') }}</span>
        </button>
        <button class="action-btn" @click="handleAddGroup" :disabled="!currentProject">
          <span class="material-symbols-rounded">folder</span>
          <span>{{ t('playlist.addGroup') }}</span>
        </button>
      </div>
    </div>
    
    <div class="playlist-content" @drop="handleDrop" @dragover.prevent>
      <div v-if="currentProject?.items.length === 0" class="empty-state">
        <p>{{ t('playlist.noItems') }}</p>
        <p class="hint">{{ t('playlist.importHint') }}</p>
      </div>
      
      <div v-else class="item-list">
        <PlaylistItem
          v-for="item in visibleItems"
          :key="item.uuid"
          :item="item"
          :depth="0"
        />
        <!-- Placeholder row so the user sees mounting is in progress while
             the remaining items hydrate across the next few frames. -->
        <div v-if="visibleItems.length < currentProject.items.length"
             class="item-list-progress">
          {{ t('common.loading') }} ({{ visibleItems.length }} / {{ currentProject.items.length }})
        </div>
      </div>
    </div>

    <!-- YouTube Import Modal -->
    <YouTubeImportModal :isOpen="showYouTubeModal" @close="showYouTubeModal = false" />

    <!-- Audio Import Modal — server browse + native upload -->
    <AudioImportModal :open="showImportModal"
                      @pick="onImportPick"
                      @close="showImportModal = false" />
  </div>
</template>

<script setup lang="ts">
import { v4 as uuidv4 } from 'uuid';
import { ref } from 'vue';
import YouTubeImportModal from './YouTubeImportModal.vue';
import AudioImportModal from './AudioImportModal.vue';
import { triggerRef } from 'vue';
import type { AudioItem, GroupItem } from '~/types/project';
import { DEFAULT_AUDIO_ITEM, DEFAULT_GROUP_ITEM } from '~/types/project';

const { currentProject, addItem, updateIndices, saveProject, triggerWaveformUpdate, isLoading, getAllItemsFlat } = useProject();
const { t } = useLocalization();

const showYouTubeModal = ref(false);
const showImportModal  = ref(false);

// ---------------------------------------------------------------------------
// Progressive mount.
// ---------------------------------------------------------------------------
// <PlaylistItem> is a heavy component (~1000 LOC, observers, refs). Mounting
// hundreds of them in a single tick freezes the renderer for hundreds of ms
// after the project header arrives. We mount in chunks across animation
// frames so the workspace stays interactive while the rest hydrate.
//
// First batch is sized to fill a typical viewport (~25 rows). Subsequent
// batches are larger (50/RAF) since by then the user is already looking at
// rendered content — they tolerate background work better than initial blank.
const INITIAL_RENDER  = 25;
const RENDER_INCREMENT = 50;
const renderLimit = ref(INITIAL_RENDER);

const visibleItems = computed(() => {
  const all = currentProject.value?.items ?? [];
  return renderLimit.value >= all.length ? all : all.slice(0, renderLimit.value);
});

// Whenever the project changes (open/new/close), reset the mount window and
// kick off the progressive expansion.
let raf: number | null = null;
function scheduleMoreItems() {
  if (raf !== null) return;
  raf = requestAnimationFrame(() => {
    raf = null;
    const total = currentProject.value?.items.length ?? 0;
    if (renderLimit.value >= total) return;
    renderLimit.value = Math.min(total, renderLimit.value + RENDER_INCREMENT);
    if (renderLimit.value < total) scheduleMoreItems();
  });
}

// React to (a) new project loaded, (b) streamed pages appended, (c) user adds.
watch(
  () => currentProject.value?.items.length ?? 0,
  (total) => {
    if (total === 0) { renderLimit.value = INITIAL_RENDER; return; }
    if (renderLimit.value < total) scheduleMoreItems();
  },
  { immediate: true },
);

onUnmounted(() => {
  if (raf !== null) { cancelAnimationFrame(raf); raf = null; }
});

// When a project finishes loading (or is already loaded on mount), queue
// waveform generation for every audio item that doesn't have waveform data.
// { immediate: true } means it also fires on mount so items loaded before
// this component mounted (e.g. via tryRejoinExistingProject) are covered.
watch(isLoading, async (loading) => {
  if (loading || !currentProject.value) return;
  try {
    const server = (await import('~/composables/useLiveplayServer')).useLiveplayServer();
    for (const item of getAllItemsFlat()) {
      if (item.type === 'audio') {
        const ai = item as AudioItem;
        if (ai.mediaServerPath && !ai.waveform) {
          server.requestWaveformGeneration(ai.mediaServerPath, ai.uuid).catch(() => {});
        }
      }
    }
  } catch (e) {
    console.warn('[waveform] project-load waveform generation failed:', e);
  }
}, { immediate: true });

const handleImport = () => {
  if (!currentProject.value) return;
  showImportModal.value = true;
};

// Called once per file the user selected in the modal (server browse or upload).
// The path is always a server-side absolute path at this point.
const onImportPick = async (serverPath: string) => {
  await importFromServerPath(serverPath);
};

// Import a file that is already on (or accessible from) the server.
// 1. Copy the source into the project's media root so the project owns the file.
// 2. Fetch duration metadata.
// 3. Add the AudioItem immediately (no waveform yet).
// 4. Queue async waveform generation — result arrives via 'waveform_ready' WS.
const importFromServerPath = async (serverPath: string) => {
  if (!currentProject.value) return;
  try {
    const server = (await import('~/composables/useLiveplayServer')).useLiveplayServer();

    // Copy file to the project's media root (no-op if already there).
    let destPath = serverPath;
    try {
      destPath = await server.copyToMedia(serverPath);
    } catch (e) {
      console.warn('[import] copyToMedia failed, using original path:', e);
    }

    const fileName = destPath.split(/[\\/]/).pop() || 'audio';
    const uuid = uuidv4();

    // Best-effort metadata — gives duration without a full decode on the client.
    let duration = 0;
    try {
      const md: any = await server.fetchMetadata(destPath);
      if (md && typeof md.duration_ms === 'number') duration = md.duration_ms / 1000;
    } catch (e) {
      console.warn('[import] fetchMetadata failed, falling back to 0 duration:', e);
    }

    const audioItem: AudioItem = {
      ...DEFAULT_AUDIO_ITEM,
      uuid,
      index: [currentProject.value.items.length],
      displayName: fileName.replace(/\.[^/.]+$/, ''),
      type: 'audio',
      mediaFileName: fileName,
      mediaPath: `media/${fileName}`,
      mediaServerPath: destPath,
      waveformPath: `${currentProject.value.folderPath}/waveforms/${uuid}.json`,
      waveform: undefined,
      outPoint: duration,
      duration,
    } as AudioItem;

    addItem(audioItem);

    // Queue waveform generation — server responds via 'waveform_ready' doc_patch.
    server.requestWaveformGeneration(destPath, uuid).catch(e => {
      console.warn(`[waveform] generation request failed for ${audioItem.displayName}:`, e);
    });
  } catch (e) {
    console.error('Error importing from server path:', e);
  }
};

const importAudioFile = async (sourcePath: string) => {
  if (!currentProject.value) return;

  try {
    const fileName = sourcePath.split(/[\\/]/).pop() || 'audio.mp3';
    const uuid = uuidv4();
    const destPath = `${currentProject.value.folderPath}/media/${fileName}`;
    
    // Copy file to media folder
    const copyResult = await window.electronAPI.copyFile(sourcePath, destPath);
    if (!copyResult.success) {
      console.error('Failed to copy file:', copyResult.error);
      return;
    }

    // Get audio duration
    const duration = await getAudioDuration(destPath);

    // Create audio item WITHOUT waveform (will be generated async via ffmpeg)
    const audioItem: AudioItem = {
      ...DEFAULT_AUDIO_ITEM,
      uuid,
      index: [currentProject.value.items.length],
      displayName: fileName.replace(/\.[^/.]+$/, ''), // Remove extension
      type: 'audio',
      mediaFileName: fileName,
      mediaPath: `media/${fileName}`, // Store relative path to project folder
      waveformPath: `${currentProject.value.folderPath}/waveforms/${uuid}.json`,
      waveform: undefined, // Will be generated asynchronously
      outPoint: duration,
      duration
    } as AudioItem;

    // Add item immediately (no blocking)
    addItem(audioItem);
    
    // Generate waveform asynchronously using ffmpeg
    generateWaveformAsync(audioItem);
  } catch (error) {
    console.error('Error importing audio:', error);
  }
};

const generateWaveformAsync = async (item: AudioItem) => {
  try {
    if (!currentProject.value) return;

    // Non-Electron / server mode: skip the file-system checks and go straight
    // to the server waveform endpoint.
    if (!window.electronAPI) {
      if (item.mediaServerPath) {
        try {
          const server = (await import('~/composables/useLiveplayServer')).useLiveplayServer();
          const serverWf = await server.fetchWaveformByPath(item.mediaServerPath);
          const peaks = serverWf.channels[0]?.peak ?? [];
          const duration = serverWf.duration_ms / 1000;
          if (peaks.length > 0) {
            item.waveform = { peaks, length: peaks.length, duration };
            if (duration > 0) { item.duration = duration; item.outPoint = duration; }
            triggerWaveformUpdate();
            console.log(`[waveform] server: ${item.displayName} — ${peaks.length} buckets, ${duration.toFixed(2)}s`);
          }
        } catch (e) {
          console.warn(`[waveform] server fetch failed for ${item.displayName}:`, e);
        }
      }
      return;
    }

    // Electron path: check for a cached waveform file first.
    // Ensure waveforms directory exists
    const waveformsDir = `${currentProject.value.folderPath}/waveforms`;
    await window.electronAPI.ensureDirectory(waveformsDir);

    // Check if waveform file already exists and is valid
    const existingWaveform = await window.electronAPI.readFile(item.waveformPath);
    if (existingWaveform.success && existingWaveform.data) {
      try {
        const waveformData = JSON.parse(existingWaveform.data);

        // Validate waveform format (duration field is optional now)
        if (waveformData.peaks && waveformData.peaks.length > 0) {
          item.waveform = waveformData;

          // Update duration from waveform data if available (more accurate than Audio API)
          if (waveformData.duration && waveformData.duration > 0) {
            item.duration = waveformData.duration;
            item.outPoint = waveformData.duration;
          }

          triggerWaveformUpdate();
          console.log(`Existing waveform loaded for ${item.displayName}`);
          return;
        }
        console.warn('Invalid waveform format, regenerating...');
      } catch (e) {
        console.warn('Failed to parse existing waveform, regenerating...');
      }
    }

    // Check if generateWaveform is available (Electron path)
    if (!window.electronAPI?.generateWaveform) {
      // Server fallback: use /api/waveform_path if this item has a server path.
      // The fetch is async so the UI is never blocked; the waveform appears when ready.
      if (item.mediaServerPath) {
        try {
          const server = (await import('~/composables/useLiveplayServer')).useLiveplayServer();
          const serverWf = await server.fetchWaveformByPath(item.mediaServerPath);
          // Flatten multi-channel peaks to a single array (use ch0, fall back to empty)
          const peaks = serverWf.channels[0]?.peak ?? [];
          const duration = serverWf.duration_ms / 1000;
          if (peaks.length > 0) {
            item.waveform = { peaks, length: peaks.length, duration };
            if (duration > 0) {
              item.duration = duration;
              item.outPoint = duration;
            }
            triggerWaveformUpdate();
            console.log(`[waveform] server: ${item.displayName} — ${peaks.length} buckets, ${duration.toFixed(2)}s`);
          }
        } catch (e) {
          console.warn(`[waveform] server fetch failed for ${item.displayName}:`, e);
        }
      } else {
        console.warn('[waveform] no Electron API and no mediaServerPath — skipping waveform generation');
      }
      return;
    }

    // Generate waveform using ffmpeg (non-blocking)
    const mediaPath = `${currentProject.value.folderPath}/media/${item.mediaFileName}`;
    const result = await window.electronAPI.generateWaveform(mediaPath, item.waveformPath);

    if (result.success) {
      console.log(`Started waveform generation for ${item.displayName}`);

      // Start polling for waveform file (check every 2 seconds)
      const pollInterval = setInterval(async () => {
        try {
          const waveformFile = await window.electronAPI.readFile(item.waveformPath);
          if (waveformFile.success && waveformFile.data) {
            const waveformData = JSON.parse(waveformFile.data);

            // Validate waveform format (duration field is optional)
            if (waveformData.peaks && waveformData.peaks.length > 0) {
              item.waveform = waveformData;

              // Update duration from waveform data if available (more accurate than Audio API)
              if (waveformData.duration && waveformData.duration > 0) {
                item.duration = waveformData.duration;
                item.outPoint = waveformData.duration;
              }

              // Force Vue reactivity update
              triggerWaveformUpdate();

              // Stop polling once loaded
              clearInterval(pollInterval);
              console.log(`Waveform loaded for ${item.displayName} (${waveformData.peaks.length} peaks, ${waveformData.duration?.toFixed(2)}s)`);
            }
          }
        } catch (error) {
          console.error('Error polling for waveform:', error);
        }
      }, 2000);

      // Stop polling after 30 seconds to prevent infinite polling
      setTimeout(() => {
        clearInterval(pollInterval);
      }, 30000);
    } else {
      console.error('Failed to generate waveform:', result.error);
    }
  } catch (error) {
    console.error('Error generating waveform:', error);
  }
};

const getAudioDuration = async (filePath: string): Promise<number> => {
  // Simplified - would use proper audio decoding
  return new Promise((resolve) => {
    if (import.meta.client) {
      const audio = new Audio(`file://${filePath}`);
      audio.addEventListener('loadedmetadata', () => {
        resolve(audio.duration);
      });
      audio.addEventListener('error', () => {
        resolve(60); // Default fallback
      });
    } else {
      resolve(60);
    }
  });
};

const handleAddGroup = () => {
  if (!currentProject.value) return;

  const groupItem: GroupItem = {
    ...DEFAULT_GROUP_ITEM,
    uuid: uuidv4(),
    index: [currentProject.value.items.length],
    displayName: 'New Group',
    type: 'group',
    children: [] // Create a new array for each group to avoid shared references
  } as GroupItem;

  addItem(groupItem);
};

const handleDrop = async (e: DragEvent) => {
  e.preventDefault();
  
  if (!e.dataTransfer) return;
  
  const files = Array.from(e.dataTransfer.files);
  const audioFiles = files.filter(file => 
    /\.(mp3|wav|ogg|flac|m4a|aac)$/i.test(file.name)
  );

  for (const file of audioFiles) {
    // Get the file path using webUtils in Electron
    if (window.electronAPI && window.electronAPI.getFilePath) {
      const filePath = window.electronAPI.getFilePath(file);
      if (filePath) {
        await importAudioFile(filePath);
      }
    }
  }
};
</script>

<style scoped>
.playlist-view {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  background-color: var(--color-background);
}

.playlist-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-md) var(--spacing-lg);
  min-height: 56px;
  box-sizing: border-box;
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
}

.playlist-header h2 {
  font-size: 18px;
  font-weight: 600;
}

.playlist-actions {
  display: flex;
  gap: var(--spacing-sm);
}

.action-btn {
  padding: var(--spacing-sm) var(--spacing-md);
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  font-size: 13px;
  display: flex;
  align-items: center;
  gap: 6px;
  cursor: pointer;
  transition: all 0.2s;
  
  &:hover:not(:disabled) {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
  }
  
  &:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
}

.youtube-btn {
  background: linear-gradient(135deg, #FF0000 0%, #CC0000 100%);
  color: white;
  border-color: #FF0000;
  
  &:hover:not(:disabled) {
    background: linear-gradient(135deg, #CC0000 0%, #990000 100%);
    border-color: #CC0000;
  }
  
  &:disabled {
    background: linear-gradient(135deg, #666666 0%, #555555 100%);
    border-color: #666666;
  }
}

.playlist-content {
  flex: 1;
  overflow-y: auto;
  padding: var(--spacing-md);
}

.empty-state {
  text-align: center;
  padding: var(--spacing-xxl);
  color: var(--color-text-secondary);
  
  p {
    margin-bottom: var(--spacing-sm);
  }
  
  .hint {
    font-size: 13px;
    font-style: italic;
  }
}

.item-list {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
}

.item-list-progress {
  padding: var(--spacing-sm) var(--spacing-md);
  font-size: 12px;
  color: var(--color-text-secondary);
  text-align: center;
  opacity: 0.7;
}
</style>
