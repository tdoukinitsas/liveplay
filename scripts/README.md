# Repository scripts

Run these commands from the repository root. The Node helpers resolve repository paths from their own location.

| Command | Script | Behavior |
|---|---|---|
| `npm run build` | [build-all.js](build-all.js) | Build server, macOS server bundle where applicable, renderer and installers; collect artifacts in root `build/` |
| `npm run build:clean` | [build-clean.js](build-clean.js) | Remove build outputs and rebuild; retain `server/build/vcpkg_installed/` |
| `npm run server:build` | [build-server.js](build-server.js) | Configure/build with `vs2022` on Windows or `default` elsewhere |
| `npm run dev` | [ensure-server.js](ensure-server.js) | Build a missing server binary, then start client development |
| `npm run server:run -- <args>` | [run-server.js](run-server.js) | Find and run the server, forwarding arguments and stdio |
| `npm run build:electron` | [build-server-app-mac.js](build-server-app-mac.js) | Build server, wrap it on macOS and package the client |
| `npm run bump -- patch` | [version.js](version.js) | Increment version; also accepts `minor` / `major` |
| `npm run version -- 2.5.0` | [version.js](version.js) | Set a specific version |

Version updates touch root/client/docs-site package manifests, `server/vcpkg.json` and the fallback version in `docs-site/app/app.vue`. A root package version change on main is used by the [release workflow](../.github/workflows/build-release.yml).

## Localization

```sh
node scripts/sync-locale-keys.js
```

[sync-locale-keys.js](sync-locale-keys.js) fills missing client locale keys from `client/locales/en.json`. Review translations separately; copying an English value does not translate it.

The `add_*.py` helpers apply specific sets of locale keys. They are not part of the build or routine synchronization command. Read a helper's target keys before running it.

## Related commands

- [Development and checks](../docs/DEVELOPMENT.md)
- [Documentation-site generation and preview](../docs-site/README.md)
- [Audio integration suites](../server/tests/e2e/README.md)
