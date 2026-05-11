## Context

E-LivePlay is an Electron 42 desktop app using Nuxt ~3.20.0 with `ssr: false`. The project has a flat directory structure with `app.vue`, `assets/`, `components/`, `composables/`, `plugins/`, `utils/`, and `types/` at root alongside `electron/`, `openspec/`, and config files. Nuxt 4 defaults to `app/` as `srcDir`.

Current devDeps include `@nuxt/types` (Nuxt 2 legacy, unused), `vue` (^3.4.3), and `vue-router` (^4.2.5) — all managed internally by Nuxt 4.

## Goals / Non-Goals

**Goals:**

- Upgrade to Nuxt 4 with zero runtime regressions
- Adopt `app/` directory for cleaner project structure and better file watcher performance
- Remove dead/conflicting dependencies
- Maintain identical build output (`.output/public` for Electron)

**Non-Goals:**

- Adopting Nuxt 5 compatibility mode (future change)
- TypeScript 5 → 6 migration (separate change)
- Adding Nuxt modules or SSR
- Restructuring Electron code

## Decisions

### 1. Move files into `app/` during the upgrade

**Rationale**: Doing both at once avoids two rounds of path fixups. Nuxt 4 auto-detects flat structure, but adopting `app/` now gives the file watcher benefit and cleaner root immediately. The `~/` alias automatically resolves to `app/` — no import path changes needed in Vue/TS files.

**Alternative considered**: Stay flat and set `srcDir: '.'` — rejected because it defers the churn without eliminating it, and misses the watcher performance benefit.

### 2. Remove `vue` and `vue-router` from devDeps

**Rationale**: Nuxt 4 manages these internally. Explicit versions can cause resolution conflicts (Nuxt 4.4 ships vue-router v5, our pin is ^4.2.5). Removing them lets Nuxt resolve compatible versions.

**Alternative considered**: Keep and bump manually — rejected because it creates ongoing maintenance burden for no benefit.

### 3. Run codemods before manual fixes

**Rationale**: `npx codemod@0.18.7 nuxt/4/migration-recipe` handles `null` → `undefined` checks, `dedupe` boolean removal, and shallow reactivity patterns automatically. Run first, then fix remaining type errors manually.

### 4. Keep `ssr: false` and `baseURL: './'` unchanged

**Rationale**: These are Electron-specific requirements. Nuxt 4 doesn't change SPA mode behavior. The relative base URL ensures the packaged app loads assets correctly from disk.

## Risks / Trade-offs

- **[SCSS path resolution]** → `~/assets/styles/main.scss` in nuxt.config.ts must resolve correctly after move. Mitigation: `~/` points to `app/` automatically when `srcDir` is `app/`.
- **[vue-router v5 in Nuxt 4.4]** → If any code uses removed vue-router APIs. Mitigation: This app uses minimal routing (SPA with no pages directory).
- **[noUncheckedIndexedAccess]** → May surface type errors on array/object indexing. Mitigation: Fix individually or disable in tsconfig if too noisy.
- **[Codemods may miss patterns]** → Manual review after running. Mitigation: Test app thoroughly after migration.
