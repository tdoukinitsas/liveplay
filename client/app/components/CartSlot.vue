<template>
  <div 
    class="cart-slot"
    ref="slotRef"
    :class="{
      'has-item': hasItem,
      'is-playing': isPlaying,
      'is-selected': isSelected,
      'show-mode': showMode,
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

    <!-- Empty slot: importing is an edit action, so in Show Mode the slot is
         inert (just a dimmed number) rather than a "click to import" target. -->
    <div v-if="!hasItem" class="empty-slot" :class="{ 'empty-slot--inert': showMode }" @click="showMode ? undefined : handleImport()">
      <span class="slot-number">{{ displayNumber }}</span>
      <span v-if="keyLabel" class="key-label">{{ keyLabel }}</span>
      <span v-if="!showMode" class="slot-hint">{{ t('cart.clickToImportOrPlaylist') }}</span>
      <button
        v-if="!showMode"
        type="button"
        class="bind-playlist"
        :title="t('cart.startPlaylist')"
        :aria-label="t('cart.startPlaylist')"
        :aria-expanded="choosingTab"
        @click.stop="onBindPlaylist"
      >
        <span class="material-symbols-rounded" aria-hidden="true">queue_music</span>
      </button>
      <!-- Covers the whole slot, so no rim of it is left to open Import by
           mistake; its own ✕ sits where the toggle was. Escape, a click
           outside or the ✕ put it away without binding anything. -->
      <div
        v-if="choosingTab"
        ref="tabListEl"
        class="bind-playlist-list"
        role="dialog"
        :aria-label="t('cart.choosePlaylist')"
        data-own-keys
        @click.stop
        @keydown.esc.stop.prevent="choosingTab = false"
      >
        <div class="bind-playlist-head">
          <span>{{ t('cart.choosePlaylist') }}</span>
          <button
            type="button"
            class="bind-playlist-cancel"
            :title="t('common.cancel')"
            :aria-label="t('common.cancel')"
            @click="choosingTab = false"
          >
            <span class="material-symbols-rounded" aria-hidden="true">close</span>
          </button>
        </div>
        <button
          v-for="(tab, i) in playlistTabs"
          :key="tab.id"
          type="button"
          class="bind-playlist-option"
          @click="bindTab(tab.id)"
        >{{ tab.name || t('tabs.defaultPlaylist', { n: i + 1 }) }}</button>
      </div>
    </div>

    <div
      v-else
      class="slot-content"
      :draggable="!showMode"
      @dragstart="handleDragStart"
      @dragend="handleDragEnd"
      @click="showMode ? handleSlotClick() : undefined"
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
        <span class="slot-number">{{ displayNumber }}</span>
        <span
          v-if="isGroup"
          class="material-symbols-rounded group-icon"
          :title="isTabBound ? t('cart.playlistSlot') : t('cart.groupSlot')"
        >{{ isTabBound ? 'queue_music' : 'folder' }}</span>
        <span class="slot-name" :class="{ 'is-peaking': isPeaking }">{{ item.displayName }}</span>
        <span
          v-if="mediaMissing"
          class="material-symbols-rounded media-missing-icon"
          :title="t('missingMedia.badge')"
          draggable="false"
          @click.stop
        >link_off</span>
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
        <!-- Action buttons -->
        <!-- Show Mode removes the play and set-as-next buttons entirely — the
             whole slot surface triggers playback instead (see @click on
             .slot-content) — along with the preview/edit/delete edit
             affordances. -->
        <div v-if="!showMode" class="slot-actions">
          <ActionButton
            v-if="item.type === 'audio'"
            :icon="'headphones'"
            :highlight-color="isPreviewing ? 'var(--color-accent)' : 'var(--color-success)'"
            :is-active="isPreviewing"
            :class="{ 'no-device': !previewReady }"
            context="Cart"
            @click.stop="isPreviewing ? handleStopPreview() : handleStartPreview()"
            :title="isPreviewing ? t('actions.stopPreview') : (previewReady ? t('actions.preview') : t('actions.previewNoBus'))"
          />
          <ActionButton
            :icon="isPlaying ? 'stop' : 'play_arrow'"
            :highlight-color="isPlaying ? 'var(--color-danger)' : 'var(--color-success)'"
            context="Cart"
            @click.stop="isPlaying ? handleStop() : handlePlay()"
            :title="isPlaying ? t('actions.stop') : t('actions.play')"
          />
          <ActionButton
            v-if="!isTabBound"
            icon="fast_forward"
            highlight-color="var(--color-warning)"
            active-text-color="black"
            :is-active="isManuallyQueued"
            context="Cart"
            @click.stop="handleSetAsNext"
            :title="t('actions.setAsNext')"
          />
          <ActionButton
            v-if="!isTabBound"
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
          <span class="slot-duration">{{ isPlaying ? "-" + formatTime(duration - currentTime) + indefiniteMark : formatDuration(item) }}</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { triggerRef } from 'vue';
