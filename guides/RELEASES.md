# Release Process Documentation

## Automated CI/CD Pipeline

LivePlay uses GitHub Actions for automated building and releasing of the application across Windows, Linux, and macOS platforms.

## Overview

The CI/CD workflow automatically:
- ✅ Detects version changes in `package.json`
- ✅ Builds for Windows (x64), Linux (x64), and macOS (x64 + ARM64)
- ✅ Generates multiple package formats per platform
- ✅ Creates auto-generated changelogs from git commits
- ✅ Publishes GitHub releases with all binaries

## Build Outputs

### Windows
- `LivePlay-Setup-{version}.exe` - NSIS installer (x64)
- `LivePlay-Setup-{version}.exe.blockmap` - Update metadata

### Linux
- `LivePlay-{version}.AppImage` - Universal Linux package (x64)
- `LivePlay_{version}_amd64.deb` - Debian/Ubuntu package (x64)
- `LivePlay-{version}.x86_64.rpm` - Fedora/RHEL package (x64)

### macOS
- `LivePlay-{version}.dmg` - Disk image installer (Universal: x64 + ARM64)
- `LivePlay-{version}.dmg.blockmap` - Update metadata
- `LivePlay-{version}-mac.zip` - Portable archive (Universal: x64 + ARM64)

## How to Create a Release

### 1. Update Version Number

