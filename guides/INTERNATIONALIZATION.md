# LivePlay Internationalization (i18n)

## Overview

LivePlay supports **20 languages** with full internationalization, including right-to-left (RTL) support for Arabic, Farsi, and Urdu.

## Supported Languages

| Language | Code | Native Name | Direction |
|----------|------|-------------|-----------|
| English | `en` | English | LTR |
| Greek | `el` | Ελληνικά | LTR |
| French | `fr` | Français | LTR |
| Spanish | `es` | Español | LTR |
| Italian | `it` | Italiano | LTR |
| Portuguese | `pt` | Português | LTR |
| Arabic | `ar` | العربية | **RTL** |
| Farsi | `fa` | فارسی | **RTL** |
| German | `de` | Deutsch | LTR |
| Swedish | `sv` | Svenska | LTR |
| Norwegian | `no` | Norsk | LTR |
| Russian | `ru` | Русский | LTR |
| Japanese | `ja` | 日本語 | LTR |
| Chinese | `zh` | 中文 | LTR |
| Hindi | `hi` | हिन्दी | LTR |
| Bengali | `bn` | বাংলা | LTR |
| Turkish | `tr` | Türkçe | LTR |
| Korean | `ko` | 한국어 | LTR |
| Albanian | `sq` | Shqip | LTR |
| Urdu | `ur` | اردو | **RTL** |

## How It Works

### Architecture

The internationalization system is built on:
- **JSON locale files** (`locales/*.json`) containing all translations
- **Dynamic locale loading** in `composables/useLocalization.ts`
- **Automatic menu generation** in `electron/main.js`
- **RTL support** through CSS in `assets/styles/main.scss`

### Locale File Structure

Each locale file (e.g., `locales/en.json`) contains:

```json
{
  "_metadata": {
    "code": "en",
    "name": "English",
    "nativeName": "English",
    "direction": "ltr"
  },
  "app": { ... },
  "menu": { ... },
  "welcome": { ... },
  "project": { ... },
  "playlist": { ... },
  "cart": { ... },
  "properties": { ... },
  "playback": { ... },
  "endBehavior": { ... },
  "startBehavior": { ... },
  "duckingBehavior": { ... },
  "actions": { ... },
  "about": { ... },
  "colors": { ... },
  "youtube": { ... },
  "update": { ... }
}
```

#### Metadata Fields

- **`code`**: ISO 639-1 language code (e.g., `en`, `ar`)
- **`name`**: English name of the language
- **`nativeName`**: Language name in its native script (used in UI)
- **`direction`**: Text direction (`ltr` or `rtl`)

### Usage in Components

Import and use the localization composable:

```vue
<script setup lang="ts">
const { t } = useLocalization();
</script>

<template>
  <button>{{ t('menu.newProject') }}</button>
  <p>{{ t('welcome.subtitle') }}</p>
</template>
```

The `t()` function accepts dot-notation keys (e.g., `menu.file`, `properties.volume`) and returns the translated string for the current locale.

### Language Switching

Users can change the language through:
1. **Menu**: `View > Language > [Choose Language]`
2. The selected language is automatically saved to `localStorage`
3. The app automatically reloads the menu with the new language

### RTL Support

For Arabic and Farsi, the app automatically:
- Sets `dir="rtl"` on the root HTML element
- Applies RTL-specific CSS styles (reversed layouts, mirrored margins, etc.)
- Flips flex directions and text alignment

RTL is triggered by the `direction` field in `_metadata`:

```json
{
  "_metadata": {
    "direction": "rtl"
  }
}
```

## Adding a New Language

To add a new language to LivePlay:

### 1. Create the Locale File

Create a new file in `locales/` (e.g., `locales/de.json` for German):

```json
{
  "_metadata": {
    "code": "de",
    "name": "German",
    "nativeName": "Deutsch",
    "direction": "ltr"
  },
  "app": {
    "name": "LivePlay"
  },
  "menu": {
    "file": "Datei",
    "newProject": "Neues Projekt",
    ...
  },
  ...
}
```

**Important**: Copy the structure from `locales/en.json` to ensure all keys are present.

### 2. Update `useLocalization.ts`

Add the import and include it in the `locales` object:

```typescript
import de from '~/locales/de.json';

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
  ko,
  sq,
  ur // Add your new locale here
};
```

The `availableLocales` array is automatically generated from the `_metadata` of each locale, so no additional changes are needed.

### 3. Update `electron/main.js`

Add the locale file to the `localeFiles` object:

```javascript
const localeFiles = {
  en: require('../locales/en.json'),
  el: require('../locales/el.json'),
  fr: require('../locales/fr.json'),
  es: require('../locales/es.json'),
  it: require('../locales/it.json'),
  pt: require('../locales/pt.json'),
  ar: require('../locales/ar.json'),
  fa: require('../locales/fa.json'),
  de: require('../locales/de.json'),
  sv: require('../locales/sv.json'),
  no: require('../locales/no.json'),
  ru: require('../locales/ru.json'),
  ja: require('../locales/ja.json'),
  zh: require('../locales/zh.json'),
  hi: require('../locales/hi.json'),
  bn: require('../locales/bn.json'),
  tr: require('../locales/tr.json'),
  ko: require('../locales/ko.json'),
  sq: require('../locales/sq.json'),
  ur: require('../locales/ur.json'),
  de: require('../locales/de.json') // Add your new locale here
};
```

The menu translations and language submenu are automatically generated from `localeFiles`.

### 4. Test Your Translation

1. **Build the app**: `npm run build`
2. **Run the app**: `npm run electron:dev`
3. Go to `View > Language` and select your new language
4. Verify all UI strings are translated correctly
5. Check that the menu updates properly

