// =====================================================================
// useWorkspaceLayout.ts
// ---------------------------------------------------------------------
// The workspace's three panes — Playlist, Cart Player, Mixer — and the ONE
// set of rules every one of them follows:
//
//   * SHOWN or not. The header's workspace bar toggles each one, and that
//     bar is the single, always-visible way back to a pane, so closing one
//     can never strand it. A pane closed from its own header, or dragged
//     shut by its splitter, is the same state as one switched off up there.
//
//   * EXPANDED. A pane can take the whole workspace; the others are switched
//     off for the duration and remembered, and Restore brings back exactly
//     that set. "Expanded" is not a separate flag: it is simply the state in
//     which this pane is the only one docked and a restore set exists. The
//     header's lit toggles therefore always say exactly what is on screen.
//
//   * DETACHED (Cart Player and Mixer, Electron only). The pane is shown, in
//     its own window rather than docked. Switching it off closes that window;
//     closing the window by any other route docks it back.
//
// Before 2.5.0 the cart and the mixer each grew their own version of these —
// the cart collapsed to a splitter handle, the mixer vanished outright, the
// playlist had neither, and header buttons appeared and disappeared to make
// up the difference. One model, one set of pane controls (PaneControls.vue),
// one bar in the header.
//
// OWNERSHIP: the MACHINE store. A layout is a statement about one screen —
// the panes that fit a console display are wrong on the laptop carried to
// rehearsal — so it neither follows an operator between desks nor travels in
// a show file. Same tier as useUiMode's Show Mode.
//
// `cartDetached` / `mixerDetached` are deliberately NOT stored: they describe
// windows, and Electron restores those itself (it knows whether they exist).
// A flag restored without its window would hide the docked pane with nothing
// in its place.
//
// NOTHING HERE IS CLAMPED. Widths are the raw numbers last dragged to, and a
// layout saved on a 4K screen will over-request on a laptop. MainWorkspace
// owns the clamping, because only it knows the row's width.
// =====================================================================

export type PaneId = 'playlist' | 'cart' | 'mixer';
export type PaneSet = Record<PaneId, boolean>;

/** Left to right, always. The first docked pane is the one that stretches. */
export const PANE_ORDER: readonly PaneId[] = ['playlist', 'cart', 'mixer'];

/** The splitters' defaults. One owner per constant — MainWorkspace reads these. */
export const CART_DEFAULT_PX       = 500;
export const MIXER_DEFAULT_PX      = 420;
export const PROPERTIES_DEFAULT_PX = 300;

const STORAGE_KEY = 'liveplay-workspace-layout';

// A drag emits a pointermove stream, and each one moves a width. The trailing
// edge is the only interesting write. Long enough to coalesce a whole drag,
// short enough that quitting straight after one keeps it.
const WRITE_DEBOUNCE_MS = 400;

const DEFAULT_PANES: PaneSet = { playlist: true, cart: true, mixer: false };

interface StoredLayout {
  panes?: Partial<PaneSet>;
  restore?: Partial<PaneSet> | null;
  cartWidth?: number;
  mixerWidth?: number;
  propertiesHeight?: number;
  // Pre-2.5.0-release shapes, read once so an upgrade keeps what was showing.
  cartClosed?: boolean;
  mixerOpen?: boolean;
  mixerCollapsed?: boolean;
}

// A stored width has to be a usable number before it is allowed near a
// stylesheet: `width: NaNpx` is a dead rule. This only rejects nonsense — the
// real clamping against THIS window happens in MainWorkspace.
const SANE_MAX_PX = 20000;
const isSanePx = (v: unknown): v is number =>
  typeof v === 'number' && Number.isFinite(v) && v > 0 && v < SANE_MAX_PX;

function readPaneSet(v: unknown): PaneSet | null {
  if (!v || typeof v !== 'object') return null;
  const o = v as Record<string, unknown>;
  const out = { ...DEFAULT_PANES };
  for (const p of PANE_ORDER) if (typeof o[p] === 'boolean') out[p] = o[p] as boolean;
  return out;
}

