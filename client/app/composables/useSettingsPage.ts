// The Settings page: which section is showing, and the deep link to it.
//
// There is no router in this app. Views are panel swaps driven by shared flags
// (`liveplay:mixerOpen`, `cartFullscreen`), and the query string is already
// spoken for by *window identity* — `?mixerWindow=1` and `?cartWindow=1` are
// how Electron decides what a window IS, so a settings section cannot live
// there without confusing the two jobs. The hash is free, survives a reload,
// costs nothing in Electron, and is a real address a person can be sent:
//
//     #/settings/audio
//
// Introducing Nuxt routing instead would mean restructuring how every detached
// window boots, which is far more than a settings page should cost.

export interface SettingsSection {
  id: string;
  /** Material Symbols Rounded glyph name. */
  icon: string;
  /** i18n key for the rail label. */
  labelKey: string;
}

// The rail, in order. Sections are registered as their panes land, so the nav
// never advertises a pane that does not exist yet.
export const SETTINGS_SECTIONS: SettingsSection[] = [
  { id: 'playback', icon: 'play_circle',    labelKey: 'settings.sectionPlayback' },
  { id: 'audio',    icon: 'graphic_eq',     labelKey: 'settings.sectionAudio'    },
  // Beside Audio rather than beside Server, though it is the machine's: an
  // operator looking for where the sound physically goes looks under audio,
  // and the map is the other half of what the Audio pane's output target and
  // LTC picker are talking about.
  { id: 'outputs',  icon: 'settings_input_hdmi', labelKey: 'settings.sectionOutputs' },
  { id: 'keyboard', icon: 'keyboard',       labelKey: 'settings.sectionKeyboard' },
  { id: 'surfaces', icon: 'piano',          labelKey: 'settings.sectionSurfaces' },
  { id: 'project',  icon: 'folder_managed', labelKey: 'settings.sectionProject'  },
  // Last in the rail on purpose: it is the machine's settings rather than the
  // show's, and it is the one an operator has least reason to open mid-session.
  { id: 'server',   icon: 'dns',            labelKey: 'settings.sectionServer'   },
  // Beside Server rather than under it: accounts are the same tier — the
  // machine's, not the show's — and an operator sees the pane but not the list.
  { id: 'users',    icon: 'group',          labelKey: 'settings.sectionUsers'    },
];

export const DEFAULT_SETTINGS_SECTION = 'playback';

const HASH_PREFIX = '#/settings/';

/** The section named by a hash, or null when the hash is not a settings link. */
function sectionFromHash(hash: string): string | null {
  if (!hash.startsWith(HASH_PREFIX)) return null;
  const id = hash.slice(HASH_PREFIX.length).replace(/\/+$/, '');
  if (!id) return DEFAULT_SETTINGS_SECTION;
  // An unknown section opens the page rather than 404ing into a blank pane:
  // a stale link from an older build should still land somewhere useful.
  return SETTINGS_SECTIONS.some(s => s.id === id) ? id : DEFAULT_SETTINGS_SECTION;
}

// Module-level, so the hashchange listener is attached once however many
// components call this — the same guard useShowControl uses for its watchers.
let _wired = false;

export const useSettingsPage = () => {
  // null = closed. Shared, so the header button, the panes and any deep link
  // are all looking at one value.
  const section = useState<string | null>('liveplay:settingsSection', () => null);

  const isOpen = computed(() => section.value !== null);

  if (import.meta.client && !_wired) {
    _wired = true;
    // Adopt whatever the URL already says, so a reload on #/settings/audio
    // comes back to the same pane instead of the playlist.
    section.value = sectionFromHash(window.location.hash);
    window.addEventListener('hashchange', () => {
      section.value = sectionFromHash(window.location.hash);
    });
  }

  /** Open Settings, optionally straight to a section. */
  const open = (id: string = DEFAULT_SETTINGS_SECTION) => {
    const next = SETTINGS_SECTIONS.some(s => s.id === id) ? id : DEFAULT_SETTINGS_SECTION;
    section.value = next;
    if (import.meta.client) window.location.hash = HASH_PREFIX + next;
  };

  /** Switch pane without adding a history entry per click. */
  const show = (id: string) => {
    if (!SETTINGS_SECTIONS.some(s => s.id === id)) return;
    section.value = id;
    if (import.meta.client) {
      history.replaceState(null, '',
        window.location.pathname + window.location.search + HASH_PREFIX + id);
    }
  };

  const close = () => {
    section.value = null;
    if (import.meta.client) {
      // replaceState rather than `location.hash = ''`, which would leave a
      // bare "#" on the URL and push a history entry for closing a panel.
      history.replaceState(null, '', window.location.pathname + window.location.search);
    }
  };

  return { section, isOpen, open, show, close, sections: SETTINGS_SECTIONS };
};
