// =====================================================================
// useProject.ts
// ---------------------------------------------------------------------
// Project state hook. The *server* owns the canonical project document
// (it's responsible for reading/writing .liveplay files and feeding
// audio items into the engine). The client maintains a reactive mirror
// of that document — components mutate it as before, and a debounced
// watcher pushes changes back to the server.
//
// Migration intent (Milestone 1): all file I/O goes through
// `useLiveplayServer`. The Electron `window.electronAPI` file methods
// are no longer used here — the server may be running on a different
// machine and Electron file paths only make sense on the same host.
//
// The public API of this composable is preserved so the existing
// components (PlaylistView, CartSlot, PropertiesPanel, etc.) can keep
// mutating `currentProject.value` and have those changes propagate.
// =====================================================================
import { v4 as uuidv4 } from 'uuid';
import { markRaw, nextTick, toRaw, triggerRef } from 'vue';
import type {
  Project,
  AudioItem,
  GroupItem,
  BaseItem,
  Theme,
  CartItem
} from '~/types/project';
import { DEFAULT_THEME, DEFAULT_CART_SLOT_KEYS } from '~/types/project';
import { applyAutoProcessing } from '~/utils/audio';

// ---------------------------------------------------------------------------
// MODULE-SCOPED state for cross-call coordination.
//
// `useProject()` is invoked once per component (35+ components do so), so any
// variables declared inside the function body live in a per-call closure.
// That used to mean every component installed its OWN set of debounced
// watchers (theme, settings, cart, items, fallback) — N watchers all firing
// on every reactive change, producing N parallel PATCH/POST/PUT calls. That
// in turn produced N broadcast echoes which fed back into the watchers, and
// the system melted down.
//
// The reactive *state* is already shared (via Nuxt's useState), so we only
// need to install the sync watchers exactly once for the whole app. The
// guard below is that one-shot latch; the install hooks are kept here so
// streamItemPages / closeProject (which live inside the per-call closure)
// can still poke into the single global watcher block.
// ---------------------------------------------------------------------------
let _syncWatchersInstalled = false;
let _refreshItemsBaselineAfterHydrate: () => void = () => {};
let _installItemsWatcherFn:   null | (() => void) = null;
let _uninstallItemsWatcherFn: null | (() => void) = null;

// UUIDs of items that were just added in this session and are waiting for
// their first waveform so that auto-process (trim + normalise) can run.
// Items loaded from the saved project file are NEVER added here — only
// items created via addItem() with no pre-existing waveform. The set is
// cleared on project close/open so stale entries don't accumulate.
const _autoProcessPendingUuids = new Set<string>();

// ---------------------------------------------------------------------------
// Client-side waveform cache (uuid -> WaveformData).
// ---------------------------------------------------------------------------
// Waveform peaks are CLIENT/worker-owned: they're generated locally and
// deliberately stripped from every sync payload to the server (the server has
// no peaks). That means any time the server re-broadcasts project state for
// multi-client sync — an item_updated echo, a remove+re-add reorder, or a
// full project_changed restream — the item objects come back WITHOUT a
// waveform. Re-rendering from those would blank every waveform on, e.g., an
// "auto-adjust volume" press.
//
// The fix is to treat the client as the source of truth for waveforms: cache
// each waveform by uuid as it arrives, and re-attach it to any item that
// reappears from the server missing one. The peaks never change unless the
// underlying media is re-imported (a new uuid), so a uuid-keyed cache is
// always valid for the session. Cleared on project close.
const _waveformCache = new Map<string, any>();

const cacheWaveform = (uuid: string, wf: any): void => {
  if (uuid && wf && Array.isArray(wf.peaks) && wf.peaks.length > 0) {
    _waveformCache.set(uuid, markRaw(wf));
  }
};

// Ensure an audio item carries its waveform: if it already has peaks, make
// sure they're cached; if it's missing them but we've seen them this session,
// re-attach from the cache. No-op for non-audio items.
const restoreWaveform = (item: any): void => {
  if (!item || item.type !== 'audio') return;
  if (item.waveform && Array.isArray(item.waveform.peaks) && item.waveform.peaks.length > 0) {
    cacheWaveform(item.uuid, item.waveform);
    return;
  }
  const cached = _waveformCache.get(item.uuid);
  if (cached) item.waveform = cached;
};

// The "anchor" of the current selection — the item a shift-click range
// extends FROM, and the item whose properties the panel shows. Module-scoped
// because the selection state itself is shared across all useProject() calls
// via useState. Not reactive: it's only ever read inside selection handlers.
let selectionAnchorUuid: string | null = null;

// ---- Repair dialog state (module-scoped so it works across all useProject() calls) ----
const repairDialogVisible = ref(false);
const repairDialogIssues  = ref<string[]>([]);
let _repairPromiseResolve: ((confirmed: boolean) => void) | null = null;

// ---- Unsaved-changes dialog state (module-scoped, shared across calls) ----
// Shown when the user tries to leave the current project (New / Open / Close)
// while autosave is off and edits are pending. Resolves the in-flight guard
// promise with the user's choice.
const unsavedDialogVisible = ref(false);
let _unsavedPromiseResolve: ((choice: 'save' | 'discard' | 'cancel') => void) | null = null;

