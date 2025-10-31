// https://nuxt.com/docs/api/configuration/nuxt-config
export default defineNuxtConfig({
  devtools: { enabled: true },
  ssr: false,
  
  app: {
    head: {
      title: 'LivePlay',
      meta: [
        { charset: 'utf-8' },
        { name: 'viewport', content: 'width=device-width, initial-scale=1' }
      ]
    }
  },

  css: [
    '~/assets/styles/main.scss'
  ],

  vite: {
    css: {
      preprocessorOptions: {
        scss: {
          additionalData: '@use "~/assets/styles/variables.scss" as *;'
        }
      }
    }
  },

  modules: [],

  compatibilityDate: '2025-10-31'
})
