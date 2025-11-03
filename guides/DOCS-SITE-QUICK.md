# 📄 Docs Site Quick Reference

## 🚀 Quick Start

```bash
# Setup (run once)
.\setup-docs-site.ps1

# Development
cd docs-site
npm run dev

# Build
npm run generate
```

## 📋 Commands

| Command | Action |
|---------|--------|
| `npm run dev` | Start dev server (http://localhost:3000) |
| `npm run build` | Generate static site |
| `npm run generate` | Generate static site (alias) |
| `npm run preview` | Preview production build |

## 📂 Important Files

| File | Purpose |
|------|---------|
| `docs-site/app.vue` | Main page component |
| `docs-site/nuxt.config.ts` | Configuration |
| `docs-site/public/package.json` | Version info (auto-copied) |
| `docs-site/public/README.md` | Content source (auto-copied) |
| `docs-site/public/assets/logo.svg` | Logo (auto-copied) |
| `.github/workflows/deploy-docs.yml` | Deployment workflow |

## 🔧 Manual Asset Copy

```bash
# From project root
cp README.md docs-site/public/README.md
cp package.json docs-site/public/package.json
cp assets/icons/SVG/liveplay-icon-darkmode@web.svg docs-site/public/assets/logo.svg
```

## 🌐 URLs

| Environment | URL |
|-------------|-----|
| Local Dev | http://localhost:3000 |
| Production | https://tdoukinitsas.github.io/liveplay/ |

## 🎨 Customization

### Change Primary Color

Find `#DA1E28` in `docs-site/app.vue` and replace with your color.

### Update Logo

Replace: `docs-site/public/assets/logo.svg`

### Modify Layout

Edit sections in `docs-site/app.vue`:
- `.site-header` - Header with logo/title
- `.download-section` - Download cards
- `.readme-section` - README content
- `.site-footer` - Footer links

## 🚢 Deployment

### Auto-Deploy Triggers
- Push to `docs-site/**`
- Push to `README.md`
- Push to `package.json`

### Manual Deploy
1. Go to **Actions** tab on GitHub
2. Select **Deploy Docs Site**
3. Click **Run workflow**

### Enable GitHub Pages
1. Settings → Pages
2. Source: **GitHub Actions**
3. Save

## 🐛 Troubleshooting

| Problem | Solution |
|---------|----------|
| Assets 404 | Copy files to `public/` directory |
| Old version shows | Clear cache (Ctrl+Shift+R) |
| Download links 404 | Ensure release exists on GitHub |
| Workflow fails | Check Actions logs, test `npm run generate` locally |

## 📊 Download Link Format

```
https://github.com/tdoukinitsas/liveplay/releases/download/v{version}/[filename]
```

**Filenames:**
- Windows: `LivePlay-Setup-{version}.exe`
- macOS DMG: `LivePlay-{version}.dmg`
- macOS ZIP: `LivePlay-{version}-mac.zip`
- Linux AppImage: `LivePlay-{version}.AppImage`
- Linux Deb: `liveplay_{version}_amd64.deb`
- Linux RPM: `liveplay-{version}.x86_64.rpm`

## 📝 Update Workflow

1. Make changes to docs site
2. Commit: `git add . && git commit -m "Update docs"`
3. Push: `git push origin main`
4. Wait 2-3 minutes
5. Check: https://tdoukinitsas.github.io/liveplay/

---

**Full Documentation:** See [DOCS-SITE-SETUP.md](./DOCS-SITE-SETUP.md)
