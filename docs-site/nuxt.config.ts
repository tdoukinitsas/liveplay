export default defineNuxtConfig({
  ssr: false,
  
  app: {
    baseURL: '/liveplay/',
    buildAssetsDir: 'assets',
    head: {
      title: 'LivePlay - Audio Cue Playback for Live Events',
      meta: [
        { charset: 'utf-8' },
        { name: 'viewport', content: 'width=device-width, initial-scale=1' },
        { name: 'description', content: 'Free, open-source audio playback system for live sound operators. Available for Windows, macOS, and Linux.' },
        { name: 'theme-color', content: '#DA1E28' }
      ],
      link: [
        { rel: 'icon', type: 'image/x-icon', href: '/liveplay/favicon.ico' }
      ]
    }
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
    preset: 'static'
  },

  compatibilityDate: '2025-01-01'
})
