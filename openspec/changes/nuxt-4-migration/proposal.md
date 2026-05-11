## Why

Nuxt 3.20 is approaching end of maintenance. Nuxt 4 brings improved TypeScript defaults, better file watcher performance with the `app/` directory convention, managed Vue/vue-router versions (eliminating version conflicts), and forward-compatibility with Nuxt 5. The `@nuxt/types` devDep is a dead Nuxt 2 package that should be removed.

## What Changes

- **BREAKING**: Upgrade Nuxt from ~3.20.0 to ^4.0.0
- **BREAKING**: Adopt `app/` directory structure — move `app.vue`, `assets/`, `components/`, `composables/`, `plugins/`, `utils/`, `types/` into `app/`
- Remove `@nuxt/types` (Nuxt 2 legacy, does nothing)
- Remove explicit `vue` and `vue-router` devDeps (Nuxt 4 manages these internally)
- Update `nuxt.config.ts` for Nuxt 4 conventions
- Run codemods for data fetching changes (`null` → `undefined`)
- Fix any TypeScript errors from `noUncheckedIndexedAccess: true` default

### Low-risk (no affected code expected)

- SPA loading template renders beside `#__nuxt` instead of inside (no custom template used)
- `window.__NUXT__` removed (not referenced in codebase)
- Unhead v2 prop removals (`vmid`, `hid`, `children`, `body`) — not used

## Capabilities

### New Capabilities

- `nuxt-4-compat`: Nuxt 4 compatibility — app directory structure, managed Vue/vue-router, updated TypeScript defaults, codemods applied

### Modified Capabilities

_(none — no existing specs have requirement-level changes)_

## Impact

- **`nuxt.config.ts`**: Update `srcDir`, SCSS paths resolve to `app/assets/`
- **`app.vue`**: Moves to `app/app.vue`
- **`assets/`**: Moves to `app/assets/`
- **`components/`**: Moves to `app/components/`
- **`composables/`**: Moves to `app/composables/`
- **`plugins/`**: Moves to `app/plugins/` (currently empty)
- **`utils/`**: Moves to `app/utils/`
- **`types/`**: Moves to `app/types/`
- **`package.json`**: Remove `@nuxt/types`, `vue`, `vue-router` from devDeps; bump nuxt to ^4.0.0
- **`tsconfig.json`**: May need updates for Nuxt 4 project references
- **`.output/public`**: Build output path unchanged — Electron integration unaffected
