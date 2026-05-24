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
import { triggerRef } from 'vue';
import type {
  Project,
  AudioItem,
  GroupItem,
  BaseItem,
  Theme,
  CartItem
} from '~/types/project';
import { DEFAULT_THEME, DEFAULT_CART_SLOT_KEYS } from '~/types/project';

export const useProject = () => {
  const currentProject = useState<Project | null>('currentProject', () => null);
  const selectedItem = useState<BaseItem | null>('selectedItem', () => null);
  const selectedItems = useState<Set<string>>('selectedItems', () => new Set()); // Track multiple selections by UUID
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
      const lastSelectedUuid = Array.from(selectedItems.value).pop();
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

    // Do not auto-open the properties panel on selection — use openItemProperties() explicitly
    if (selectedItems.value.size === 0) {
      selectedItem.value = null;
    }
  };

  // Explicitly open the properties panel for an item (called by Edit buttons)
  const openItemProperties = (uuid: string) => {
    const item = findItemByUuid(uuid);
    if (item) {
      selectedItem.value = item;
    }
  };

  // Get all items as a flat array (for shift-select)
  const getAllItemsFlat = (items: (AudioItem | GroupItem)[]): BaseItem[] => {
    const result: BaseItem[] = [];
    for (const item of items) {
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

      // Server-side: replace document with this fresh shell.
      isHydrating.value = true;
      try {
        const doc = await server.replaceProjectDocument(newProject as any);
        applyServerDocument(doc);
      } finally {
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

  // Open an existing project (path is on the server's filesystem).
  // UX: we show the central spinner only while waiting on the HTTP round-trip.
  // The moment the document arrives, the spinner is hidden so Vue can paint
  // the playlist. Background audio loading is reflected in the corner
  // AudioLoadProgress banner — *not* the full-screen modal.
  const openProject = async (projectFilePath: string): Promise<boolean> => {
    isLoading.value = true;
    loadingMessage.value = 'Loading project…';
    try {
      const server = useLiveplayServer();
      if (!server.connected) {
        try { server.connect(); } catch { /* noop */ }
      }

      const doc = await server.loadProjectFromPath(projectFilePath);

      // 1) Hide the central spinner IMMEDIATELY — we already have the
      //    document. Vue will start rendering the (potentially large)
      //    playlist next; the corner banner takes over for the audio-
      //    mirror phase that still runs on the server.
      isLoading.value = false;

      // 2) Hand the document to Vue. Under the hydrating flag so the
      //    diff-watcher doesn't echo the assignment back to the server.
      isHydrating.value = true;
      try {
        applyServerDocument(doc);
      } finally {
        isHydrating.value = false;
      }
      projectFilePathRef.value = projectFilePath;

      // Restore cart-only items to client-side memory store (used by CartSlot).
      const { clearCartOnlyItems, addCartOnlyItem } = useCartItems();
      clearCartOnlyItems();
      const cartOnly = currentProject.value?.cartOnlyItems ?? [];
      for (const item of cartOnly) addCartOnlyItem(item);

      // 3) Surface the audio-loading progress as a corner indicator.
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

  // Migrate raw server doc into the client's reactive ref. Mutating the local
  // `currentProject` shouldn't trigger a sync back to the server because
  // we're already mirroring its state.
  const applyServerDocument = (doc: any) => {
    if (!doc || typeof doc !== 'object') return;
    // Pluck out server-only decorations so they don't pollute the client model.
    const server = doc.server;
    if (server?.projectFilePath) {
      projectFilePathRef.value = server.projectFilePath;
    }
    // Drop unknown extras but keep the shape compatible with the client Project type.
    const project: Project = {
      name: doc.name ?? 'Untitled',
      version: doc.version ?? '2.0.0',
      folderPath: doc.folderPath ?? '',
      items: doc.items ?? [],
      cartItems: doc.cartItems ?? [],
      cartSlotKeys: doc.cartSlotKeys ?? { ...DEFAULT_CART_SLOT_KEYS },
      playbackKeys: doc.playbackKeys,
      cartOnlyItems: doc.cartOnlyItems ?? [],
      theme: doc.theme ?? { ...DEFAULT_THEME },
      createdAt: doc.createdAt ?? new Date().toISOString(),
      lastModified: doc.lastModified ?? new Date().toISOString(),
    };
    currentProject.value = project;
    updateIndices(project.items);
  };

  // Save the current project — the server already has the document, it just
  // needs to write to disk.
  const saveProject = async (): Promise<boolean> => {
    try {
      if (!currentProject.value) return false;

      // Mirror cart-only items from the client-side memory store back into
      // the project doc before pushing.
      const { cartOnlyItems } = useCartItems();
      currentProject.value.cartOnlyItems = Array.from(cartOnlyItems.value.values());
      currentProject.value.lastModified = new Date().toISOString();

      const server = useLiveplayServer();
      // Push the current document to the server (in case local edits haven't
      // synced yet) then ask it to write to disk.
      await server.replaceProjectDocument(toJSON(currentProject.value));
      const path = projectFilePathRef.value ||
                   `${currentProject.value.folderPath}/${currentProject.value.name}.liveplay`;
      const res = await server.saveProjectTo(path);
      return !!res?.ok;
    } catch (error) {
      console.error('Error saving project:', error);
      return false;
    }
  };

  // Close the current project — clears local state; does NOT delete server-side.
  const closeProject = () => {
    currentProject.value = null;
    selectedItem.value = null;
    activeCues.value.clear();
    projectFilePathRef.value = '';

    // Clear cart-only items from memory
    const { clearCartOnlyItems } = useCartItems();
    clearCartOnlyItems();
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

  if (import.meta.client) {
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
    watch(isHydrating, (h) => {
      if (h) return;
      const p = currentProject.value;
      lastItems    = toJSON(p?.items);
      lastCart     = toJSON(p?.cartItems);
      lastCartOnly = toJSON(p?.cartOnlyItems);
      lastTheme    = toJSON(p?.theme);
      lastSettings = toJSON((p as any)?.settings);
    });

    const server = () => useLiveplayServer();

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
    const scheduleItemsDiff = () => {
      if (isHydrating.value || !currentProject.value) return;
      if (itemsTimer) clearTimeout(itemsTimer);
      itemsTimer = setTimeout(() => syncItemsDiff().catch(e =>
        console.warn('[useProject] items diff failed:', e)
      ), 300);
    };
    watch(() => currentProject.value?.items,         scheduleItemsDiff, { deep: true });
    watch(() => currentProject.value?.cartOnlyItems, scheduleItemsDiff, { deep: true });

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
      const nextItems    = toJSON(p.items)         ?? [];
      const nextCartOnly = toJSON(p.cartOnlyItems) ?? [];

      const items_changed    = stableJson(nextItems)    !== stableJson(lastItems);
      const cartonly_changed = stableJson(nextCartOnly) !== stableJson(lastCartOnly);
      if (!items_changed && !cartonly_changed) return;

      const prev = flatten(lastItems, lastCartOnly);
      const curr = flatten(nextItems, nextCartOnly);

      // Rotate baselines BEFORE awaiting any API calls so a subsequent
      // watcher fire while we're mid-sync sees the new baseline.
      lastItems    = nextItems;
      lastCartOnly = nextCartOnly;

      for (const [uuid] of prev) {
        if (!curr.has(uuid)) { try { await srv.removeProjectItem(uuid); } catch {} }
      }
      for (const [uuid, { item, parentUuid }] of curr) {
        if (!prev.has(uuid)) { try { await srv.addProjectItem(item, parentUuid); } catch {} }
      }
      for (const [uuid, { item }] of curr) {
        const before = prev.get(uuid);
        if (!before) continue;
        if (stableJson(before.item) === stableJson(item)) continue;
        try { await srv.updateProjectItem(uuid, item); } catch {}
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

  return {
    currentProject,
    selectedItem,
    selectedItems,
    activeCues,
    waveformUpdateKey,
    triggerWaveformUpdate,
    toggleItemSelection,
    openItemProperties,
    getSelectedItems,
    getAllItemsFlat,
    createNewProject,
    openProject,
    saveProject,
    closeProject,
    addItem,
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
  };
};
