<template>
  <div class="main-workspace" :class="{ 'show-mode': uiMode === 'playback' }">
    <!-- Show Mode reuses the full editor layout (header, transport, resizable/
         detachable playlist⇄cart split). It is not a separate view: the child
         components read useUiMode() and hide their edit affordances + enlarge
         touch targets, so waveforms, colours, durations, behaviour flags and
         warnings all render exactly as in edit mode. -->
    <ProjectHeader />

    <!-- D12/D17 — dismissible, non-blocking banner shown in every connected
         window when the server migrates a legacy (pre-mixer-buses) project on
         load. Never a modal: invariant 3 says a banner must never block a
         show from loading. Dismissal is per-client view state, tracked in the
         composable (server.migrationBanner is null once dismissed here). -->
    <div v-if="server.migrationBanner" class="migration-banner" role="status">
      <div class="migration-banner__text">
        <p class="migration-banner__title">{{ t('migration.title') }}</p>
        <p class="migration-banner__body">{{ t('migration.body') }}</p>
        <p v-if="server.migrationBanner.busesFromDeviceOverride > 0" class="migration-banner__body">
          {{ t('migration.busesFromOverrides', { count: server.migrationBanner.busesFromDeviceOverride }) }}
        </p>
      </div>
      <div class="migration-banner__actions">
        <button class="migration-banner__btn migration-banner__btn--primary" @click="openMixerFromBanner">
          {{ t('migration.openMixer') }}
        </button>
        <button class="migration-banner__btn" @click="server.dismissMigrationBanner()">
          {{ t('migration.dismiss') }}
        </button>
      </div>
    </div>

    <PlaybackControls />

    <!-- One flex row, left to right: playlist | handle | cart | handle | mixer.
         The playlist is the only flexible pane (flex: 1, min-width 0); cart
         and mixer carry explicit px widths with flex-shrink 0, and the drag
         handlers clamp those widths so the row never over-requests and the
         playlist never drops below PLAYLIST_MIN_PX. Every separator snaps the
         same way: drag it past a threshold and the pane it is dragged over
         collapses to maximise the others, while the handle itself stays as a
         thin bar on the edge so the collapsed pane can be dragged back out.
         Collapsed handles are ordinary 8px flex items in their natural slot —
         not absolutely positioned — so a closed cart's handle sits between the
         playlist and a docked mixer rather than floating over the mixer.

         The mixer in 'full' mode is the same row with the playlist and cart
         withheld, so every handle keeps its DOM node across a mode flip and a
         drag that crosses the threshold carries straight on. -->
    <div ref="workspaceEl" class="workspace-content">
      <div v-if="!cartFullscreen && !mixerFull" class="playlist-section">
        <PlaylistView />
      </div>

      <div
        v-if="!cartDetached && !mixerFull"
        class="resize-handle"
        :class="{ 'collapsed-left': cartFullscreen, 'collapsed-right': cartClosed, dragging: isResizing }"
        @pointerdown="startResize"
      ></div>

      <div
        v-if="!cartClosed && !cartDetached && !mixerFull"
        class="cart-section"
        :class="{ 'cart-section--fill': cartFullscreen }"
        :style="cartFullscreen ? undefined : { width: `${cartWidth}px` }"
      >
        <CartPlayer />
      </div>

      <!-- Docked mixer: a resizable right-hand pane, so a rig with a handful of
           buses can leave it up permanently instead of swapping views. In
           'full' mode the pane fills the row and its handle sits on the left
           edge, so the user can drag it back down to a side pane; collapsed,
           only the handle remains, on the right edge. -->
      <template v-if="mixerOpen && !mixerDetached">
        <div
          class="resize-handle mixer-resize-handle"
          :class="{ 'collapsed-left': mixerFull, 'collapsed-right': mixerCollapsed, dragging: isMixerResizing }"
          @pointerdown="startMixerResize"
        ></div>
        <div
          v-if="!mixerCollapsed"
          class="mixer-section"
          :class="{ 'mixer-section--fill': mixerFull }"
          :style="mixerFull ? undefined : { width: `${mixerWidth}px` }"
        >
          <MixerPanel :mode="mixerMode" @close="mixerOpen = false" @mode="setMixerMode" />
        </div>
      </template>
    </div>

    <!-- Properties panel is an edit affordance — never surfaced in Show Mode. -->
    <PropertiesPanel v-if="uiMode !== 'playback' && propertiesPanelOpen && selectedItem" />

    <ProgressModal
      :visible="progressModal.visible"
      :title="progressModal.title"
      :message="progressModal.message"
      :percentage="progressModal.percentage"
    />

    <!-- Export: server vs client choice (only shown for remote servers). -->
    <LocationChoiceModal
      :visible="exportChoiceVisible"
      :title="t('exportProject.chooseLocationTitle')"
      :message="t('exportProject.chooseLocationMessage')"
      :server-label="t('exportProject.saveOnServer')"
      :client-label="t('exportProject.downloadHere')"
      :cancel-label="t('common.cancel')"
      @pick="onExportChoice"
      @cancel="exportChoiceVisible = false"
    />

    <!-- Server file picker (directory mode) for "save on server" path. -->
    <ServerFilePickerModal
      :open="exportServerPickerOpen"
      mode="directory"
      filter="all"
      :filter-options="['all']"
      :start-path="currentProject?.folderPath ?? ''"
      @pick="onExportServerPath"
      @close="exportServerPickerOpen = false"
    />
  </div>
