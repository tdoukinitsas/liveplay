import { ref, computed } from 'vue';

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
const localeData = ref<LocaleData | null>(null);
// English data kept loaded as a fallback so untranslated/new keys in other
// locales render real text instead of the raw key path.
const fallbackData = ref<LocaleData | null>(null);

export const useI18n = () => {
  // Get URL parameter
  const getUrlParam = (param: string): string | null => {
    if (typeof window === 'undefined') return null;
    const urlParams = new URLSearchParams(window.location.search);
    return urlParams.get(param);
  };

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
      const response = await fetch(`/liveplay/locales/${locale}.json`);
      const data = await response.json();
      localeData.value = data;
      currentLocale.value = locale;

      // Keep English loaded as the fallback for any keys missing in `locale`.
      if (locale === 'en') {
        fallbackData.value = data;
      } else if (!fallbackData.value) {
        try {
          const enResponse = await fetch('/liveplay/locales/en.json');
          fallbackData.value = await enResponse.json();
        } catch { /* fallback is best-effort */ }
      }

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
  const t = (path: string): string => {
    return resolve(localeData.value, path)
      ?? resolve(fallbackData.value, path)
      ?? path;
  };

  // Initialize locale
  const initLocale = async () => {
    let locale = 'en';

    if (typeof window !== 'undefined') {
      // Check URL parameter first (highest priority)
      const urlLang = getUrlParam('lang');
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
    locale,
    locales,
    direction,
    isLocaleLoaded,
    initLocale,
    setLocale
  };
};