export const useProject = () => {
  const currentProject = useState<Project | null>('currentProject', () => null);
  // The active/anchor selection is stored as a UUID, not as an object
  // reference. `selectedItem` is then a computed that re-resolves that UUID
  // against the LIVE items tree on every read. This is critical for
  // multi-client correctness: when the server echoes a change back, the items
  // array (or individual item objects) can be rebuilt/replaced, which would
  // leave a cached object reference "orphaned" — detached from the rendered
  // tree. Editing an orphan silently does nothing (the playlist keeps showing
  // the old object, the diff-watcher never sees the change), which is the
  // "I can't change properties again until I reopen the panel" bug. Resolving
  // by UUID means the panel always edits the object that is actually rendered
  // and synced.
  const selectedItemUuid = useState<string | null>('selectedItemUuid', () => null);
  const selectedItem = computed<BaseItem | null>({
    get: () => (selectedItemUuid.value ? findItemByUuid(selectedItemUuid.value) : null),
    set: (item) => { selectedItemUuid.value = item ? (item as BaseItem).uuid : null; },
  });
  const selectedItems = useState<Set<string>>('selectedItems', () => new Set()); // Track multiple selections by UUID
  // Whether the properties panel is actually shown. Kept SEPARATE from
  // selectedItem so that plain row/slot clicks can update the active
  // selection (and the panel's contents, when it's open) WITHOUT forcing
  // the panel open on every click. The panel is only opened explicitly via
  // openItemProperties() (the gear button).
  const propertiesPanelOpen = useState<boolean>('propertiesPanelOpen', () => false);
  // NB: activeCues is owned by useAudioEngine (it's the server projection).
  // useProject only clears it on closeProject; we re-use the same useState key.
  const activeCues = useState<Map<string, any>>('activeCues', () => new Map());
  const waveformUpdateKey = useState<number>('waveformUpdateKey', () => 0);

  // ---- Server sync plumbing --------------------------------------------
  // We keep a flag so the watcher can ignore reactivity bursts triggered by
  // hydration from the server (otherwise we'd ping-pong updates).
  const isHydrating = useState<boolean>('useProject.isHydrating', () => false);
  // Path the server should write to on save. Populated by openProject /
  // createNewProject. Persisted in the document under `server.projectFilePath`
  // by the server itself, so we can read it back on subsequent fetches.
  const projectFilePathRef = useState<string>('useProject.projectFilePath', () => '');

  // Loading state — set true during open/create/save so the UI can render a
  // loading overlay. `loadingMessage` is the title shown in the overlay.
  const isLoading = useState<boolean>('useProject.isLoading', () => false);
  const loadingMessage = useState<string>('useProject.loadingMessage', () => '');
  // Background audio-loading progress (polled while the server is still
  // mirroring cues into the engine in a worker thread). loading=false means
  // every audio cue is ready to play.
  const audioLoadingProgress = useState<{ loading: boolean; loaded: number; total: number }>(
    'useProject.audioLoadingProgress',
    () => ({ loading: false, loaded: 0, total: 0 }),
  );
  // Server-owned preview state: at most one item is "being previewed" at a
  // time (DJ pre-listen on settings.previewDevice). Empty string = no
  // preview active. The server updates this; the client mirrors it for UI.
  const previewItemUuid = useState<string>('useProject.previewItemUuid', () => '');
  // Engine cue ID for the active preview — needed to subscribe to its meter
  // stream and drive the seek bar / playhead time in the preview card.
  const previewCueId = useState<string>('useProject.previewCueId', () => '');

  // ---- Autosave -----------------------------------------------------------
  // By default the project auto-saves to disk on every edit (saveProject() is
  // called all over the app after mutations). The user can turn that off via
  // the header toggle; the preference lives in the project's settings so it
  // persists across reopens. When autosave is off, edit-triggered saves only
  // flag `hasUnsavedChanges` instead of writing the file — the server's
  // in-memory document is still kept current by the granular sync watchers, so
  // only the .liveplay file on disk lags until an explicit save flushes it.
  const hasUnsavedChanges = useState<boolean>('useProject.hasUnsavedChanges', () => false);
  // Autosave is on unless the project explicitly opted out (older projects
  // without the key default to on).
  const autoSaveEnabled = computed<boolean>(
    () => (currentProject.value as any)?.settings?.autoSave !== false,
  );

  // ---- Preview controls (delegate to server) ----------------------------
  async function startPreview(itemUuid: string) {
    if (!itemUuid) return;
    const server = useLiveplayServer();
    try {
      const res = await server.startPreview(itemUuid);
      previewItemUuid.value = itemUuid;
      previewCueId.value = (res as any)?.cueId ?? '';
    } catch (e) {
      console.warn('[useProject] startPreview failed:', e);
      // Refresh authoritative state from the server.
      try {
        const s = await server.fetchPreviewState();
        previewItemUuid.value = s.itemUuid || '';
        previewCueId.value = (s as any).cueId ?? '';
      }
      catch { /* ignore */ }
    }
  }
  async function stopPreview() {
    const server = useLiveplayServer();
    try {
      await server.stopPreview();
    } catch { /* ignore */ }
    previewItemUuid.value = '';
    previewCueId.value = '';
  }

  // Force UI update for waveforms
  const triggerWaveformUpdate = () => {
    waveformUpdateKey.value++;
  };

  // Multi-select helpers
  const toggleItemSelection = (uuid: string, isCtrlKey: boolean, isShiftKey: boolean) => {
    if (!currentProject.value) return;

    if (isShiftKey && selectedItems.value.size > 0) {
      // Shift-click: select range
      const allItems = getAllItemsFlat(currentProject.value.items);
      const lastSelectedUuid = selectionAnchorUuid ?? Array.from(selectedItems.value).pop();
      const lastIndex = allItems.findIndex(item => item.uuid === lastSelectedUuid);
      const currentIndex = allItems.findIndex(item => item.uuid === uuid);

      if (lastIndex !== -1 && currentIndex !== -1) {
        const start = Math.min(lastIndex, currentIndex);
        const end = Math.max(lastIndex, currentIndex);

        for (let i = start; i <= end; i++) {
          selectedItems.value.add(allItems[i].uuid);
        }
      }
    } else if (isCtrlKey) {
      // Ctrl-click: toggle individual item
      if (selectedItems.value.has(uuid)) {
        selectedItems.value.delete(uuid);
      } else {
        selectedItems.value.add(uuid);
      }
    } else {
      // Normal click: select only this item
      selectedItems.value.clear();
      selectedItems.value.add(uuid);
    }

    // Keep `selectedItem` (the panel anchor) in sync with the click so that
    // an OPEN properties panel follows the current selection. The panel's
    // visibility is governed by propertiesPanelOpen, not by selectedItem, so
    // updating this here does NOT pop the panel open on a plain click.
    if (selectedItems.value.size === 0) {
      selectedItem.value = null;
      selectionAnchorUuid = null;
    } else if (selectedItems.value.has(uuid)) {
      // The clicked item is now selected — make it the anchor.
      selectedItem.value = findItemByUuid(uuid);
      selectionAnchorUuid = uuid;
    } else {
      // Ctrl-click that DESELECTED the clicked item — fall back to any
      // remaining selected item as the anchor.
      const remaining = Array.from(selectedItems.value).pop()!;
      selectedItem.value = findItemByUuid(remaining);
      selectionAnchorUuid = remaining;
    }
  };

  // Explicitly open the properties panel for an item (called by Edit buttons)
  const openItemProperties = (uuid: string) => {
    const item = findItemByUuid(uuid);
    if (item) {
      selectedItem.value = item;
      selectionAnchorUuid = uuid;
      // Ensure the item is in selectedItems so batch operations (normalize,
      // trim silence, etc.) in PropertiesPanel target the correct item.
      // Cart Edit buttons don't go through row/header selection, so the set
      // may contain stale playlist UUIDs — clear and select only this item.
      if (!selectedItems.value.has(uuid)) {
        selectedItems.value.clear();
        selectedItems.value.add(uuid);
      }
      propertiesPanelOpen.value = true;
    }
  };

  // Select every item in the playlist (Ctrl+A). The last item becomes the
  // anchor so a subsequent shift-click extends sensibly.
  const selectAllItems = () => {
    if (!currentProject.value) return;
    const all = getAllItemsFlat(currentProject.value.items);
    selectedItems.value.clear();
    for (const it of all) selectedItems.value.add(it.uuid);
    if (all.length > 0) {
      const last = all[all.length - 1];
      selectedItem.value = last;
      selectionAnchorUuid = last.uuid;
    }
  };

  // Get all items as a flat array (for shift-select)
  const getAllItemsFlat = (items?: (AudioItem | GroupItem)[]): BaseItem[] => {
    const src = items ?? currentProject.value?.items ?? [];
    const result: BaseItem[] = [];
    for (const item of src) {
      result.push(item);
      if (item.type === 'group') {
        result.push(...getAllItemsFlat(item.children));
      }
    }
    return result;
  };

  // Get all selected items
  const getSelectedItems = (): BaseItem[] => {
    return Array.from(selectedItems.value)
      .map(uuid => findItemByUuid(uuid))
      .filter(item => item !== null) as BaseItem[];
  };

  // ----------------------------------------------------------------------
  // Clipboard / duplicate operations.
  // ----------------------------------------------------------------------
  // Deep-clone an item (and, for groups, its whole subtree) assigning fresh
  // UUIDs throughout. The `waveform` blob is dropped — it's large read-only
  // server data and will be regenerated from mediaServerPath after the clone
  // is inserted (the playlist's missing-waveform scanner picks it up).
  const cloneItemWithNewIds = (item: any): AudioItem | GroupItem => {
    const clone = JSON.parse(JSON.stringify(_deepToRaw(item)));
    const reassign = (it: any) => {
      it.uuid = uuidv4();
      delete it.waveform;
      if (it.type === 'group' && Array.isArray(it.children)) it.children.forEach(reassign);
    };
    reassign(clone);
    return clone as AudioItem | GroupItem;
  };

  // Locate the array (root items or a group's children) that directly
  // contains the given uuid, plus its position within that array.
  const findContainer = (
    uuid: string,
    arr?: (AudioItem | GroupItem)[],
  ): { arr: (AudioItem | GroupItem)[]; i: number } | null => {
    const list = arr ?? currentProject.value?.items ?? [];
    const i = list.findIndex(x => x.uuid === uuid);
    if (i !== -1) return { arr: list, i };
    for (const it of list) {
      if (it.type === 'group') {
        const r = findContainer(uuid, it.children);
        if (r) return r;
      }
    }
    return null;
  };

  // Duplicate the given items in place (each clone inserted right after its
  // original, in the same parent). New selection becomes the clones. Ctrl+D.
  const duplicateItems = (uuids: string[]) => {
    if (!currentProject.value || uuids.length === 0) return;
    const newUuids: string[] = [];
    // Insert after each original independently. Re-resolving the container for
    // every uuid keeps positions correct even as earlier inserts shift things.
    for (const uuid of uuids) {
      const c = findContainer(uuid);
      if (!c) continue;
      const clone = cloneItemWithNewIds(c.arr[c.i]);
      c.arr.splice(c.i + 1, 0, clone);
      newUuids.push(clone.uuid);
    }
    if (newUuids.length === 0) return;
    updateIndices(currentProject.value.items);
    selectedItems.value.clear();
    for (const u of newUuids) selectedItems.value.add(u);
    const last = findItemByUuid(newUuids[newUuids.length - 1]);
    if (last) { selectedItem.value = last; selectionAnchorUuid = last.uuid; }
    saveProject();
  };

  // Copy the given items to the system clipboard as JSON. The payload is
  // wrapped with a type/version marker so paste can recognise it, but it's
  // also just a readable items array so it can be hand-edited / shared.
  const copyItemsToClipboard = async (uuids: string[]): Promise<boolean> => {
    if (!import.meta.client || uuids.length === 0) return false;
    const items = uuids
      .map(uuid => findItemByUuid(uuid))
      .filter((it): it is AudioItem | GroupItem => it !== null)
      .map(it => {
        const raw = _deepToRaw(it);    // strips reactivity + waveform
        return raw;
      });
    if (items.length === 0) return false;
    const payload = { type: 'liveplay/items', version: '2.0.0', items };
    try {
      await navigator.clipboard.writeText(JSON.stringify(payload, null, 2));
      return true;
    } catch (e) {
      console.warn('[useProject] copy to clipboard failed:', e);
      return false;
    }
  };

  // Parse clipboard text into an array of item-like objects. Accepts our
  // wrapped { type:'liveplay/items', items } payload, a bare array of items,
  // or a single item object. Returns [] when the text isn't valid items JSON.
  const parseItemsFromText = (text: string): any[] => {
    let data: any;
    try { data = JSON.parse(text); } catch { return []; }
    let candidates: any[];
    if (data && data.type === 'liveplay/items' && Array.isArray(data.items)) candidates = data.items;
    else if (Array.isArray(data)) candidates = data;
    else if (data && typeof data === 'object') candidates = [data];
    else return [];
    // Keep only things that look like project items.
    return candidates.filter(it =>
      it && typeof it === 'object'
      && (it.type === 'audio' || it.type === 'group')
      && typeof it.displayName === 'string'
    );
  };

  // Paste items from the clipboard into the playlist root (appended). Each
  // pasted item gets fresh UUIDs so it can coexist with the source — even
  // when pasting back into the same project / a different LivePlay instance.
  const pasteItemsFromClipboard = async (): Promise<number> => {
    if (!import.meta.client || !currentProject.value) return 0;
    let text = '';
    try { text = await navigator.clipboard.readText(); } catch (e) {
      console.warn('[useProject] read clipboard failed:', e);
      return 0;
    }
    const parsed = parseItemsFromText(text);
    if (parsed.length === 0) return 0;
    const newUuids: string[] = [];
    for (const it of parsed) {
      const clone = cloneItemWithNewIds(it);
      currentProject.value.items.push(clone);
      newUuids.push(clone.uuid);
    }
    updateIndices(currentProject.value.items);
    selectedItems.value.clear();
    for (const u of newUuids) selectedItems.value.add(u);
    const last = findItemByUuid(newUuids[newUuids.length - 1]);
    if (last) { selectedItem.value = last; selectionAnchorUuid = last.uuid; }
    saveProject();
    return newUuids.length;
  };

  // ----------------------------------------------------------------------
  // Server-backed project I/O.
  // ----------------------------------------------------------------------
  // Create a new empty project, push it to the server, and ask the server
  // to write the .liveplay file.
  const createNewProject = async (name: string, folderPath: string): Promise<boolean> => {
    isLoading.value = true;
    loadingMessage.value = 'Creating project…';
    try {
      const server = useLiveplayServer();
      // Ensure server is reachable; without it we can't actually own state.
      if (!server.connected) {
        try { server.connect(); } catch { /* noop */ }
      }

      const newProject: Project = {
        name,
        version: '2.0.0',
        folderPath,
        items: [],
        cartItems: [],
        cartSlotKeys: { ...DEFAULT_CART_SLOT_KEYS },
        cartOnlyItems: [],
        theme: { ...DEFAULT_THEME },
        createdAt: new Date().toISOString(),
        lastModified: new Date().toISOString()
      };

      // Server-side: replace document with this fresh shell. Returns the
      // header (items array is empty for a fresh project anyway, so no
      // streaming is needed here).
      isHydrating.value = true;
      try {
        const header = await server.replaceProjectDocument(newProject as any);
        applyServerHeader(header);
      } finally {
        // Wait for Vue to flush watchers (which see isHydrating=true and
        // bail) BEFORE clearing the flag. Setting it false synchronously
        // would lose the race: watchers run in a microtask after this
        // function returns, see isHydrating=false, and echo every change
        // back to the server.
        await nextTick();
        isHydrating.value = false;
      }

      // Ask the server to write the file. It joins folderPath/name.liveplay
      // and the server has the document already, so we just say "save here".
      const projectFilePath = `${folderPath}/${name}.liveplay`;
      await server.saveProjectTo(projectFilePath);
      projectFilePathRef.value = projectFilePath;
      return true;
    } catch (error) {
      console.error('Error creating project:', error);
      return false;
    } finally {
      isLoading.value = false;
    }
  };

  // Probe the server for a project already in memory and, if there is one,
  // hydrate the client directly into it (no file load, no spinner). Used by
  // the welcome screen on connect: if another client already opened a
  // project, we drop straight into MainWorkspace instead of showing
  // New/Open. Returns true if a project was rejoined.
  const tryRejoinExistingProject = async (): Promise<boolean> => {
    try {
      const server = useLiveplayServer();
      if (!server.connected) {
        try { server.connect(); } catch { /* noop */ }
      }
      const header = await server.fetchProjectHeader();
      if (!header || !header.hasOpenProject) return false;

      isHydrating.value = true;
      try {
        applyServerHeader(header);
      } finally {
        await nextTick();
        isHydrating.value = false;
      }
      if (header.server?.projectFilePath) {
        projectFilePathRef.value = header.server.projectFilePath;
      }

      const { clearCartOnlyItems, addCartOnlyItem } = useCartItems();
      clearCartOnlyItems();
      const cartOnly = currentProject.value?.cartOnlyItems ?? [];
      for (const item of cartOnly) addCartOnlyItem(item);

      void streamItemPages(server, header.itemCount ?? 0);
      void pollLoadingProgress(server);
      return true;
    } catch (e) {
      console.warn('[useProject] tryRejoinExistingProject failed:', e);
      return false;
    }
  };

  // Open an existing project (path is on the server's filesystem).
  // UX flow:
  //   1. POST /api/project/load — server reads the file, kicks off async
  //      engine mirror, and returns the *header* (theme/settings/cart/
  //      itemCount). The full items array is NOT in this payload, so the
  //      round-trip stays cheap regardless of project size.
  //   2. Apply the header → hide the spinner so the workspace shell can
  //      paint (cart slots, theme, project name).
  //   3. Stream item pages from /api/project/items in batches of 100,
  //      yielding to the renderer between pages so the playlist paints
  //      incrementally instead of in one giant tick.
  //   4. The corner AudioLoadProgress banner takes over for the
  //      audio-mirror phase still running on the server.
  const openProject = async (projectFilePath: string): Promise<boolean> => {
    isLoading.value = true;
    loadingMessage.value = 'Loading project…';
    try {
      const server = useLiveplayServer();
      if (!server.connected) {
        try { server.connect(); } catch { /* noop */ }
      }

      // (1) Ask the server to load the file. Returns the header.
      const header = await server.loadProjectFromPath(projectFilePath);

      // (1b) If the server detected and auto-repaired corruption, prompt the
      //      user and offer to save the repaired version.
      if (header?.needsRepair) {
        repairDialogIssues.value = Array.isArray(header.repairIssues) ? header.repairIssues : [];
        repairDialogVisible.value = true;
        const confirmed = await new Promise<boolean>(resolve => {
          _repairPromiseResolve = resolve;
        });
        repairDialogVisible.value = false;
        if (confirmed) {
          try { await server.repairProject(); } catch { /* save failure is non-fatal */ }
        }
      }

      // (2) Hide the central spinner IMMEDIATELY — header is enough for
      //     the shell to paint. The items array is empty at this point;
      //     pages are pushed in below.
      isLoading.value = false;

      isHydrating.value = true;
      try {
        applyServerHeader(header);
      } finally {
        await nextTick();
        isHydrating.value = false;
      }
      projectFilePathRef.value = projectFilePath;

      // Restore cart-only items to client-side memory store (used by CartSlot).
      const { clearCartOnlyItems, addCartOnlyItem } = useCartItems();
      clearCartOnlyItems();
      const cartOnly = currentProject.value?.cartOnlyItems ?? [];
      for (const item of cartOnly) addCartOnlyItem(item);

      // (3) Stream pages in the background. Don't await — let the caller
      //     return so the workspace can paint while pages trickle in.
      void streamItemPages(server, header?.itemCount ?? 0);

      // (4) Surface the audio-loading progress as a corner indicator.
      void pollLoadingProgress(server);
      return true;
    } catch (error) {
      console.error('Error opening project:', error);
      return false;
    } finally {
      // Defensive — should already be false at this point on success.
      isLoading.value = false;
    }
  };

  // Stream paged items from the server into currentProject.items. Yields
  // to the renderer between pages via requestAnimationFrame so the
  // playlist paints incrementally rather than blocking on one giant tick.
  // Safe to call multiple times — bails if currentProject becomes null.
  let itemStreamSeq = 0;
  async function streamItemPages(server: any, totalHint: number) {
    const mySeq = ++itemStreamSeq;
    const PAGE = 100;
    let offset = 0;
    try {
      while (true) {
        // Abort if another openProject started, or the project was closed.
        if (mySeq !== itemStreamSeq) return;
        if (!currentProject.value) return;
        let page: { offset: number; limit: number; total: number; items: any[] };
        try { page = await server.fetchProjectItemsPage(offset, PAGE); }
        catch (e) { console.warn('[useProject] item page fetch failed:', e); return; }

        if (!Array.isArray(page.items) || page.items.length === 0) break;
        if (mySeq !== itemStreamSeq || !currentProject.value) return;

        // Push under the hydrating flag so the diff-watcher doesn't echo
        // these items right back to the server as "client adds".
        isHydrating.value = true;
        try {
          // Prevent Vue from deeply tracking waveform peaks arrays — they are
          // read-only server data (thousands of floats per item) and making
          // them reactive freezes the UI when the deep watcher is installed.
          // Pages from the server are waveform-less; re-attach any waveform we
          // already cached this session so a project_changed restream (another
          // client reloaded) doesn't blank waveforms we already have.
          for (const it of page.items) {
            if (it?.waveform) it.waveform = markRaw(it.waveform);
            restoreWaveform(it);
          }
          currentProject.value.items.push(...page.items);
          updateIndices(currentProject.value.items);
        } finally {
          // Defer clearing the flag until after Vue flushes its watcher
          // queue — otherwise the items/cartOnly deep watchers run with
          // isHydrating=false and echo the just-pushed page back as a
          // diff (PUT /api/project/document storm).
          await nextTick();
          isHydrating.value = false;
        }

        offset += page.items.length;
        if (offset >= page.total) break;

        // Yield so Vue paints the freshly-pushed rows before we fetch
        // the next page.
        await new Promise<void>(r => {
          if (typeof requestAnimationFrame === 'function') {
            requestAnimationFrame(() => r());
          } else {
            setTimeout(r, 0);
          }
        });
      }
      // After streaming completes, refresh the items-diff baseline so the
      // next user edit doesn't get diffed against the empty array we
      // started with (which would push the entire playlist back to the
      // server as "adds"). Then install the items deep-watcher — this is
      // deferred until *now* on purpose. Installing it before pages were
      // streamed would force Vue to walk the growing items array on every
      // push (O(n²)) and is what made large projects feel like they
      // hung for several seconds after opening.
      refreshItemsBaselineAfterHydrate();
      installItemsWatcherFn();
      void totalHint; // currently informational only
    } catch (e) {
      console.warn('[useProject] streamItemPages crashed:', e);
    }
  }

  // Apply a header-only response to currentProject. Items array is left
  // empty for streamItemPages() to populate.
  const applyServerHeader = (header: any) => {
    if (!header || typeof header !== 'object') return;
    if (header.server?.projectFilePath) {
      projectFilePathRef.value = header.server.projectFilePath;
    }
    const project: Project = {
      name:           header.name ?? 'Untitled',
      version:        header.version ?? '2.0.0',
      folderPath:     header.folderPath ?? '',
      items:          [], // populated by streamItemPages
      cartItems:      header.cartItems ?? [],
      cartSlotKeys:   header.cartSlotKeys ?? { ...DEFAULT_CART_SLOT_KEYS },
      playbackKeys:   header.playbackKeys,
      cartOnlyItems:  header.cartOnlyItems ?? [],
      theme:          header.theme ?? { ...DEFAULT_THEME },
      createdAt:      header.createdAt ?? new Date().toISOString(),
      lastModified:   header.lastModified ?? new Date().toISOString(),
    };
    if (header.settings) (project as any).settings = header.settings;
    currentProject.value = project;
    updateIndices(project.items);
    // A freshly loaded/created project matches its on-disk file.
    hasUnsavedChanges.value = false;
  };

  // Refer to the module-scoped install hooks so streamItemPages and
  // closeProject below can still call them via familiar names. The actual
  // assignment happens once inside the install-once block further down.
  const refreshItemsBaselineAfterHydrate = () => _refreshItemsBaselineAfterHydrate();
  const installItemsWatcherFn   = () => _installItemsWatcherFn?.();
  const uninstallItemsWatcherFn = () => _uninstallItemsWatcherFn?.();

  // Background poll of audio-loading progress. Updates `audioLoadingProgress`
  // until the server reports loading=false, then clears it. Safe to call
  // multiple times — only one poll runs at a time. Bails out fast when the
  // first poll already shows loading=false (e.g. small projects that finish
  // mirroring before the client even asks), avoiding a second HTTP call.
  let progressPolling = false;
  async function pollLoadingProgress(server: any) {
    if (progressPolling) return;
    progressPolling = true;
    try {
      let firstTick = true;
      while (true) {
        let p: { loading: boolean; loaded: number; total: number };
        try { p = await server.fetchProjectProgress(); }
        catch { break; }
        // Skip surfacing tiny loads — if the server's already done on the
        // first tick, there's nothing to show.
        if (firstTick && !p.loading) break;
        firstTick = false;
        audioLoadingProgress.value = p;
        if (!p.loading) break;
        await new Promise(r => setTimeout(r, 400));
      }
    } finally {
      audioLoadingProgress.value = { loading: false, loaded: 0, total: 0 };
      progressPolling = false;
    }
  }


  // Save the current project — the server already has the document, it just
  // needs to write to disk.
  const saveProject = async (opts?: { force?: boolean }): Promise<boolean> => {
    try {
      if (!currentProject.value) return false;

      // Autosave gating: when the user has turned autosave off, an ordinary
      // edit-triggered save doesn't touch the disk file — we only flag that
      // there are unsaved changes. Explicit saves (File > Save, or toggling
      // autosave) pass { force: true } to bypass this and always persist.
      if (!opts?.force && !autoSaveEnabled.value) {
        hasUnsavedChanges.value = true;
        return true;
      }

      // Mirror cart-only items from the client-side memory store back into
      // the project doc before pushing.
      const { cartOnlyItems } = useCartItems();
      currentProject.value.cartOnlyItems = Array.from(cartOnlyItems.value.values());
      currentProject.value.lastModified = new Date().toISOString();

      const server = useLiveplayServer();
      // Build the authoritative document snapshot the server will persist.
      // The granular per-property watchers SHOULD have kept the server's
      // in-memory copy in sync — but we pass the document explicitly so a
      // missed PATCH (race, debounce, hidden watcher gap) can never leave
      // the file (or the engine) with stale property values.
      const docSnapshot = {
        name:          currentProject.value.name,
        version:       currentProject.value.version,
        folderPath:    currentProject.value.folderPath,
        items:         itemsToJSON(currentProject.value.items) ?? [],
        cartItems:     toJSON(currentProject.value.cartItems) ?? [],
        cartSlotKeys:  toJSON((currentProject.value as any).cartSlotKeys),
        playbackKeys:  toJSON((currentProject.value as any).playbackKeys),
        cartOnlyItems: itemsToJSON(currentProject.value.cartOnlyItems) ?? [],
        theme:         toJSON(currentProject.value.theme),
        settings:      toJSON((currentProject.value as any).settings),
        createdAt:     currentProject.value.createdAt,
        lastModified:  currentProject.value.lastModified,
      };
      const path = projectFilePathRef.value ||
                   `${currentProject.value.folderPath}/${currentProject.value.name}.liveplay`;
      const res = await server.saveProjectTo(path, docSnapshot);
      if (res?.ok) hasUnsavedChanges.value = false;
      return !!res?.ok;
    } catch (error) {
      console.error('Error saving project:', error);
      return false;
    }
  };

  // Toggle autosave on/off. The preference lives in project settings (so it
  // persists across reopens) and is force-saved to disk immediately: turning
  // it OFF would otherwise never write `autoSave: false` to the file, and
  // turning it ON should flush whatever edits accumulated while it was off.
  const setAutoSave = (enabled: boolean) => {
    if (!currentProject.value) return;
    const settings = ((currentProject.value as any).settings ?? {});
    (currentProject.value as any).settings = { ...settings, autoSave: enabled };
    void saveProject({ force: true });
  };

  // Close the current project — clears local state AND tells the server to
  // unload its in-memory document so we land back on the welcome screen
  // (where the user can pick New or Open).
  const closeProject = async () => {
    // Tear down the items deep-watcher before nulling the project so the
    // null assignment doesn't trigger one last (now meaningless) sync.
    // The watcher is re-installed by streamItemPages when the next
    // project is opened.
    uninstallItemsWatcherFn();

    currentProject.value = null;
    selectedItem.value = null;
    selectedItems.value.clear();
    selectionAnchorUuid = null;
    propertiesPanelOpen.value = false;
    activeCues.value.clear();
    projectFilePathRef.value = '';
    _autoProcessPendingUuids.clear();
    _waveformCache.clear();

    // Clear cart-only items from memory
    const { clearCartOnlyItems } = useCartItems();
    clearCartOnlyItems();

    // Let the Electron main process know the project closed so it can
    // re-grey-out the "Export Project" menu item etc. The watch() in
    // MainWorkspace only fires on project *changes* (and skips null), so
    // without this nudge the menu would stay "open" forever once a
    // project had been opened.
    try {
      if (import.meta.client && (window as any).electronAPI?.syncProjectData) {
        (window as any).electronAPI.syncProjectData(null);
      }
    } catch { /* noop */ }

    // Ask the server to drop its in-memory project too. If we don't do this,
    // tryRejoinExistingProject() on the welcome screen would immediately
    // pull the user right back into the project we just dismissed.
    try {
      const server = useLiveplayServer();
      if (server.connected) {
        await server.closeProjectOnServer();
      }
    } catch (e) {
      console.warn('[useProject] closeProjectOnServer failed:', e);
    }
  };

  // Update item indices recursively
  const updateIndices = (items: (AudioItem | GroupItem)[], parentIndex: number[] = []): void => {
    items.forEach((item, i) => {
      item.index = [...parentIndex, i];
      if (item.type === 'group') {
        updateIndices(item.children, item.index);
      }
    });
  };

  // Add an item to the project (local mutation; sync watcher will push).
  const addItem = (item: AudioItem | GroupItem, parentIndex?: number[]) => {
    if (!currentProject.value) return;

    // If this is a brand-new audio item (no waveform yet), mark it so the
    // waveform_ready handler knows it should run auto-process once.
    if (item.type === 'audio' && !(item as AudioItem).waveform) {
      _autoProcessPendingUuids.add(item.uuid);
    }

    if (parentIndex && parentIndex.length > 0) {
      const parent = findItemByIndex(parentIndex);
      if (parent && parent.type === 'group') {
        parent.children.push(item);
        updateIndices(parent.children, parentIndex);
      }
    } else {
      currentProject.value.items.push(item);
      updateIndices(currentProject.value.items);
    }
  };

  // Allow external callers (e.g. CartSlot) to mark a UUID for one-shot
  // auto-processing when its waveform arrives.
  const markPendingAutoProcess = (uuid: string) => {
    _autoProcessPendingUuids.add(uuid);
  };

  // Check whether a UUID is pending auto-process and consume the mark
  // atomically. Returns true only when the item was marked AND removes it
  // so it can never be processed twice (handles both the waveform_ready
  // and generateWaveformAsync code paths).
  const consumePendingAutoProcess = (uuid: string): boolean => {
    if (!_autoProcessPendingUuids.has(uuid)) return false;
    _autoProcessPendingUuids.delete(uuid);
    return true;
  };

  // Remove an item
  const removeItem = (uuid: string) => {
    if (!currentProject.value) return;

    const removeFromArray = (items: (AudioItem | GroupItem)[]): boolean => {
      const index = items.findIndex(item => item.uuid === uuid);
      if (index !== -1) {
        items.splice(index, 1);
        updateIndices(items);
        return true;
      }

      for (const item of items) {
        if (item.type === 'group') {
          if (removeFromArray(item.children)) {
            updateIndices(item.children, item.index);
            return true;
          }
        }
      }
      return false;
    };

    removeFromArray(currentProject.value.items);
    if (selectedItem.value?.uuid === uuid) {
      selectedItem.value = null;
    }
  };

  // Find item by UUID
  const findItemByUuid = (uuid: string): AudioItem | GroupItem | null => {
    if (!currentProject.value) return null;

    const search = (items: (AudioItem | GroupItem)[]): AudioItem | GroupItem | null => {
      for (const item of items) {
        if (item.uuid === uuid) return item;
        if (item.type === 'group') {
          const found = search(item.children);
          if (found) return found;
        }
      }
      return null;
    };

    const found = search(currentProject.value.items);
    if (found) return found;

    const { getCartOnlyItem } = useCartItems();
    return getCartOnlyItem(uuid);
  };

  // Find item by index
  const findItemByIndex = (index: number[]): AudioItem | GroupItem | null => {
    if (!currentProject.value) return null;

    let items: (AudioItem | GroupItem)[] = currentProject.value.items;
    let currentItem: AudioItem | GroupItem | null = null;

    for (const idx of index) {
      if (idx >= items.length) return null;
      currentItem = items[idx];
      if (currentItem.type === 'group') {
        items = currentItem.children;
      }
    }

    return currentItem;
  };

  // Move item
  const moveItem = (fromIndex: number[], toIndex: number[]) => {
    if (!currentProject.value) return;

    const item = findItemByIndex(fromIndex);
    if (!item) return;

    removeItem(item.uuid);

    if (toIndex.length === 1) {
      currentProject.value.items.splice(toIndex[0], 0, item);
      updateIndices(currentProject.value.items);
    } else {
      const parentIndex = toIndex.slice(0, -1);
      const parent = findItemByIndex(parentIndex);
      if (parent && parent.type === 'group') {
        parent.children.splice(toIndex[toIndex.length - 1], 0, item);
        updateIndices(parent.children, parentIndex);
      }
    }
  };

  // ----------------------------------------------------------------------
  // Local→server sync. Targeted watchers per top-level section, so settings
  // / theme changes don't pay the cost of walking the entire items tree.
  // ----------------------------------------------------------------------
  const toJSON = (v: any): any => v == null ? null : JSON.parse(JSON.stringify(v));
  const stableJson = (v: any): string => JSON.stringify(v ?? null);

  // Serialise items while stripping the `waveform` field (large read-only
  // server blobs that must never be diff'd or pushed back).
  //
  // Uses toRaw() to bypass Vue's reactive proxy before traversing — this
  // avoids triggering Vue's track() getter on every property access, which
  // was the main cause of the multi-second UI freeze when serialising a large
  // project (JSON.stringify on a reactive object calls track() for every
  // property, adding ~1ms per item × 87 items = ~900ms of overhead alone).
  const _deepToRaw = (v: any): any => {
    const r = toRaw(v);
    if (r === null || typeof r !== 'object') return r;
    if (Array.isArray(r)) return r.map(_deepToRaw);
    const out: Record<string, any> = {};
    for (const k of Object.keys(r)) {
      if (k === 'waveform') continue;
      out[k] = _deepToRaw(r[k]);
    }
    return out;
  };
  const itemsToJSON = _deepToRaw;

  const isCartWindowMode = import.meta.client
    ? new URLSearchParams(window.location.search).get('cartWindow') === '1'
    : false;

  if (import.meta.client && !_syncWatchersInstalled && !isCartWindowMode) {
    _syncWatchersInstalled = true;
    // Per-section debounced sync timers.
    let itemsTimer:    ReturnType<typeof setTimeout> | null = null;
    let cartTimer:     ReturnType<typeof setTimeout> | null = null;
    let themeTimer:    ReturnType<typeof setTimeout> | null = null;
    let settingsTimer: ReturnType<typeof setTimeout> | null = null;
    // Diff baselines per section. Each is a plain (proxy-stripped) snapshot
    // of the section as it last left this client. Reset on hydrate.
    let lastItems:    any = null;
    let lastCart:     any = null;
    let lastCartOnly: any = null;
    let lastTheme:    any = null;
    let lastSettings: any = null;

    // After hydrate, capture per-section baselines so the per-section
    // watchers don't fire spuriously on the assignment.
    const captureBaselines = () => {
      const p = currentProject.value;
      // Strip waveform peaks from the items baseline — they are large
      // read-only server blobs that must never be included in the diff.
      lastItems    = itemsToJSON(p?.items);
      lastCart     = toJSON(p?.cartItems);
      lastCartOnly = itemsToJSON(p?.cartOnlyItems);
      lastTheme    = toJSON(p?.theme);
      lastSettings = toJSON((p as any)?.settings);
    };
    watch(isHydrating, (h) => { if (!h) captureBaselines(); });
    // Expose a refresh hook for streamItemPages — after it finishes
    // pushing all pages, the items baseline needs to reflect the
    // *streamed* items, otherwise the next user edit will be diffed
    // against the empty array we started with and re-push the whole
    // playlist as "client adds".
    _refreshItemsBaselineAfterHydrate = captureBaselines;

    const server = () => useLiveplayServer();

    // ---- Inbound doc_patch handler (multi-client mirror) ----
    // Helper that finds an item anywhere in the items tree.
    const findItemAndParent = (
      uuid: string,
    ): { item: any; parent: any[] | null } | null => {
      const p = currentProject.value;
      if (!p) return null;
      const search = (arr: any[], parent: any[] | null): any => {
        for (const it of arr) {
          if (it.uuid === uuid) return { item: it, parent: parent ?? arr };
          if (it.type === 'group' && Array.isArray(it.children)) {
            const r = search(it.children, it.children);
            if (r) return r;
          }
        }
        return null;
      };
      return search(p.items, p.items);
    };

    const applyDocPatch = (patch: any) => {
      const p = currentProject.value;
      if (!p) return;
      const op = patch?.op;
      // All patch application happens under isHydrating so the local
      // diff-watcher doesn't echo these back to the server as fresh edits.
      isHydrating.value = true;
      try {
        switch (op) {
          case 'item_added': {
            // Skip if we already have this uuid (originating client).
            const existing = findItemAndParent(patch.uuid);
            if (existing) return;
            const parentUuid = patch.parentUuid || '';
            if (patch.item?.waveform) patch.item.waveform = markRaw(patch.item.waveform);
            // The item may have arrived without peaks (server strips them, and
            // a reorder is a remove+re-add). Re-attach the cached waveform so a
            // moved/re-synced item doesn't render blank.
            restoreWaveform(patch.item);
            if (!parentUuid) {
              p.items.push(patch.item);
              updateIndices(p.items);
            } else {
              const parentHit = findItemAndParent(parentUuid);
              if (parentHit?.item?.type === 'group') {
                parentHit.item.children.push(patch.item);
                updateIndices(parentHit.item.children, parentHit.item.index);
              }
            }
            break;
          }
          case 'item_updated': {
            const hit = findItemAndParent(patch.uuid);
            if (!hit) return;
            // The waveform is client/worker-owned: its peaks are generated
            // locally and deliberately stripped from every sync payload, so
            // the server's echo of an item carries `waveform: null`. Applying
            // that here would wipe the locally-loaded waveform on EVERY
            // property edit (the diff-watcher PATCHes the item → the server
            // broadcasts item_updated right back). Never let a server patch
            // touch the local waveform — keep whatever we already have.
            const incoming = { ...(patch.patch ?? {}) };
            if ('waveform' in incoming) delete incoming.waveform;
            Object.assign(hit.item, incoming);
            // Defensive: if this item somehow has no peaks anymore, restore
            // them from the session cache.
            restoreWaveform(hit.item);
            break;
          }
          case 'item_removed': {
            server().invalidateWaveformCache(patch.uuid);
            const hit = findItemAndParent(patch.uuid);
            if (!hit || !hit.parent) return;
            const idx = hit.parent.findIndex((x: any) => x.uuid === patch.uuid);
            if (idx >= 0) hit.parent.splice(idx, 1);
            updateIndices(p.items);
            break;
          }
          case 'items_reordered': {
            const parentUuid = patch.parentUuid || '';
            const uuids: string[] = Array.isArray(patch.uuids) ? patch.uuids : [];
            const target = parentUuid
              ? findItemAndParent(parentUuid)?.item?.children
              : p.items;
            if (!Array.isArray(target)) return;
            const byUuid = new Map(target.map((x: any) => [x.uuid, x]));
            const reordered: any[] = [];
            for (const u of uuids) {
              const it = byUuid.get(u);
              if (it) reordered.push(it);
            }
            // Append anything we didn't see in the patch (defensive).
            for (const it of target) {
              if (!uuids.includes(it.uuid)) reordered.push(it);
            }
            target.splice(0, target.length, ...reordered);
            updateIndices(p.items);
            break;
          }
          case 'cart_slot_set': {
            const slot = patch.slot;
            const uuid = patch.itemUuid;
            if (typeof slot !== 'number' || !uuid) return;
            const existing = (p.cartItems ?? []).findIndex((c: any) => c.slot === slot);
            const next: CartItem = { slot, itemUuid: uuid, index: [-1, slot] };
            if (existing >= 0) p.cartItems[existing] = next;
            else p.cartItems.push(next);
            break;
          }
          case 'cart_slot_cleared': {
            const slot = patch.slot;
            if (typeof slot !== 'number') return;
            p.cartItems = (p.cartItems ?? []).filter((c: any) => c.slot !== slot);
            break;
          }
          case 'theme_patched': {
            if (patch.theme && typeof patch.theme === 'object') {
              p.theme = { ...p.theme, ...patch.theme };
            }
            break;
          }
          case 'settings_patched': {
            if (patch.settings && typeof patch.settings === 'object') {
              (p as any).settings = { ...(p as any).settings, ...patch.settings };
            }
            break;
          }
          case 'waveform_ready': {
            const target = findItemByUuid(patch.item_uuid);
            if (target && target.type === 'audio') {
              const peaks: number[] = patch.channels?.[0]?.peak ?? [];
              const duration: number = (patch.duration_ms ?? 0) / 1000;
              if (peaks.length > 0) {
                (target as any).waveform = markRaw({ peaks, length: peaks.length, duration });
                // Seed the session cache so this waveform survives later
                // server re-syncs that strip it.
                cacheWaveform(patch.item_uuid, (target as any).waveform);
                if (duration > 0) {
                  (target as any).duration = duration;
                  (target as any).outPoint  = duration;
                }
                // Auto-process (trim silence + normalise) only for items that
                // were just added in this session — never for items restored
                // from the saved project file (which already have user-set
                // trim points and volume).
                if (_autoProcessPendingUuids.has(patch.item_uuid)) {
                  _autoProcessPendingUuids.delete(patch.item_uuid);
                  const settings = (currentProject.value as any)?.settings;
                  if (!settings?.disableAutoVolumeAndTrim) {
                    const targetDb: number = settings?.outputTargetLevels?.autoVolumeTargetDb ?? -23;
                    applyAutoProcessing(target as AudioItem, targetDb);
                  }
                }
                triggerWaveformUpdate();
              }
            }
            break;
          }
          case 'waveform_failed':
            // Server couldn't decode the file — nothing to do on the client.
            break;
          case 'next_item_set':
            // Handled in useAudioEngine.onDocPatch — server-authoritative.
            break;
          case 'master_gain_changed':
          case 'custom_action_http':
            // Handled in useAudioEngine.onDocPatch.
            break;
          case 'preview_started': {
            previewItemUuid.value = patch.itemUuid || '';
            previewCueId.value = patch.cueId || '';
            break;
          }
          case 'preview_stopped': {
            previewItemUuid.value = '';
            previewCueId.value = '';
            break;
          }
          case 'project_changed': {
            // New project → all cached waveforms are invalid.
            server().invalidateWaveformCache();
            // A new project was loaded server-side by some other client.
            // Refetch header + restream items. isHydrating is reset to false
            // by the outer finally before the async fetch resolves, so we must
            // re-set it around applyServerHeader to prevent the fallback
            // watcher from echoing the new header back as a PUT (which would
            // create an infinite project_changed loop).
            void (async () => {
              try {
                const header = await server().fetchProjectHeader();
                if (!header) return;
                isHydrating.value = true;
                try {
                  applyServerHeader(header);
                } finally {
                  await nextTick();
                  isHydrating.value = false;
                }
                if (header.hasOpenProject) {
                  void streamItemPages(server(), header.itemCount ?? 0);
                }
              } catch (e) {
                console.warn('[useProject] project_changed refetch failed:', e);
              }
            })();
            break;
          }
        }
      } finally {
        // Defer the flag reset by a microtask so any reactive watchers
        // running this tick still see hydrating=true.
        queueMicrotask(() => { isHydrating.value = false; });
      }
    };

    server().onDocPatch(applyDocPatch);

    // Apply the server's connect/reconnect snapshot. Covers preview state;
    // transport + master gain + next-item are handled in useAudioEngine.
    server().onPlaybackSnapshot((snap: any) => {
      const prev = snap?.preview ?? {};
      previewItemUuid.value = prev.item_uuid || '';
      previewCueId.value = prev.cue_id || '';
    });

    // ---- Theme ----
    watch(() => currentProject.value?.theme, () => {
      if (isHydrating.value || !currentProject.value) return;
      if (themeTimer) clearTimeout(themeTimer);
      themeTimer = setTimeout(async () => {
        const next = toJSON(currentProject.value?.theme);
        if (stableJson(next) === stableJson(lastTheme)) return;
        lastTheme = next;
        try { await server().patchTheme(next ?? {}); }
        catch (e) { console.warn('[useProject] patchTheme failed:', e); }
      }, 250);
    }, { deep: true });

    // ---- Settings ----
    watch(() => (currentProject.value as any)?.settings, () => {
      if (isHydrating.value || !currentProject.value) return;
      if (settingsTimer) clearTimeout(settingsTimer);
      settingsTimer = setTimeout(async () => {
        const next = toJSON((currentProject.value as any)?.settings);
        if (stableJson(next) === stableJson(lastSettings)) return;
        lastSettings = next;
        try { await server().patchSettings(next ?? {}); }
        catch (e) { console.warn('[useProject] patchSettings failed:', e); }
      }, 250);
    }, { deep: true });

    // ---- Cart slot bindings ----
    watch(() => currentProject.value?.cartItems, () => {
      if (isHydrating.value || !currentProject.value) return;
      if (cartTimer) clearTimeout(cartTimer);
      cartTimer = setTimeout(async () => {
        const next = toJSON(currentProject.value?.cartItems) ?? [];
        if (stableJson(next) === stableJson(lastCart)) return;
        const prev = lastCart ?? [];
        lastCart = next;
        const cartByPrev = new Map<number, string>();
        const cartByCurr = new Map<number, string>();
        for (const c of prev)  if (typeof c?.slot === 'number') cartByPrev.set(c.slot, c.itemUuid ?? '');
        for (const c of next)  if (typeof c?.slot === 'number') cartByCurr.set(c.slot, c.itemUuid ?? '');
        for (const [slot] of cartByPrev) {
          if (!cartByCurr.has(slot)) { try { await server().clearCartSlot(slot); } catch {} }
        }
        for (const [slot, uuid] of cartByCurr) {
          if (cartByPrev.get(slot) !== uuid) { try { await server().setCartSlot(slot, uuid); } catch {} }
        }
      }, 250);
    }, { deep: true });

    // ---- Items + cart-only items (structural / per-item property changes) ----
    // The watcher only fires when the items array or cart-only items array
    // actually changes. We diff against `lastItems`/`lastCartOnly` to push
    // a minimal set of add/update/remove calls.
    //
    // CRUCIAL PERF NOTE: `{ deep: true }` makes Vue traverse the entire
    // items tree to maintain reactivity tracking. During streamItemPages
    // we push pages of 100 items each — re-running deep-tracking on each
    // push is O(n²) on the cumulative item count and is what makes the
    // UI hang for a few seconds on large projects. We solve that by NOT
    // installing the watcher up front: `installItemsWatcher` is called
    // from `refreshItemsBaselineAfterHydrate` (the hook fired after
    // streamItemPages finishes), and torn down in closeProject.
    const scheduleItemsDiff = () => {
      if (isHydrating.value || !currentProject.value) return;
      if (itemsTimer) clearTimeout(itemsTimer);
      itemsTimer = setTimeout(() => syncItemsDiff().catch(e =>
        console.warn('[useProject] items diff failed:', e)
      ), 300);
    };

    let stopItemsWatcher:    null | (() => void) = null;
    let stopCartOnlyWatcher: null | (() => void) = null;
    const installItemsWatcher = () => {
      if (stopItemsWatcher && stopCartOnlyWatcher) return;
      stopItemsWatcher    = watch(() => currentProject.value?.items,
                                  scheduleItemsDiff, { deep: true });
      stopCartOnlyWatcher = watch(() => currentProject.value?.cartOnlyItems,
                                  scheduleItemsDiff, { deep: true });
    };
    const uninstallItemsWatcher = () => {
      if (stopItemsWatcher)    { try { stopItemsWatcher(); }    catch {} stopItemsWatcher    = null; }
      if (stopCartOnlyWatcher) { try { stopCartOnlyWatcher(); } catch {} stopCartOnlyWatcher = null; }
    };
    _installItemsWatcherFn   = installItemsWatcher;
    _uninstallItemsWatcherFn = uninstallItemsWatcher;

    const flatten = (items: any[] | null | undefined,
                     cartOnly: any[] | null | undefined) => {
      const m = new Map<string, { item: any; parentUuid: string }>();
      const walk = (arr: any[] | null | undefined, parentUuid: string) => {
        if (!Array.isArray(arr)) return;
        for (const it of arr) {
          if (!it || typeof it !== 'object') continue;
          const u = it.uuid;
          if (!u) continue;
          m.set(u, { item: it, parentUuid });
          if (it.type === 'group' && Array.isArray(it.children)) walk(it.children, u);
        }
      };
      walk(items, '');
      walk(cartOnly, '');
      return m;
    };

    async function syncItemsDiff() {
      const srv = server();
      if (!srv.connected) return;
      const p = currentProject.value;
      if (!p) return;
      const nextItems    = itemsToJSON(p.items)         ?? [];
      const nextCartOnly = itemsToJSON(p.cartOnlyItems) ?? [];

      const items_changed    = stableJson(nextItems)    !== stableJson(lastItems);
      const cartonly_changed = stableJson(nextCartOnly) !== stableJson(lastCartOnly);
      if (!items_changed && !cartonly_changed) return;

      const prev = flatten(lastItems, lastCartOnly);
      const curr = flatten(nextItems, nextCartOnly);

      // Rotate baselines BEFORE awaiting any API calls so a subsequent
      // watcher fire while we're mid-sync sees the new baseline.
      lastItems    = nextItems;
      lastCartOnly = nextCartOnly;

      // 1. Cross-parent moves: same uuid but parentUuid changed.
      //    Must remove-then-add so the server tracks the new parent.
      //    (updateProjectItem only patches content, not parent placement.)
      for (const [uuid, { item, parentUuid }] of curr) {
        const before = prev.get(uuid);
        if (!before || before.parentUuid === parentUuid) continue;
        try { await srv.removeProjectItem(uuid); } catch {}
        try { await srv.addProjectItem(item, parentUuid); } catch {}
      }

      // 2. Removes: items present in prev but gone from curr.
      for (const [uuid] of prev) {
        if (!curr.has(uuid)) { try { await srv.removeProjectItem(uuid); } catch {} }
      }

      // 3. Adds: items in curr that weren't in prev (and aren't cross-parent moves).
      for (const [uuid, { item, parentUuid }] of curr) {
        if (prev.has(uuid)) continue;
        try { await srv.addProjectItem(item, parentUuid); } catch {}
      }

      // 4. Updates: items whose content changed (not cross-parent, not new).
      for (const [uuid, { item, parentUuid }] of curr) {
        const before = prev.get(uuid);
        if (!before) continue;
        if (before.parentUuid !== parentUuid) continue; // handled in step 1
        if (stableJson(before.item) === stableJson(item)) continue;
        try { await srv.updateProjectItem(uuid, item); } catch {}
      }

      // 5. Reorder: for each parent level, if the item order changed call
      //    reorderProjectItems so other clients see the correct list order.
      //    The JavaScript Map preserves insertion order (= walk/array order),
      //    so [...map.entries()] gives items in their array position order.
      const parentUuidsInCurr = new Set<string>();
      for (const [, { parentUuid }] of curr) parentUuidsInCurr.add(parentUuid);

      for (const parentUuid of parentUuidsInCurr) {
        const prevOrder = [...prev.entries()]
          .filter(([, v]) => v.parentUuid === parentUuid)
          .map(([u]) => u);
        const currOrder = [...curr.entries()]
          .filter(([, v]) => v.parentUuid === parentUuid)
          .map(([u]) => u);

        // Restrict comparison to items that exist in both snapshots so that
        // add/remove operations (already handled above) don't falsely look
        // like a reorder.
        const prevCommon = prevOrder.filter(u => curr.has(u));
        const currCommon = currOrder.filter(u => prev.has(u));

        if (stableJson(prevCommon) !== stableJson(currCommon)) {
          // Send the full current order for this parent (server ignores unknown uuids).
          try { await srv.reorderProjectItems(currOrder, parentUuid); } catch {}
        }
      }
    }

    // ---- Fallback for keys without granular endpoints ----
    // Only fires when one of the specific "no-endpoint-yet" fields changes
    // (hotkey bindings, project name). Critically does NOT fire on items
    // / settings / theme — those have targeted watchers above.
    let fallbackTimer: ReturnType<typeof setTimeout> | null = null;
    watch([
      () => (currentProject.value as any)?.cartSlotKeys,
      () => (currentProject.value as any)?.playbackKeys,
      () => currentProject.value?.name,
    ], () => {
      if (isHydrating.value || !currentProject.value) return;
      if (fallbackTimer) clearTimeout(fallbackTimer);
      fallbackTimer = setTimeout(async () => {
        const p = currentProject.value;
        if (!p) return;
        // Only push the whole document for these rare fields. The cost is
        // acceptable here precisely because the trigger fires rarely.
        try { await server().replaceProjectDocument(toJSON(p)); }
        catch (e) { console.warn('[useProject] fallback PUT failed:', e); }
      }, 800);
    }, { deep: true });
  }

  const confirmRepair = () => { _repairPromiseResolve?.(true);  _repairPromiseResolve = null; };
  const cancelRepair  = () => { _repairPromiseResolve?.(false); _repairPromiseResolve = null; };

  // Guard a project-leaving action (New / Open / Close). Resolves true when
  // it's safe to proceed, false when the user cancels. When autosave is on or
  // nothing is pending, it's a no-op that proceeds immediately. Otherwise it
  // pops the unsaved-changes modal and acts on the choice: "save" force-saves
  // first, "discard" proceeds without saving, "cancel" aborts.
  const confirmUnsavedChanges = (): Promise<boolean> => {
    if (autoSaveEnabled.value || !hasUnsavedChanges.value) return Promise.resolve(true);
    unsavedDialogVisible.value = true;
    return new Promise<boolean>((resolve) => {
      _unsavedPromiseResolve = async (choice) => {
        unsavedDialogVisible.value = false;
        _unsavedPromiseResolve = null;
        if (choice === 'cancel') { resolve(false); return; }
        if (choice === 'save') { await saveProject({ force: true }); }
        resolve(true);
      };
    });
  };
  const unsavedSave    = () => _unsavedPromiseResolve?.('save');
  const unsavedDiscard = () => _unsavedPromiseResolve?.('discard');
  const unsavedCancel  = () => _unsavedPromiseResolve?.('cancel');

  return {
    currentProject,
    selectedItem,
    selectedItems,
    propertiesPanelOpen,
    activeCues,
    waveformUpdateKey,
    triggerWaveformUpdate,
    toggleItemSelection,
    openItemProperties,
    selectAllItems,
    duplicateItems,
    copyItemsToClipboard,
    pasteItemsFromClipboard,
    getSelectedItems,
    getAllItemsFlat,
    createNewProject,
    openProject,
    tryRejoinExistingProject,
    saveProject,
    hasUnsavedChanges,
    autoSaveEnabled,
    setAutoSave,
    closeProject,
    addItem,
    markPendingAutoProcess,
    consumePendingAutoProcess,
    removeItem,
    findItemByUuid,
    findItemByIndex,
    moveItem,
    updateIndices,
    projectFilePath: projectFilePathRef,
    isLoading,
    loadingMessage,
    audioLoadingProgress,
    previewItemUuid,
    previewCueId,
    startPreview,
    stopPreview,
    repairDialogVisible,
    repairDialogIssues,
    confirmRepair,
    cancelRepair,
    unsavedDialogVisible,
    confirmUnsavedChanges,
    unsavedSave,
    unsavedDiscard,
    unsavedCancel,
  };
};