</template>

<script setup lang="ts">
import LocationChoiceModal from './LocationChoiceModal.vue';
import ServerFilePickerModal from './ServerFilePickerModal.vue';

const {
  selectedItem,
  selectedItems,
  propertiesPanelOpen,
  saveProject,
  closeProject,
  confirmUnsavedChanges,
  currentProject,
  findItemByUuid,
  selectAllItems,
  duplicateItems,
  copyItemsToClipboard,
  pasteItemsFromClipboard,
  requestDeleteFromKeyboard,
} = useProject();
const { triggerByUuid, triggerByIndex, stopCue, stopAllCues, playCue } = useAudioEngine();
const { getCartItem, cartOnlyItems, updateCartOnlyItem } = useCartItems();
const { t } = useLocalization();
const server = useLiveplayServer();
const { uiMode } = useUiMode();

// Progress modal state
const progressModal = ref({
  visible: false,
  title: '',
  message: '',
  percentage: 0
});

// Resizable cart width
// Shared with ProjectHeader's toggle. There is no router, so views are panel
// swaps driven by a flag — the same shape cartFullscreen / cartClosed use.
const mixerOpen = useState<boolean>('liveplay:mixerOpen', () => false);
// 'side' docks it as a resizable right-hand pane (good for a few buses, can
// stay up permanently); 'full' gives it the whole workspace. Per-device, so it
// is remembered locally rather than travelling in the project.
const mixerMode = useState<'side' | 'full'>('liveplay:mixerMode', () => 'side');
// Popped out into its own window: the in-app panel steps aside rather than
// drawing a second copy of the same faders. Shared with ProjectHeader, whose
// toggle focuses the window instead of opening the panel while this is true.
const mixerDetached = useState<boolean>('liveplay:mixerDetached', () => false);
const mixerWidth = ref(420);
const isMixerResizing = ref(false);
// Collapsed by dragging its separator to the right edge: the pane is not
// rendered but mixerOpen stays true, so the header toggle still reads as
// "open" and only the thin handle remains to drag it back out. Pure view
// state, like cartClosed — the server never hears about pane layout.
const mixerCollapsed = ref(false);
// Handy alias for the template/handlers: the mixer is rendered full-width.
const mixerFull = computed(() => mixerOpen.value && !mixerDetached.value && mixerMode.value === 'full');

const workspaceEl = ref<HTMLElement | null>(null);

// Splitter geometry, shared by both handles so the two clamps agree.
const PLAYLIST_MIN_PX = 240;      // the flexible pane never shrinks below this
const CART_MIN_PX = 300;
const MIXER_MIN_PX = 220;
const HANDLE_PX = 5;              // an open separator
const COLLAPSED_HANDLE_PX = 8;    // a separator standing in for a collapsed pane
const SNAP_PX = 100;              // drag this far past an edge/limit to snap

// How much of the row the cart side (pane + its handle) takes up, as seen by
// the mixer's clamp. A fullscreen cart is the flexible pane itself, so only
// its collapsed handle counts as fixed width.
function cartReservedPx(): number {
  if (cartDetached.value) return 0;
  if (cartClosed.value || cartFullscreen.value) return COLLAPSED_HANDLE_PX;
  return cartWidth.value + HANDLE_PX;
}

