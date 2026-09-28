// =====================================================================
// useWorkspaceLayout.ts  —  P4
// ---------------------------------------------------------------------
// Where the workspace's boundaries are, and which panes are showing.
// Everything the splitters in MainWorkspace move, remembered between
// launches instead of snapping back to a constant every time.
//
// OWNERSHIP: the MACHINE store, and it is not a close call. A pane width
// is a statement about one screen — the widths that fit a console display
// are wrong on the laptop carried to rehearsal — so this must not follow
// an operator between desks the way U4's preferences do, and it must not
// travel in a show file the way routing does. Same tier and the same
// mechanism as useUiMode's Show Mode and useMixerView's docked mode.
//
// WHAT IS *NOT* HERE, deliberately:
//
//   * `mixerMode` (side / full) stays in useMixerView. It is layout too,
//     but it is a NAMED SETTING with a Settings pane that has to report
//     and change it, which is why it got its own key and its own
//     vocabulary. Everything in this file is geometry nobody chooses by
//     name. Moving it here would also mean migrating a key that has
//     already shipped in this stack for no gain.
//
//   * `cartDetached` / `mixerDetached`. Those describe WINDOWS, and a
//     flag saying "detached" restored without the window it names would
//     hide the pane and hide the header button that brings it back —
//     ProjectHeader shows neither button while its pane is detached,
//     because there is supposed to be a window. That is a stranding bug,
//     so which windows were open is the Electron half's problem or
//     nobody's, and it is not solved by writing the flag down here.
//
// STORED SPARSELY. A key that is absent means "never dragged", so a
// default can still be improved later for everyone who has not moved that
// boundary themselves — the same rule U4's profiles follow.
//
// NOTHING HERE IS CLAMPED. These are the raw numbers last dragged to, and
// a layout saved on a 4K screen will over-request on a laptop. MainWorkspace
// owns the clamping, because only it knows the container width and the
// minimums the row has to honour; it calls reclampPanes() once after this
// restores, which is the same function the window-resize listener uses.
// =====================================================================

/** The splitters' defaults. One owner per constant (O5) — MainWorkspace reads these. */
export const CART_DEFAULT_PX       = 500;
export const MIXER_DEFAULT_PX      = 420;
export const PROPERTIES_DEFAULT_PX = 300;

const STORAGE_KEY = 'liveplay-workspace-layout';

// A drag emits a pointermove stream, and each one moves a width. Writing on
// every frame would hammer localStorage for a value only the last of which
// matters; the trailing edge is the only interesting one. Long enough to
// coalesce a whole drag, short enough that quitting straight after one keeps it.
const WRITE_DEBOUNCE_MS = 400;

interface StoredLayout {
  cartWidth?: number;
  mixerWidth?: number;
  propertiesHeight?: number;
  cartClosed?: boolean;
  cartFullscreen?: boolean;
  mixerOpen?: boolean;
  mixerCollapsed?: boolean;
}

// A stored width has to be a usable number before it is allowed near a
// stylesheet: `width: NaNpx` is a dead rule, and a negative or absurd value
// would lay the row out off screen. This only rejects nonsense — the real
// clamping against THIS window happens in MainWorkspace.
const SANE_MAX_PX = 20000;
const isSanePx = (v: unknown): v is number =>
  typeof v === 'number' && Number.isFinite(v) && v > 0 && v < SANE_MAX_PX;

let _hydrated = false;

export const useWorkspaceLayout = () => {
  // The same shared keys MainWorkspace and ProjectHeader already use for the
  // visibility flags, so this is a memory bolted onto the existing state rather
  // than a second copy of it.
  const cartWidth        = useState<number>('liveplay:cartWidth', () => CART_DEFAULT_PX);
  const mixerWidth       = useState<number>('liveplay:mixerWidth', () => MIXER_DEFAULT_PX);
  const propertiesHeight = useState<number>('liveplay:propertiesHeight', () => PROPERTIES_DEFAULT_PX);
  const cartClosed       = useState<boolean>('liveplay:cartClosed', () => false);
  const cartFullscreen   = useState<boolean>('liveplay:cartFullscreen', () => false);
  const mixerOpen        = useState<boolean>('liveplay:mixerOpen', () => false);
  const mixerCollapsed   = useState<boolean>('liveplay:mixerCollapsed', () => false);

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
          if (typeof s.cartClosed     === 'boolean') cartClosed.value     = s.cartClosed;
          if (typeof s.cartFullscreen === 'boolean') cartFullscreen.value = s.cartFullscreen;
          if (typeof s.mixerOpen      === 'boolean') mixerOpen.value      = s.mixerOpen;
          if (typeof s.mixerCollapsed === 'boolean') mixerCollapsed.value = s.mixerCollapsed;
        }
      }
    } catch {
      // Unparseable, or storage denied in a private window. The defaults above
      // are already in place, so there is nothing to repair — and a layout is
      // not worth surfacing an error over.
    }

    // A cart that is both closed and filling the workspace is not a state any
    // control can produce (every writer clears one when setting the other), but
    // it IS expressible in the file, and restoring it would draw nothing while
    // reporting a fullscreen cart. Closed wins: it is the one with a way back.
    if (cartClosed.value) cartFullscreen.value = false;
  }

  let timer: ReturnType<typeof setTimeout> | null = null;
  const write = () => {
    timer = null;
    try {
      const out: StoredLayout = {
        cartWidth:        cartWidth.value,
        mixerWidth:       mixerWidth.value,
        propertiesHeight: propertiesHeight.value,
        cartClosed:       cartClosed.value,
        cartFullscreen:   cartFullscreen.value,
        mixerOpen:        mixerOpen.value,
        mixerCollapsed:   mixerCollapsed.value,
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

  /**
   * Write NOW, skipping the debounce. For a page teardown, where a pending
   * timer would be thrown away with the window and the last drag lost.
   */
  const flushLayout = () => {
    if (!import.meta.client) return;
    if (timer) { clearTimeout(timer); timer = null; }
    write();
  };

  return {
    cartWidth, mixerWidth, propertiesHeight,
    cartClosed, cartFullscreen, mixerOpen, mixerCollapsed,
    persistLayout, flushLayout,
  };
};
