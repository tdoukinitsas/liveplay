<template>
  <div 
    class="cart-slot"
    ref="slotRef"
    :class="{
      'has-item': hasItem,
      'is-playing': isPlaying,
      'is-selected': isSelected,
      'warning-yellow': warningState === 'yellow',
      'warning-orange': warningState === 'orange',
      'warning-red': warningState === 'red',
      'drag-over': isDragOver
    }"
    :style="slotStyle"
  >
    <!-- Audio Import Modal — server browse + upload, same as PlaylistView -->
    <AudioImportModal :open="showImportModal"
                      @pick="onImportPick"
                      @close="showImportModal = false" />

    <div v-if="!hasItem" class="empty-slot" @click="handleImport">
      <span class="slot-number">{{ slot + 1 }}</span>
      <span v-if="keyLabel" class="key-label">{{ keyLabel }}</span>
      <span class="slot-hint">{{ t('cart.clickToImport') }}</span>
    </div>
    
    <div
      v-else
      class="slot-content"
      draggable="true"
      @dragstart="handleDragStart"
      @dragend="handleDragEnd"
    >
      <!-- Waveform canvas -->
      <canvas
        v-if="item.type === 'audio' && item.waveform"
        ref="waveformCanvas"
        class="cart-waveform-canvas"
      ></canvas>

      <!-- Progress overlay -->
      <div v-if="isPlaying" class="cart-progress" :style="progressStyle"></div>

      <!-- Item info section -->
      <div class="slot-header" @click="handleSelect($event)">
        <span class="slot-number">{{ slot + 1 }}</span>
        <span class="slot-name" :class="{ 'is-peaking': isPeaking }">{{ item.displayName }}</span>
        <span
          v-if="isPeaking"
          class="material-symbols-rounded peak-warning-icon"
          :title="t('properties.peakWarning')"
          draggable="false"
          @click.stop
        >bomb</span>
        <span v-if="isPlaying" class="status-pill playing">{{ t('status.playing') }}</span>
        <span v-else-if="isQueuedNext" class="status-pill up-next">{{ t('status.upNext') }}</span>
        <span v-if="isPreviewing" class="status-pill preview">{{ t('status.previewing') }}</span>
        <span v-if="keyLabel" class="key-label">{{ keyLabel }}</span>
      </div>
      
      <!-- Waveform/Progress section at bottom -->
      <div class="slot-waveform-area">
        <!-- Progress info 
        <div v-if="isPlaying" class="slot-time-info">
          <span>{{ formatTime(currentTime) }}</span>
          <span>-{{ formatTime(duration - currentTime) }}</span>
          
        </div>
        -->
      </div>
      
      <!-- Bottom info bar with action buttons, behavior icons, and duration -->
      <div class="slot-footer">
        <!-- Action buttons (always visible) -->
        <div class="slot-actions">
          <ActionButton
            v-if="item.type === 'audio'"
            :icon="'headphones'"
            :highlight-color="isPreviewing ? 'var(--color-accent)' : 'var(--color-success)'"
            :is-active="isPreviewing"
            :class="{ 'no-device': !hasPreviewDevice }"
            context="Cart"
            @click.stop="isPreviewing ? handleStopPreview() : handleStartPreview()"
            :title="isPreviewing ? t('actions.stopPreview') : (hasPreviewDevice ? t('actions.preview') : t('actions.previewNoDevice'))"
          />
          <ActionButton
            :icon="isPlaying ? 'stop' : 'play_arrow'"
            :highlight-color="isPlaying ? 'var(--color-danger)' : 'var(--color-success)'"
            context="Cart"
            @click.stop="isPlaying ? handleStop() : handlePlay()"
            :title="isPlaying ? t('actions.stop') : t('actions.play')"
          />
          <ActionButton
            icon="fast_forward"
            highlight-color="var(--color-warning)"
            active-text-color="black"
            :is-active="isManuallyQueued"
            context="Cart"
            @click.stop="handleSetAsNext"
            :title="t('actions.setAsNext')"
          />
          <ActionButton
            icon="settings"
            highlight-color="var(--color-accent)"
            context="Cart"
            @click.stop="handleEdit"
            :title="t('actions.edit')"
          />
          <ActionButton
            icon="delete"
            highlight-color="var(--color-danger)"
            context="Cart"
            @click.stop="handleDelete"
            :title="t('actions.remove')"
          />
        </div>
        
        <!-- Behavior indicators and duration -->
        <div class="slot-info">
          <!-- Behavior indicators -->
          <div class="behavior-indicators">
            <!-- Start behavior -->
            <span
              v-if="item.startBehavior?.action === 'play-next'"
              class="material-symbols-rounded behavior-icon"
              :title="t('behaviors.startPlayNext')"
            >skip_next</span>
            <span
              v-else-if="item.startBehavior?.action === 'play-item'"
              class="material-symbols-rounded behavior-icon"
              :title="t('behaviors.startPlayItem')"
            >arrow_forward</span>
            <span
              v-else-if="item.startBehavior?.action === 'play-index'"
              class="material-symbols-rounded behavior-icon"
              :title="t('behaviors.startPlayIndex')"
            >arrow_forward</span>

            <!-- Ducking behavior -->
            <span
              v-if="item.duckingBehavior?.mode === 'duck-others'"
              class="material-symbols-rounded behavior-icon"
              :title="t('behaviors.duckingOthers')"
            >volume_down</span>

            <!-- End behavior -->
            <span
              v-if="item.endBehavior?.action === 'next'"
              class="material-symbols-rounded behavior-icon"
              :title="t('behaviors.endPlayNext')"
            >skip_next</span>
            <span
              v-else-if="item.endBehavior?.action === 'goto-item'"
              class="material-symbols-rounded behavior-icon"
              :title="t('behaviors.endGotoItem')"
            >arrow_forward</span>
            <span
              v-else-if="item.endBehavior?.action === 'goto-index'"
              class="material-symbols-rounded behavior-icon"
              :title="t('behaviors.endGotoIndex')"
            >arrow_forward</span>
            <span
              v-else-if="item.endBehavior?.action === 'loop'"
              class="material-symbols-rounded behavior-icon"
              :title="t('behaviors.endLoop')"
            >replay</span>
          </div>
          
          <!-- Duration -->
          <span class="slot-duration">{{ isPlaying ? "-" + formatTime(duration - currentTime) : formatDuration(item) }}</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { triggerRef } from 'vue';