// Likewise the mixer side, as seen by the cart's clamp.
function mixerReservedPx(): number {
  if (!mixerOpen.value || mixerDetached.value) return 0;
  if (mixerCollapsed.value) return COLLAPSED_HANDLE_PX;
  return mixerWidth.value + HANDLE_PX;
}

function maxCartWidth(containerWidth: number): number {
  return containerWidth - mixerReservedPx() - HANDLE_PX - PLAYLIST_MIN_PX;
}

function maxMixerWidth(containerWidth: number): number {
  return containerWidth - cartReservedPx() - HANDLE_PX - PLAYLIST_MIN_PX;
}

// Minimums win over the maximum: only on a window too narrow for every
// minimum at once (≈ 770px with cart + mixer docked) can the row still
// over-request, and then the playlist is what gives.
const clampWidth = (w: number, min: number, max: number) => Math.max(min, Math.min(w, max));

function setMixerMode(mode: 'side' | 'full') {
  mixerMode.value = mode;
  // A mode button can only be pressed on a rendered panel, but the header
  // toggle and the watcher below also route through here in spirit: any
  // explicit mode/open change brings a collapsed mixer back.
  mixerCollapsed.value = false;
}

// Un-collapse whenever the mixer is opened/closed from the header toggle or
// flipped to 'full' by a panel button — a collapsed pane that silently stayed
// collapsed after "Open Mixer" would look like the toggle did nothing.
watch([mixerOpen, mixerMode], () => { mixerCollapsed.value = false; });

// Re-clamp after a window resize so the widths the user last dragged to can
// never leave the row over-requesting once the viewport gets narrower.
// Mixer first (it is the newest pane), then the cart.
function reclampPanes() {
  const container = workspaceEl.value;
  if (!container) return;
  const width = container.getBoundingClientRect().width;
  if (width <= 0) return;
  if (mixerOpen.value && !mixerDetached.value && !mixerCollapsed.value && mixerMode.value === 'side') {
    mixerWidth.value = clampWidth(mixerWidth.value, MIXER_MIN_PX, maxMixerWidth(width));
  }
  if (!cartDetached.value && !cartClosed.value && !cartFullscreen.value) {
    cartWidth.value = clampWidth(cartWidth.value, CART_MIN_PX, maxCartWidth(width));
  }
}

// Migration banner's "Open Mixer" action (D12): open the panel and dismiss
// the banner locally — dismissal is per-client view state, see the composable.
function openMixerFromBanner() {
  mixerOpen.value = true;
  server.dismissMigrationBanner();
}

// The mixer's separator. Same pointer-capture shape as startResize below (see
// the comment there for why), and the same snap semantics as the cart's: the
// pane is anchored to the right edge, so dragging left widens it. Past the
// row's limit it snaps to 'full'; dragged off the right edge it collapses to
// a bare handle. One handler serves the side, collapsed and full-mode
// handle alike — the width under the pointer decides which state we're in.
function startMixerResize(e: PointerEvent) {
  if (isMixerResizing.value || !e.isPrimary || (e.pointerType === 'mouse' && e.button !== 0)) return;
  const handle = e.currentTarget as HTMLElement | null;
  isMixerResizing.value = true;
  e.preventDefault();
  try { handle?.setPointerCapture(e.pointerId); } catch { /* capture is best-effort */ }

  const onMove = (ev: PointerEvent) => {
    if (!isMixerResizing.value) return;
    const container = workspaceEl.value;
    if (!container) return;
    const rect = container.getBoundingClientRect();

    // Continuous width: from the pointer to the row's right edge, which is
    // the mixer pane's right edge in every mode.
    const width = rect.right - ev.clientX;
    const maxWidth = maxMixerWidth(rect.width);

    // Full mode: the handle is on the left edge; dragging it right past the
    // threshold brings the pane back down to a side pane at the pointer.
    if (mixerMode.value === 'full') {
      if (width > rect.width - SNAP_PX) return;
      mixerMode.value = 'side';
      mixerCollapsed.value = false;
      mixerWidth.value = clampWidth(width, MIXER_MIN_PX, maxWidth);
      return;
    }

    // Dragged off the right edge: collapse, keeping mixerWidth so the pane
    // comes back at a sensible size when dragged out again.
    if (width < SNAP_PX) {
      mixerCollapsed.value = true;
      return;
    }

    // Dragged past the room the playlist and cart can spare: go full width.
    if (width > maxWidth + SNAP_PX) {
      mixerCollapsed.value = false;
      mixerMode.value = 'full';
      return;
    }

    mixerCollapsed.value = false;
    mixerWidth.value = clampWidth(width, MIXER_MIN_PX, maxWidth);
  };

  const onUp = () => {
    isMixerResizing.value = false;
    try { handle?.releasePointerCapture(e.pointerId); } catch { /* already released */ }
    document.removeEventListener('pointermove', onMove);
    document.removeEventListener('pointerup', onUp);
    document.removeEventListener('pointercancel', onUp);
  };

  document.addEventListener('pointermove', onMove);
  document.addEventListener('pointerup', onUp);
  document.addEventListener('pointercancel', onUp);
}

