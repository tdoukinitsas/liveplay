// =====================================================================
// useShowControl.ts
// ---------------------------------------------------------------------
// Two-way sync for the operator UI state the SERVER owns:
//
//   * the selected playlist item   (ProjectState::selected_item_uuid)
//   * Show Mode                    (ProjectState::show_mode)
//   * the display locale           (ProjectState::ui_locale)
//
// These used to be purely local (selection in useProject, Show Mode in
// localStorage, locale in localStorage). They moved server-side so that a
// Bitfocus Companion surface, a touch tablet and the operator's laptop all
// show the same selected cue and the same view mode — a control surface that
// can't see what the operator has selected can't safely arm it.
//
// This composable does NOT introduce a parallel store. It reads and writes
// the very same useState keys the rest of the app already binds to
// (`selectedItemUuid`, `useUiMode.uiMode`, `locale`), so no component needs
// to change. Direction of travel:
//
//   server → client   playback_snapshot on (re)connect, then doc_patch ops
//                     (selection_changed / show_mode_changed / locale_changed)
//   client → server   watchers on those same refs
//
// Echo safety comes from both ends: the server only broadcasts when a value
// actually changes, and `applying` suppresses the watcher while we write a
// server value in. Pushes are also gated on `hydrated` so the locally
// restored (localStorage) Show Mode and locale can't clobber a running show's
// state in the fraction of a second before the first snapshot lands.
//
// Wired once per renderer from plugins/liveplay-server.client.ts.
// =====================================================================
import type { UiMode } from '~/composables/useUiMode';

// Renderer-scoped, not per-call: the plugin calls this once, but guard anyway
// so an accidental second call can't double-subscribe to the WS streams.
let _wired = false;

export const useShowControl = () => {
  const server = useLiveplayServer();

  // The same state keys useProject / useUiMode / useLocalization own.
  const selectedItemUuid = useState<string | null>('selectedItemUuid', () => null);
  const selectedItems    = useState<Set<string>>('selectedItems', () => new Set());
  const uiMode           = useState<UiMode>('useUiMode.uiMode', () => 'edit');
  const currentLocale    = useState<string>('locale', () => 'en');

  const showMode = computed(() => uiMode.value === 'playback');

  // Intent helpers for components/hotkeys that want to drive the shared
  // selection explicitly rather than mutating the ref.
  const select     = (uuid: string | null) => server.setSelection(uuid);
  const selectStep = (delta: number)       => server.stepSelection(delta);
  const selectNext = () => selectStep(1);
  const selectPrev = () => selectStep(-1);
  const setShowMode    = (enabled: boolean) => server.setShowMode(enabled);
  const toggleShowMode = () => server.setShowMode();

  if (_wired || !import.meta.client) {
    return { selectedItemUuid, showMode, currentLocale,
             select, selectStep, selectNext, selectPrev, setShowMode, toggleShowMode };
  }
  _wired = true;

  // True while we're writing a server-sourced value into the refs — the
  // watchers below check this so an inbound patch never bounces back out.
  let applying = false;
  // Flipped by the first snapshot. Until then the refs still hold whatever
  // localStorage restored, which must not be pushed at a live server.
  let hydrated = false;

  const applyFromServer = (fn: () => void) => {
    applying = true;
    try { fn(); } finally { nextTick(() => { applying = false; }); }
  };

  const applySelection = (uuid: string | null) => {
    const next = uuid || null;
    if (selectedItemUuid.value === next) return;
    applyFromServer(() => {
      selectedItemUuid.value = next;
      // Keep the multi-select set in step: a server-driven selection is
      // always a single item (Companion has no shift-click), and leaving a
      // stale multi-selection behind would make a subsequent DEL delete
      // items the operator can no longer see highlighted.
      selectedItems.value = new Set(next ? [next] : []);
    });
  };

  const applyShowMode = (enabled: boolean) => {
    const next: UiMode = enabled ? 'playback' : 'edit';
    if (uiMode.value === next) return;
    applyFromServer(() => { uiMode.value = next; });
    // Mirror into this device's own persistence so a restart comes back in
    // the mode the show ended in, matching the pre-server behaviour.
    try { localStorage.setItem('liveplay-ui-mode', next); } catch { /* private browsing */ }
  };

  const applyLocale = (code: string) => {
    if (!code || currentLocale.value === code) return;
    applyFromServer(() => { currentLocale.value = code; });
    try { localStorage.setItem('liveplay-locale', code); } catch { /* private browsing */ }
  };

  // ---- server → client ------------------------------------------------
  server.onPlaybackSnapshot((snap: any) => {
    if (!snap) return;
    applySelection(snap.selected_item_uuid ?? null);
    if (typeof snap.show_mode === 'boolean') applyShowMode(snap.show_mode);
    // Language is this operator's, not the rig's (server U2). The snapshot
    // carries the installation DEFAULT, so it is adopted only by a client that
    // has never chosen for itself — otherwise reconnecting to a house running
    // in English would silently undo the Greek this person set, every time.
    //
    // Selection and Show Mode keep the opposite rule on purpose: those are the
    // show's state, shared by everyone looking at it, and a client joining
    // mid-show must adopt them rather than impose its own stale copy.
    let storedLocale: string | null = null;
    try { storedLocale = localStorage.getItem('liveplay-locale'); } catch { /* private browsing */ }
    if (storedLocale) {
      // Tell the server what this client is actually showing, so the session
      // record and anything reading it are not describing a language nobody
      // is looking at.
      server.setServerLocale(storedLocale);
    } else if (typeof snap.locale === 'string') {
      applyLocale(snap.locale);
    }
    // Only now may local changes travel outward.
    hydrated = true;
  });

  server.onDocPatch((patch: any) => {
    if (!patch || typeof patch !== 'object') return;
    switch (patch.op) {
      case 'selection_changed':
        applySelection(patch.itemUuid ?? null);
        break;
      case 'show_mode_changed':
        if (typeof patch.enabled === 'boolean') applyShowMode(patch.enabled);
        break;
      case 'locale_changed':
        if (typeof patch.locale === 'string') applyLocale(patch.locale);
        break;
    }
  });

  // ---- client → server ------------------------------------------------
  // Watching the refs (rather than patching every call site) means clicks in
  // the playlist, arrow keys, MIDI and the language picker all publish for
  // free, whichever path they took to change the value.
  watch(selectedItemUuid, (uuid) => {
    if (applying || !hydrated) return;
    server.setSelection(uuid ?? null);
  });

  watch(uiMode, (mode) => {
    if (applying || !hydrated) return;
    server.setShowMode(mode === 'playback');
  });

  watch(currentLocale, (code) => {
    if (applying || !hydrated || !code) return;
    server.setServerLocale(code);
  });

  return { selectedItemUuid, showMode, currentLocale,
           select, selectStep, selectNext, selectPrev, setShowMode, toggleShowMode };
};
