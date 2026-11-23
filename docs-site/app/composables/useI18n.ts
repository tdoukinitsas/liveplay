import { ref, computed } from 'vue';
import en from '../../public/locales/en.json';

interface LocaleMetadata {
  code: string;
  name: string;
  nativeName: string;
  direction: 'ltr' | 'rtl';
}

interface LocaleData {
  _metadata: LocaleMetadata;
  [key: string]: any;
}

const availableLocales: Record<string, string> = {
  'ar': 'العربية',
  'bn': 'বাংলা',
  'de': 'Deutsch',
  'el': 'Ελληνικά',
  'en': 'English',
  'es': 'Español',
  'fa': 'فارسی',
  'fr': 'Français',
  'hi': 'हिन्दी',
  'it': 'Italiano',
  'ja': '日本語',
  'ko': '한국어',
  'no': 'Norsk',
  'pt': 'Português',
  'ro': 'Română',
  'ru': 'Русский',
  'sq': 'Shqip',
  'sv': 'Svenska',
  'tr': 'Türkçe',
  'ur': 'اردو',
  'zh': '中文'
};

const currentLocale = ref<string>('en');
// English ships in the bundle: it is what the prerendered pages contain, so a
// crawler or a slow connection always gets real text.
const localeData = ref<LocaleData | null>(en as LocaleData);
// English data kept loaded as a fallback so untranslated/new keys in other
// locales render real text instead of the raw key path.
const fallbackData = ref<LocaleData | null>(en as LocaleData);
// Read once at load: while a prerendered page hydrates, the router briefly
// rewrites the address bar without its query string.
const initialLang = typeof window !== 'undefined'
  ? new URLSearchParams(window.location.search).get('lang')
  : null;

export const useI18n = () => {
  // Detect browser language
  const detectBrowserLanguage = (): string => {
    if (typeof window === 'undefined') return 'en';
    
    const browserLang = navigator.language || (navigator as any).userLanguage;
    const langCode = browserLang.split('-')[0]; // Get 'en' from 'en-US'
    
    // Check if we support this language
    return availableLocales[langCode] ? langCode : 'en';
  };

  // Load locale data
  const loadLocale = async (locale: string) => {
    try {
      const data = locale === 'en'
        ? en as LocaleData
        : await (await fetch(`/liveplay/locales/${locale}.json`)).json();
      localeData.value = data;
      currentLocale.value = locale;

      // Save to localStorage
      if (typeof window !== 'undefined') {
        localStorage.setItem('docs-locale', locale);
      }
    } catch (error) {
      console.error(`Failed to load locale ${locale}:`, error);
      // Fallback to English
      if (locale !== 'en') {
        await loadLocale('en');
      }
    }
  };

  // Resolve a dotted path against a locale object, or return null if absent.
  const resolve = (source: LocaleData | null, path: string): string | null => {
    if (!source) return null;
    let value: any = source;
    for (const key of path.split('.')) {
      if (value && typeof value === 'object' && key in value) {
        value = value[key];
      } else {
        return null;
      }
    }
    return typeof value === 'string' ? value : null;
  };

  // Get translation by path (e.g., 'header.tagline'). Falls back to English,
  // then to the raw path if even English is missing the key.
  const t = (path: string, params?: Record<string, string | number>): string => {
    const text = resolve(localeData.value, path)
      ?? resolve(fallbackData.value, path)
      ?? path;
    if (!params) return text;
    return text.replace(/\{(\w+)\}/g, (match, key) => (key in params ? String(params[key]) : match));
  };

  // A list of strings (feature points). Falls back to English as a whole, so a
  // half-translated list never mixes languages.
  const tl = (path: string): string[] => {
    const read = (source: LocaleData | null): string[] | null => {
      if (!source) return null;
      let value: any = source;
      for (const key of path.split('.')) {
        if (value && typeof value === 'object' && key in value) value = value[key];
        else return null;
      }
      return Array.isArray(value) ? value : null;
    };
    return read(localeData.value) ?? read(fallbackData.value) ?? [];
  };

  // Initialize locale
  const initLocale = async () => {
    let locale = 'en';

    if (typeof window !== 'undefined') {
      // Check URL parameter first (highest priority)
      const urlLang = initialLang;
      if (urlLang && availableLocales[urlLang]) {
        locale = urlLang;
      } else {
        // Check localStorage
        const savedLocale = localStorage.getItem('docs-locale');
        if (savedLocale && availableLocales[savedLocale]) {
          locale = savedLocale;
        } else {
          // Detect browser language
          locale = detectBrowserLanguage();
        }
      }
    }

    await loadLocale(locale);
  };

  // Change locale
  const setLocale = async (locale: string) => {
    if (availableLocales[locale]) {
      await loadLocale(locale);
    }
  };

  const locale = computed(() => currentLocale.value);
  const locales = computed(() => availableLocales);
  const direction = computed(() => localeData.value?._metadata?.direction || 'ltr');
  const isLocaleLoaded = computed(() => localeData.value !== null);

  return {
    t,
    tl,
    locale,
    locales,
    direction,
    isLocaleLoaded,
    initLocale,
    setLocale
  };
};
