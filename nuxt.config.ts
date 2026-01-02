// https://nuxt.com/docs/api/configuration/nuxt-config
export default defineNuxtConfig({
  devtools: { enabled: true },
  ssr: false,

  devServer: {
    host: '127.0.0.1',
    port: 3000
  },
  
  app: {
    head: {
      title: 'LivePlay',
      meta: [
        { charset: 'utf-8' },
        { name: 'viewport', content: 'width=device-width, initial-scale=1' }
      ]
    },
    // Use relative paths for Electron
    baseURL: process.env.NODE_ENV === 'production' ? './' : '/',
    buildAssetsDir: '_nuxt/',
    cdnURL: process.env.NODE_ENV === 'production' ? './' : ''
  },

  css: [
    '@/assets/styles/main.scss'
  ],

  vite: {
    server: {
      host: '127.0.0.1',
      port: 3000,
      strictPort: true,
      hmr: {
        protocol: 'ws',
        host: '127.0.0.1',
        port: 3000,
        clientPort: 3000
      }
    },
    css: {
      preprocessorOptions: {
        scss: {
          additionalData: '@use "~/assets/styles/variables.scss" as *;'
        }
      }
    },
    build: {
      // Workaround for Windows path issues in Nuxt 3.20+
      rollupOptions: {
        external: []
      }
    }
  },

  modules: [],

  compatibilityDate: '2025-10-31'
})