import { v4 as uuidv4 } from 'uuid';
import type { AudioItem } from '~/types/project';
import ActionButton from './ActionButton.vue';
import AudioImportModal from './AudioImportModal.vue';
import { useOutputTarget, METER_COLORS } from '~/composables/useOutputTarget';
import { calculatePerceivedLoudness } from '~/utils/audio';

const props = defineProps<{
  slot: number;
  item: AudioItem | null;
  keyLabel?: string;
}>();

const slotRef = ref<HTMLElement | null>(null);
const showImportModal = ref(false);

const { currentProject, selectedItem, selectedItems, selectionContext, requestDeleteFromButton, findItemByUuid, triggerWaveformUpdate, markPendingAutoProcess } = useProject();
const { levels: outputTargetLevels } = useOutputTarget();
const { playCue, stopCue, activeCues, nextItemOverrideUuid, autoNextItemUuid, setNextItem } = useAudioEngine();
const { t } = useLocalization();
const { addCartOnlyItem, updateCartOnlyItem, removeCartOnlyItem } = useCartItems();

const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const currentTime = ref(0);
const duration = ref(0);
const playbackProgress = ref(0);
const warningState = ref<'yellow' | 'orange' | 'red' | null>(null);
const isDragOver = ref(false);

const hasItem = computed(() => props.item !== null);

const isPeaking = computed(() => {
  if (!props.item || props.item.type !== 'audio') return false;
  const peaks = props.item.waveform?.peaks;
  if (!peaks || peaks.length === 0) return false;

  const duration = props.item.duration || 0;
  const inPoint  = props.item.inPoint  || 0;
  const outPoint = props.item.outPoint || duration;
  const startIdx = duration > 0 ? Math.floor((inPoint  / duration) * peaks.length) : 0;
  const endIdx   = duration > 0 ? Math.ceil ((outPoint / duration) * peaks.length) : peaks.length;

  const intrinsicLoudness = calculatePerceivedLoudness(peaks, startIdx, endIdx);
  const volume = props.item.volume ?? 1;
  const volumeDb = volume > 0 ? 20 * Math.log10(volume) : -60;
  const effectiveLoudness = intrinsicLoudness + volumeDb;

  return effectiveLoudness > outputTargetLevels.value.autoVolumeTargetDb + 3;
});
const isPlaying = computed(() => props.item ? activeCues.value.has(props.item.uuid) : false);
const isSelected = computed(() => props.item ? selectedItems.value.has(props.item.uuid) : false);
const isManuallyQueued = computed(() => props.item ? nextItemOverrideUuid.value === props.item.uuid : false);
const isQueuedNext = computed(() => {
  if (!props.item) return false;
  if (nextItemOverrideUuid.value) return nextItemOverrideUuid.value === props.item.uuid;
  return autoNextItemUuid.value === props.item.uuid;
});