import { v4 as uuidv4 } from 'uuid';
import type { AudioItem, GroupItem } from '~/types/project';
import ActionButton from './ActionButton.vue';
import { runTime, formatRunTime } from '~/utils/groupTiming';
import { drawRowWaveform } from '~/utils/rowWaveform';
import { copyEventsWithNewIds } from '~/utils/events';
import AudioImportModal from './AudioImportModal.vue';
import { useOutputTarget, METER_COLORS } from '~/composables/useOutputTarget';
import { calculatePerceivedLoudness, parseWaveformFileData } from '~/utils/audio';
import { isSecondaryWindow } from '~/composables/useProject';
import { isTabTarget, tabTarget } from '~/utils/tabs';

const props = defineProps<{
  slot: number;
  /** Where it sits on its page: what the operator counts by. */
  position?: number;
  /** The last slot on this page, for making room when carts are reordered. */
  maxSlot?: number;
  item: AudioItem | GroupItem | null;
  keyLabel?: string;
}>();

const displayNumber = computed(() => (props.position ?? props.slot) + 1);
const cartSlotMax = computed(() => props.maxSlot ?? 15);

const slotRef = ref<HTMLElement | null>(null);
const showImportModal = ref(false);

const { currentProject, selectedItem, selectedItems, selectionContext, requestDeleteFromButton, findItemByUuid, triggerWaveformUpdate, markPendingAutoProcess, resolveProjectPath } = useProject();
const { levels: outputTargetLevels } = useOutputTarget();
const { activeCues, activeGroups, nextItemOverrideUuid, autoNextItemUuid, setNextItem,
        isItemPlaying, fireItem, stopItemAny } = useAudioEngine();
const { t } = useLocalization();
const { addCartOnlyItem, updateCartOnlyItem, removeCartOnlyItem, getCartOnlyItem } = useCartItems();
const { uiMode } = useUiMode();

// Show Mode: hide edit affordances (import/preview/edit/delete + drag) and
// enlarge the slot for touch. Waveform, colour, flags and warnings unchanged.
const showMode = computed(() => uiMode.value === 'playback');

// A cart item created here normally reaches the server via useProject's item
// diff-watcher, but a detached window runs without it — nothing pushed the new
// item, so the main window never learned about it. Its next project sync then
// pushed a copy that predates the item back over IPC, and the cart window
// cleared and repopulated from it, wiping the item it had just created. It only
// came back on restart, when the file (which the save had written) was re-read.
//
// So in a detached window we publish the item and its slot binding explicitly.
// The server broadcasts both, and the main window's doc_patch handler folds
// them into its own cart store — which is what makes the two agree.
async function publishNewCartItem(item: AudioItem, slot: number) {
  if (!isSecondaryWindow) return;   // the diff-watcher already handles it
  const server = useLiveplayServer();
  try {
    await server.addProjectItem(item, '', true);
    await server.setCartSlot(slot, item.uuid);
  } catch (e) {
    console.warn('[cart] could not publish new cart item to the server:', e);
  }
}

// The same for carts moved or deleted in a detached window: its saveProject
// sends nothing, so a reorder or a delete changed only this window's copy and
// the next sync from the main window put it back.
const cartBindings = () => new Map<number, string>(
  (currentProject.value?.cartItems ?? []).map(c => [c.slot, c.itemUuid] as [number, string]));

// After a reorder: what moved, slot by slot, as the main window's sync sends
// it (useProject syncCart): the clears first, then the new bindings.
async function publishCartMoves(before: Map<number, string>) {
  const project = currentProject.value;
  if (!isSecondaryWindow || !project) return;
  const after = cartBindings();
  // The server drops the pin of a slot it clears, and tells this window so
  // while the calls are still going: the pins as they were are what show
  // where a moved cart's key has to go.
  const pins = project.cartPinnedKeys;
  const server = useLiveplayServer();
  try {
    for (const slot of before.keys()) if (!after.has(slot)) await server.clearCartSlot(slot);
    for (const [slot, uuid] of after) if (before.get(slot) !== uuid) await server.setCartSlot(slot, uuid);
    if (pins && currentProject.value === project) {
      project.cartPinnedKeys = pins;
      useCartHotkeys().reconcilePinnedKeys(before, after);
    }
  } catch (e) {
    console.warn('[cart] could not move the cart on the server:', e);
  }
}

