<template>
  <!-- The properties panel's height rides on the CSS variable it already reads
       (PropertiesPanel.vue: `height: var(--properties-panel-height)`), set here
       rather than in main.scss so the drag can change it without the panel
       needing to know it is resizable. -->
  <div
    class="main-workspace"
    :class="{ 'show-mode': uiMode === 'playback' }"
    :style="{ '--properties-panel-height': propertiesHeight + 'px' }"
  >
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

    <!-- A bus whose output names hardware this machine does not have is
         silent, deliberately: the server stopped falling back to the default
         device, because a sub-mix arriving out of the house at the wrong venue
         is worse than silence. Silence still has to be SAID, though, or the
         operator finds out by firing a cue and hearing nothing.

         Only buses that actually have cues on them, and never the preview bus
         — an unmapped Preview Out is the shipped default and correct. -->
    <div v-if="unboundBuses.length && !unboundDismissed" class="migration-banner" role="alert">
      <div class="migration-banner__text">
        <p class="migration-banner__title">{{ t('mixer.unboundTitle') }}</p>
        <p class="migration-banner__body">
          {{ t('mixer.unboundBody', { buses: unboundBuses.map(b => b.name).join(', ') }) }}
        </p>
      </div>
      <div class="migration-banner__actions">
        <button class="migration-banner__btn migration-banner__btn--primary" @click="openOutputMap">
          {{ t('mixer.outputMapButton') }}
        </button>
        <button class="migration-banner__btn" @click="unboundSignatureDismissed = unboundSignature">
          {{ t('migration.dismiss') }}
        </button>
      </div>
    </div>

    <PlaybackControls />

    <!-- One flex row of whichever panes are docked, always in the order
         playlist | cart | mixer. The leftmost docked pane stretches (flex: 1,
         min-width 0); every pane to its right carries an explicit px width with
         flex-shrink 0, and the drag handlers clamp those widths so the row
         never over-requests.

         Every separator follows one rule: it sizes the pane on its RIGHT. Drag
         it far enough right and that pane closes; far enough left and that pane
         expands to fill the workspace. Both end the drag — the pane's own
         header and the workspace bar are the way back, so nothing has to stay
         behind as a handle, and there is no threshold for a resting pointer to
         flicker across.

         Keyed by pane, so toggling one never remounts the others. -->
    <div ref="workspaceEl" class="workspace-content">
      <template v-for="(p, i) in dockedPanes" :key="p">
        <div
          v-if="i > 0"
          class="resize-handle"
          :class="{ dragging: resizingPane === p }"
          role="separator"
          aria-orientation="vertical"
          @pointerdown="startResize($event, p)"
          @dblclick="resetWidth(p)"
        ><span class="resize-grip" aria-hidden="true"></span></div>
        <div
          class="pane"
          :class="[`pane--${p}`, { 'pane--fill': p === flexPane }]"
          :style="p === flexPane ? undefined : { width: `${paneWidth(p)}px` }"
        >
          <PlaylistView v-if="p === 'playlist'" />
          <CartPlayer v-else-if="p === 'cart'" />
          <MixerPanel v-else :mode="mixerMode" @mode="onMixerMode" />
        </div>
      </template>

      <!-- Nothing docked: every pane is switched off, or away in its own
           window. Say so, and offer the way back right here too. -->
      <div v-if="dockedPanes.length === 0" class="workspace-empty">
        <span class="material-symbols-rounded workspace-empty__icon">dashboard</span>
        <p class="workspace-empty__title">{{ t('workspace.emptyTitle') }}</p>
        <p class="workspace-empty__hint">{{ t('workspace.emptyHint') }}</p>
        <div class="workspace-empty__actions">
          <Btn icon="queue_music" :text="t('playlist.title')" @click="showPane('playlist')" />
          <Btn icon="grid_view" :text="t('cart.title')" @click="showPane('cart')" />
          <Btn icon="instant_mix" :text="t('mixer.title')" @click="showPane('mixer')" />
        </div>
      </div>
    </div>

    <!-- Properties panel is an edit affordance — never surfaced in Show Mode.
         Its handle is a sibling above it rather than part of the panel, for the
         same reason the vertical ones are: the thing being dragged is the
         boundary between two panes, and it has to keep working while the pane
         below it is at its minimum. -->
    <template v-if="uiMode !== 'playback' && propertiesPanelOpen && selectedItem">
      <div
        class="resize-handle-h"
        :class="{ dragging: isResizingProps }"
        role="separator"
        aria-orientation="horizontal"
        :aria-label="t('properties.resize')"
        @pointerdown="startPropsResize"
        @dblclick="propertiesHeight = PROPERTIES_DEFAULT_PX"
      ><span class="resize-grip-h" aria-hidden="true"></span></div>
      <PropertiesPanel />
    </template>

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
import Btn from './Btn.vue';
import {
  CART_DEFAULT_PX, MIXER_DEFAULT_PX, PROPERTIES_DEFAULT_PX, type PaneId,
} from '~/composables/useWorkspaceLayout';

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