const electron = () => (import.meta.client ? (window as any).electronAPI : undefined);

let _hydrated = false;

export const useWorkspaceLayout = () => {
  const panes            = useState<PaneSet>('liveplay:panes', () => ({ ...DEFAULT_PANES }));
  // What Restore brings back after a pane was expanded; null when nothing is.
  const restoreSet       = useState<PaneSet | null>('liveplay:paneRestore', () => null);
  const cartWidth        = useState<number>('liveplay:cartWidth', () => CART_DEFAULT_PX);
  const mixerWidth       = useState<number>('liveplay:mixerWidth', () => MIXER_DEFAULT_PX);
  const propertiesHeight = useState<number>('liveplay:propertiesHeight', () => PROPERTIES_DEFAULT_PX);
  // Set by the main window when Electron reports the window opened/closed.
  const cartDetached     = useState<boolean>('liveplay:cartDetached', () => false);
  const mixerDetached    = useState<boolean>('liveplay:mixerDetached', () => false);
  // Read here, at setup, rather than inside openWindow: the handlers below run
  // from click events, where Nuxt's composables cannot be called.
  const currentProject   = useState<{ folderPath?: string } | null>('currentProject', () => null);

  if (import.meta.client && !_hydrated) {
    _hydrated = true;
    try {
      const raw = localStorage.getItem(STORAGE_KEY);
      if (raw) {
        const s = JSON.parse(raw) as StoredLayout;
        if (s && typeof s === 'object') {
          if (isSanePx(s.cartWidth))        cartWidth.value        = s.cartWidth;
          if (isSanePx(s.mixerWidth))       mixerWidth.value       = s.mixerWidth;
          if (isSanePx(s.propertiesHeight)) propertiesHeight.value = s.propertiesHeight;
          const stored = readPaneSet(s.panes);
          if (stored) {
            panes.value = stored;
            restoreSet.value = readPaneSet(s.restore);
          } else {
            // The 2.5.0 betas' flags. Fullscreen/expanded states are not carried
            // over — landing docked is the predictable answer after an upgrade.
            panes.value = {
              playlist: true,
              cart:  s.cartClosed !== true,
              mixer: s.mixerOpen === true && s.mixerCollapsed !== true,
            };
          }
        }
      }
    } catch {
      // Unparseable, or storage denied. The defaults are already in place.
    }
  }

  const isDetached = (p: PaneId) =>
    p === 'cart' ? cartDetached.value : p === 'mixer' ? mixerDetached.value : false;

  /** Shown in this window's workspace row (on, and not in a window of its own). */
  const dockedPanes = computed<PaneId[]>(() =>
    PANE_ORDER.filter(p => panes.value[p] && !isDetached(p)));

  /** The pane that stretches to fill the row: the leftmost docked one. */
  const flexPane = computed<PaneId | null>(() => dockedPanes.value[0] ?? null);

  /** This pane is filling the workspace because it was expanded. */
  const isExpanded = (p: PaneId) =>
    restoreSet.value !== null && dockedPanes.value.length === 1 && dockedPanes.value[0] === p;

  /** Whether Expand means anything: some other pane is docked beside it. */
  const canExpand = (p: PaneId) =>
    panes.value[p] && !isDetached(p) && dockedPanes.value.length > 1;

  // Electron only: a browser has no second window to put a pane in.
  const canDetach = (p: PaneId) => {
    const api = electron();
    if (p === 'cart')  return !!api?.openCartPlayerWindow;
    if (p === 'mixer') return !!api?.openMixerWindow;
    return false;
  };

  function openWindow(p: PaneId) {
    const api = electron();
    // Both factories focus an existing window rather than making a second.
    if (p === 'cart') {
      void api?.openCartPlayerWindow?.(currentProject.value?.folderPath);
    } else if (p === 'mixer') {
      void api?.openMixerWindow?.();
    }
  }

  function closeWindow(p: PaneId) {
    const api = electron();
    try {
      if (p === 'cart')  api?.attachCartPlayerWindow?.();
      if (p === 'mixer') api?.attachMixerWindow?.();
    } catch { /* not Electron */ }
  }

  /**
   * Switch a pane on. A pane that is already in its own window is raised
   * instead — it is already on, just somewhere else.
   *
   * Any explicit choice of what to show ends an expanded layout: the lit
   * toggles are now the layout, and a Restore that later threw them away
   * would be undoing something the operator just did.
   */
  function showPane(p: PaneId) {
    if (panes.value[p] && isDetached(p)) { openWindow(p); return; }
    restoreSet.value = null;
    panes.value = { ...panes.value, [p]: true };
  }

  /**
   * Switch a pane off, wherever it is. Closing an EXPANDED pane gives the
   * workspace back to the panes it had hidden — the same as closing a
   * maximised window reveals what was behind it — rather than leaving an
   * empty row.
   */
  function hidePane(p: PaneId) {
    const wasDetached = isDetached(p);
    if (isExpanded(p) && restoreSet.value) {
      panes.value = { ...restoreSet.value, [p]: false };
      restoreSet.value = null;
    } else {
      panes.value = { ...panes.value, [p]: false };
    }
    // Off first, THEN close: the window's `closed` event clears the detached
    // flag, and with the pane still on that would dock it straight back.
    if (wasDetached) closeWindow(p);
  }

  function togglePane(p: PaneId) {
    if (panes.value[p]) hidePane(p);
    else showPane(p);
  }

  /** Give this pane the whole workspace; the others are remembered for Restore. */
  function expandPane(p: PaneId) {
    if (isDetached(p)) return;
    const next: PaneSet = { ...panes.value, [p]: true };
    // Detached panes keep their windows — expanding one pane is a statement
    // about this window, not an order to close the others.
    for (const q of PANE_ORDER) if (q !== p && !isDetached(q)) next[q] = false;
    restoreSet.value = { ...panes.value, [p]: true };
    panes.value = next;
  }

  /** Put back the panes an expand hid. */
  function restorePanes() {
    if (!restoreSet.value) return;
    const next = { ...restoreSet.value };
    // A pane that went into a window while expanded is still on.
    for (const q of PANE_ORDER) if (isDetached(q)) next[q] = true;
    panes.value = next;
    restoreSet.value = null;
  }

  /** Into its own window. Leaving an expanded pane gives the others back. */
  function detachPane(p: PaneId) {
    if (!canDetach(p)) return;
    if (isExpanded(p)) restorePanes();
    panes.value = { ...panes.value, [p]: true };
    openWindow(p);
  }

  /** Back from its own window into the workspace row. */
  function attachPane(p: PaneId) {
    closeWindow(p);
  }

  // ---- Persistence ---------------------------------------------------------
  let timer: ReturnType<typeof setTimeout> | null = null;
  const write = () => {
    timer = null;
    try {
      const out: StoredLayout = {
        panes:            panes.value,
        restore:          restoreSet.value,
        cartWidth:        cartWidth.value,
        mixerWidth:       mixerWidth.value,
        propertiesHeight: propertiesHeight.value,
      };
      localStorage.setItem(STORAGE_KEY, JSON.stringify(out));
    } catch { /* no storage — the session keeps working, it just won't be recalled */ }
  };

  /** Remember the current layout. Coalesces a drag into one write. */
  const persistLayout = () => {
    if (!import.meta.client) return;
    if (timer) clearTimeout(timer);
    timer = setTimeout(write, WRITE_DEBOUNCE_MS);
  };

  /** Write NOW, for a page teardown where a pending timer would be lost. */
  const flushLayout = () => {
    if (!import.meta.client) return;
    if (timer) { clearTimeout(timer); timer = null; }
    write();
  };

  return {
    panes, restoreSet, dockedPanes, flexPane,
    cartWidth, mixerWidth, propertiesHeight,
    cartDetached, mixerDetached,
    isDetached, isExpanded, canExpand, canDetach,
    showPane, hidePane, togglePane, expandPane, restorePanes, detachPane, attachPane,
    persistLayout, flushLayout,
  };
};