// After a delete: the slot is cleared, and a cue that lived only in the cart
// goes too, as the main window's item sync would remove it.
async function publishCartDelete(slot: number, cartOnlyUuid: string | null) {
  if (!isSecondaryWindow) return;
  const server = useLiveplayServer();
  try {
    await server.clearCartSlot(slot);
    if (cartOnlyUuid) await server.removeProjectItem(cartOnlyUuid);
  } catch (e) {
    console.warn('[cart] could not delete the cart on the server:', e);
  }
}

const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const currentTime = ref(0);
const duration = ref(0);
const playbackProgress = ref(0);
const warningState = ref<'yellow' | 'orange' | 'red' | null>(null);
const isDragOver = ref(false);

const hasItem = computed(() => props.item !== null);

// The server can't find this slot's media file (issue #80).
const { isMissing } = useMissingMedia();
const mediaMissing = computed(() => !!props.item && props.item.type === 'audio' && isMissing(props.item.uuid));

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
// A slot can hold a playlist group (discussion #61): it fires the group, and
// its time and progress are the whole group's.
const isGroup = computed(() => props.item?.type === 'group');
// A cart that starts a whole playlist tab. It has no item of its own to edit
// or arm: the cues on the tab are the playlist's.
const isTabBound = computed(() => isTabTarget(props.item?.uuid));
const isPlaying = computed(() => isItemPlaying(props.item));
const indefiniteMark = computed(() =>
  isGroup.value && props.item && activeGroups.value.get(props.item.uuid)?.indefinite ? '+' : '');
const isSelected = computed(() => props.item ? selectedItems.value.has(props.item.uuid) : false);
const isManuallyQueued = computed(() => props.item ? nextItemOverrideUuid.value === props.item.uuid : false);
const isQueuedNext = computed(() => {
  if (!props.item) return false;
  if (nextItemOverrideUuid.value) return nextItemOverrideUuid.value === props.item.uuid;
  return autoNextItemUuid.value === props.item.uuid;
});