// The pane layout (see useWorkspaceLayout for the model). The refs live there
// so every writer — the splitters below, the panes' own controls, the header's
// workspace bar — is changing the same state, and persistence is one watcher.
const {
  panes, restoreSet, dockedPanes, flexPane,
  cartWidth, mixerWidth, propertiesHeight,
  cartDetached, mixerDetached,
  showPane, hidePane, expandPane, restorePanes,
  persistLayout, flushLayout,
} = useWorkspaceLayout();
// 'full' whenever the mixer is the pane that stretches — the one state in
// which its channel view has the room it needs.
const { mixerMode } = useMixerView();

const workspaceEl = ref<HTMLElement | null>(null);

// Splitter geometry.
// The mixer's floor is its header: Add Bus, Hardware Outputs and the window
// controls have to fit as icons beside the two pinned strips.
const PANE_MIN_PX: Record<PaneId, number> = { playlist: 240, cart: 300, mixer: 280 };
const PANE_DEFAULT_PX: Record<PaneId, number> = {
  playlist: 0, cart: CART_DEFAULT_PX, mixer: MIXER_DEFAULT_PX,
};
const HANDLE_PX = 10;             // keep in step with .resize-handle
const SNAP_PX = 100;              // drag this far past a limit to close/expand

// Only the cart and the mixer ever have a fixed width: the playlist is always
// leftmost when docked, so it is always the pane that stretches.
function paneWidth(p: PaneId): number {
  return p === 'cart' ? cartWidth.value : p === 'mixer' ? mixerWidth.value : 0;
}
function setPaneWidth(p: PaneId, w: number) {
  if (p === 'cart') cartWidth.value = w;
  else if (p === 'mixer') mixerWidth.value = w;
}

/** The fixed-width panes, i.e. every docked pane but the stretching one. */
const fixedPanes = computed(() => dockedPanes.value.slice(1));

/**
 * The most `p` may have while the stretching pane keeps its minimum and every
 * other fixed pane keeps its width. Minimums win over this: only on a window
 * too narrow for every minimum at once can the row still over-request.
 */
function maxWidthFor(p: PaneId, containerWidth: number): number {
  const flex = flexPane.value;
  const others = fixedPanes.value
    .filter(q => q !== p)
    .reduce((sum, q) => sum + paneWidth(q), 0);
  const handles = HANDLE_PX * Math.max(0, dockedPanes.value.length - 1);
  return containerWidth - others - handles - (flex ? PANE_MIN_PX[flex] : 0);
}

const clampWidth = (w: number, min: number, max: number) => Math.max(min, Math.min(w, max));

function resetWidth(p: PaneId) {
  setPaneWidth(p, PANE_DEFAULT_PX[p]);
  nextTick(reclampPanes);
}

// The mixer's own Expand/Dock requests (opening a channel view asks for the
// whole row). Routed through the same model as every other pane's controls.
function onMixerMode(mode: 'side' | 'full') {
  if (mode === 'full') expandPane('mixer');
  else restorePanes();
}