const cartWidth = ref(500);
const isResizing = ref(false);
const cartClosed = ref(false);
const cartFullscreen = ref(false);
const cartDetached = ref(false);

// Pointer events (not mouse events) so the splitter is draggable by touch and
// pen as well as mouse. Pointer capture keeps the drag alive when the finger
// slides off the 5px bar — without it a touch drag died on the first move,
// which is why the divider was effectively immovable on touch devices. The
// handle also carries `touch-action: none` so the browser doesn't claim the
// gesture for scrolling before we ever see a pointermove.
const startResize = (e: PointerEvent) => {
  // Ignore secondary mouse buttons and any second finger landing on the bar —
  // a concurrent drag would register a duplicate set of document listeners.
  if (isResizing.value || !e.isPrimary || (e.pointerType === 'mouse' && e.button !== 0)) return;
  const handle = e.currentTarget as HTMLElement | null;
  isResizing.value = true;
  e.preventDefault();
  try { handle?.setPointerCapture(e.pointerId); } catch { /* capture is best-effort */ }

  const handleMouseMove = (e: PointerEvent) => {
    if (!isResizing.value) return;

    const container = workspaceEl.value;
    if (!container) return;

    const rect = container.getBoundingClientRect();

    // The cart pane's right edge is the container's right edge only when
    // nothing else sits to its right. When the mixer is docked ('side' mode)
    // its panel and resize handle occupy the same row, so the cart's actual
    // boundary is wherever the mixer's own handle starts — not the far edge
    // of the workspace. Measuring from the container's edge unconditionally
    // left this divider trailing the pointer by the mixer's width whenever
    // the mixer panel was open, which is why it looked like it wasn't
    // tracking the mouse at all.
    const rightEdge = rect.right - mixerReservedPx();
    const newWidth = rightEdge - e.clientX;
    // The most the cart can have while the playlist keeps its minimum and a
    // docked mixer keeps its width — so the fullscreen snap below is measured
    // against what is actually reachable, not the raw container width.
    const maxWidth = maxCartWidth(rect.width);

    // Close snap: dragged off the cart's right edge.
    if (newWidth < SNAP_PX) {
      cartClosed.value = true;
      cartFullscreen.value = false;
      return;
    }

    // Fullscreen snap: dragged past the room the playlist can spare.
    if (newWidth > maxWidth + SNAP_PX) {
      cartFullscreen.value = true;
      cartClosed.value = false;
      return;
    }

    // Normal resize
    cartClosed.value = false;
    cartFullscreen.value = false;
    cartWidth.value = clampWidth(newWidth, CART_MIN_PX, maxWidth);
  };
  
  const handleMouseUp = () => {
    isResizing.value = false;
    try { handle?.releasePointerCapture(e.pointerId); } catch { /* already released */ }
    document.removeEventListener('pointermove', handleMouseMove);
    document.removeEventListener('pointerup', handleMouseUp);
    document.removeEventListener('pointercancel', handleMouseUp);
  };

  document.addEventListener('pointermove', handleMouseMove);
  document.addEventListener('pointerup', handleMouseUp);
  // A touch drag interrupted by the OS (gesture takeover, call, etc.) fires
  // pointercancel instead of pointerup — without this the handle stayed "stuck"
  // to the finger and kept resizing on the next touch anywhere.
  document.addEventListener('pointercancel', handleMouseUp);
};

