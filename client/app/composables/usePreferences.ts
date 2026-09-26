// =====================================================================
// usePreferences.ts
// ---------------------------------------------------------------------
// The four values that belong to the PERSON at the desk rather than to the
// show on it (U4):
//
//   theme.mode / theme.accentColor   the colour scheme
//   meterMode                        which unit a meter is drawn in
//   uiScrollToPlaying                does the playlist chase the playing cue
//   playbackKeys                     the transport keymap
//
// All four lived in the .liveplay document until now, which meant they
// travelled with it. Opening a colleague's show changed your colours and
// silently reassigned the keys your hands already knew — the keymap being much
// the worse of the two, because you find out during a cue.
//
// ---------------------------------------------------------------------
// Where they live depends on whether anyone is signed in
// ---------------------------------------------------------------------
//   no accounts configured  →  this SURFACE's preference, in localStorage.
//                              There is no person for it to belong to, so the
//                              honest owner is the machine in front of you.
//   signed in               →  this PERSON's profile on the server, following
//                              them to the tablet, the desk and back.
//
// localStorage is written in both cases. When signed in it is a cache, not the
// authority: it is what paints the first frame before /api/prefs answers, so
// the app does not flash the default theme on every launch.
//
// ---------------------------------------------------------------------
// Sparse, and why that matters
// ---------------------------------------------------------------------
// An absent key means NOT CHOSEN, not "off". Only what somebody actually set
// is stored, so an installation keeps taking improved defaults instead of
// being frozen at whatever they were the day someone first opened a colour
// picker. `effective*` below is what to read; the raw refs are what to write.
//
// ---------------------------------------------------------------------
// The migration
// ---------------------------------------------------------------------
// Two halves, and neither is destructive:
//   * signed in — the server seeds a profile from the open project the first
//     time /api/prefs is read (see UserPrefs::get_or_seed).
//   * not signed in — adoptFromProject() below copies the open project's
//     legacy fields into the local store, once, for a surface that has never
//     chosen. That is what makes a 2.4 file still look right when opened on a
//     machine that has never run 2.5.
// The server drops the legacy fields on the next save, and says so through the
// project_migrated banner, so nothing changes shape without the operator being
// told.
// =====================================================================
import type { CartSlotKeyBinding, Theme } from '~/types/project';
import { DEFAULT_THEME } from '~/types/project';

const STORAGE_KEY = 'liveplay.prefs';

export interface Preferences {
  theme?: Partial<Theme>;
  meterMode?: string;
  uiScrollToPlaying?: boolean;
  playbackKeys?: Record<string, CartSlotKeyBinding | null>;
  locale?: string;
}

// Renderer-scoped: the plugin wires the server sync once, but every component
// may call usePreferences() to read.
let _wired = false;

const readLocal = (): Preferences => {
  if (!import.meta.client) return {};
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return {};
    const parsed = JSON.parse(raw);
    return parsed && typeof parsed === 'object' ? parsed : {};
  } catch {
    // Private browsing, disabled storage, or a half-written value. Defaults
    // are a perfectly good desk; refusing to start over a colour is not.
    return {};
  }
};

const writeLocal = (prefs: Preferences) => {
  if (!import.meta.client) return;
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(prefs)); } catch { /* ignore */ }
};

