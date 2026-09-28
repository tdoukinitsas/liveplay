# Documentation site

The public product website is a Nuxt 4 / Vue 3 static SPA hosted at [tdoukinitsas.github.io/liveplay](https://tdoukinitsas.github.io/liveplay/). Its homepage remains user-facing; the developer API reference is a separate static page.

## Run locally

From `docs-site/`:

```sh
npm install
npm run dev
```

Open **http://localhost:3000/liveplay/** (or the port Nuxt prints). The API page is **http://localhost:3000/liveplay/api/**.

## Preview generated output

```sh
npm run generate
```

The site is generated into `.output/public`. Assets use the configured `/liveplay/` base path. Serving `.output/public` at `/` with a plain static server causes requests such as `/liveplay/assets/...` to return 404; `npm run preview` can have this behavior.

To preserve the deployment path without changing configuration, use a temporary parent directory. PowerShell, from `docs-site/`:

```powershell
$previewRoot = Join-Path ([IO.Path]::GetTempPath()) ("liveplay-docs-" + [guid]::NewGuid().ToString("N"))
$previewSite = Join-Path $previewRoot "liveplay"
New-Item -ItemType Directory -Path $previewSite | Out-Null
Copy-Item -Path ".output/public/*" -Destination $previewSite -Recurse
python -m http.server 3000 --bind 127.0.0.1 --directory $previewRoot
```

Open **http://localhost:3000/liveplay/** and **http://localhost:3000/liveplay/api/**. Stop the server with Ctrl+C. Python 3 is needed only for this static preview option; `npm run dev` does not need it.

## Layout

| File / directory | Purpose |
|---|---|
| `app/app.vue` | Product homepage and link to the API page |
| `app/components/` | Feature cards and language switcher |
| `app/composables/useI18n.ts` | Locale loading, English fallback and saved locale |
| `app/assets/styles/main.scss` | Global styles |
| `public/locales/` | Website translations, separate from client translations |
| `public/api/index.html` | API connection guide and reference structure |
| `public/api/reference.json` | Authoritative API contract catalog |
| `public/api/reference.js` | Search, accordions, links and table rendering |
| `public/api/reference.css` | Styles scoped to the separate API page |
| `scripts/check-api-reference.mjs` | Read-only inventory/catalog consistency check |
| `scripts/copy-screenshots.mjs` | Copy canonical client screenshots before dev/generation |
| `nuxt.config.ts` | Static output and `/liveplay/` base path |

The homepage fetches `public/package.json` for version information and `contributors.json` for contributors. CI copies the root README into `public/README.md`; the current homepage does not render that file.

## Maintain the API reference

The API page uses ordinary HTML/CSS/JavaScript and files under `public/api/`; it needs no new router, UI framework or runtime dependencies. Nuxt copies it into the static output.

1. Verify behavior in server handlers and the stores/engine they call.
2. Update the relevant entry in `reference.json`. Keep one entry per route registration; list compatible method aliases together.
3. Reuse a shared schema for complex objects rather than copying it into several endpoints.
4. Include validation, actual error behavior, examples and persistence/broadcast notes.
5. For WebSocket changes, check commands, emitted frame types and `doc_patch` operations independently.
6. Run the inventory check and generation:
   ```sh
   node scripts/check-api-reference.mjs
   npm run generate
   ```
7. Preview under `/liveplay/`; check search, deep links and mobile layout.

Field-table rows are `[name, type, required, default, description]`. Query field names begin with `?`; path identifiers are described in endpoint notes. Simple response field tables derive from the examples; complex responses link to explicit schemas. Examples are illustrative, not recorded server responses.

Endpoint `sections` contain detailed behavior and additional request examples under the existing HTTP entries. Section field tables select prefixes from a shared schema; edit DSP ranges/defaults in `BusDsp`. WebSocket behavior belongs to the corresponding command/event/notification entries. `links` and `relatedSchemas` connect entries without creating a separate protocol catalog.

The inventory checker verifies the current literal registration/message patterns, catalog completeness, duplicate entries, schema references and endpoint detail sections and request examples. It is not a C++ parser or an integration test. Re-audit its extraction when registration or serialization mechanisms change. Crow's implicit static route and HEAD/OPTIONS behavior require separate dependency review.

Repository guides link to this reference instead of maintaining another REST/WebSocket schema copy. The API page is currently English; other homepage content continues to use the existing translations and fallback.

## Deployment and checks

[deploy-docs.yml](../.github/workflows/deploy-docs.yml) copies root README/package metadata, installs site dependencies, generates the site and publishes `.output/public/` through GitHub Pages.

The package defines `build` and `generate` as the same generation command. It has no lint/check script. The API inventory check is run explicitly and does not change the existing build pipeline.
