// =====================================================================
// useShowTabs.ts
// ---------------------------------------------------------------------
// Playlist tabs and cart pages (#74). What exists is the show's, and every
// client sees it. Which one a window is LOOKING at is that window's own:
// it lives here, never on the server, so operators browse independently
// while playback stays shared.
//
// A pop-out window is told what to show by its URL (?cartPage= / ?playlistTab=)
// so a reload brings it back to the same place.
// =====================================================================
import { v4 as uuidv4 } from 'uuid';
import type { CartPage, PlaylistTab, TabLimits } from '~/types/project';
import {
  DEFAULT_TAB_LIMITS,
  IMPLICIT_ID,
  detachItem,
  insertIndexForTab,
  isTabTarget,
  itemsOnTab,
  keepingGotoTargets,
  nextFreeSlotBase,
  pageOfSlot,
  pagesOf,
  rootTabId,
  sortItemsByTabs,
  tabIdOfTarget,
  tabTarget,
  tabsOf,
} from '~/utils/tabs';
import { isSecondaryWindow } from '~/composables/useProject';

export type TabKind = 'playlist' | 'cart';

export interface ClientView {
  id: number;
  label: string;
  playlistTab: string;
  cartPage: string;
}

const FOLLOW_KEY = 'liveplay.followOperator';
const SAVE_DELAY_MS = 250;

let _wired = false;
let _saveTimer: ReturnType<typeof setTimeout> | null = null;

const fromUrl = (key: string): string | null =>
  import.meta.client ? new URLSearchParams(window.location.search).get(key) : null;

const fromSession = (key: string): string | null => {
  if (!import.meta.client) return null;
  try { return sessionStorage.getItem(key); } catch { return null; }
};
const toSession = (key: string, value: string) => {
  if (!import.meta.client) return;
  try { sessionStorage.setItem(key, value); } catch { /* private window */ }
};