export const usePreferences = () => {
  const server = useLiveplayServer();
  const prefs  = useState<Preferences>('liveplay.prefs', () => readLocal());

  // True once the server's answer (or its 409) has landed. Guards the push
  // watcher so the locally-restored copy is never sent back at a profile that
  // has not been read yet — the same rule useShowControl keeps for Show Mode.
  const hydrated = useState<boolean>('liveplay.prefs.hydrated', () => false);
  // Whether what we hold came from a server profile rather than this machine.
  // adoptFromProject must not run over a real profile.
  const hydratedFromServer = useState<boolean>('liveplay.prefs.fromServer', () => false);

  // ---- What to read ----------------------------------------------------
  const theme = computed<Theme>(() => ({
    mode:        (prefs.value.theme?.mode as Theme['mode']) ?? DEFAULT_THEME.mode,
    accentColor: prefs.value.theme?.accentColor ?? DEFAULT_THEME.accentColor,
  }));
  const scrollToPlaying = computed<boolean>(() => prefs.value.uiScrollToPlaying === true);
  // Deliberately `string | null` rather than a default: absent means "follow
  // the project's output target", which only useOutputTarget can resolve.
  const meterMode = computed<string | null>(() => prefs.value.meterMode ?? null);
  const playbackKeys = computed<Record<string, CartSlotKeyBinding | null> | null>(
    () => prefs.value.playbackKeys ?? null);

  // ---- What to write ---------------------------------------------------
  // One funnel, so local persistence and the server push cannot disagree
  // about what was set. A null value clears a key, which is how "stop
  // choosing this" is said — necessary because sparse means absent and
  // present-but-default are genuinely different.
  const applying = ref(false);

  const patch = (p: Preferences) => {
    const next: Preferences = { ...prefs.value };
    for (const [k, v] of Object.entries(p)) {
      if (v === null || v === undefined) {
        delete (next as any)[k];
      } else if (k === 'theme') {
        // Merge, so setting an accent colour does not drop the mode.
        next.theme = { ...(next.theme ?? {}), ...(v as Partial<Theme>) };
      } else {
        (next as any)[k] = v;
      }
    }
    prefs.value = next;
    writeLocal(next);
    // Fire and forget: the local store already has it, and a failed push must
    // not undo a change the operator can see on screen.
    if (!applying.value && hydrated.value) void server.patchPrefs(p);
  };

  const setTheme      = (t: Partial<Theme>) => patch({ theme: t });
  const setMeterMode  = (m: string | null)  => patch({ meterMode: m as any });
  const setScrollToPlaying = (on: boolean)  => patch({ uiScrollToPlaying: on });
  const setPlaybackKeys = (keys: Record<string, CartSlotKeyBinding | null>) =>
    patch({ playbackKeys: keys });

  // Apply a profile the server handed us. Not merged with what is here: the
  // profile IS the authority once somebody is signed in, and merging would let
  // a stale local value outlive a preference the operator cleared on another
  // machine.
  const applyFromServer = (incoming: Preferences | null) => {
    if (!incoming) return;
    applying.value = true;
    try {
      const next = { ...incoming };
      delete (next as any).schemaVersion;
      prefs.value = next;
      writeLocal(next);
    } finally {
      nextTick(() => { applying.value = false; });
    }
  };

  // The unauthenticated half of the migration. Copies whatever the open
  // project is still carrying into a surface that has never chosen — once,
  // per key, so it can never overwrite something the operator set. Called by
  // useProject when a document lands.
  const adoptFromProject = (doc: any) => {
    if (!doc || hydratedFromServer.value) return;
    const seed: Preferences = {};
    if (prefs.value.theme === undefined && doc.theme && typeof doc.theme === 'object') {
      seed.theme = { mode: doc.theme.mode, accentColor: doc.theme.accentColor };
    }
    if (prefs.value.playbackKeys === undefined &&
        doc.playbackKeys && typeof doc.playbackKeys === 'object' &&
        Object.keys(doc.playbackKeys).length > 0) {
      seed.playbackKeys = doc.playbackKeys;
    }
    const s = doc.settings ?? {};
    if (prefs.value.meterMode === undefined && typeof s.meterMode === 'string') {
      seed.meterMode = s.meterMode;
    }
    if (prefs.value.uiScrollToPlaying === undefined &&
        typeof s.uiScrollToPlaying === 'boolean') {
      seed.uiScrollToPlaying = s.uiScrollToPlaying;
    }
    if (Object.keys(seed).length === 0) return;
    // Written locally only. Pushing it at the server would be this surface
    // deciding what a signed-in person's profile should say on the strength of
    // whatever file happened to be open, and the server does its own seeding
    // for exactly that case.
    const next = { ...prefs.value, ...seed };
    prefs.value = next;
    writeLocal(next);
  };

  // ---- server → client -------------------------------------------------
  // Read the signed-in operator's profile. Also the moment the SERVER-side
  // migration happens: GET /api/prefs seeds a profile that does not exist yet
  // from whatever project is open, so this is called again after a project
  // loads, not only on connect. Without that second call the ordinary startup
  // order — socket up, then project opened — would take a person's one chance
  // to inherit their theme and keymap from a 2.4 file and spend it on an empty
  // desk. (The server's half of that guarantee is get_or_seed refusing to
  // write an empty profile.)
  const refresh = async () => {
    const profile = await server.fetchPrefs();
    if (profile && Object.keys(profile).length > 0) {
      applyFromServer(profile);
      hydratedFromServer.value = true;
    } else if (profile) {
      // Signed in, but nothing stored yet. Whatever this machine has stands,
      // and the next thing they change starts their profile.
      hydratedFromServer.value = true;
    } else {
      // 409, or no server: this surface owns its own preferences and whatever
      // localStorage restored is the truth.
      hydratedFromServer.value = false;
    }
    hydrated.value = true;
  };

  const api = {
    prefs, theme, meterMode, scrollToPlaying, playbackKeys,
    patch, setTheme, setMeterMode, setScrollToPlaying, setPlaybackKeys,
    adoptFromProject, applyFromServer, hydrated, refresh,
  };

  if (_wired || !import.meta.client) return api;
  _wired = true;

  // Re-read on every connect, not just the first. A reconnect can be a
  // different server, or the same one after somebody signed in elsewhere.
  watch(() => server.connected, (up) => { if (up) void refresh(); });
  // ...and once up front regardless, so `hydrated` cannot stay false forever
  // on a client whose socket never comes up — that would leave every write
  // local-only with nothing saying why.
  void refresh();

  // The same person's other windows — the detached cart and mixer players hold
  // their own sockets, so without this they would sit on the old theme until
  // they happened to reconnect.
  server.onDocPatch((p: any) => {
    if (p?.op === 'prefs_changed') applyFromServer(p.prefs ?? null);
  });

  // Another window on this machine, signed in or not. Mirrors useUiMode's
  // cross-window rule so a detached player follows the desk even with no
  // server profile in play.
  window.addEventListener('storage', (e) => {
    if (e.key !== STORAGE_KEY) return;
    applying.value = true;
    try { prefs.value = readLocal(); }
    finally { nextTick(() => { applying.value = false; }); }
  });

  return api;
};
