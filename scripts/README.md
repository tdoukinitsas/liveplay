# LivePlay scripts

Cross-platform helper scripts used by the monorepo `package.json` and by CI. Every script here is invoked from the **repository root**, not from this directory — paths inside the scripts are resolved relative to the repo root.

Node scripts use CommonJS (`require`) and target Node 20 LTS. Python scripts target Python 3 and reconfigure stdout to UTF-8 so they work on Windows.

---

## Build pipeline (Node)

These are the scripts wired into `npm run …` commands at the root. They are the supported entry points; CI calls them directly.

| Script | Invoked by | What it does |
|--------|-----------|--------------|
| [`build-all.js`](build-all.js)               | `npm run build`              | Unified release build: builds the C++ server, wraps it as `LivePlay Server.app` on macOS, runs `nuxt generate` + `electron-builder`, copies the installer artefacts (`.exe`, `.dmg`, `.AppImage`, `.deb`, `.rpm`) into `/build/` at the repo root. |
| [`build-clean.js`](build-clean.js)           | `npm run build:clean`        | Wipes build outputs, then delegates to `build-all.js`. Deliberately **preserves** `server/build/vcpkg_installed/` so the (slow) compiled C++ dependencies are not re-downloaded. |
| [`build-server.js`](build-server.js)         | `npm run server:build` / CI  | Configures (idempotently) and builds the C++ server using the appropriate CMake preset (`vs2022` on Windows, `default` elsewhere). |
| [`build-server-app-mac.js`](build-server-app-mac.js) | `npm run build:electron` (macOS only) | Wraps the freshly-built `liveplay-server` binary into a proper `.app` bundle that `electron-builder` can include in the DMG. No-ops on non-macOS hosts. |
| [`ensure-server.js`](ensure-server.js)       | `npm run dev`                | Pre-flight check before launching the renderer. If the server binary is already built, this is a no-op (fast dev-loop iteration). Otherwise it triggers a configure + build. |
| [`run-server.js`](run-server.js)             | `npm run server:run` (and `npm run dev:all`) | Locates the compiled `liveplay-server[.exe]` (searching both single-config and multi-config CMake output directories) and execs it, forwarding stdio and any CLI args. |

### Versioning

| Script | Invoked by | What it does |
|--------|-----------|--------------|
| [`version.js`](version.js) | `npm run version -- 2.1.4` (set) or `npm run bump -- patch|minor|major` (bump) | Updates the version across the monorepo. Touches every place a version is referenced: `package.json`, `client/package.json`, `docs-site/package.json`, `server/vcpkg.json` (`version-string`), and the version constant in `docs-site/app.vue`. |

Bumping the version on `main` is what triggers the [release workflow](../.github/workflows/build-release.yml).

---

## Localisation helpers

| Script | What it does |
|--------|--------------|
| [`sync-locale-keys.js`](sync-locale-keys.js) | Walks every JSON file in `client/locales/` and copies any keys missing relative to `en.json` (the source of truth). Run after adding new strings to `en.json`. Idempotent. |

### One-off localisation migrations (Python)

The four `add_*.py` scripts are migration tools, each written once to bulk-add a specific set of translation keys to every locale. They are kept in tree as documentation of past locale changes and to make follow-up fixes easier, but they are **not part of the regular workflow** — use `sync-locale-keys.js` and edit JSON files directly instead.

| Script | Purpose (historical) |
|--------|----------------------|
| [`add_all_missing_locales.py`](add_all_missing_locales.py) | Bulk-add of about/license + miscellaneous missing keys across every locale. |
| [`add_dual_dialog_locales.py`](add_dual_dialog_locales.py) | Added `exportProject`, `importProject`, `importAudio` sections + `exportProgress.downloading` + `common.cancel` for the dual-dialog import/export flow. |
| [`add_missing_locales.py`](add_missing_locales.py) | Added local-server / network-discovery strings (e.g. `startingLocalServer`, `serversOnThisNetwork`). |
| [`add_repair_locales.py`](add_repair_locales.py) | Added the project-repair flow strings. |

If you need to bulk-add new keys across all locales again, prefer writing a one-shot script in this same style rather than editing 21 files by hand. Keep it idempotent (re-runnable) and run `sync-locale-keys.js` afterwards to be sure.

---

## Conventions

- **Working directory**: every script resolves paths from `path.resolve(__dirname, '..')` and is safe to invoke from anywhere — never `cd` first.
- **Cross-platform**: avoid shell-specific syntax. Use `spawnSync({ shell: process.platform === 'win32' })` only when calling `npm`/`npx` on Windows.
- **Idempotent**: scripts should be safe to re-run. `ensure-server.js`, `build-server.js`, and `sync-locale-keys.js` all follow this.
- **Failure exits with non-zero**: any non-zero status from a child process should propagate up via `process.exit(res.status ?? 1)`.
- **Print, don't decorate**: stdio is forwarded to the user / CI log. Add a short `[script-name]` prefix on summary lines; don't add ANSI colour (CI logs choke on it).
