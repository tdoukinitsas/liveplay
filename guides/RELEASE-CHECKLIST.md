# 🚀 CI/CD Deployment Checklist

Use this checklist when creating a new release.

## Pre-Release Checklist

### 1. Code Quality ✅
- [ ] All features tested locally
- [ ] No critical bugs in issue tracker
- [ ] Code builds successfully (`npm run build`)
- [ ] No console errors in dev mode

### 2. Version Bump 📝
- [ ] Update `version` in `package.json`
- [ ] Follow semantic versioning (MAJOR.MINOR.PATCH)
- [ ] Version number is correct format (e.g., 1.2.0)

### 3. Git Hygiene 🧹
- [ ] All changes committed
- [ ] Working directory clean (`git status`)
- [ ] On `main` branch
- [ ] Latest changes pulled (`git pull`)

### 4. Local Testing 🧪
- [ ] Run `npm run build` successfully
- [ ] Run `npm run electron:build` if possible
- [ ] Check `dist-electron/` output

## Release Process

### Step 1: Commit Version Bump
```bash
git add package.json
git commit -m "Release v1.2.0"
git push origin main
```

### Step 2: Monitor Build
- [ ] Go to [Actions tab](https://github.com/tdoukinitsas/liveplay/actions)
- [ ] Find "Build and Release" workflow run
- [ ] Verify all 3 platform builds start
- [ ] Wait for completion (~15-30 minutes)

### Step 3: Verify Builds

#### Windows Build ✅
- [ ] Job completed successfully
- [ ] Artifacts uploaded (2 files)
- [ ] No error messages in logs

#### Linux Build ✅
- [ ] Job completed successfully
- [ ] Artifacts uploaded (3 files)
- [ ] No error messages in logs

#### macOS Build ✅
- [ ] Job completed successfully
- [ ] Artifacts uploaded (3 files)
- [ ] No error messages in logs

### Step 4: Verify Release

- [ ] Go to [Releases](https://github.com/tdoukinitsas/liveplay/releases)
- [ ] New release created with correct version tag
- [ ] All 7+ files attached to release:

#### Required Files
- [ ] `LivePlay-Setup-{version}.exe` (Windows installer)
- [ ] `LivePlay-Setup-{version}.exe.blockmap` (Update metadata)
- [ ] `LivePlay-{version}.AppImage` (Linux universal)
- [ ] `LivePlay_{version}_amd64.deb` (Debian/Ubuntu)
- [ ] `LivePlay-{version}.x86_64.rpm` (Fedora/RHEL)
- [ ] `LivePlay-{version}.dmg` (macOS installer)
- [ ] `LivePlay-{version}-mac.zip` (macOS portable)

#### Optional Files
- [ ] `LivePlay-{version}.dmg.blockmap` (macOS update metadata)
- [ ] `latest-linux.yml` (Linux auto-update)
- [ ] `latest-mac.yml` (macOS auto-update)
- [ ] `latest.yml` (Windows auto-update)

### Step 5: Verify Changelog

- [ ] Changelog auto-generated correctly
- [ ] Commit messages readable and descriptive
- [ ] Statistics included (commits, contributors, languages)
- [ ] Full changelog link present

## Post-Release Checklist

### Testing Downloads 🧪

#### Windows
- [ ] Download .exe installer
- [ ] Install on Windows machine
- [ ] Launch application
- [ ] Verify version in About dialog
- [ ] Test basic functionality

#### Linux
- [ ] Download .AppImage/.deb/.rpm
- [ ] Install on Linux machine
- [ ] Launch application
- [ ] Verify version
- [ ] Test basic functionality

#### macOS
- [ ] Download .dmg
- [ ] Install on macOS machine
- [ ] Launch application (check for security warnings)
- [ ] Verify version
- [ ] Test basic functionality

### Communication 📢
- [ ] Update project website (if applicable)
- [ ] Post announcement on social media
- [ ] Notify Discord/Slack community (if applicable)
- [ ] Update documentation site (if applicable)
- [ ] Email notification to mailing list (if applicable)

### Monitoring 📊
- [ ] Monitor GitHub issues for new bug reports
- [ ] Check download statistics
- [ ] Review user feedback
- [ ] Monitor crash reports (if tracking enabled)

## Rollback Plan 🔄

If critical issues are discovered:

### Option 1: Quick Fix
1. Fix the bug locally
2. Bump version to patch release (e.g., 1.2.0 → 1.2.1)
3. Commit and push
4. Follow release process again

### Option 2: Remove Release
1. Go to GitHub Releases
2. Delete the problematic release
3. Delete the git tag: `git push --delete origin v1.2.0`
4. Fix issues locally
5. Re-release with same or new version

### Option 3: Mark as Pre-release
1. Edit the GitHub release
2. Check "This is a pre-release"
3. Add warning in release notes
4. Prepare fixed version

## Common Issues & Solutions

### ❌ Build fails on specific platform
**Solution:** Check Actions logs → Fix issue → Push fix → Release will retry

### ❌ Changelog looks messy
**Solution:** Use better commit messages next time (see: Conventional Commits)

### ❌ Wrong version number released
**Solution:** Delete release, update version, re-release

### ❌ Missing files in release
**Solution:** Check build logs for errors → Verify `dist-electron/` contents

### ❌ macOS "unverified developer" warning
**Solution:** (For users) Right-click → Open → Confirm
**Long-term:** Set up code signing and notarization

### ❌ Auto-update not working
**Solution:** Ensure `latest*.yml` files are in release assets

## Version History Template

Keep a `CHANGELOG.md` manually for major changes:

```markdown
## [1.2.0] - 2025-01-XX

### Added
- New cart player feature
- Remote API endpoints
- German language support

### Changed
- Improved waveform rendering performance
- Updated dependencies

### Fixed
- Fixed audio playback on macOS
- Fixed playlist sorting bug

### Security
- Updated vulnerable dependencies
```

## Notes

- Build time: ~15-30 minutes (parallel builds)
- Artifact retention: 5 days in Actions storage
- Release files: Permanent in GitHub Releases
- Workflow triggers: Only on main branch, only on package.json changes

## Support

- 📖 Full docs: [RELEASES.md](./RELEASES.md)
- ⚡ Quick ref: [RELEASE-QUICK.md](./RELEASE-QUICK.md)
- 🛠️ Setup info: [CI-CD-SETUP.md](./CI-CD-SETUP.md)
- 🐛 Issues: https://github.com/tdoukinitsas/liveplay/issues

---

**Remember:** Test locally before releasing! 🧪

**Tip:** Keep commit messages clear for better changelogs! 📝
