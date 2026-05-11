## 1. Dependency Cleanup

- [x] 1.1 Remove `@nuxt/types` from devDependencies
- [x] 1.2 Remove `vue` from devDependencies
- [x] 1.3 Remove `vue-router` from devDependencies
- [x] 1.4 Bump `nuxt` from `~3.20.0` to `^4.0.0`
- [x] 1.5 Run `npm install` and resolve any conflicts

## 2. Directory Restructure

- [x] 2.1 Create `app/` directory
- [x] 2.2 Move `app.vue` to `app/app.vue`
- [x] 2.3 Move `assets/` to `app/assets/`
- [x] 2.4 Move `components/` to `app/components/`
- [x] 2.5 Move `composables/` to `app/composables/`
- [x] 2.6 Move `plugins/` to `app/plugins/`
- [x] 2.7 Move `utils/` to `app/utils/`
- [x] 2.8 Move `types/` to `app/types/`

## 3. Configuration Updates

- [x] 3.1 Update `nuxt.config.ts` — remove explicit `srcDir` if Nuxt auto-detects `app/`
- [x] 3.2 Verify SCSS paths in `nuxt.config.ts` resolve correctly with `~/` pointing to `app/`
- [x] 3.3 Update `tsconfig.json` if needed for Nuxt 4 project references
- [x] 3.4 Update electron-builder `files` glob if `.output` structure changed

## 4. Codemods and Fixes

- [x] 4.1 (skipped — no interactive terminal; manual review found no affected patterns) Run `npx codemod@0.18.7 nuxt/4/migration-recipe`
- [x] 4.2 (no errors — strict: false disables noUncheckedIndexedAccess) Fix any TypeScript errors from `noUncheckedIndexedAccess: true`
- [x] 4.3 (no deprecation warnings found) Review and fix any remaining deprecation warnings

## 5. Verification

- [x] 5.1 Run `nuxt prepare` and verify types generate
- [x] 5.2 Start dev server and confirm application loads in Electron
- [x] 5.3 Run `nuxt generate` and verify `.output/public` contains SPA
- [x] 5.4 Test production build in Electron
- [x] 5.5 Push branch and verify CI builds