// Listen for menu events
if (import.meta.client && window.electronAPI) {
  window.electronAPI.onMenuSaveProject(() => {
    // File > Save always writes to disk, even when autosave is off.
    saveProject({ force: true });
  });

  window.electronAPI.onMenuExportProject(() => {
    startExportFlow();
  });

  window.electronAPI.onMenuCloseProject(async () => {
    if (!(await confirmUnsavedChanges())) return;
    void closeProject();
  });

  // File > New and File > Open while a project is already open: close the
  // current project (locally + on the server) and stash the intent so the
  // welcome screen pops the corresponding picker as soon as it mounts.
  // Without this, these menu items were silent when something was open —
  // only WelcomeScreen used to subscribe, and it isn't mounted right now.
  window.electronAPI.onMenuNewProject(async () => {
    if (!(await confirmUnsavedChanges())) return;
    try { sessionStorage.setItem('liveplay:welcomeIntent', 'new'); } catch {}
    await closeProject();
  });

  window.electronAPI.onMenuOpenProject(async () => {
    if (!(await confirmUnsavedChanges())) return;
    try { sessionStorage.setItem('liveplay:welcomeIntent', 'open'); } catch {}
    await closeProject();
  });

  // File > Open Recent > <project> while a project is already open. Same
  // shape as onMenuOpenProject, but we stash the exact path so the welcome
  // screen opens it directly instead of popping the file picker.
  window.electronAPI.onMenuOpenRecentProject(async (_e, projectPath) => {
    if (!projectPath) return;
    if (!(await confirmUnsavedChanges())) return;
    try { sessionStorage.setItem('liveplay:welcomeOpenPath', projectPath); } catch {}
    await closeProject();
  });

  window.electronAPI.onMenuOpenProjectFolder(() => {
    if (currentProject.value) {
      window.electronAPI.openFolder(currentProject.value.folderPath);
    }
  });

  // Cart window detach/attach
  window.electronAPI.onCartPlayerWindowOpened(() => {
    cartDetached.value = true;
  });
  window.electronAPI.onCartPlayerWindowClosed(() => {
    cartDetached.value = false;
  });

  // Mixer window detach/attach. mixerOpen is left alone on both edges, so
  // closing the detached window puts the panel back exactly where it was.
  window.electronAPI.onMixerWindowOpened?.(() => {
    mixerDetached.value = true;
  });
  window.electronAPI.onMixerWindowClosed?.(() => {
    mixerDetached.value = false;
  });

  // Listen for API triggers
  window.electronAPI.onTriggerItem((_event, data) => {
    if (data.type === 'uuid') {
      triggerByUuid(data.value);
    } else if (data.type === 'index') {
      triggerByIndex(data.value);
    }
  });

  window.electronAPI.onStopItem((_event, data) => {
    if (data.type === 'uuid') {
      stopCue(data.value);
    }
  });

  // Trigger a cart slot by slot number (from HTTP API)
  window.electronAPI.onTriggerCartSlot((_event, data) => {
    const item = getCartItem(data.slot);
    if (item) playCue(item);
  });

  // Stop all cues (from HTTP API)
  window.electronAPI.onStopAllCues(() => {
    stopAllCues();
  });

  // Update a cue's properties (from HTTP API PATCH /api/cues/:id)
  const READONLY_ITEM_KEYS = new Set(['uuid', 'type', 'index', 'mediaFileName', 'mediaPath', 'waveformPath', 'waveform', 'duration']);
  window.electronAPI.onApiUpdateItem((_event, { requestId, id, updates }) => {
    const item = findItemByUuid(id);
    if (!item || item.type !== 'audio') {
      window.electronAPI.sendApiResponse({ requestId, success: false, message: 'Cue not found' });
      return;
    }
    for (const [key, value] of Object.entries(updates)) {
      if (!READONLY_ITEM_KEYS.has(key)) (item as any)[key] = value;
    }
    saveProject();
    window.electronAPI.sendApiResponse({ requestId, success: true, cue: item });
  });

  // Update a cart slot's audio item properties (from HTTP API PATCH /api/carts/:slot)
  window.electronAPI.onApiUpdateCartItem((_event, { requestId, slot, updates }) => {
    const item = getCartItem(slot);
    if (!item) {
      window.electronAPI.sendApiResponse({ requestId, success: false, message: 'Cart slot is empty' });
      return;
    }
    for (const [key, value] of Object.entries(updates)) {
      if (!READONLY_ITEM_KEYS.has(key)) (item as any)[key] = value;
    }
    updateCartOnlyItem(item.uuid, item);
    saveProject();
    window.electronAPI.sendApiResponse({ requestId, success: true, cart: { slot, item } });
  });
}

