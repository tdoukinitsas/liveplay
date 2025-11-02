import en from '~/locales/en.json';
import el from '~/locales/el.json';
import fr from '~/locales/fr.json';
import es from '~/locales/es.json';
import it from '~/locales/it.json';
import pt from '~/locales/pt.json';
import ar from '~/locales/ar.json';
import fa from '~/locales/fa.json';
import de from '~/locales/de.json';
import sv from '~/locales/sv.json';
import no from '~/locales/no.json';
import ru from '~/locales/ru.json';
import ja from '~/locales/ja.json';
import zh from '~/locales/zh.json';
import hi from '~/locales/hi.json';
import bn from '~/locales/bn.json';
import tr from '~/locales/tr.json';
import ko from '~/locales/ko.json';

const locales = {
  en,
  el,
  fr,
  es,
  it,
  pt,
  ar,
  fa,
  de,
  sv,
  no,
  ru,
  ja,
  zh,
  hi,
  bn,
  tr,
  ko
};

// Dynamically build available locales from metadata
export const availableLocales = Object.values(locales).map((locale: any) => ({
  code: locale._metadata.code,
  name: locale._metadata.nativeName,
  direction: locale._metadata.direction
}));

export const useLocalization = () => {
  const currentLocale = useState<string>('locale', () => 'en');

  const t = (key: string): string => {
    const keys = key.split('.');
    let value: any = locales[currentLocale.value as keyof typeof locales];
    
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
    if (locale in locales) {
      currentLocale.value = locale;
      // Save to project if available
      if (import.meta.client) {
        localStorage.setItem('liveplay-locale', locale);
      }
    }
  };

  const getDirection = (): string => {
    const locale = locales[currentLocale.value as keyof typeof locales];
    return (locale as any)._metadata.direction || 'ltr';
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
    availableLocales,
    getDirection
  };
};