// Re-clamp after a window resize, or when a pane appears, so widths last
// dragged on a bigger screen can never leave the row over-requesting. Right
// to left: the rightmost pane is the one furthest from the stretching one.
function reclampPanes() {
  const container = workspaceEl.value;
  if (!container) return;
  const width = container.getBoundingClientRect().width;
  if (width <= 0) return;
  for (const p of [...fixedPanes.value].reverse()) {
    setPaneWidth(p, clampWidth(paneWidth(p), PANE_MIN_PX[p], maxWidthFor(p, width)));
  }
  // The properties panel is clamped against the WINDOW, not this row's width,
  // because it is the one pane that competes with the row for height rather
  // than sitting inside it. Shortening the window otherwise leaves a panel
  // taller than the space above it and pushes the playlist off the bottom.
  propertiesHeight.value =
    clampWidth(propertiesHeight.value, PROPERTIES_MIN_PX, maxPropertiesHeight());
}

// Migration banner's "Open Mixer" action: show the pane and dismiss the banner
// locally — dismissal is per-client view state, see the composable.
function openMixerFromBanner() {
  showPane('mixer');
  server.dismissMigrationBanner();
}

// ---- Properties panel height ---------------------------------------------
// The waveform editor inside the panel is why this is adjustable at all: at
// the fixed 300px it inherited from main.scss it had a few dozen pixels to
// draw in, and setting trim points by eye needs the height.
//
// The panel reads `height: var(--properties-panel-height)` already, so nothing
// in PropertiesPanel changes — the variable is simply set on .main-workspace
// from here instead of being a constant in the stylesheet.
const PROPERTIES_MIN_PX     = 160;
// PROPERTIES_DEFAULT_PX comes from useWorkspaceLayout, which owns the splitter
// defaults now that it has to seed them before any drag (O5: one owner).
// What the workspace above has to keep. The ceiling is measured against the
// window rather than being a second constant, so the panel can take most of a
// tall screen without being able to swallow the playlist on a short one.
const PROPERTIES_TOP_MIN_PX = 220;
const isResizingProps   = ref(false);

function maxPropertiesHeight(): number {
  // Measured from the top of the workspace row: the header, transport and any
  // banner above it are fixed furniture this drag must not eat into.
  const top = workspaceEl.value?.getBoundingClientRect().top ?? 0;
  return Math.max(PROPERTIES_MIN_PX, window.innerHeight - top - PROPERTIES_TOP_MIN_PX);
}

// Same pointer-capture shape as the vertical handles, and for the same reason:
// without capture a touch drag dies the moment the finger leaves the bar.
const startPropsResize = (e: PointerEvent) => {
  if (isResizingProps.value || !e.isPrimary || (e.pointerType === 'mouse' && e.button !== 0)) return;
  const handle = e.currentTarget as HTMLElement | null;
  isResizingProps.value = true;
  e.preventDefault();
  try { handle?.setPointerCapture(e.pointerId); } catch { /* capture is best-effort */ }

  const onMove = (ev: PointerEvent) => {
    if (!isResizingProps.value) return;
    // Distance from the pointer to the bottom of the window, so dragging up
    // grows the panel — the direction the gesture implies.
    propertiesHeight.value =
      clampWidth(window.innerHeight - ev.clientY, PROPERTIES_MIN_PX, maxPropertiesHeight());
  };
  const onUp = (ev: PointerEvent) => {
    isResizingProps.value = false;
    try { handle?.releasePointerCapture(ev.pointerId); } catch { /* best-effort */ }
    document.removeEventListener('pointermove', onMove);
    document.removeEventListener('pointerup', onUp);
    document.removeEventListener('pointercancel', onUp);
  };
  document.addEventListener('pointermove', onMove);
  document.addEventListener('pointerup', onUp);
  document.addEventListener('pointercancel', onUp);
};

// ---- The vertical splitters ----------------------------------------------
// One handler for every separator: it sizes the pane on its right, which is
// always a fixed-width pane (the stretching one is leftmost). That pane's right
// edge is the row's right edge minus whatever fixed panes sit beyond it.
//
// Pointer events (not mouse events) so the splitter is draggable by touch and
// pen as well as mouse. Pointer capture keeps the drag alive when the finger
// slides off the bar — without it a touch drag died on the first move. The
// handle also carries `touch-action: none` so the browser doesn't claim the
// gesture for scrolling before we ever see a pointermove.
//
// Past either limit the drag SNAPS and ends: dragged off to the right, the pane
// closes; dragged past the room the others can spare, it expands to fill the
// workspace. Ending the drag there is what makes the snap clean — the handle
// being dragged no longer exists afterwards, and a pointer resting near a
// threshold can no longer flip the layout back and forth on every move.
const resizingPane = ref<PaneId | null>(null);