// ---------------------------------------------------------------------------
// Export project flow (dual-dialog when the server is on another machine).
// ---------------------------------------------------------------------------
// When server runs locally, jump straight to a server-side directory picker.
// Otherwise, ask the user where to save: on the server, or back to this
// computer (via a one-shot download token). This replaces the old purely-
// Electron archiver path, which only worked when the project files were
// reachable from this machine.
const exportChoiceVisible   = ref(false);
const exportServerPickerOpen = ref(false);

async function startExportFlow() {
  if (!currentProject.value) return;
  if (server.isLocalServer.value) {
    // Local: skip the choice modal and go straight to the server picker
    // (the "server" here is this same computer, so this matches the user's
    // expectation of a familiar OS-style directory chooser).
    exportServerPickerOpen.value = true;
  } else {
    exportChoiceVisible.value = true;
  }
}

async function onExportChoice(choice: 'server' | 'client') {
  exportChoiceVisible.value = false;
  if (!currentProject.value) return;

  if (choice === 'server') {
    exportServerPickerOpen.value = true;
    return;
  }

  // client → server packages to its temp dir, returns a token, we download
  // the blob and save it via Electron's native save dialog.
  await exportToClientDownload();
}

async function onExportServerPath(serverDir: string) {
  exportServerPickerOpen.value = false;
  if (!serverDir || !currentProject.value) return;
  const project = currentProject.value;
  const outPath = `${serverDir.replace(/[\\/]+$/, '')}/${project.name}.lpa`;
  await runExport({ outputPath: outPath });
}

async function exportToClientDownload() {
  if (!currentProject.value) return;
  const project = currentProject.value;
  const defaultName = `${project.name}.lpa`;
  // Pick the local destination FIRST so a cancelled save dialog doesn't
  // leave a stray .lpa sitting in the server's temp dir.
  const localDest = await window.electronAPI.showSaveArchiveDialog(defaultName);
  if (!localDest) return;
  await runExport({ outputPath: '', downloadTo: localDest });
}

async function runExport(opts: { outputPath: string; downloadTo?: string }) {
  if (!currentProject.value) return;
  const project = currentProject.value;
  progressModal.value = {
    visible: true,
    title: t('exportProgress.title'),
    message: `${t('exportProgress.message')} ${project.name}.lpa…`,
    percentage: 30,
  };
  try {
    const result = await server.exportProjectArchive(
      project.folderPath, project.name, opts.outputPath);
    progressModal.value.percentage = opts.downloadTo ? 60 : 100;

    if (opts.downloadTo && result.downloadToken) {
      progressModal.value.message =
        `${t('exportProgress.downloading')} ${project.name}.lpa…`;
      const blob = await server.downloadArchive(result.downloadToken);
      const buf  = await blob.arrayBuffer();
      const w = await window.electronAPI.writeBinaryFile(opts.downloadTo, buf);
      if (!w.success) throw new Error(w.error || 'write failed');
    }
    progressModal.value.percentage = 100;
  } catch (e) {
    console.error('Export failed:', e);
  } finally {
    setTimeout(() => { progressModal.value.visible = false; }, 400);
  }
}

// Keep the main process HTTP API server up-to-date with the full project state.
// Waveform peak arrays are stripped from the bulk playlist (large, and API
// consumers don't need them), but PRESERVED for items the detached cart
// window has to render: cart-only items, plus any playlist item referenced
// by a cart slot. Without this the detached cart window's waveform canvases
// stay blank because it can't poll the project folder when the server is
// remote.
// NOTE: This watcher should NOT run in cart window mode to avoid feedback loops.
const stripWaveformsKeeping = (items: any[], keep: Set<string>): any[] =>
  items.map(item => {
    const copy: any = { ...item };
    if (!keep.has(item.uuid)) copy.waveform = null;
    if (copy.children) copy.children = stripWaveformsKeeping(copy.children, keep);
    return copy;
  });

// Check if this is cart window mode by looking at URL query param
const isCartWindowMode = import.meta.client
  ? new URLSearchParams(window.location.search).get('cartWindow') === '1'
  : false;