export const useShowTabs = () => {
  const { currentProject, saveProject, findItemByIndex, findItemByUuid, updateIndices, removeItem } = useProject();
  const server = useLiveplayServer();
  const { t } = useLocalization();

  // Read by key: useProject's doc-patch handler holds back an echo of our own
  // edit while one of these is waiting to be sent.
  const localEditPending = useState<boolean>('showTabs.localEditPending', () => false);

  const wantedPlaylistTab = useState<string | null>('showTabs.activePlaylistTab',
    () => fromUrl('playlistTab') ?? fromSession('liveplay.playlistTab'));
  const wantedCartPage = useState<string | null>('showTabs.activeCartPage',
    () => fromUrl('cartPage') ?? fromSession('liveplay.cartPage'));

  const limits = computed<TabLimits>(() => currentProject.value?.tabLimits ?? DEFAULT_TAB_LIMITS);
  const playlistTabs = computed(() => tabsOf(currentProject.value));
  const cartPages = computed(() => pagesOf(currentProject.value));

  // Falls back to the first when what was showing has been deleted.
  const activePlaylistTab = computed<PlaylistTab>(() =>
    playlistTabs.value.find(tab => tab.id === wantedPlaylistTab.value) ?? playlistTabs.value[0]!);
  const activeCartPage = computed<CartPage>(() =>
    cartPages.value.find(page => page.id === wantedCartPage.value) ?? cartPages.value[0]!);
  const activePlaylistTabId = computed(() => activePlaylistTab.value.id);
  const activeCartPageId = computed(() => activeCartPage.value.id);

  const itemsOnActiveTab = computed(() =>
    itemsOnTab(currentProject.value?.items ?? [], playlistTabs.value, activePlaylistTabId.value));

  // ---- Following another operator ---------------------------------------
  const clientViews = useState<ClientView[]>('showTabs.clientViews', () => []);
  const ownViewId = useState<number | null>('showTabs.ownViewId', () => null);
  const followLabel = useState<string>('showTabs.followLabel', () => {
    if (!import.meta.client) return '';
    try { return localStorage.getItem(FOLLOW_KEY) ?? ''; } catch { return ''; }
  });

  const followOptions = computed(() => clientViews.value.filter(v => v.id !== ownViewId.value));
  const followedView = computed(() =>
    followLabel.value ? followOptions.value.find(v => v.label === followLabel.value) ?? null : null);
  const followLost = computed(() => !!followLabel.value && !followedView.value);

  function setFollowing(label: string) {
    followLabel.value = label;
    if (!import.meta.client) return;
    try {
      if (label) localStorage.setItem(FOLLOW_KEY, label);
      else localStorage.removeItem(FOLLOW_KEY);
    } catch { /* private window */ }
  }

  // ---- Choosing what this window shows -----------------------------------
  function showPlaylistTab(id: string) {
    wantedPlaylistTab.value = id;
    toSession('liveplay.playlistTab', id);
  }
  function showCartPage(id: string) {
    wantedCartPage.value = id;
    toSession('liveplay.cartPage', id);
  }
  // Going somewhere by hand ends following: the operator has decided to be
  // somewhere else, and being pulled back would fight them.
  function selectTab(kind: TabKind, id: string) {
    if (followLabel.value) setFollowing('');
    if (kind === 'playlist') showPlaylistTab(id);
    else showCartPage(id);
  }

  if (import.meta.client && !_wired) {
    _wired = true;
    server.onDocPatch((p: any) => {
      if (p?.op === 'client_view_id' && typeof p.id === 'number') ownViewId.value = p.id;
      if (p?.op === 'client_views' && Array.isArray(p.views)) clientViews.value = p.views;
    });

    // Only a main window is an operator to follow: a pop-out shows one fixed
    // tab and would just be a duplicate entry in the list.
    if (!isSecondaryWindow) {
      watch(
        () => [server.connected, activePlaylistTabId.value, activeCartPageId.value, !!currentProject.value] as const,
        ([connected, playlistTab, cartPage, open]) => {
          if (connected && open) server.setClientView({ playlistTab, cartPage });
        },
        { immediate: true },
      );
      watch(followedView, (view) => {
        if (!view) return;
        if (playlistTabs.value.some(tab => tab.id === view.playlistTab)) showPlaylistTab(view.playlistTab);
        if (cartPages.value.some(page => page.id === view.cartPage)) showCartPage(view.cartPage);
      }, { deep: true });
    }
  }

  // ---- Saving ------------------------------------------------------------
  // Edits show at once and go to the server shortly after, so typing a name
  // isn't a request per keystroke.
  function scheduleSave() {
    localEditPending.value = true;
    if (_saveTimer) clearTimeout(_saveTimer);
    _saveTimer = setTimeout(() => { void flushSave(); }, SAVE_DELAY_MS);
  }

  async function flushSave() {
    if (_saveTimer) { clearTimeout(_saveTimer); _saveTimer = null; }
    const project = currentProject.value;
    if (!project) { localEditPending.value = false; return; }
    try {
      await server.saveProjectTabs({
        playlistTabs: JSON.parse(JSON.stringify(project.playlistTabs ?? [])),
        cartPages: JSON.parse(JSON.stringify(project.cartPages ?? [])),
      });
    } catch (e) {
      console.warn('[tabs] could not save:', e);
    } finally {
      localEditPending.value = false;
    }
    await saveProject();
  }

  // The first real entry keeps the implicit id, so nothing bound to it moves.
  function materialisedTabs(): PlaylistTab[] {
    const project = currentProject.value!;
    if (!project.playlistTabs?.length) {
      project.playlistTabs = [{ id: IMPLICIT_ID, name: t('tabs.defaultPlaylist', { n: 1 }), atEnd: 'wrap' }];
    }
    return project.playlistTabs;
  }
  function materialisedPages(): CartPage[] {
    const project = currentProject.value!;
    if (!project.cartPages?.length) {
      project.cartPages = [{
        id: IMPLICIT_ID,
        name: t('tabs.defaultPage', { n: 1 }),
        size: limits.value.defaultCartsPerPage,
        slotBase: 0,
      }];
    }
    return project.cartPages;
  }

  const canAdd = (kind: TabKind) => (kind === 'playlist'
    ? playlistTabs.value.length < limits.value.maxPlaylistTabs
    : cartPages.value.length < limits.value.maxCartPages);

  function addTab(kind: TabKind): string | null {
    if (!currentProject.value || !canAdd(kind)) return null;
    const id = uuidv4();
    if (kind === 'playlist') {
      const list = materialisedTabs();
      list.push({ id, name: t('tabs.defaultPlaylist', { n: list.length + 1 }), atEnd: 'wrap' });
      showPlaylistTab(id);
    } else {
      const list = materialisedPages();
      const slotBase = nextFreeSlotBase(list, limits.value);
      if (slotBase === null) return null;
      list.push({
        id,
        name: t('tabs.defaultPage', { n: list.length + 1 }),
        size: limits.value.defaultCartsPerPage,
        slotBase,
      });
      showCartPage(id);
    }
    scheduleSave();
    return id;
  }

  function updateTab(kind: TabKind, id: string, patch: Partial<PlaylistTab & CartPage>) {
    if (!currentProject.value) return;
    const list: (PlaylistTab | CartPage)[] = kind === 'playlist' ? materialisedTabs() : materialisedPages();
    const entry = list.find(x => x.id === id);
    if (!entry) return;
    if (kind === 'cart' && patch.size !== undefined) {
      patch = { ...patch, size: Math.max(minCartsFor(id), Math.min(limits.value.maxCartsPerPage, patch.size)) };
    }
    Object.assign(entry, patch);
    if (patch.color === undefined && 'color' in patch) delete entry.color;
    scheduleSave();
  }

  // ---- What's on a tab ---------------------------------------------------
  const cartsOnPage = (page: CartPage) =>
    (currentProject.value?.cartItems ?? []).filter(
      c => c.slot >= page.slotBase && c.slot < page.slotBase + limits.value.cartSlotsPerPage);

  /** A page can't shrink past its last occupied cart. */
  function minCartsFor(pageId: string): number {
    const page = cartPages.value.find(p => p.id === pageId);
    if (!page) return limits.value.minCartsPerPage;
    const highest = Math.max(-1, ...cartsOnPage(page).map(c => c.slot - page.slotBase));
    return Math.max(limits.value.minCartsPerPage, highest + 1);
  }

  const { activeCues, nextItemOverrideUuid, autoNextItemUuid } = useAudioEngine();

  const uuidsUnder = (item: any): string[] =>
    item.type === 'group' ? item.children.flatMap(uuidsUnder) : [item.uuid];

  function tabUsage(kind: TabKind, id: string): { count: number; playing: boolean } {
    const onAir = activeCues.value;
    if (kind === 'playlist') {
      const items = itemsOnTab(currentProject.value?.items ?? [], playlistTabs.value, id);
      return { count: items.length, playing: items.flatMap(uuidsUnder).some(u => onAir.has(u)) };
    }
    const page = cartPages.value.find(p => p.id === id);
    if (!page) return { count: 0, playing: false };
    const carts = cartsOnPage(page);
    const playing = carts.some((c) => {
      if (isTabTarget(c.itemUuid)) {
        const items = itemsOnTab(currentProject.value?.items ?? [], playlistTabs.value, tabIdOfTarget(c.itemUuid));
        return items.flatMap(uuidsUnder).some(u => onAir.has(u));
      }
      const item = findItemByUuid(c.itemUuid);
      return item ? uuidsUnder(item).some(u => onAir.has(u)) : false;
    });
    return { count: carts.length, playing };
  }

  // What each tab shows on its label. Keyed on the playing cues rather than on
  // their playheads, which change twenty times a second.
  const onAirKey = computed(() => [...activeCues.value.keys()].join('|'));
  const nextUuid = computed(() => nextItemOverrideUuid.value ?? autoNextItemUuid.value ?? null);

  const playlistIndicators = computed(() => {
    void onAirKey.value;
    const out = new Map<string, { playing: boolean; upNext: string | null }>();
    const tabs = playlistTabs.value;
    for (const tab of tabs) out.set(tab.id, { playing: false, upNext: null });
    for (const root of currentProject.value?.items ?? []) {
      const entry = out.get(rootTabId(root, tabs));
      if (!entry) continue;
      const under = uuidsUnder(root);
      if (under.some(u => activeCues.value.has(u))) entry.playing = true;
      if (nextUuid.value && (root.uuid === nextUuid.value || under.includes(nextUuid.value))) {
        entry.upNext = findItemByUuid(nextUuid.value)?.displayName ?? '';
      }
    }
    return out;
  });
  const cartIndicators = computed(() => {
    void onAirKey.value;
    const out = new Map<string, { playing: boolean; upNext: string | null }>();
    for (const page of cartPages.value) {
      out.set(page.id, { playing: tabUsage('cart', page.id).playing, upNext: null });
    }
    return out;
  });
  const indicatorFor = (kind: TabKind, id: string) =>
    (kind === 'playlist' ? playlistIndicators.value : cartIndicators.value).get(id)
      ?? { playing: false, upNext: null };

  // ---- Deleting ----------------------------------------------------------
  function deleteTab(kind: TabKind, id: string): boolean {
    const project = currentProject.value;
    if (!project) return false;
    // Pulling the floor out from under a running cue is never what was meant.
    if (tabUsage(kind, id).playing) return false;
    if (kind === 'playlist') {
      const list = materialisedTabs();
      if (list.length <= 1) return false;
      for (const item of itemsOnTab(project.items, list, id)) removeItem(item.uuid);
      project.playlistTabs = list.filter(tab => tab.id !== id);
      project.cartItems = project.cartItems.filter(c => c.itemUuid !== tabTarget(id));
    } else {
      const list = materialisedPages();
      const page = list.find(p => p.id === id);
      if (!page || list.length <= 1) return false;
      const { removeCartOnlyItem } = useCartItems();
      const gone = cartsOnPage(page);
      for (const c of gone) removeCartOnlyItem(c.itemUuid);
      const goneSlots = new Set(gone.map(c => c.slot));
      project.cartItems = project.cartItems.filter(c => !goneSlots.has(c.slot));
      project.cartPages = list.filter(p => p.id !== id);
    }
    scheduleSave();
    return true;
  }

  // ---- Reordering --------------------------------------------------------
  function reorderTab(kind: TabKind, id: string, toIndex: number) {
    const project = currentProject.value;
    if (!project) return;
    const list: (PlaylistTab | CartPage)[] = kind === 'playlist' ? materialisedTabs() : materialisedPages();
    const from = list.findIndex(x => x.id === id);
    if (from === -1 || from === toIndex) return;
    const [moved] = list.splice(from, 1);
    list.splice(Math.max(0, Math.min(list.length, toIndex)), 0, moved!);
    if (kind === 'playlist') {
      // `items` stays in tab order, so reordering tabs moves the cues.
      keepingGotoTargets(project.items, findItemByIndex, () => {
        const sorted = sortItemsByTabs(project.items, project.playlistTabs!);
        project.items.splice(0, project.items.length, ...sorted);
        updateIndices(project.items);
      }, findItemByUuid);
    }
    scheduleSave();
  }

  // ---- Moving things between tabs ----------------------------------------
  function moveItemsToTab(uuids: string[], tabId: string) {
    const project = currentProject.value;
    if (!project || !uuids.length) return;
    materialisedTabs();
    const tabs = project.playlistTabs!;
    if (!tabs.some(tab => tab.id === tabId)) return;
    keepingGotoTargets(project.items, findItemByIndex, () => {
      for (const uuid of uuids) {
        const item = detachItem(project.items, uuid);
        if (!item) continue;
        item.tabId = tabId;
        project.items.splice(insertIndexForTab(project.items, tabs, tabId), 0, item);
      }
      updateIndices(project.items);
    }, findItemByUuid);
    scheduleSave();
  }

  /** Moves a cart to the first free place on a page. False if it's full. */
  function moveCartToPage(slot: number, pageId: string): boolean {
    const project = currentProject.value;
    if (!project) return false;
    const page = cartPages.value.find(p => p.id === pageId);
    const cart = project.cartItems.find(c => c.slot === slot);
    if (!page || !cart || pageOfSlot(cartPages.value, slot)?.id === pageId) return false;
    const taken = new Set(project.cartItems.map(c => c.slot));
    for (let position = 0; position < page.size; position++) {
      const target = page.slotBase + position;
      if (taken.has(target)) continue;
      cart.slot = target;
      cart.index = [-1, target];
      if (isSecondaryWindow) {
        // A pop-out has no sync watcher: tell the server directly.
        void server.clearCartSlot(slot).then(() => server.setCartSlot(target, cart.itemUuid))
          .catch((e: unknown) => console.warn('[tabs] could not move the cart:', e));
      } else {
        void saveProject();
      }
      return true;
    }
    return false;
  }

  // ---- Pop-out windows ---------------------------------------------------
  const canPopOut = computed(() =>
    import.meta.client && !isSecondaryWindow && !!(window as any).electronAPI?.openTabWindow);
  const openTabWindow = (kind: TabKind, id: string) => {
    void (window as any).electronAPI?.openTabWindow?.(kind, id);
  };

  return {
    limits,
    playlistTabs,
    cartPages,
    activePlaylistTab,
    activeCartPage,
    activePlaylistTabId,
    activeCartPageId,
    itemsOnActiveTab,
    selectTab,
    canAdd,
    addTab,
    updateTab,
    deleteTab,
    reorderTab,
    tabUsage,
    indicatorFor,
    minCartsFor,
    moveItemsToTab,
    moveCartToPage,
    canPopOut,
    openTabWindow,
    followOptions,
    followLabel,
    followedView,
    followLost,
    setFollowing,
  };
};
