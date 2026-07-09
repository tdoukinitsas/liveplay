// =====================================================================
// useUiMode.ts
// ---------------------------------------------------------------------
// Global edit/playback ("Show Mode") toggle for Stage 1 of the
// touch-friendly playback surface (see IMPROVEMENTS_PLAN.md §2).
//
// Persisted per-device via localStorage (NOT in the project file) — the
// same project may be open on an editing laptop and a touch tablet at
// the same time, and each device should remember its own preference.
// =====================================================================

export type UiMode = 'edit' | 'playback';

const STORAGE_KEY = 'liveplay-ui-mode';

// Guards the one-time localStorage read so multiple components calling
// useUiMode() don't repeatedly touch storage or clobber each other.
let _hydrated = false;

export const useUiMode = () => {
  const uiMode = useState<UiMode>('useUiMode.uiMode', () => 'edit');

  if (import.meta.client && !_hydrated) {
    _hydrated = true;
    try {
      const saved = localStorage.getItem(STORAGE_KEY);
      if (saved === 'edit' || saved === 'playback') uiMode.value = saved;
    } catch {
      // localStorage unavailable (e.g. private browsing) — fall back to 'edit'.
    }

    // Keep separate windows (e.g. the detached cart player) in sync. Each
    // window is its own renderer with its own useState, so a mode change in
    // one window would otherwise not reach the others. The `storage` event
    // fires in every *other* same-origin window when localStorage changes.
    try {
      window.addEventListener('storage', (e) => {
        if (e.key !== STORAGE_KEY) return;
        const next = e.newValue;
        if (next === 'edit' || next === 'playback') uiMode.value = next;
      });
    } catch {
      // window/addEventListener unavailable — sync simply won't happen.
    }

    // The `storage` event is unreliable across separate Electron
    // BrowserWindows (especially file:// origins), so also sync over IPC.
    try {
      window.electronAPI?.onUiModeSet?.((_event, mode) => {
        if (mode === 'edit' || mode === 'playback') uiMode.value = mode;
      });
    } catch {
      // electronAPI unavailable (browser context) — IPC sync won't happen.
    }
  }

  const setUiMode = (mode: UiMode) => {
    uiMode.value = mode;
    if (import.meta.client) {
      try { localStorage.setItem(STORAGE_KEY, mode); } catch {}
      // Broadcast to other windows (detached cart player) so they follow the
      // overall application show-mode state rather than the mode they launched in.
      try { window.electronAPI?.broadcastUiMode?.(mode); } catch {}
    }
  };

  const enterPlaybackMode = () => setUiMode('playback');
  const exitPlaybackMode = () => setUiMode('edit');
  const toggleUiMode = () => setUiMode(uiMode.value === 'playback' ? 'edit' : 'playback');

  return { uiMode, setUiMode, enterPlaybackMode, exitPlaybackMode, toggleUiMode };
};