watch(currentProject, (project) => {
  // Only sync from main window, not from detached cart window
  if (!import.meta.client || !window.electronAPI || !project || isCartWindowMode) return;
  const cartReferenced = new Set<string>(
    (project.cartItems || []).map((ci: any) => ci.itemUuid).filter(Boolean)
  );
  const data = {
    ...project,
    items: stripWaveformsKeeping(project.items || [], cartReferenced),
    // Cart-only items always keep their waveform — the detached cart window
    // needs them and they're a small set (≤ 16 slots in practice).
    cartOnlyItems: Array.from(cartOnlyItems.value.values()).map(i => ({ ...i }))
  };
  window.electronAPI.syncProjectData(JSON.parse(JSON.stringify(data)));
}, { deep: true, immediate: true });

// True when the user is typing in a text field — selection/clipboard
// shortcuts must defer to native editing behaviour there.
const isTextInputFocused = (): boolean => {
  const el = document.activeElement as HTMLElement | null;
  if (!el) return false;
  const tag = el.tagName.toLowerCase();
  return tag === 'input' || tag === 'textarea' || el.isContentEditable;
};

const handleKeydown = (e: KeyboardEvent) => {
  // Save on F1 key (alternative to big play button)
  if (e.key === 'F1') {
    e.preventDefault();
    if (selectedItem.value && selectedItem.value.type === 'audio') {
      const { playCue } = useAudioEngine();
      playCue(selectedItem.value as any);
    }
    return;
  }

  // Delete / Backspace removes the current selection. A multi-selection opens
  // the confirm dialog; a single item is removed outright. Must defer to
  // native editing inside text fields.
  if (e.key === 'Delete' || e.key === 'Backspace') {
    if (isTextInputFocused() || !currentProject.value) return;
    if (requestDeleteFromKeyboard()) e.preventDefault();
    return;
  }

  // Selection / clipboard shortcuts. These all require a project and must not
  // fire while editing text (so native Ctrl+A/C/V keep working in inputs).
  const ctrl = e.ctrlKey || e.metaKey;
  if (!ctrl || e.altKey || !currentProject.value || isTextInputFocused()) return;

  const key = e.key.toLowerCase();

  if (key === 'a') {
    e.preventDefault();
    selectAllItems();
  } else if (key === 'd') {
    e.preventDefault();
    const uuids = Array.from(selectedItems.value);
    if (uuids.length > 0) duplicateItems(uuids);
  } else if (key === 'c') {
    const uuids = Array.from(selectedItems.value);
    if (uuids.length > 0) {
      e.preventDefault();
      void copyItemsToClipboard(uuids);
    }
  } else if (key === 'v') {
    e.preventDefault();
    void pasteItemsFromClipboard();
  }
};

onMounted(() => {
  if (import.meta.client) {
    window.addEventListener('keydown', handleKeydown);
    window.addEventListener('resize', reclampPanes);
  }
});

onUnmounted(() => {
  if (import.meta.client) {
    window.removeEventListener('keydown', handleKeydown);
    window.removeEventListener('resize', reclampPanes);
  }
});
</script>

<style scoped lang="scss">
.main-workspace {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.workspace-content {
  flex: 1;
  display: flex;
  overflow: hidden;
  position: relative;
}

// D12 migration banner. Same warn tint used elsewhere (e.g. ProjectHeader's
// unsaved-changes pill, MixerStrip's output warn) — a yellow-on-dark strip,
// never a modal.
.migration-banner {
  flex: 0 0 auto;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacing-md);
  padding: var(--spacing-sm) var(--spacing-md);
  background-color: var(--color-warning);
  color: black;
}