// Helper to convert hex to rgba
const hexToRgba = (hex: string, alpha: number) => {
  // A cue made through the API may have no colour, or not a #rrggbb one:
  // that must not stop the whole list drawing.
  if (typeof hex !== 'string' || !/^#[0-9a-f]{6}/i.test(hex)) hex = '#808080';
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
    // The same shape for a cue and for a group: how long, and how far in.
    const timing = () => {
      if (!props.item) return null;
      if (props.item.type === 'group') {
        const g = activeGroups.value.get(props.item.uuid);
        return g ? { duration: g.totalDuration, currentTime: g.currentTime } : null;
      }
      return activeCues.value.get(props.item.uuid) ?? null;
    };
    const cue = timing();
    if (cue) {
      duration.value = cue.duration;
      progressInterval = setInterval(() => {
        if (!props.item) return;
        const cue = timing();
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
      // Relative to the project folder so the project stays portable.
      waveformPath: `waveforms/${uuid}.json`,
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

    await publishNewCartItem(newItem, props.slot);

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


// Show Mode: the whole slot surface is the trigger — click to play, click
// again to stop. Selection and drag-reorder are edit-only affordances and
// stay disabled here.
const handleSlotClick = () => {
  if (!props.item) return;
  isPlaying.value ? handleStop() : handlePlay();
};

const handleSelect = (event?: MouseEvent) => {
  if (!props.item || isTabBound.value) return;
  if (showMode.value) return;
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
  fireItem(props.item);
};

// ---- A cart that starts a whole playlist tab (discussion #61) ---------------
const { playlistTabs } = useShowTabs();
const choosingTab = ref(false);
const tabListEl = ref<HTMLElement | null>(null);

const onBindPlaylist = () => {
  if (playlistTabs.value.length === 1) bindTab(playlistTabs.value[0]!.id);
  else choosingTab.value = !choosingTab.value;
};

// While the list is up: a press anywhere outside it puts it away, and so does
// Escape wherever focus is, caught before the transport keys would take it
// as Stop All.
function onOutsidePress(e: PointerEvent) {
  if (tabListEl.value && !tabListEl.value.contains(e.target as Node)) choosingTab.value = false;
}
function onEscapeCapture(e: KeyboardEvent) {
  if (e.key !== 'Escape') return;
  e.preventDefault();
  e.stopPropagation();
  choosingTab.value = false;
}
function unbindChooser() {
  window.removeEventListener('pointerdown', onOutsidePress, true);
  window.removeEventListener('keydown', onEscapeCapture, true);
}
watch(choosingTab, (open) => {
  if (!import.meta.client) return;
  unbindChooser();
  if (!open) return;
  window.addEventListener('pointerdown', onOutsidePress, true);
  window.addEventListener('keydown', onEscapeCapture, true);
  // Into the list, so the keyboard can choose from it.
  void nextTick(() => tabListEl.value?.querySelector<HTMLElement>('.bind-playlist-option')?.focus());
});
// An edit action: Show Mode, or the slot being filled, puts it away.
watch([showMode, hasItem], () => { choosingTab.value = false; });
onBeforeUnmount(() => { if (import.meta.client) unbindChooser(); });

const bindTab = (tabId: string) => {
  const project = currentProject.value;
  if (!project) return;
  choosingTab.value = false;
  const uuid = tabTarget(tabId);
  const existing = project.cartItems.find((ci: any) => ci.slot === props.slot);
  if (existing) {
    if (existing.itemUuid) removeCartOnlyItem(existing.itemUuid);
    existing.itemUuid = uuid;
    existing.index = [-1, props.slot];
  } else {
    project.cartItems.push({ slot: props.slot, itemUuid: uuid, index: [-1, props.slot] });
  }
  if (isSecondaryWindow) {
    useLiveplayServer().setCartSlot(props.slot, uuid).catch((err: unknown) =>
      console.warn('[cart] could not bind the playlist to the slot on the server:', err));
  }
  void useProject().saveProject();
};

const handleStop = () => {
  if (!props.item) return;
  stopItemAny(props.item);
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
  const cartOnlyUuid = getCartOnlyItem(props.item.uuid) ? props.item.uuid : null;
  removeCartOnlyItem(props.item.uuid);

  // Remove from cart
  const index = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);
  if (index !== -1) {
    currentProject.value.cartItems.splice(index, 1);
    // A detached window tells the server itself (see publishCartDelete).
    void publishCartDelete(props.slot, cartOnlyUuid);
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
// Pre-listen lands on the preview-role bus (D24), so the button is live only
// when that bus reaches hardware. Before the first /api/buses fetch the list
// is empty, which reads as "not ready" — the composable fetches on connect.
// When there is nowhere to listen, the button opens the mixer so the fix is
// one click away (D30); the class name `no-device` is kept for the CSS hook.
const previewBusStore = useLiveplayServer();
const previewReady = computed(() => !!previewBusStore.buses.find(b => b.preview)?.bound);
const { showPane } = useWorkspaceLayout();
const handleStartPreview = () => {
  if (!props.item || props.item.type !== 'audio') return;
  if (!previewReady.value) {
    showPane('mixer');
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

const formatDuration = (item: AudioItem | GroupItem | null): string => {
  if (!item) return '';
  if (item.type === 'group') {
    const total = runTime(item as GroupItem);
    return formatRunTime(total.seconds) + (total.indefinite ? '+' : '');
  }
  
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

// Draw waveform (the shared row drawing, utils/rowWaveform.ts)
const drawWaveform = () => {
  if (!waveformCanvas.value || !props.item || props.item.type !== 'audio') return;
  drawRowWaveform(waveformCanvas.value, props.item as AudioItem);
};

// Watch for item changes and redraw
let resizeObserver: ResizeObserver | null = null;
let waveformPollInterval: NodeJS.Timeout | null = null;

onMounted(() => {
  // Use native DOM listeners for drag-and-drop — Vue event handlers
  // on this component don't receive drop events (likely a Vue 3 scoped template issue).
  // Always registered, and Show Mode checked on each event: registered only
  // when the slot mounted in Edit Mode, a slot that mounted in Show Mode
  // stayed dead after switching to Edit Mode. In Show Mode a drop is still
  // taken (handleDrop), so a file let go over a cart is not opened by the
  // window, but it changes nothing.
  if (slotRef.value) {
    slotRef.value.addEventListener('dragenter', (e) => {
      e.preventDefault();
    });
    slotRef.value.addEventListener('dragover', (e) => {
      e.preventDefault();
      if (!e.dataTransfer) return;
      if (showMode.value) {
        e.dataTransfer.dropEffect = 'none';
        return;
      }
      isDragOver.value = true;
      e.dataTransfer.dropEffect = 'move';
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
        const result = await window.electronAPI.readFile(resolveProjectPath(audioItem.waveformPath));
        if (result.success && result.data) {
          // Accepts both the server's per-channel cache and legacy ffmpeg files.
          const waveformData = parseWaveformFileData(JSON.parse(result.data));
          if (waveformData) {
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

  // No cart edits (reorder / import / replace) in Show Mode, even via an OS
  // file drop — guards the case where the slot mounted in edit mode first.
  if (showMode.value) return;
  if (!e.dataTransfer || !currentProject.value) return;
  
  // A playlist tab dropped here: this cart now starts it.
  const droppedTab = e.dataTransfer.getData('liveplay-playlist-tab');
  if (droppedTab) {
    bindTab(droppedTab);
    return;
  }

  // Check if it's a cart item being reordered
  const sourceSlotStr = e.dataTransfer.getData('cart-slot');
  if (sourceSlotStr) {
    const sourceSlot = parseInt(sourceSlotStr);
    const targetSlot = props.slot;

    if (sourceSlot === targetSlot) return; // Same slot, do nothing

    const cartItems = currentProject.value.cartItems;
    const sourceCartItem = cartItems.find((ci: any) => ci.slot === sourceSlot);
    if (!sourceCartItem) return;
    const before = cartBindings();

    // Cart slots are an addressable grid (the user may have bound hotkeys to
    // specific slots), so reordering must be conservative — only disturb what
    // it has to:
    //   • Dropping on an EMPTY slot just relocates the source; every other
    //     slot stays put and the source's old slot is left blank.
    //   • Dropping on an OCCUPIED slot pushes only the *contiguous* run of
    //     occupied slots starting at the target forward by one to open a gap.
    //     Slots past the first blank are untouched.
    // The source's own slot counts as free for this cascade (it's vacating),
    // which also stops the run early when the source sits within it.
    const occupied = new Set<number>(cartItems.map((ci: any) => ci.slot));
    occupied.delete(sourceSlot);

    if (!occupied.has(targetSlot)) {
      // Target empty → drop straight in; nothing else moves.
      sourceCartItem.slot = targetSlot;
      sourceCartItem.index = [-1, targetSlot];
    } else {
      // Find the first blank slot at or after the target — the contiguous
      // occupied run is [targetSlot, firstFree - 1].
      let firstFree = targetSlot;
      while (occupied.has(firstFree)) firstFree += 1;

      // No blank slot remains before the end of the grid → no room to push.
      if (firstFree > cartSlotMax.value) return;

      // Shift the run up by one, highest slot first to avoid collisions.
      for (let s = firstFree - 1; s >= targetSlot; s--) {
        const ci = cartItems.find((c: any) => c.slot === s);
        if (ci) { ci.slot = s + 1; ci.index = [-1, s + 1]; }
      }
      sourceCartItem.slot = targetSlot;
      sourceCartItem.index = [-1, targetSlot];
    }

    // A detached window tells the server itself (see publishCartMoves).
    await publishCartMoves(before);

    // Save the project
    const { saveProject } = useProject();
    await saveProject();
    return;
  }
  
  // Check if it's a file drop
  if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
    const [file] = useImportNotice().keepPlayable([e.dataTransfer.files[0]!], f => f.name);
    if (!file) return;
    // Upload to (local mode: copy into) the server's media root, then import
    // the resulting server path. Works whether or not Electron can hand us an
    // OS path and whether the server is local or remote.
    const server = useLiveplayServer();
    const serverPath = await server.resolveDroppedFileToMedia(file);
    if (serverPath) await importFromServerPath(serverPath);
    return;
  }
  
  // Otherwise, check if it's an item UUID from the playlist
  const sourceUuid = e.dataTransfer.getData('item-uuid');
  if (!sourceUuid) return;

  // A playlist group is bound by reference: the slot fires the group itself
  // (its start behaviour, then its cues in turn), and edits to the group show
  // up here. Nothing is cloned, so nothing is duplicated.
  const sourceGroup = findItemByUuid(sourceUuid);
  if (sourceGroup && sourceGroup.type === 'group') {
    const at = currentProject.value.cartItems.findIndex((ci: any) => ci.slot === props.slot);
    if (at !== -1) {
      const prev = currentProject.value.cartItems[at];
      if (prev?.itemUuid) removeCartOnlyItem(prev.itemUuid);
      prev.itemUuid = sourceUuid;
      prev.index = [-1, props.slot];
    } else {
      currentProject.value.cartItems.push({ slot: props.slot, itemUuid: sourceUuid, index: [-1, props.slot] });
    }
    if (isSecondaryWindow) {
      useLiveplayServer().setCartSlot(props.slot, sourceUuid).catch((err: unknown) =>
        console.warn('[cart] could not bind the group to the slot on the server:', err));
    }
    const { saveProject } = useProject();
    saveProject();
    return;
  }

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
    // Its own copy: a spread would share the array, and editing the cart
    // copy's events would quietly edit the playlist cue's as well.
    events: copyEventsWithNewIds((sourceItem as AudioItem).events),
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

  await publishNewCartItem(cloned, props.slot);

  // Save the project
  const { saveProject } = useProject();
  saveProject();
};
</script>

<style scoped lang="scss">
.bind-playlist {
  position: absolute;
  right: 6px;
  top: 6px;
  display: flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  height: 28px;
  padding: 0;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-background);
  color: var(--color-text-secondary);
  cursor: pointer;

  .material-symbols-rounded { font-size: 18px; }
  &:hover { color: var(--color-text-primary); border-color: var(--color-accent); }
}

/* Edge to edge: a rim left showing was the empty slot, and a click on it
   opened Import. */
.bind-playlist-list {
  position: absolute;
  inset: 0;
  z-index: 2;
  display: flex;
  flex-direction: column;
  gap: 2px;
  overflow-y: auto;
  padding: 4px;
  background: var(--color-surface);
  cursor: default;
}

.bind-playlist-head {
  display: flex;
  align-items: center;
  gap: 4px;
  flex: 0 0 auto;
  padding: 2px 2px 2px 6px;
  font-size: 11px;
  color: var(--color-text-secondary);

  > span { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
}

.bind-playlist-cancel {
  display: flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  width: 28px;
  height: 28px;
  padding: 0;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-background);
  color: var(--color-text-secondary);
  cursor: pointer;

  .material-symbols-rounded { font-size: 18px; }
  &:hover { color: var(--color-text-primary); border-color: var(--color-accent); }
}

.bind-playlist-option {
  flex: 0 0 auto;
  padding: 4px 6px;
  border: none;
  border-radius: 4px;
  background: none;
  color: var(--color-text-primary);
  font-size: 12px;
  text-align: left;
  cursor: pointer;

  &:hover, &:focus-visible { background: var(--color-surface-hover); }
  &:focus-visible { outline: 2px solid var(--color-accent); outline-offset: -2px; }
}

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
  
  .group-icon {
  font-size: 16px;
  opacity: 0.8;
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

  .media-missing-icon {
    font-size: 16px;
    color: var(--color-warning);
    flex-shrink: 0;
    cursor: help;
    line-height: 1;
    align-self: flex-start;
    margin-top: 1px;
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

/* ------------------------------------------------------------------ */
/* Show Mode — bigger name text and chunky play/stop / set-next        */
/* buttons for touch. Empty slots become inert (no import cursor).      */
/* ------------------------------------------------------------------ */
.cart-slot.show-mode {
  &:hover {
    /* No hover-grow in Show Mode — touch has no hover and the scale
       nudge just makes big targets feel jittery. */
    transform: none;
  }

  .empty-slot--inert {
    cursor: default;
  }

  .slot-name {
    font-size: 17px;
  }

  .slot-duration {
    font-size: 14px;
  }

  .behavior-icon {
    font-size: 18px !important;
  }

  :deep(.action-btn--cart) {
    width: 46px;
    height: 46px;

    .material-symbols-rounded {
      font-size: 24px;
    }
  }

  .slot-actions {
    gap: var(--spacing-sm);
  }

  /* No buttons in Show Mode — the whole slot is the play/stop trigger, and
     it's not draggable, so drop the "move" cursor and shrink the footer
     padding back down (no buttons to clear). */
  .slot-content {
    cursor: pointer;
    padding-bottom: var(--spacing-sm);

    &:active {
      cursor: pointer;
    }
  }
}
</style>