## Translation Keys Reference

### Menu (`menu.*`)
- `file`, `newProject`, `openProject`, `saveProject`, `closeProject`, `openProjectFolder`, `exit`
- `view`, `toggleDarkMode`, `changeAccentColor`, `fullscreen`, `language`
- `help`, `about`

### Welcome Screen (`welcome.*`)
- `title`, `subtitle`, `newProject`, `openProject`

### Project (`project.*`)
- `enterName`, `placeholder`, `cancel`, `ok`, `importAudio`, `noProject`

### Playlist (`playlist.*`)
- `title`, `import`, `addGroup`, `noItems`, `importAudio`, `importHint`

### Cart Player (`cart.*`)
- `title`, `emptySlot`, `clickToImport`

### Properties Panel (`properties.*`)
- `title`, `noSelection`, `displayName`, `color`, `inPoint`, `outPoint`, `volume`
- `endBehavior`, `startBehavior`, `duckingBehavior`
- `fadeOutDuration`, `duckFadeIn`, `duckFadeOut`
- `children`, `seconds`, `uuid`, `index`, `apiTriggerUrl`, `file`, `duration`, `mode`, `action`
- `playbackSection`, `media`, `replace`, `basicInfo`, `loadingAudioData`, `playback`, `ducking`, `duckLevel`, `trimSilence`

### Playback Controls (`playback.*`)
- `panic`, `noActiveCues`, `play`, `stop`, `delete`, `edit`

### Behaviors
- **End**: `nothing`, `next`, `gotoItem`, `gotoIndex`, `loop`
- **Start**: `nothing`, `playNext`, `playItem`, `playIndex`, `playFirst`, `playAll`
- **Ducking**: `stopAll`, `noDucking`, `duckOthers`, `duckLevel`

### Actions (`actions.*`)
- `delete`, `confirm`, `close`

### About (`about.*`)
- `developedBy`, `githubRepo`, `developerName`

### Colors (`colors.*`)
- `chooseAccent`

### YouTube Import (`youtube.*`)
- `importFromYouTube`, `searchPlaceholder`, `searching`, `noResults`, `searchError`
- `preview`, `download`, `downloading`, `previewHint`
- `downloadQueue`, `statusDownloading`, `statusConverting`, `statusCompleted`, `statusError`, `downloadError`

### Updates (`update.*`)
- `updateAvailable`, `currentVersion`, `newVersion`, `whatsNew`, `updatePrompt`
- `downloading`, `downloadComplete`, `installInfo`
- `later`, `downloadAndInstall`, `installNow`, `installOnExit`, `downloadFailed`

## RTL CSS Classes

The following CSS utilities are automatically applied for RTL languages:

```scss
[dir="rtl"] {
  text-align: right;
  
  .flex-row {
    flex-direction: row-reverse;
  }
  
  .ml-auto {
    margin-left: 0;
    margin-right: auto;
  }
  
  .mr-auto {
    margin-right: 0;
    margin-left: auto;
  }
  
  .float-left {
    float: right;
  }
  
  .float-right {
    float: left;
  }
}
```

## Best Practices

### For Translators

1. **Keep context in mind**: Some terms may have different meanings in different contexts
2. **Preserve placeholders**: Don't translate interpolation markers like `{{variable}}`
3. **Maintain formatting**: Preserve capitalization styles (Title Case, sentence case)
4. **Test in app**: Always test your translations in the running application
5. **Check for truncation**: Long translations may get cut off in UI

### For Developers

1. **Use semantic keys**: Use descriptive key names like `playlist.noItems` instead of `text1`
2. **Avoid hardcoded strings**: Always use `t('key')` instead of hardcoding English text
3. **Provide context**: Add comments in locale files for ambiguous terms
4. **Test all languages**: Verify that UI layouts work with all supported languages
5. **Support RTL**: Use CSS flexbox/grid instead of absolute positioning when possible

## Language Persistence

The selected language is automatically saved to:
- **Browser**: `localStorage` key `liveplay-locale`
- **Electron**: Synced across app restarts

## Troubleshooting

### Language not showing in menu
- Ensure the locale file is imported in `composables/useLocalization.ts`
- Ensure the locale file is required in `electron/main.js` `localeFiles` object
- Verify the `_metadata` object is correctly formatted

### Translations not appearing
- Check that the key exists in the locale file
- Verify you're using the correct dot-notation path
- Check browser console for errors

### RTL layout issues
- Ensure `direction: "rtl"` is set in `_metadata`
- Check that components use flexbox for layout (not absolute positioning)
- Add custom RTL styles in `assets/styles/main.scss` if needed

### Menu not updating
- Make sure `createMenu()` is called after language change
- Verify `mainWindow.webContents.send('menu-change-language', code)` is triggered
- Check that `menuTranslations` object includes the new language

## Contributing Translations

We welcome contributions for new languages! To contribute:

1. Fork the repository
2. Create a new locale file in `locales/`
3. Translate all keys from `locales/en.json`
4. Update `useLocalization.ts` and `electron/main.js`
5. Test your translation thoroughly
6. Submit a pull request with:
   - The new locale file
   - Updated code files
   - Screenshots showing the translation in action

## Future Enhancements

Potential improvements to the i18n system:

- [ ] Automatic locale detection based on system language
- [ ] Plural forms support (e.g., "1 file" vs "2 files")
- [ ] Date/time localization
- [ ] Number formatting (decimals, thousands separators)
- [ ] Dynamic translation loading (reduce bundle size)
- [ ] Translation management UI for non-developers
- [ ] Crowdsourcing platform integration (Crowdin, Weblate)

---

**Last Updated**: January 2025  
**Version**: 1.2.0
