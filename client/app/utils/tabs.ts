// =====================================================================
// utils/tabs.ts
// ---------------------------------------------------------------------
// Pure helpers for playlist tabs and cart pages (#74). The document's
// shape is described in server/include/liveplay/core/show_tabs.hpp:
//
//   * `items` stays one flat list in tab order and each top-level item
//     names its tab, so a cue's number and every index stay meaningful.
//     Cue numbers run on across tabs for the same reason.
//   * Each cart page owns a block of the existing slot numbers.
//
// A show with neither has one implicit tab and one implicit page, whose
// id is IMPLICIT_ID. The first real tab keeps that id, so a cart already
// bound to "the playlist" stays bound when a second tab is added.
// =====================================================================
import type {
  AudioItem,
  CartPage,
  GroupItem,
  PlaylistTab,
  Project,
  TabLimits,
} from '~/types/project';

type Item = AudioItem | GroupItem;

export const IMPLICIT_ID = 'default';
export const TAB_TARGET_PREFIX = 'tab:';

// Only what a client shows until the server has told it the real ones.
export const DEFAULT_TAB_LIMITS: TabLimits = {
  maxPlaylistTabs: 100,
  maxCartPages: 100,
  minCartsPerPage: 4,
  maxCartsPerPage: 64,
  defaultCartsPerPage: 16,
  cartSlotsPerPage: 64,
};

export const isTabTarget = (uuid: string | null | undefined): boolean =>
  typeof uuid === 'string' && uuid.startsWith(TAB_TARGET_PREFIX);
export const tabTarget = (tabId: string) => `${TAB_TARGET_PREFIX}${tabId}`;
export const tabIdOfTarget = (uuid: string) => uuid.slice(TAB_TARGET_PREFIX.length);

export const tabsOf = (project: Partial<Project> | null | undefined): PlaylistTab[] =>
  project?.playlistTabs?.length
    ? project.playlistTabs
    : [{ id: IMPLICIT_ID, name: '', atEnd: 'wrap' }];

export const pagesOf = (project: Partial<Project> | null | undefined): CartPage[] =>
  project?.cartPages?.length
    ? project.cartPages
    : [{ id: IMPLICIT_ID, name: '', size: DEFAULT_TAB_LIMITS.defaultCartsPerPage, slotBase: 0 }];

/** The tab a top-level item is on. No tabId, or a deleted one, is the first tab. */
export function rootTabId(item: Pick<Item, 'tabId'>, tabs: PlaylistTab[]): string {
  const first = tabs[0]?.id ?? IMPLICIT_ID;
  if (!item.tabId) return first;
  return tabs.some(t => t.id === item.tabId) ? item.tabId : first;
}

export const itemsOnTab = (items: Item[], tabs: PlaylistTab[], tabId: string): Item[] =>
  tabs.length <= 1 ? items : items.filter(i => rootTabId(i, tabs) === tabId);

/** Where a new top-level item goes so its tab stays together, in tab order. */
export function insertIndexForTab(items: Item[], tabs: PlaylistTab[], tabId: string): number {
  if (tabs.length <= 1) return items.length;
  let last = -1;
  items.forEach((item, i) => { if (rootTabId(item, tabs) === tabId) last = i; });
  if (last >= 0) return last + 1;
  const position = tabs.findIndex(t => t.id === tabId);
  const later = new Set(tabs.slice(position + 1).map(t => t.id));
  const firstLater = items.findIndex(item => later.has(rootTabId(item, tabs)));
  return firstLater === -1 ? items.length : firstLater;
}

/** `items` regrouped into tab order, keeping the order within each tab. */
export function sortItemsByTabs(items: Item[], tabs: PlaylistTab[]): Item[] {
  if (tabs.length <= 1) return [...items];
  return tabs.flatMap(tab => items.filter(item => rootTabId(item, tabs) === tab.id));
}

/**
 * What follows a top-level item in running order. Mirrors the server's
 * next_in_tab, so the Up Next shown is the one GO will fire.
 */
