// https://nuxt.com/docs/api/configuration/nuxt-config
export default defineNuxtConfig({
  devtools: {
    enabled: true,

    // Timeline disabled: it wraps every auto-imported composable in a
    // `__nuxtTimelineWrap` at module-top-level, which reads the wrapped
    // binding eagerly. With the useCartItems ↔ useProject auto-import
    // cycle that read hits TDZ and crashes the renderer to a white screen.
    // Re-enable only after breaking the cycle (e.g. move the shared map
    // out of useCartItems into a neutral module imported by both).
    timeline: {
      enabled: false
    }
  },
  ssr: false,
  
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
    // assets/ lives at the project root (shared with Electron), so use the
    // rootDir alias (~~) rather than the srcDir alias (~), which now points
    // at app/ under Nuxt 4.
    '~~/assets/styles/main.scss'
  ],

  vite: {
    css: {
      preprocessorOptions: {
        scss: {
          additionalData: '@use "~~/assets/styles/variables.scss" as *;'
        }
      }
    }
  },

  modules: [],

  // Preserve the pre-Nuxt-4 non-strict TypeScript behaviour for this project.
  typescript: {
    strict: false
  },

  compatibilityDate: '2025-10-31'
})