// Helper to convert hex to rgba
const hexToRgba = (hex: string, alpha: number) => {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
};

const slotStyle = computed(() => {
  if (!props.item) return {};
  
  const styles: any = {
    borderColor: props.item.color
  };
  
  if (isPlaying.value) {
    styles.backgroundColor = hexToRgba(props.item.color, 0.3);
  } else {
    styles.backgroundColor = hexToRgba(props.item.color, 0.15);
  }
  
  return styles;
});

const progressStyle = computed(() => {
  if (!props.item) return {};
  return {
    width: `${playbackProgress.value}%`,
    backgroundColor: hexToRgba(props.item.color, 0.5),
  };
});

// Watch for playback
let progressInterval: any = null;
watch(isPlaying, (playing) => {
  if (playing && props.item) {
    const cue = activeCues.value.get(props.item.uuid);
    if (cue) {
      duration.value = cue.duration;
      progressInterval = setInterval(() => {
        if (!props.item) return;
        const cue = activeCues.value.get(props.item.uuid);
        if (cue) {
          currentTime.value = cue.currentTime;
          duration.value = cue.duration;
          playbackProgress.value = duration.value > 0 ? (currentTime.value / duration.value) * 100 : 0;
          
          // Update warning state based on time remaining
          const timeRemaining = duration.value - currentTime.value;
          if (timeRemaining <= 5) {
            warningState.value = 'red';
          } else if (timeRemaining <= 10) {
            warningState.value = 'orange';
          } else if (timeRemaining <= 30) {
            warningState.value = 'yellow';
          } else {
            warningState.value = null;
          }
        }
      }, 100);
    }
  } else {
    if (progressInterval) {
      clearInterval(progressInterval);
      progressInterval = null;
    }
    playbackProgress.value = 0;
    currentTime.value = 0;
    warningState.value = null;
  }
});

onUnmounted(() => {
  if (progressInterval) {
    clearInterval(progressInterval);
  }
});

const handleImport = () => {
  if (!currentProject.value) return;
  showImportModal.value = true;
};

// Called by AudioImportModal when the user picks file(s) (server paths).
// A cart slot holds a single item, so only the first selection is used.
const onImportPick = async (serverPaths: string | string[]) => {
  const first = Array.isArray(serverPaths) ? serverPaths[0] : serverPaths;
  if (!first) return;
  await importFromServerPath(first);
  showImportModal.value = false;
};

// Import a file already on (or uploaded to) the server into this cart slot.
// Mirrors PlaylistView.importFromServerPath but stores the result as a
// cart-only item bound to this slot instead of adding it to the playlist.
const importFromServerPath = async (serverPath: string) => {
  if (!currentProject.value) return;
  try {
    const server = useLiveplayServer();

    // Copy file into the project's media root (no-op if already there).
    let destPath = serverPath;
    try {
      destPath = await server.copyToMedia(serverPath);
    } catch (e) {
      console.warn('[cart import] copyToMedia failed, using original path:', e);
    }

    const fileName = destPath.split(/[\\/]/).pop() || 'audio';
    const uuid = uuidv4();

    let duration = 0;
    try {
      const md: any = await server.fetchMetadata(destPath);
      if (md && typeof md.duration_ms === 'number') duration = md.duration_ms / 1000;
    } catch (e) {
      console.warn('[cart import] fetchMetadata failed, falling back to 0 duration:', e);
    }

    const { DEFAULT_CART_AUDIO_ITEM } = await import('~/types/project');
    const newItem: AudioItem = {
      ...DEFAULT_CART_AUDIO_ITEM,
      uuid,
      type: 'audio' as const,
      displayName: fileName.replace(/\.[^/.]+$/, ''),
      mediaFileName: fileName,
      mediaPath: `media/${fileName}`,
      mediaServerPath: destPath,
      waveformPath: `${currentProject.value.folderPath}/waveforms/${uuid}.json`,
      waveform: undefined,
      duration,
      outPoint: duration,
      index: [-1, props.slot],
    } as AudioItem;

    addCartOnlyItem(newItem);
    // Mark for one-shot auto-process when the waveform arrives.
    markPendingAutoProcess(uuid);

    const existingIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);
    if (existingIndex !== -1) {
      currentProject.value.cartItems[existingIndex].itemUuid = uuid;
      currentProject.value.cartItems[existingIndex].index = [-1, props.slot];
    } else {
      currentProject.value.cartItems.push({ slot: props.slot, itemUuid: uuid, index: [-1, props.slot] });
    }

    const { saveProject } = useProject();
    await saveProject();

    // Queue async waveform generation — result arrives via 'waveform_ready' WS.
    server.requestWaveformGeneration(destPath, uuid).catch(e => {
      console.warn(`[cart waveform] generation failed for ${newItem.displayName}:`, e);
    });
  } catch (error) {
    console.error('Error importing audio to cart:', error);
  }
};