export function nextRootItem(items: Item[], tabs: PlaylistTab[], uuid: string): Item | null {
  const at = items.findIndex(i => i.uuid === uuid);
  if (at === -1) return null;
  if (tabs.length <= 1) return items[at + 1] ?? null;
  const tabId = rootTabId(items[at]!, tabs);
  for (let j = at + 1; j < items.length; j++) {
    if (rootTabId(items[j]!, tabs) === tabId) return items[j]!;
  }
  const tabIndex = tabs.findIndex(t => t.id === tabId);
  if (tabs[tabIndex]?.atEnd !== 'continue') return null;
  for (let t = tabIndex + 1; t < tabs.length; t++) {
    const first = items.find(i => rootTabId(i, tabs) === tabs[t]!.id);
    if (first) return first;
  }
  return null;
}

/** The lowest block of slot numbers no page is using, or null if all are. */
export function nextFreeSlotBase(pages: CartPage[], limits: TabLimits): number | null {
  const used = new Set(pages.map(p => p.slotBase));
  for (let i = 0; i < limits.maxCartPages; i++) {
    const base = i * limits.cartSlotsPerPage;
    if (!used.has(base)) return base;
  }
  return null;
}

export const pageOfSlot = (pages: CartPage[], slot: number): CartPage | null =>
  pages.find(p => slot >= p.slotBase && slot < p.slotBase + p.size) ?? null;

/**
 * Runs a change that moves top-level items, keeping every "go to index" end
 * behaviour pointed at the same cue. Those store a position, so moving cues
 * between tabs would otherwise quietly retarget them.
 */
export function keepingGotoTargets(
  items: Item[],
  findByIndex: (index: number[]) => Item | null,
  change: () => void,
  findByUuid: (uuid: string) => Item | null,
): void {
  const targets: { item: Item; uuid: string }[] = [];
  const walk = (list: Item[]) => {
    for (const item of list) {
      const end = item.endBehavior;
      if (end?.action === 'goto-index' && end.targetIndex) {
        const target = findByIndex(end.targetIndex);
        if (target) targets.push({ item, uuid: target.uuid });
      }
      if (item.type === 'group') walk((item as GroupItem).children);
    }
  };
  walk(items);
  change();
  for (const { item, uuid } of targets) {
    const moved = findByUuid(uuid);
    if (moved && item.endBehavior) item.endBehavior.targetIndex = [...moved.index];
  }
}

/** Takes an item out of the tree, wherever it is, and returns it. */
export function detachItem(items: Item[], uuid: string): Item | null {
  const at = items.findIndex(i => i.uuid === uuid);
  if (at !== -1) return items.splice(at, 1)[0] ?? null;
  for (const item of items) {
    if (item.type !== 'group') continue;
    const found = detachItem((item as GroupItem).children, uuid);
    if (found) return found;
  }
  return null;
}

// One stable object per tab: a cart slot watches its item deeply, and a fresh
// object on every read would redraw it on every tick.
const tabGroupCache = new Map<string, GroupItem>();

/**
 * A playlist tab presented as a group, which is what a cart slot already knows
 * how to fire, time and stop (discussion #61). It isn't in the document: only
 * the cart's binding to "tab:<id>" is.
 */
export function tabGroupView(project: Project, tabId: string, fallbackName?: string): GroupItem | null {
  const tabs = tabsOf(project);
  const tab = tabs.find(t => t.id === tabId);
  if (!tab) return null;
  const uuid = tabTarget(tabId);
  let view = tabGroupCache.get(uuid);
  if (!view) {
    view = {
      uuid,
      type: 'group',
      index: [-1, -1],
      displayName: '',
      color: '',
      children: [],
      startBehavior: { action: 'play-first' },
      endBehavior: { action: 'nothing' },
      isExpanded: false,
    } as unknown as GroupItem;
    tabGroupCache.set(uuid, view);
  }
  view.displayName = tab.name || fallbackName || view.displayName;
  view.color = tab.color || '#607D8B';
  view.children = itemsOnTab(project.items, tabs, tabId);
  return view;
}
