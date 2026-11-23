import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const SITE_URL = 'https://tdoukinitsas.github.io/liveplay/';

// Every whatsNew.v<major><minor> entry in the English copy is a release page, so
// adding one is enough to get its route prerendered.
const en = JSON.parse(readFileSync(resolve(__dirname, 'public/locales/en.json'), 'utf8'));
const releaseRoutes = Object.keys(en.whatsNew)
  .filter(key => /^v\d{2,}$/.test(key))
  .map(key => `/whats-new/${key[1]}-${key.slice(2)}/`);

// The icon font is subset to what the site uses. A new icon must be added here,
// in alphabetical order, or it shows as its name.
const ICONS = [
  'audio_file', 'call_split', 'celebration', 'close', 'dock_to_bottom', 'download', 'edit_note',
  'folder_open', 'keyboard', 'language', 'menu', 'motion_photos_paused', 'open_in_full', 'pan_tool',
  'queue_music', 'radio', 'settings', 'stadia_controller', 'theater_comedy', 'timer', 'trending_down',
];

// Runs before first paint. The pages are prerendered in English, so a visitor
// who will get another language has the content held back until it arrives,
// with a timeout so a failed load can never leave the page blank.
const LOCALES = ['ar', 'bn', 'de', 'el', 'es', 'fa', 'fr', 'hi', 'it', 'ja', 'ko', 'no', 'pt', 'ro', 'ru', 'sq', 'sv', 'tr', 'ur', 'zh'];
const holdForLocale = `(function(){try{var a=${JSON.stringify(LOCALES)},q=new URLSearchParams(location.search).get('lang'),s=localStorage.getItem('docs-locale'),b=(navigator.language||'en').split('-')[0],l=q&&(q==='en'||a.indexOf(q)>-1)?q:s||b;if(a.indexOf(l)>-1){var r=document.documentElement;r.classList.add('i18n-pending');setTimeout(function(){r.classList.remove('i18n-pending')},2500)}}catch(e){}})()`;

export default defineNuxtConfig({
  // Prerendered to real HTML at build time: search engines and most AI crawlers
  // read the page without running JavaScript.
  ssr: true,

  // Ensure we don't inherit from parent project
  rootDir: './',

  app: {
    baseURL: '/liveplay/',
    buildAssetsDir: 'assets',
    head: {
      htmlAttrs: { lang: 'en', dir: 'ltr' },
      meta: [
        { charset: 'utf-8' },
        { name: 'viewport', content: 'width=device-width, initial-scale=1' },
        { name: 'theme-color', content: '#09090b' },
        { name: 'color-scheme', content: 'dark' },
        { name: 'application-name', content: 'LivePlay' },
        { name: 'apple-mobile-web-app-title', content: 'LivePlay' },
      ],
      link: [
        { rel: 'icon', href: '/liveplay/favicon.ico', sizes: '32x32' },
        { rel: 'icon', type: 'image/svg+xml', media: '(prefers-color-scheme: dark)', href: '/liveplay/assets/icons/SVG/liveplay-icon-darkmode@web.svg' },
        { rel: 'icon', type: 'image/svg+xml', media: '(prefers-color-scheme: light)', href: '/liveplay/assets/icons/SVG/liveplay-icon-lightmode@web.svg' },
        { rel: 'apple-touch-icon', sizes: '180x180', href: '/liveplay/og/apple-touch-icon.png' },
        { rel: 'sitemap', type: 'application/xml', href: `${SITE_URL}sitemap.xml` },
        { rel: 'alternate', type: 'text/plain', title: 'LivePlay for AI assistants', href: `${SITE_URL}llms.txt` },
        { rel: 'preconnect', href: 'https://fonts.googleapis.com' },
        { rel: 'preconnect', href: 'https://fonts.gstatic.com', crossorigin: '' },
        { rel: 'preload', as: 'font', type: 'font/ttf', href: '/liveplay/fonts/IBM_Plex_Sans/static/IBMPlexSans_Condensed-Bold.ttf', crossorigin: '' },
        { rel: 'stylesheet', href: 'https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;500;600;700&display=swap' },
        { rel: 'stylesheet', href: `https://fonts.googleapis.com/css2?family=Material+Symbols+Rounded:opsz,wght,FILL,GRAD@24,400,0,0&icon_names=${ICONS.join(',')}&display=block` },
      ],
      script: [{ innerHTML: holdForLocale, tagPosition: 'head' }],
    },
  },

  css: ['~/assets/styles/main.scss'],

  vite: {
    css: {
      preprocessorOptions: {
        scss: {
          additionalData: ''
        }
      }
    }
  },

  nitro: {
    preset: 'static',
    // GitHub Pages serves real files only, so each page gets its own HTML.
    prerender: {
      crawlLinks: false,
      failOnError: true,
      routes: ['/', '/whats-new/', ...releaseRoutes]
    }
  },

  typescript: {
    strict: false,
    typeCheck: false
  },

  compatibilityDate: '2025-01-01'
})