const startResize = (e: PointerEvent, p: PaneId) => {
  // Ignore secondary mouse buttons and any second finger landing on the bar —
  // a concurrent drag would register a duplicate set of document listeners.
  if (resizingPane.value || !e.isPrimary || (e.pointerType === 'mouse' && e.button !== 0)) return;
  const handle = e.currentTarget as HTMLElement | null;
  resizingPane.value = p;
  // A snap puts this back: the drag that closes or expands a pane pinned it to
  // a limit on the way, and Restore / reopening should bring back the width it
  // had, not the edge it was dragged against.
  const startWidth = paneWidth(p);
  e.preventDefault();
  try { handle?.setPointerCapture(e.pointerId); } catch { /* capture is best-effort */ }

  const onMove = (ev: PointerEvent) => {
    if (resizingPane.value !== p) return;
    const container = workspaceEl.value;
    if (!container) return;
    const rect = container.getBoundingClientRect();

    const order = fixedPanes.value;
    const beyond = order.slice(order.indexOf(p) + 1)
      .reduce((sum, q) => sum + paneWidth(q) + HANDLE_PX, 0);
    const width = rect.right - beyond - ev.clientX;
    const maxWidth = maxWidthFor(p, rect.width);

    if (width < SNAP_PX) {
      end();
      setPaneWidth(p, startWidth);
      hidePane(p);
      return;
    }
    if (width > maxWidth + SNAP_PX) {
      end();
      setPaneWidth(p, startWidth);
      expandPane(p);
      return;
    }
    setPaneWidth(p, clampWidth(width, PANE_MIN_PX[p], maxWidth));
  };

  const end = () => {
    resizingPane.value = null;
    try { handle?.releasePointerCapture(e.pointerId); } catch { /* already released */ }
    document.removeEventListener('pointermove', onMove);
    document.removeEventListener('pointerup', end);
    // A touch drag interrupted by the OS (gesture takeover, call, etc.) fires
    // pointercancel instead of pointerup — without this the handle stayed
    // "stuck" to the finger and kept resizing on the next touch anywhere.
    document.removeEventListener('pointercancel', end);
  };

  document.addEventListener('pointermove', onMove);
  document.addEventListener('pointerup', end);
  document.addEventListener('pointercancel', end);
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

  // Detached windows. However a window was opened — the pane's own button, the
  // OS Window menu, or Electron restoring it at launch — the pane is ON while it
  // exists, so the workspace bar lights it. Closing the window by any route
  // other than switching the pane off docks it back where it was: switching it
  // off clears `panes` first, so this sees nothing left to dock.
  window.electronAPI.onCartPlayerWindowOpened(() => {
    cartDetached.value = true;
    if (!panes.value.cart) panes.value = { ...panes.value, cart: true };
  });
  window.electronAPI.onCartPlayerWindowClosed(() => {
    cartDetached.value = false;
  });
  window.electronAPI.onMixerWindowOpened?.(() => {
    mixerDetached.value = true;
    if (!panes.value.mixer) panes.value = { ...panes.value, mixer: true };
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
  if (server.isLocalServer) {
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

// Buses that will make no sound: unbound, carrying cues, and not the preview
// bus. `bound` is server-computed and server.buses is kept fresh by the
// buses_patched / outputs_changed broadcasts, so this needs no polling.
//
// The itemUuids test is what keeps this from nagging. An unbound bus with
// nothing routed to it is a configuration detail; an unbound bus with cues on
// it means those cues are silent, which is the thing worth interrupting for.
const unboundBuses = computed(() =>
  (server.buses ?? []).filter(
    (b: any) => !b.preview && b.bound === false && (b.itemUuids?.length ?? 0) > 0
  )
);
// Dismissal is per-client view state, like the migration banner — but keyed on
// WHICH buses are unbound, so dismissing today's warning does not hide a
// different one tomorrow.
const unboundSignature = computed(() =>
  unboundBuses.value.map((b: any) => b.id).sort().join('|')
);
const unboundSignatureDismissed = ref('');
const unboundDismissed = computed(
  () => unboundSignature.value !== '' && unboundSignature.value === unboundSignatureDismissed.value
);
// Exactly what the note here predicted: the map has moved into Settings, so
// this is a plain deep link to that section. It no longer has to force the
// mixer panel open first — the banner used to need the panel up because the
// modal was rendered inside it, which meant reporting an unbound output also
// rearranged the operator's workspace.
function openOutputMap() {
  useSettingsPage().open('outputs');
}

// Transport keys and MIDI belong to the workspace, not to the cart pane.
//
// They used to be mounted by CartPlayer, which meant Space, Escape, the arrows
// and every MIDI binding stopped working whenever the cart pane was closed,
// collapsed or popped out — a transport key dying because an unrelated panel
// was hidden is a show-stopper, and it gets worse as more panes become
// closable. The workspace is the right owner: it exists for exactly as long as
// a project is open, which was the real precondition all along.
//
// Exactly one owner per window. The detached cart window has no MainWorkspace,
// so CartPlayer still mounts them there, and only when detached — so the two
// never both claim the same window.
const { mount: mountHotkeys, unmount: unmountHotkeys } = useCartHotkeys();
const { mount: mountMidi, unmount: unmountMidi } = useMidiController();

// One watcher for the whole layout, so every site that moves a boundary or
// switches a pane persists it without knowing that it does. The write is
// debounced inside the composable, which is what stops a pointermove stream
// becoming a localStorage write per frame.
watch(
  [cartWidth, mixerWidth, propertiesHeight, panes, restoreSet],
  () => persistLayout(),
);

// Re-clamp when the set of docked panes changes, not only when the window
// resizes. A width stored while its pane was hidden has never been measured
// against this row. reclampPanes is idempotent, so the write it may make
// settles immediately rather than chasing itself.
watch(dockedPanes, () => {
  if (import.meta.client) nextTick(reclampPanes);
});

onMounted(() => {
  if (import.meta.client) {
    window.addEventListener('keydown', handleKeydown);
    window.addEventListener('resize', reclampPanes);
    // The restored widths are the raw numbers last dragged to, on whatever
    // screen that was. Clamp them against THIS window before anyone sees them:
    // a layout saved on a 4K display would otherwise over-request on a laptop
    // and the playlist would be what gave. nextTick so the row has been laid
    // out and getBoundingClientRect has a width to measure.
    nextTick(reclampPanes);
    // A pending debounce dies with the page, taking the last drag with it.
    window.addEventListener('pagehide', flushLayout);
    mountHotkeys();
    mountMidi();
  }
});

onUnmounted(() => {
  if (import.meta.client) {
    window.removeEventListener('keydown', handleKeydown);
    window.removeEventListener('resize', reclampPanes);
    window.removeEventListener('pagehide', flushLayout);
    // Same reason as pagehide, for the case where this component goes away
    // while the page stays — a detached window taking the panel over, say.
    flushLayout();
    unmountHotkeys();
    unmountMidi();
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
  /* Paint the row: anything transparent above it would otherwise show
     the window's default white. */
  background-color: var(--color-background);
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

.pane {
  // Explicit px width, never shrunk: with the default flex-shrink: 1 any
  // over-request got divided between panes, so a pane's rendered width was
  // less than the width just set and the divider tracked the pointer at a
  // fractional slope instead of 1:1. The stretching pane gives instead, and
  // the drag clamps keep it above its minimum.
  flex: 0 0 auto;
  display: flex;
  flex-direction: column;
  min-width: 0;
  overflow: hidden;

  // The leftmost docked pane takes whatever the others leave. Its floor is
  // enforced by the drag clamps rather than a CSS min-width — a CSS minimum on
  // a flex item that can't shrink past it made the row overflow instead, with
  // the right-hand panes pushed off the window.
  &.pane--fill {
    flex: 1 1 0;
  }

  > :deep(*) {
    flex: 1 1 auto;
    min-height: 0;
  }
}

.workspace-empty {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-sm);
  padding: var(--spacing-xl);
  text-align: center;
  color: var(--color-text-secondary);

  p { margin: 0; color: inherit; }
}

.workspace-empty__icon {
  font-size: 48px;
  opacity: 0.5;
}

.workspace-empty__title {
  font-size: 16px;
  font-weight: 600;
  color: var(--color-text-primary) !important;
}

.workspace-empty__hint {
  font-size: 13px;
  max-width: 420px;
}

.workspace-empty__actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: center;
  gap: var(--spacing-sm);
  margin-top: var(--spacing-sm);
}

/* The properties panel's top edge. The same object as the vertical handles
   turned through ninety degrees: same grab zone, same coarse-pointer widening,
   same grip, row-resize instead of col-resize. It is flex:0 0 auto in the
   workspace column, so it holds its 10px while the panel below it changes
   height.

   Double-click restores PROPERTIES_DEFAULT_PX — the usual escape hatch for a
   splitter dragged somewhere unhelpful, and the only way back to the height
   the panel used to have. */
.resize-handle-h {
  height: 10px;
  box-sizing: border-box;
  background-color: var(--color-surface);
  border-top: 1px solid var(--color-border);
  border-bottom: 1px solid var(--color-border);
  cursor: row-resize;
  transition: background-color var(--transition-fast), border-color var(--transition-fast);
  position: relative;
  z-index: 10;
  flex: 0 0 auto;
  display: flex;
  align-items: center;
  justify-content: center;
  touch-action: none;
  -webkit-user-select: none;
  user-select: none;
  -webkit-tap-highlight-color: transparent;

  &::before {
    content: '';
    position: absolute;
    top: 0;
    bottom: 0;
    left: 0;
    right: 0;
  }
  /* A finger needs ~24px; on a mouse rig the extra height would sit over the
     panel's own header for no gain. */
  @media (any-pointer: coarse) {
    &::before {
      top: -10px;
      bottom: -10px;
    }
  }

  /* The grip runs the other way: wide and short rather than tall and narrow. */
  .resize-grip-h {
    width: 16px;
    height: 4px;
    background-image: radial-gradient(circle, var(--color-text-secondary) 1px, transparent 1.4px);
    background-size: 5.33px 4px;
    background-position: center;
    opacity: 0.8;
    pointer-events: none;
  }

  &:hover,
  &.dragging {
    background-color: var(--color-accent);
    border-color: var(--color-accent);
  }
}

.resize-handle {
  /* Keep in step with HANDLE_PX in the script: the splitter maths reserves
     exactly this width. */
  width: 10px;
  box-sizing: border-box;
  background-color: var(--color-surface);
  border-left: 1px solid var(--color-border);
  border-right: 1px solid var(--color-border);
  cursor: col-resize;
  transition: background-color var(--transition-fast), border-color var(--transition-fast);
  position: relative;
  z-index: 10;
  flex: 0 0 auto;
  display: flex;
  align-items: center;
  justify-content: center;
  /* Claim the gesture outright: without this the browser treats a touch-drag
     on the bar as a pan and never delivers pointermove to us. */
  touch-action: none;
  -webkit-user-select: none;
  user-select: none;
  -webkit-tap-highlight-color: transparent;

  /* Invisible grab zone, widened on touch displays only (a finger needs
     ~24px; on a mouse rig the extra width would sit over the playlist's
     scrollbar for no gain). */
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

  /* Six-dot grip in the middle: says "drag me" without a label. */
  .resize-grip {
    width: 4px;
    height: 16px;
    background-image: radial-gradient(circle, var(--color-text-secondary) 1px, transparent 1.4px);
    background-size: 4px 5.33px;
    background-position: center;
    opacity: 0.8;
    pointer-events: none;
  }

  &:hover,
  &:active,
  &.dragging {
    background-color: var(--color-surface-hover, var(--color-surface));
    border-color: var(--color-accent);

    .resize-grip {
      background-image: radial-gradient(circle, var(--color-accent) 1px, transparent 1.4px);
      opacity: 1;
    }
  }
}

</style>
