import en from '~/locales/en.json';
import el from '~/locales/el.json';

const locales = {
  en,
  el
};

export const availableLocales = [
  { code: 'en', name: 'English' },
  { code: 'el', name: 'Ελληνικά' }
];

export const useLocalization = () => {
  const currentLocale = useState<string>('locale', () => 'en');

  const t = (key: string): string => {
    const keys = key.split('.');
    let value: any = locales[currentLocale.value as keyof typeof locales];
    
    for (const k of keys) {
      if (value && typeof value === 'object') {
        value = value[k];
      } else {
        return key; // Return key if translation not found
      }
    }
    
    return typeof value === 'string' ? value : key;
  };

  const setLocale = (locale: string) => {
    if (locale in locales) {
      currentLocale.value = locale;
      // Save to project if available
      if (import.meta.client) {
        localStorage.setItem('liveplay-locale', locale);
      }
    }
  };

  // Load saved locale on mount
  if (import.meta.client) {
    const savedLocale = localStorage.getItem('liveplay-locale');
    if (savedLocale && savedLocale in locales) {
      currentLocale.value = savedLocale;
    }
  }

  return {
    t,
    currentLocale,
    setLocale,
    availableLocales
  };
};