.migration-banner__text {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.migration-banner__title,
.migration-banner__body {
  margin: 0;
  font-size: 13px;
  // The global `p { color: var(--color-text-primary) }` rule beats the colour
  // inherited from the banner, which painted this white on yellow in the dark
  // theme. Set it on the paragraphs themselves.
  color: black;
}

.migration-banner__title {
  font-weight: 700;
}

.migration-banner__actions {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  flex-shrink: 0;
}

.migration-banner__btn {
  padding: 4px 10px;
  border-radius: var(--border-radius-sm);
  border: 1px solid rgba(0, 0, 0, 0.4);
  background: transparent;
  color: black;
  font-size: 13px;
  font-weight: 600;
  cursor: pointer;

  &:hover {
    background: rgba(0, 0, 0, 0.1);
  }
}

.migration-banner__btn--primary {
  background: black;
  color: var(--color-warning);
  border-color: black;

  &:hover {
    opacity: 0.85;
    background: black;
  }
}

.playlist-section {
  // The one flexible pane: it takes whatever the cart and mixer leave. Its
  // floor is PLAYLIST_MIN_PX, enforced by the drag clamps in the script
  // rather than a CSS min-width — a CSS minimum on a flex item that can't
  // shrink past it made the row overflow instead, with the mixer pushed off
  // the right of the window.
  flex: 1 1 0;
  min-width: 0;
  overflow: hidden;
}

.mixer-section {
  flex: 0 0 auto;
  display: flex;
  min-width: 0;
  overflow: hidden;
  border-left: 1px solid var(--color-border);

  // 'full' mode: the pane takes the whole row (the playlist and cart are
  // withheld), with only its left-edge handle beside it.
  &.mixer-section--fill {
    flex: 1 1 0;
    border-left: none;
  }
}

.resize-handle {
  width: 5px;
  background-color: var(--color-border);
  cursor: col-resize;
  transition: background-color var(--transition-fast);
  position: relative;
  z-index: 10;
  flex: 0 0 auto;
  /* Claim the gesture outright: without this the browser treats a touch-drag
     on the bar as a pan and never delivers pointermove to us. */
  touch-action: none;
  -webkit-user-select: none;
  user-select: none;
  -webkit-tap-highlight-color: transparent;

  /* Invisible grab zone. A 5px bar is a fine mouse target but far below the
     ~24px a finger can reliably hit, so widen the *hit* area without moving the
     pixels the user sees. Only on touch-capable displays — on a pure mouse
     setup the extra 20px would sit over the playlist's scrollbar for no gain,
     and mouse dragging already works at 5px. */
  &::before {
    content: '';
    position: absolute;
    top: 0;
    bottom: 0;
    left: 0;
    right: 0;
  }

  @media (any-pointer: coarse) {
    &::before {
      left: -10px;
      right: -10px;
    }
  }

  &:hover {
    background-color: var(--color-accent);
  }

  &:active,
  &.dragging {
    background-color: var(--color-accent);
  }

  /* Collapsed states: the handle stays in its flex slot as a thin transparent
     bar (8px, the line drawn by ::after) on the edge of the pane it reopens —
     left edge for a pane that filled the row, right edge for one that closed.
     Staying in flow (rather than position: absolute against the row) is what
     keeps a closed cart's handle between the playlist and a docked mixer
     instead of floating over the mixer's own handle. */
  &.collapsed-left {
    width: 8px;
    background-color: transparent;

    /* The grab zone may only grow inward, over the open pane beside it —
       growing outward too would sit over the neighbouring separator. */
    @media (any-pointer: coarse) {
      &::before {
        left: 0;
        right: -16px;
      }
    }

    &::after {
      content: '';
      position: absolute;
      left: 0;
      top: 0;
      bottom: 0;
      width: 2px;
      background-color: var(--color-border);
      opacity: 0.5;
    }
    
    &:hover::after {
      width: 4px;
      background-color: var(--color-accent);
      opacity: 1;
    }
  }
  
  &.collapsed-right {
    width: 8px;
    background-color: transparent;

    @media (any-pointer: coarse) {
      &::before {
        left: -16px;
        right: 0;
      }
    }

    &::after {
      content: '';
      position: absolute;
      right: 0;
      top: 0;
      bottom: 0;
      width: 2px;
      background-color: var(--color-border);
      opacity: 0.5;
    }
    
    &:hover::after {
      width: 4px;
      background-color: var(--color-accent);
      opacity: 1;
    }
  }
}

.cart-section {
  overflow: hidden;
  // Explicit px width, never shrunk: with the default flex-shrink: 1 any
  // over-request got divided between playlist *and* cart, so cart's rendered
  // width was less than the `cartWidth` just set and the divider tracked the
  // pointer at a fractional slope instead of 1:1. Pinning shrink to 0 makes
  // the rendered width equal `cartWidth` by construction; the playlist (the
  // one flexible item) gives instead, and the drag clamps keep it above
  // PLAYLIST_MIN_PX.
  flex: 0 0 auto;

  // Fullscreen cart: it becomes the flexible pane in the playlist's place,
  // so a docked mixer keeps its own width beside it.
  &.cart-section--fill {
    flex: 1 1 0;
    min-width: 0;
  }
}
</style>