Edit `package.json` and bump the version number following [Semantic Versioning](https://semver.org/):

```json
{
  "version": "1.2.0"  // Changed from 1.1.3
}
```

**Version Guidelines:**
- **MAJOR** (1.0.0 → 2.0.0): Breaking changes, incompatible API changes
- **MINOR** (1.1.0 → 1.2.0): New features, backward-compatible
- **PATCH** (1.1.1 → 1.1.2): Bug fixes, backward-compatible

### 2. Commit and Push

```bash
git add package.json
git commit -m "Bump version to 1.2.0"
git push origin main
```

### 3. Watch the Build

1. Go to the **Actions** tab in your GitHub repository
2. Find the "Build and Release" workflow run
3. Monitor the progress of all three platform builds
4. Typical build time: 15-30 minutes (runs in parallel)

### 4. Release Published

Once complete, the release will be automatically published at:
```
https://github.com/tdoukinitsas/liveplay/releases
```

## Workflow Architecture

### Jobs Overview

```
┌─────────────────┐
│ check-version   │ Detects version changes
└────────┬────────┘
         │
         ├──────────────────────────────┐
         │                              │
         v                              v
┌────────────────┐              ┌──────────────┐
│ build          │              │ build        │
│ (Windows)      │              │ (Linux)      │
└────────┬───────┘              └──────┬───────┘
         │                              │
         v                              v
┌────────────────┐              ┌──────────────┐
│ build          │              │ Upload       │
│ (macOS)        │              │ Artifacts    │
└────────┬───────┘              └──────┬───────┘
         │                              │
         └──────────────┬───────────────┘
                        v
                ┌───────────────┐
                │ release       │ Creates GitHub release
                └───────────────┘
```

### Job 1: check-version
- **Runs on:** Ubuntu
- **Purpose:** Detects if package.json version changed
- **Outputs:** 
  - `version`: The new version (e.g., v1.2.0)
  - `version-changed`: true/false flag

### Job 2: build (Matrix Strategy)
- **Runs on:** Windows, Ubuntu, macOS (parallel)
- **Purpose:** Build platform-specific binaries
- **Steps:**
  1. Checkout code
  2. Setup Node.js 20
  3. Install dependencies (`npm ci`)
  4. Build Nuxt app (`npm run build`)
  5. Build Electron app with platform flags
  6. Upload artifacts to GitHub
- **Only runs if:** Version changed in Job 1

### Job 3: release
- **Runs on:** Ubuntu
- **Purpose:** Create GitHub release with all binaries
- **Steps:**
  1. Download all platform artifacts
  2. Generate changelog from git commits
  3. Create GitHub release with tag
  4. Upload all binaries to release

## Changelog Generation

The workflow automatically generates a changelog from git commits since the last release:

```markdown
## What's Changed

### 🎉 Highlights
- [Auto-generated commit messages]

### 📊 Statistics
- **X commits** since last release
- **Y contributors**
- **20 languages** supported

### 🔗 Full Changelog
[Compare link to previous version]
```

**Best Practices for Commit Messages:**
- Use clear, descriptive commit messages
- Start with action verbs (Add, Fix, Update, Remove)
- Examples:
  - ✅ `Add German language support`
  - ✅ `Fix audio playback bug on Windows`
  - ✅ `Update dependencies to latest versions`
  - ❌ `wip`, `fix stuff`, `updates`

## Troubleshooting

### Build Fails on Specific Platform

**Check the GitHub Actions logs:**
1. Go to Actions tab → Failed workflow
2. Click on the failing job (Windows/Linux/macOS)
3. Expand the failed step to see error details

**Common issues:**
- Missing dependencies in `package.json`
- Icon file path issues
- Code signing configuration (macOS/Windows)

### Version Not Detected

**Possible causes:**
1. Version number wasn't actually changed in `package.json`
2. Changes pushed to a branch other than `main`
3. `package.json` not included in the commit

**Solution:**
```bash
# Verify version changed
git diff HEAD^ HEAD -- package.json

# Ensure on main branch
git branch

# Push to main
git push origin main
```

### Release Not Created

**Check:**
1. Build jobs completed successfully
2. Repository has sufficient permissions
3. GITHUB_TOKEN has write access (should be automatic)

### Artifacts Missing from Release

**Verify:**
1. All three build jobs completed
2. Artifact upload steps succeeded
3. Check `dist-electron/` directory contents in build logs

## Local Testing

Before pushing, you can test builds locally:

### Test Windows Build
```powershell
npm run build
npm run electron:build -- --win --x64
```

### Test Linux Build (on Linux/WSL)
```bash
npm run build
npm run electron:build -- --linux --x64
```

### Test macOS Build (on macOS)
```bash
npm run build
npm run electron:build -- --mac --x64 --arm64
```

**Note:** Cross-platform building may require additional configuration.

## Manual Release (Emergency)

If automated workflow fails, you can create a release manually:

1. Build locally for your platform
2. Go to GitHub → Releases → Draft a new release
3. Create a new tag (e.g., v1.2.0)
4. Upload binaries from `dist-electron/` directory
5. Write release notes manually
6. Publish release

## Future Enhancements

Planned improvements for the CI/CD pipeline:

- [ ] Code signing for Windows executables
- [ ] Notarization for macOS applications
- [ ] Auto-update functionality in the app
- [ ] Draft releases for pre-release testing
- [ ] Build caching for faster subsequent builds
- [ ] Automated testing before release
- [ ] Beta/nightly build channels

## Configuration Files

### GitHub Actions Workflow
- **Location:** `.github/workflows/build-release.yml`
- **Trigger:** Push to `main` branch with `package.json` changes
- **Permissions:** Contents: write (for creating releases)

### Electron Builder Config
- **Location:** `package.json` → `build` section
- **Output:** `dist-electron/` directory
- **Publisher:** GitHub (owner: tdoukinitsas, repo: liveplay)

### Build Scripts
- `npm run build` - Build Nuxt application
- `npm run electron:build` - Build Electron wrapper
- `npm run build:electron` - Build both in sequence

## Support

If you encounter issues with the release process:

1. Check GitHub Actions logs for detailed error messages
2. Review this documentation for troubleshooting steps
3. Check electron-builder documentation: https://www.electron.build/
4. Open an issue on GitHub with workflow run link

## Version History

- **v1.1.3** - Current version (20 languages, RTL support)
- **v1.1.x** - Initial multi-language support
- **v1.0.x** - Initial release

---

**Last Updated:** January 2025
**Maintained by:** Thomas Doukinitsas
