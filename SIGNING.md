# Packaging and signing

This describes the checked-in build configuration. It does not attest to the signature of a particular downloaded artifact.

| Platform | Current configuration |
|---|---|
| Windows | electron-builder creates an x64 NSIS installer. No certificate or SignPath signing step is configured in the release workflow. |
| macOS | electron-builder sets `mac.identity` to `null`. Builds produce Intel and Apple Silicon DMG/ZIP packages without configured Developer ID signing or notarization. |
| Linux | electron-builder produces x64 AppImage, Debian and RPM packages. |

Sources: [client package configuration](client/package.json), [release workflow](.github/workflows/build-release.yml), and [macOS server bundle helper](scripts/build-server-app-mac.js).

Changing signing credentials, notarization or release workflow steps is release engineering work. The current workflow contains no ready-to-enable SignPath integration.

For installation guidance, see [the README](README.md#install).
