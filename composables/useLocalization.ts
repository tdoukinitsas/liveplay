// Dynamically loaded locales from main process
const locales = ref<Record<string, any>>({});
const availableLocalesData = ref<Array<{ code: string; name: string; direction: string }>>([]);

// Load locales from main process
const loadLocales = async () => {
  if (import.meta.client && window.electronAPI) {
    try {
      // Get available locales list
      const localesList = await window.electronAPI.getAvailableLocales();
      availableLocalesData.value = localesList;
      
      // Load all locale data
      const localePromises = localesList.map(async (locale: any) => {
        const data = await window.electronAPI.getLocaleData(locale.code);
        locales.value[locale.code] = data;
      });
      
      await Promise.all(localePromises);
      console.log(`Dynamically loaded ${Object.keys(locales.value).length} locales`);
    } catch (error) {
      console.error('Failed to load locales from main process:', error);
    }
  }
};

// Dynamically build available locales from loaded data
export const availableLocales = computed(() => availableLocalesData.value);

export const useLocalization = () => {
  const currentLocale = useState<string>('locale', () => 'en');

  // Initialize locales on first use
  if (import.meta.client && Object.keys(locales.value).length === 0) {
    loadLocales();
  }

  const t = (key: string): string => {
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
    
    return typeof value === 'string' ? value : key;
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
