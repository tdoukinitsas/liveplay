# Quick Release Guide

## TL;DR - Release in 3 Steps

### 1️⃣ Update Version
```json
// package.json
{
  "version": "1.2.0"  // Bump this number
}
```

### 2️⃣ Commit & Push
```bash
git add package.json
git commit -m "Release v1.2.0"
git push origin main
```

### 3️⃣ Wait & Download
- Watch: https://github.com/tdoukinitsas/liveplay/actions
- Download: https://github.com/tdoukinitsas/liveplay/releases

---

## Version Numbering

```
MAJOR.MINOR.PATCH
  │     │     │
  │     │     └─ Bug fixes (1.1.1 → 1.1.2)
  │     └─────── New features (1.1.0 → 1.2.0)
  └───────────── Breaking changes (1.0.0 → 2.0.0)
```

## What Gets Built

| Platform | Formats | Architecture |
|----------|---------|--------------|
| 🪟 Windows | `.exe` (NSIS installer) | x64 |
| 🐧 Linux | `.AppImage`, `.deb`, `.rpm` | x64 |
| 🍎 macOS | `.dmg`, `.zip` | x64 + ARM64 (Universal) |

## Workflow Status

✅ **Success** - Release published automatically  
⏳ **In Progress** - Building (~15-30 minutes)  
❌ **Failed** - Check Actions tab for errors  
⏭️ **Skipped** - Version number didn't change  

## Common Commands

```bash
# Test build locally (your platform only)
npm run build
npm run build:electron

# Test specific platform build
npm run electron:build -- --win --x64     # Windows
npm run electron:build -- --linux --x64   # Linux
npm run electron:build -- --mac --x64     # macOS

# Check current version
node -p "require('./package.json').version"

# View recent commits (for changelog preview)
git log --oneline -10
```

## Troubleshooting Quick Fixes

### ❌ Version not detected
```bash
# Make sure you're on main branch
git checkout main

# Verify version actually changed
git diff HEAD^ HEAD -- package.json
```

### ❌ Build failed
1. Check Actions tab → Click failed job
2. Read error message in logs
3. Fix issue and push again

### ❌ Release not created
- Ensure all 3 platform builds succeeded
- Check repository permissions in Settings

## File Locations

```
📁 liveplay/
├── 📄 package.json              # Version number here
├── 📁 .github/workflows/
│   └── 📄 build-release.yml     # CI/CD workflow
├── 📁 dist-electron/            # Build output (gitignored)
├── 📄 RELEASES.md               # Full release documentation
└── 📄 RELEASE-QUICK.md          # This file
```

## Next Steps After Release

1. ✅ Test downloads from GitHub releases
2. ✅ Update app update URLs (if applicable)
3. ✅ Announce on social media / Discord / etc.
4. ✅ Monitor for user-reported issues

---

**Need more details?** See [RELEASES.md](./RELEASES.md)
