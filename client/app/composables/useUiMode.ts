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
  }

  const setUiMode = (mode: UiMode) => {
    uiMode.value = mode;
    if (import.meta.client) {
      try { localStorage.setItem(STORAGE_KEY, mode); } catch {}
    }
  };

  const enterPlaybackMode = () => setUiMode('playback');
  const exitPlaybackMode = () => setUiMode('edit');
  const toggleUiMode = () => setUiMode(uiMode.value === 'playback' ? 'edit' : 'playback');

  return { uiMode, setUiMode, enterPlaybackMode, exitPlaybackMode, toggleUiMode };
};