const handleSelect = (event?: MouseEvent) => {
  if (!props.item) return;
  // Stamp the selection as cart-owned so a global keyboard DEL clears cart
  // slots (rather than deleting the underlying playlist items).
  selectionContext.value = 'cart';
  const ctrl = !!(event && (event.ctrlKey || event.metaKey));
  const shift = !!(event && event.shiftKey);

  if (shift && selectedItems.value.size > 0) {
    // Shift-click: select the contiguous range of cart slots between the
    // last selection and this one (ordered by slot number).
    const ordered = [...(currentProject.value?.cartItems ?? [])]
      .sort((a, b) => a.slot - b.slot)
      .map(ci => ci.itemUuid);
    const anchor = Array.from(selectedItems.value).pop();
    const from = ordered.indexOf(anchor ?? '');
    const to = ordered.indexOf(props.item.uuid);
    if (from !== -1 && to !== -1) {
      const [s, e] = from < to ? [from, to] : [to, from];
      for (let i = s; i <= e; i++) selectedItems.value.add(ordered[i]);
    } else {
      selectedItems.value.add(props.item.uuid);
    }
  } else if (ctrl) {
    // Ctrl-click: toggle this slot's item in/out of the selection.
    if (selectedItems.value.has(props.item.uuid)) selectedItems.value.delete(props.item.uuid);
    else selectedItems.value.add(props.item.uuid);
  } else {
    // Plain click: select only this item.
    selectedItems.value.clear();
    selectedItems.value.add(props.item.uuid);
  }

  // Keep the panel anchor in sync (so an open properties panel follows the
  // cart selection), without forcing the panel open.
  if (selectedItems.value.has(props.item.uuid)) {
    selectedItem.value = props.item;
  } else {
    const remaining = Array.from(selectedItems.value).pop();
    selectedItem.value = remaining ? (findItemByUuid(remaining) ?? null) : null;
  }
};

const handlePlay = () => {
  if (!props.item) return;
  playCue(props.item);
};

const handleStop = () => {
  if (!props.item) return;
  stopCue(props.item.uuid);
};

const handleSetAsNext = () => {
  if (!props.item) return;
  if (isManuallyQueued.value) {
    setNextItem(null);
  } else {
    setNextItem(props.item.uuid);
  }
};

const handleDelete = () => {
  if (!currentProject.value || !props.item) return;

  // When this slot's item is part of a multi-selection, defer to the confirm
  // dialog (Delete N Selected / Delete Only this / Cancel), deleting on the
  // cart path so selected slots are unassigned rather than removed from the
  // playlist.
  if (requestDeleteFromButton(props.item.uuid, 'cart')) return;

  // Remove from cart-only items
  removeCartOnlyItem(props.item.uuid);

  // Remove from cart
  const index = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);
  if (index !== -1) {
    currentProject.value.cartItems.splice(index, 1);
    const { saveProject } = useProject();
    saveProject();
  }
};

const handleEdit = () => {
  if (!props.item) return;
  const { openItemProperties } = useProject();
  openItemProperties(props.item.uuid);
};

// ---- Preview (pre-listen) handlers -------------------------------------
// Preview routes the cue through the configured preview device (typically
// headphones) without disturbing main project playback. The previewing
// state is owned by the server (only one preview at a time); the client
// reads it from useProject().previewItemUuid.
const { previewItemUuid, startPreview, stopPreview } = useProject();
const isPreviewing = computed(() =>
  props.item ? previewItemUuid.value === props.item.uuid : false,
);
const hasPreviewDevice = computed(() => !!(currentProject.value as any)?.settings?.previewDevice);
const showProjectSettings = useState('showProjectSettings', () => false);
const handleStartPreview = () => {
  if (!props.item || props.item.type !== 'audio') return;
  if (!hasPreviewDevice.value) {
    showProjectSettings.value = true;
    return;
  }
  startPreview(props.item.uuid);
};
const handleStopPreview = () => {
  stopPreview();
};

