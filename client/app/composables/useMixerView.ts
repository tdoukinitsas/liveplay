// =====================================================================
// useMixerView.ts
// ---------------------------------------------------------------------
// How the mixer is laid out: docked as a right-hand pane, filling the
// workspace, or popped into its own window.
//
// OWNERSHIP: the MACHINE store, not the user profile and not the project.
// A mixer view is a statement about the screen in front of you — a console
// display wants the whole workspace, the laptop you carry to rehearsal
// wants it docked beside the playlist — so it must not follow an operator
// from one machine to another the way their theme does, and it must not
// travel in a show file the way D-numbered routing does. Same tier and the
// same mechanism as useUiMode's Show Mode, for the same reason.
//
// This also finally KEEPS the promise MainWorkspace already made: the
// comment on `liveplay:mixerMode` has always said the mode is "per-device,
// so it is remembered locally rather than travelling in the project", but
// nothing wrote it down, so every launch came up docked whatever you had
// chosen. `mixerMode` stays the live view state; this composable is what
// remembers it.
//
// R2 is not in question here and R1 holds through `setMixerMode` being the
// one writer: the live `useState` flag is still what the workspace renders
// from, and the store is only ever written through here.
// =====================================================================

export type MixerMode = 'side' | 'full';

const STORAGE_KEY = 'liveplay-mixer-mode';

const isMixerMode = (v: unknown): v is MixerMode => v === 'side' || v === 'full';

// One-time hydration guard, so however many components call this the store is
// read once and no caller clobbers another. Same shape as useUiMode's.
let _hydrated = false;

export const useMixerView = () => {
  // The SAME useState keys MainWorkspace and ProjectHeader already share — this
  // composable is a memory bolted onto the existing flags, not a second copy of
  // them. A second source of truth for "is the mixer full width" is exactly the
  // bug this codebase has already paid for once.
  const mixerMode     = useState<MixerMode>('liveplay:mixerMode', () => 'side');
  // `mixerOpen` belongs to useWorkspaceLayout (P4), which persists it along with
  // the rest of the pane layout. Taken from there rather than re-declared, so
  // there is one default for it and setMixerView's write below is remembered.
  const { mixerOpen } = useWorkspaceLayout();
  const mixerDetached = useState<boolean>('liveplay:mixerDetached', () => false);

  if (import.meta.client && !_hydrated) {
    _hydrated = true;
    try {
      const saved = localStorage.getItem(STORAGE_KEY);
      if (isMixerMode(saved)) mixerMode.value = saved;
    } catch {
      // Private browsing, or storage denied. 'side' is the default either way.
    }

    // Keep the windows agreeing. Each renderer has its own useState, so a
    // change in one window would otherwise not reach the others. `storage`
    // fires in every OTHER same-origin window — which is why there is no echo
    // to guard against here.
    //
    // useUiMode notes that this event is unreliable between Electron
    // BrowserWindows on file:// origins and adds an IPC path for that reason.
    // Deliberately NOT copied: Show Mode has to be identical everywhere at once
    // because it changes what the surface does, whereas a detached mixer window
    // is not rendering the docked pane at all and has nothing to restyle. A
    // stale value there costs nothing until that window docks again, by which
    // point it has re-read the store.
    try {
      window.addEventListener('storage', (e) => {
        if (e.key !== STORAGE_KEY) return;
        if (isMixerMode(e.newValue)) mixerMode.value = e.newValue;
      });
    } catch {
      // No window/addEventListener — the sync simply does not happen.
    }
  }

  /** Set the docked layout, and remember it for the next launch. */
  const setMixerMode = (mode: MixerMode) => {
    mixerMode.value = mode;
    if (import.meta.client) {
      try { localStorage.setItem(STORAGE_KEY, mode); } catch { /* no storage */ }
    }
  };

  // Electron only: in a browser there is no second window to open. Same test
  // MixerPanel's own detach button uses, so the two agree about when it exists.
  const canDetach = computed(() =>
    import.meta.client && !!(window as any).electronAPI?.openMixerWindow);

  /**
   * The view as one value, for a control that offers all three as a choice.
   * Detached wins over the mode because a detached mixer is not rendering the
   * docked pane at all — reporting 'full' for it would describe a pane that is
   * not on screen.
   */
  const mixerView = computed<'side' | 'full' | 'window'>(() =>
    mixerDetached.value ? 'window' : mixerMode.value);

  /**
   * Put the mixer into one of the three views, from anywhere.
   *
   * Detaching does NOT set `mixerDetached` here: the main window flips its own
   * panel off when it hears `mixer-window-opened`, so going through the same IPC
   * the mixer's own button uses keeps one path for both — and keeps working when
   * the window is reopened later rather than spawned fresh.
   */
  const setMixerView = (view: 'side' | 'full' | 'window') => {
    if (view === 'window') {
      if (!canDetach.value) return;
      void (window as any).electronAPI?.openMixerWindow?.();
      return;
    }
    // Coming back from a detached window: close it. The window's own 'closed'
    // handler is what clears `mixerDetached`, again so there is one path.
    if (mixerDetached.value) {
      try { (window as any).electronAPI?.attachMixerWindow?.(); } catch { /* not Electron */ }
    }
    setMixerMode(view);
    // Choosing a docked view is also asking to see it. Without this, picking
    // "docked to the side" while the mixer is closed silently sets a preference
    // and shows nothing, which reads as a control that does not work.
    mixerOpen.value = true;
  };

  return { mixerMode, mixerView, mixerOpen, mixerDetached, canDetach, setMixerMode, setMixerView };
};
