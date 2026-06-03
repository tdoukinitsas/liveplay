// Dynamically loaded locales from main process
const locales = ref<Record<string, any>>({});
const availableLocalesData = ref<Array<{ code: string; name: string; direction: string }>>([]);

// loadLocales is idempotent — without this guard every component that calls
// useLocalization() re-fetches all 21 locale files from the main process via
// IPC on every invocation, which saturates the IPC channel and starves
// other async work in the renderer (e.g. fetch body reads from the server).
let _loadLocalesPromise: Promise<void> | null = null;
const loadLocales = async (): Promise<void> => {
  if (!import.meta.client || !(window as any).electronAPI) return;
  if (_loadLocalesPromise) return _loadLocalesPromise;
  _loadLocalesPromise = (async () => {
    try {
      const localesList = await (window as any).electronAPI.getAvailableLocales();
      availableLocalesData.value = localesList;
      const localePromises = localesList.map(async (locale: any) => {
        const data = await (window as any).electronAPI.getLocaleData(locale.code);
        locales.value[locale.code] = data;
      });
      await Promise.all(localePromises);
      // eslint-disable-next-line no-console
      console.log(`Dynamically loaded ${Object.keys(locales.value).length} locales`);
    } catch (error) {
      console.error('Failed to load locales from main process:', error);
      _loadLocalesPromise = null;   // allow a retry on transient failures
      throw error;
    }
  })();
  return _loadLocalesPromise;
};

// Dynamically build available locales from loaded data
export const availableLocales = computed(() => availableLocalesData.value);

export const useLocalization = () => {
  const currentLocale = useState<string>('locale', () => 'en');

  // Initialize locales on first use
  if (import.meta.client && Object.keys(locales.value).length === 0) {
    loadLocales();
  }

  const t = (key: string, params?: Record<string, string | number>): string => {
    const keys = key.split('.');
    let value: any = locales.value[currentLocale.value];

    if (!value) {
      return key; // Return key if locale not loaded yet
    }

    for (const k of keys) {
      if (value && typeof value === 'object' && k !== '_metadata') {
        value = value[k];
      } else {
        return key; // Return key if translation not found
      }
    }

    let result = typeof value === 'string' ? value : key;
    if (params) {
      for (const [param, val] of Object.entries(params)) {
        result = result.replace(`{${param}}`, String(val));
      }
    }
    return result;
  };

  const setLocale = (locale: string) => {
    if (locale in locales.value) {
      currentLocale.value = locale;
      // Save to project if available
      if (import.meta.client) {
        localStorage.setItem('liveplay-locale', locale);
      }
    }
  };

  const getDirection = (): string => {
    const locale = locales.value[currentLocale.value];
    return (locale as any)?._metadata?.direction || 'ltr';
  };

  // Load saved locale on mount, or detect system locale
  if (import.meta.client) {
    // Wait for locales to load before setting locale
    loadLocales().then(() => {
      const savedLocale = localStorage.getItem('liveplay-locale');
      if (savedLocale && savedLocale in locales.value) {
        currentLocale.value = savedLocale;
      } else if (window.electronAPI && window.electronAPI.getSystemLocale) {
        // No saved preference, try to detect system locale
        window.electronAPI.getSystemLocale().then((systemLocale: string) => {
          // Check if we have a translation for this locale
          if (systemLocale in locales.value) {
            currentLocale.value = systemLocale;
            // Save the detected locale
            localStorage.setItem('liveplay-locale', systemLocale);
            // Update the menu language
            if (window.electronAPI.updateMenuLanguage) {
              window.electronAPI.updateMenuLanguage(systemLocale);
            }
            console.log(`Auto-detected system locale: ${systemLocale}`);
          } else {
            console.log(`System locale ${systemLocale} not available, using default: en`);
          }
        }).catch((error: any) => {
          console.warn('Failed to detect system locale:', error);
        });
      }
    });
  }

  return {
    t,
    currentLocale,
    setLocale,
    availableLocales,
    getDirection
  };
};
