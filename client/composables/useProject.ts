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
  const openProject = async (projectFilePath: string): Promise<boolean> => {
    isLoading.value = true;
    loadingMessage.value = 'Loading project…';
    try {
      const server = useLiveplayServer();
      if (!server.connected) {
        try { server.connect(); } catch { /* noop */ }
      }

      isHydrating.value = true;
      try {
        const doc = await server.loadProjectFromPath(projectFilePath);
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

      // The document landed in milliseconds — but audio cues finish loading
      // on the server *after* the response returns. Hide the modal screen
      // and instead surface a progress indicator on a corner of the UI by
      // polling /api/project/progress. The user can interact with non-audio
      // controls immediately.
      void pollLoadingProgress(server);
      return true;
    } catch (error) {
      console.error('Error opening project:', error);
      return false;
    } finally {
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
  // Local→server sync. Diff-based: only the sections that actually changed
  // get pushed, and they go through *targeted* endpoints (PATCH theme, PATCH
  // settings, individual item updates) rather than re-sending the whole
  // document. The previous bulk-PUT approach made every keystroke in the
  // settings modal re-send the entire project, which (a) saturated the
  // WebSocket on large projects and (b) caused the server to re-mirror
  // every cue into the engine on every change.
  // ----------------------------------------------------------------------
  const toJSON = (v: any): any => JSON.parse(JSON.stringify(v));
  const stableJson = (v: any): string => JSON.stringify(v ?? null);

  if (import.meta.client) {
    let syncTimer: ReturnType<typeof setTimeout> | null = null;
    // Snapshot of the last-synced state, used as the diff base.
    let lastSynced: any = null;

    // After a hydrate (open/create), set the diff base from the freshly
    // loaded document so the first watcher fire doesn't trigger spurious
    // pushes.
    watch(isHydrating, (h) => {
      if (h) return;
      lastSynced = currentProject.value ? toJSON(currentProject.value) : null;
    });

    watch(currentProject, (p) => {
      if (!p || isHydrating.value) return;
      if (syncTimer) clearTimeout(syncTimer);
      syncTimer = setTimeout(() => syncDiff(toJSON(p)).catch(err => {
        // eslint-disable-next-line no-console
        console.warn('[useProject] sync failed:', err);
      }), 250);
    }, { deep: true });

    async function syncDiff(next: any) {
      const server = useLiveplayServer();
      if (!server.connected) return;
      if (!lastSynced) {
        // No baseline yet — adopt this as the baseline silently.
        lastSynced = next;
        return;
      }

      // Top-level keys we treat as "small" and patch directly. Everything
      // else falls into the items/cart granular paths.
      const TARGETED_KEYS = ['theme', 'settings'] as const;
      for (const k of TARGETED_KEYS) {
        if (stableJson(next[k]) === stableJson(lastSynced[k])) continue;
        try {
          if (k === 'theme')    await server.patchTheme(next.theme ?? {});
          if (k === 'settings') await server.patchSettings(next.settings ?? {});
        } catch (e) {
          // eslint-disable-next-line no-console
          console.warn(`[useProject] patch ${k} failed:`, e);
        }
      }

      // Items diff: flatten both sides into uuid→item maps, then push the
      // minimum set of add / update / delete calls. Reorders fall out as a
      // single reorder-call per affected parent.
      const flatten = (root: any): Map<string, { item: any, parentUuid: string }> => {
        const m = new Map<string, any>();
        const walk = (arr: any[], parentUuid: string) => {
          if (!Array.isArray(arr)) return;
          for (const it of arr) {
            if (!it || typeof it !== 'object') continue;
            const u = it.uuid;
            if (!u) continue;
            m.set(u, { item: it, parentUuid });
            if (it.type === 'group' && Array.isArray(it.children)) {
              walk(it.children, u);
            }
          }
        };
        walk(root.items ?? [], '');
        // Cart-only items are flat — treat them as top-level for diff purposes.
        walk(root.cartOnlyItems ?? [], '');
        return m;
      };

      const prev = flatten(lastSynced);
      const curr = flatten(next);

      // Deletions
      for (const [uuid] of prev) {
        if (!curr.has(uuid)) {
          try { await server.removeProjectItem(uuid); } catch {}
        }
      }
      // Additions
      for (const [uuid, { item, parentUuid }] of curr) {
        if (!prev.has(uuid)) {
          try { await server.addProjectItem(item, parentUuid); } catch {}
        }
      }
      // Updates — push a patch for items whose JSON changed.
      for (const [uuid, { item }] of curr) {
        const before = prev.get(uuid);
        if (!before) continue;        // was new — already handled by Add
        if (stableJson(before.item) === stableJson(item)) continue;
        try { await server.updateProjectItem(uuid, item); } catch {}
      }

      // Cart-slot bindings — diff the top-level `cartItems` array.
      const cartByPrev = new Map<number, string>();
      const cartByCurr = new Map<number, string>();
      for (const c of (lastSynced.cartItems ?? [])) {
        if (typeof c?.slot === 'number') cartByPrev.set(c.slot, c.itemUuid ?? '');
      }
      for (const c of (next.cartItems ?? [])) {
        if (typeof c?.slot === 'number') cartByCurr.set(c.slot, c.itemUuid ?? '');
      }
      // Cleared slots
      for (const [slot] of cartByPrev) {
        if (!cartByCurr.has(slot)) { try { await server.clearCartSlot(slot); } catch {} }
      }
      // Added / changed bindings
      for (const [slot, uuid] of cartByCurr) {
        if (cartByPrev.get(slot) !== uuid) {
          try { await server.setCartSlot(slot, uuid); } catch {}
        }
      }

      // Fallback for top-level keys we don't have a granular endpoint for
      // yet (hotkey bindings, name, lastModified, ...). Only triggers if one
      // of them actually changed — common-path edits (settings/theme/items/
      // cart) never reach this branch.
      const HANDLED = new Set([
        'theme', 'settings', 'items', 'cartItems', 'cartOnlyItems',
        // ignored-by-sync because they're either server-decorated or
        // updated as a side effect of other changes
        'server', 'lastModified',
      ]);
      const fallback_keys: string[] = [];
      for (const k of Object.keys(next)) {
        if (HANDLED.has(k)) continue;
        if (stableJson(next[k]) !== stableJson(lastSynced[k])) {
          fallback_keys.push(k);
        }
      }
      // Also catch deletions of top-level keys.
      for (const k of Object.keys(lastSynced)) {
        if (HANDLED.has(k) || k in next) continue;
        fallback_keys.push(k);
      }
      if (fallback_keys.length > 0) {
        try {
          // For now use the full-document PUT for unhandled keys. This
          // remains heavy but only fires for rare edits like hotkey
          // bindings — not for every settings change.
          await server.replaceProjectDocument(next);
        } catch (e) {
          // eslint-disable-next-line no-console
          console.warn('[useProject] fallback PUT failed for', fallback_keys, e);
        }
      }

      lastSynced = next;
    }
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
  };
};