const formatTime = (seconds: number): string => {
  const mins = Math.floor(seconds / 60);
  const secs = Math.floor(seconds % 60);
  return `${mins}:${secs.toString().padStart(2, '0')}`;
};

const formatDuration = (item: AudioItem | null): string => {
  if (!item) return '';
  
  // Calculate trimmed duration based on in/out points
  const totalDuration = item.duration;
  const inPoint = item.inPoint || 0;
  const outPoint = item.outPoint || totalDuration;
  const trimmedDuration = outPoint - inPoint;
  
  const totalSeconds = Math.floor(trimmedDuration);
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const secs = totalSeconds % 60;
  
  if (hours > 0) {
    return `${hours}:${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
  } else {
    return `${minutes}:${secs.toString().padStart(2, '0')}`;
  }
};

// Draw waveform
const drawWaveform = () => {
  if (!waveformCanvas.value || !props.item || props.item.type !== 'audio') return;
  
  const audioItem = props.item as AudioItem;
  if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) return;
  
  const canvas = waveformCanvas.value;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  
  // Set canvas size to match element size
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  ctx.scale(dpr, dpr);
  
  ctx.clearRect(0, 0, rect.width, rect.height);

  // Use the item's own colour; opacity:0.3 in CSS gives a natural dark tint.
  ctx.fillStyle = audioItem.color || '#ffffff';

  const peaks = audioItem.waveform.peaks;
  
  // Calculate trimmed region if in/out points are set
  const totalDuration = audioItem.duration;
  const inPoint = audioItem.inPoint || 0;
  const outPoint = audioItem.outPoint || totalDuration;
  const trimmedDuration = outPoint - inPoint;
  
  // Calculate which peaks to show (slice based on in/out ratios)
  const startIndex = Math.floor((inPoint / totalDuration) * peaks.length);
  const endIndex = Math.ceil((outPoint / totalDuration) * peaks.length);
  const trimmedPeaks = peaks.slice(startIndex, endIndex);
  
  const barWidth = rect.width / trimmedPeaks.length;
  const centerY = rect.height / 2;

  trimmedPeaks.forEach((value, i) => {
    const clamped = Math.min(1, Math.max(0, value));
    const shaped = clamped * clamped;
    const barHeight = shaped * rect.height * 0.8;
    const x = i * barWidth;
    const y = centerY - barHeight / 2;

    ctx.fillRect(x, y, Math.max(barWidth, 1), barHeight);
  });
};

// Watch for item changes and redraw
let resizeObserver: ResizeObserver | null = null;
let waveformPollInterval: NodeJS.Timeout | null = null;

onMounted(() => {
  // Use native DOM listeners for drag-and-drop — Vue event handlers
  // on this component don't receive drop events (likely a Vue 3 scoped template issue)
  if (slotRef.value) {
    slotRef.value.addEventListener('dragenter', (e) => {
      e.preventDefault();
    });
    slotRef.value.addEventListener('dragover', (e) => {
      e.preventDefault();
      if (e.dataTransfer) {
        isDragOver.value = true;
        e.dataTransfer.dropEffect = 'move';
      }
    });
    slotRef.value.addEventListener('dragleave', () => {
      isDragOver.value = false;
    });
    slotRef.value.addEventListener('drop', (e) => {
      e.preventDefault();
      handleDrop(e);
    });
  }

  if (props.item && waveformCanvas.value) {
    nextTick(drawWaveform);
    resizeObserver = new ResizeObserver(() => {
      drawWaveform();
    });
    resizeObserver.observe(waveformCanvas.value);
    
    // Start polling for waveform if not available yet
    const audioItem = props.item as AudioItem;
    if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) {
      startWaveformPolling();
    }
  }
});

onUnmounted(() => {
  if (resizeObserver && waveformCanvas.value) {
    resizeObserver.unobserve(waveformCanvas.value);
    resizeObserver.disconnect();
  }
  
  if (waveformPollInterval) {
    clearInterval(waveformPollInterval);
  }
});

// Poll for waveform data if not yet available
const startWaveformPolling = () => {
  if (waveformPollInterval) return; // Already polling
  
  waveformPollInterval = setInterval(async () => {
    if (!props.item || props.item.type !== 'audio') {
      clearInterval(waveformPollInterval!);
      waveformPollInterval = null;
      return;
    }
    
    const audioItem = props.item as AudioItem;
    
    // Check if waveform is now available
    if (audioItem.waveform && audioItem.waveform.peaks && audioItem.waveform.peaks.length > 0) {
      // Waveform loaded, stop polling and redraw
      clearInterval(waveformPollInterval!);
      waveformPollInterval = null;
      nextTick(drawWaveform);
      return;
    }
    
    // Try to load waveform from file
    if (window.electronAPI && audioItem.waveformPath) {
      try {
        const result = await window.electronAPI.readFile(audioItem.waveformPath);
        if (result.success && result.data) {
          const waveformData = JSON.parse(result.data);
          if (waveformData.peaks && waveformData.peaks.length > 0) {
            audioItem.waveform = waveformData;
            
            // Update duration from waveform data if available
            if (waveformData.duration && waveformData.duration > 0) {
              audioItem.duration = waveformData.duration;
              audioItem.outPoint = waveformData.duration;
            }
            
            // Update the cart-only item with waveform data
            updateCartOnlyItem(audioItem.uuid, audioItem);
            
            clearInterval(waveformPollInterval!);
            waveformPollInterval = null;
            
            // Force reactivity update
            triggerWaveformUpdate();
            nextTick(drawWaveform);
            console.log(`Waveform polling found data for cart slot ${props.slot + 1}`);
          }
        }
      } catch (error) {
        // Silently ignore, will retry on next poll
      }
    }
  }, 2000); // Poll every 2 seconds
  
  // Stop polling after 30 seconds
  setTimeout(() => {
    if (waveformPollInterval) {
      clearInterval(waveformPollInterval);
      waveformPollInterval = null;
    }
  }, 30000);
};

watch(() => props.item, (newItem, oldItem) => {
  if (newItem && newItem.type === 'audio') {
    nextTick(drawWaveform);
    
    // Start polling if waveform not available
    const audioItem = newItem as AudioItem;
    if (!audioItem.waveform || !audioItem.waveform.peaks || audioItem.waveform.peaks.length === 0) {
      startWaveformPolling();
    }
  }
}, { deep: true });

const handleDragStart = (e: DragEvent) => {
  if (!e.dataTransfer || !props.item) return;
  
  // Set the drag data to include slot number and item UUID
  e.dataTransfer.effectAllowed = 'move';
  e.dataTransfer.setData('cart-slot', props.slot.toString());
  e.dataTransfer.setData('item-uuid', props.item.uuid);
};

const handleDragEnd = () => {
  isDragOver.value = false;
};

const handleDrop = async (e: DragEvent) => {
  e.preventDefault();
  e.stopPropagation();
  isDragOver.value = false;
  
  if (!e.dataTransfer || !currentProject.value) return;
  
  // Check if it's a cart item being reordered
  const sourceSlotStr = e.dataTransfer.getData('cart-slot');
  if (sourceSlotStr) {
    const sourceSlot = parseInt(sourceSlotStr);
    const targetSlot = props.slot;
    
    if (sourceSlot === targetSlot) return; // Same slot, do nothing
    
    // Get the source cart item
    const sourceIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === sourceSlot);
    if (sourceIndex === -1) return;
    
    const sourceCartItem = currentProject.value.cartItems[sourceIndex];
    
    // Check if target slot is occupied
    const targetIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === targetSlot);
    
    if (targetIndex === -1) {
      // Target slot is empty - simple move
      currentProject.value.cartItems[sourceIndex].slot = targetSlot;
    } else {
      // Target slot is occupied - push/insert behavior
      // Remove source item first
      currentProject.value.cartItems.splice(sourceIndex, 1);
      
      // Shift all items at target slot and beyond forward by 1
      for (const cartItem of currentProject.value.cartItems) {
        if (cartItem.slot >= targetSlot) {
          cartItem.slot += 1;
          cartItem.index = [-1, cartItem.slot];
        }
      }
      
      // Insert source item at target slot
      currentProject.value.cartItems.push({
        slot: targetSlot,
        itemUuid: sourceCartItem.itemUuid,
        index: [-1, targetSlot]
      });
    }
    
    // Save the project
    const { saveProject } = useProject();
    await saveProject();
    return;
  }
  
  // Check if it's a file drop
  if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
    const file = e.dataTransfer.files[0];
    // Check if it's an audio file
    if (file.type.startsWith('audio/') || /\.(mp3|wav|flac|ogg|m4a|aac)$/i.test(file.name)) {
      // Upload to (local mode: copy into) the server's media root, then import
      // the resulting server path. Works whether or not Electron can hand us an
      // OS path and whether the server is local or remote.
      const server = useLiveplayServer();
      const serverPath = await server.resolveDroppedFileToMedia(file);
      if (serverPath) await importFromServerPath(serverPath);
      return;
    }
  }
  
  // Otherwise, check if it's an item UUID from the playlist
  const sourceUuid = e.dataTransfer.getData('item-uuid');
  if (!sourceUuid) return;

  // Clone the source item into a cart-only item with its OWN uuid so the cart
  // copy can carry independent name / attenuation / in-out points without
  // mutating the playlist source (or any other cart copy of the same file).
  const sourceItem = findItemByUuid(sourceUuid);
  if (!sourceItem || sourceItem.type !== 'audio') return;

  const { v4: uuidv4 } = await import('uuid');
  const newUuid = uuidv4();
  const cloned: AudioItem = {
    ...(sourceItem as AudioItem),
    uuid: newUuid,
    index: [-1, props.slot],
  };
  addCartOnlyItem(cloned);

  // Add or replace cart item — bind the slot to the freshly-cloned uuid.
  const existingIndex = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);

  if (existingIndex !== -1) {
    const prev = currentProject.value.cartItems[existingIndex];
    // If the slot previously held a cart-only item, drop its backing entry to
    // avoid leaking now-unreferenced cart-only items into the project file.
    if (prev?.itemUuid) removeCartOnlyItem(prev.itemUuid);
    currentProject.value.cartItems[existingIndex].itemUuid = newUuid;
    currentProject.value.cartItems[existingIndex].index = [-1, props.slot];
  } else {
    currentProject.value.cartItems.push({
      slot: props.slot,
      itemUuid: newUuid,
      index: [-1, props.slot]
    });
  }

  // Save the project
  const { saveProject } = useProject();
  saveProject();
};
</script>

<style scoped lang="scss">
.cart-slot {
  border: 3px solid var(--color-border);
  border-radius: var(--border-radius-md);
  background-color: var(--color-surface);
  cursor: pointer;
  transition: all var(--transition-fast);
  position: relative;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  
  &:hover {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
    transform: scale(1.02);
  }
  
  &.has-item {
    border-width: 4px;
  }

  &.is-selected {
    box-shadow: 0 0 0 2px var(--color-accent);
  }
  
  &.drag-over {
    background-color: var(--color-accent);
    opacity: 0.5;
    transform: scale(1.05);
    border-color: var(--color-accent);
  }
  
  /* End-of-cue warning border. Reuses the slot's own border (never clipped),
     bumped to 8px so it reads clearly over the slot's existing 4px border, and
     blinks at the same rate as the ProjectHeader silence-warning banner
     (yellow ≤30s, orange ≤10s, red ≤5s). */
  &.warning-yellow {
    border-width: 8px;
    animation: cart-warning-flash-yellow 2s ease-in-out infinite;
  }

  &.warning-orange {
    border-width: 8px;
    animation: cart-warning-flash-orange 1s ease-in-out infinite;
  }

  &.warning-red {
    border-width: 8px;
    animation: cart-warning-flash-red 0.5s ease-in-out infinite;
  }
}

@keyframes cart-warning-flash-yellow {
  0%, 100% { border-color: var(--color-border); }
  50% { border-color: #FFC107; }
}

@keyframes cart-warning-flash-orange {
  0%, 100% { border-color: var(--color-border); }
  50% { border-color: #FF9800; }
}

@keyframes cart-warning-flash-red {
  0%, 100% { border-color: var(--color-border); }
  50% { border-color: #F44336; }
}

.empty-slot {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-sm);
  
  .slot-number {
    font-size: 32px;
    font-weight: 700;
    color: var(--color-text-primary);
  }
  
  .slot-hint {
    font-size: 12px;
    font-style: italic;
    color: var(--color-text-secondary);
    padding-left: 4px;
    text-align: center;
  }

  .key-label {
    font-size: 11px;
    font-weight: 600;
    color: var(--color-text-secondary);
    background: var(--color-surface);
    border: 1px solid var(--color-border);
    border-radius: 3px;
    padding: 1px 5px;
    font-family: var(--font-mono);
  }
}

.slot-content {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  position: relative;
  padding: var(--spacing-sm);
  padding-bottom: 40px; /* Space for absolute positioned footer */
  cursor: move;
  min-height: 0;
  &:active {
    cursor: grabbing;
  }
}

.slot-header {
  display: flex;
  align-items: flex-start;
  gap: var(--spacing-xs);
  margin-bottom: var(--spacing-xs);
  cursor: pointer;
  z-index: 2;
  
  .slot-number {
    font-size: 16px;
    font-weight: 700;
    color: var(--color-text-secondary);
    min-width: 24px;
    flex-shrink: 0;
  }
  
  .slot-name {
    font-size: 14px;
    font-weight: 600;
    color: var(--color-text-primary);
    overflow: hidden;
    display: -webkit-box;
    -webkit-line-clamp: 3;
    line-clamp: 3;
    -webkit-box-orient: vertical;
    line-height: 1.3;
    flex: 1;

    &.is-peaking {
      color: #ff3b5c;
    }
  }

  .peak-warning-icon {
    font-size: 16px;
    color: #ff3b5c;
    flex-shrink: 0;
    cursor: help;
    line-height: 1;
    align-self: flex-start;
    margin-top: 1px;
  }

  .key-label {
    font-size: 10px;
    font-weight: 600;
    color: var(--color-text-secondary);
    background: rgba(0, 0, 0, 0.2);
    border: 1px solid var(--color-border);
    border-radius: 3px;
    padding: 0 4px;
    font-family: var(--font-mono);
    flex-shrink: 0;
    line-height: 1.6;
  }
}

.content-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  margin-bottom: var(--spacing-xs);
}

.slot-title {
  flex: 1;
  
  .slot-number {
    font-size: 12px;
    font-weight: 600;
    color: var(--color-text-secondary);
    margin-bottom: 2px;
  }
  
  .slot-name {
    font-size: 14px;
    font-weight: 600;
    color: var(--color-text-primary);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
}

.status-pill {
  display: inline-flex;
  align-items: center;
  padding: 1px 6px;
  border-radius: 2px;
  font-size: 10px;
  font-weight: 600;
  white-space: nowrap;
  flex-shrink: 0;
  line-height: 1.4;

  &.playing {
    background-color: var(--color-success);
    color: white;
  }

  &.up-next {
    background-color: var(--color-warning);
    color: black;
  }

  &.preview {
    background-color: var(--color-success);
    color: black;
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }
}

.slot-actions {
  display: flex;
  gap: 4px;
  flex-shrink: 0;
}

.no-device {
  opacity: 0.4;
}

.slot-footer {
  position: absolute;
  bottom: var(--spacing-sm);
  left: var(--spacing-sm);
  right: var(--spacing-sm);
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  justify-content: space-between;
  flex-shrink: 0;
  z-index: 2;
}

.slot-info {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  margin-left: auto;
}

.behavior-indicators {
  display: flex;
  gap: 2px;
  align-items: center;
  
  .behavior-icon {
    font-size: 14px;
    color: var(--color-text-secondary);
    opacity: 0.7;
  }
}

.slot-duration {
  font-size: 12px;
  color: var(--color-text-secondary);
  white-space: nowrap;
}

.slot-waveform-area {
  flex: 1;
  display: flex;
  flex-direction: column;
  justify-content: flex-end;
  min-height: 30px;
  overflow: hidden; /* Prevent overflow */
}

.slot-time-info {
  display: flex;
  justify-content: space-between;
  font-size: 11px;
  color: var(--color-text-secondary);
  margin-bottom: var(--spacing-xs);
}

.cart-waveform-canvas {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  opacity: 0.3;
  pointer-events: none;
  z-index: 0;
}

.cart-progress {
  position: absolute;
  top: 0;
  left: 0;
  bottom: 0;
  pointer-events: none;
  z-index: 1;
}

.content-footer {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 11px;
  color: var(--color-text-secondary);
}

.playing-dot {
  display: block;
  width: 12px;
  height: 12px;
  border-radius: 50%;
  background-color: var(--color-success);
  animation: blink 1s ease-in-out infinite;
}

@keyframes blink {
  0%, 100% {
    opacity: 1;
  }
  50% {
    opacity: 0.3;
  }
}
</style>
