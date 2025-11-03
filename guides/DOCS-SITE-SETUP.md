# Documentation Site Setup Guide

## Quick Start

### 1. Install Dependencies

Run the setup script from the project root:

```powershell
.\setup-docs-site.ps1
```

Or manually:

```bash
cd docs-site
npm install
```

### 2. Copy Required Assets

The setup script automatically copies these files, but you can do it manually:

```bash
# From project root
mkdir -p docs-site/public/assets
cp assets/icons/SVG/liveplay-icon-darkmode@web.svg docs-site/public/assets/logo.svg
cp README.md docs-site/public/README.md
cp package.json docs-site/public/package.json
```

### 3. Run Development Server

```bash
cd docs-site
npm run dev
```

Visit: http://localhost:3000

### 4. Build for Production

```bash
cd docs-site
npm run generate
```

Output: `docs-site/.output/public/`

---

## GitHub Pages Deployment

### Enable GitHub Pages

1. Go to your repository on GitHub
2. Click **Settings** → **Pages**
3. Under **Source**, select **GitHub Actions**
4. Save

### Automatic Deployment

The site will automatically deploy when you push changes to:
- `docs-site/**` (any file in the docs site)
- `README.md` (updates documentation)
- `package.json` (updates version)

### Manual Deployment

You can also trigger deployment manually:
1. Go to **Actions** tab
2. Select **Deploy Docs Site** workflow
3. Click **Run workflow**

### View Your Site

After deployment, your site will be available at:
```
https://tdoukinitsas.github.io/liveplay/
```

---

## How It Works

### Version Detection

The site fetches version information from the deployed `package.json`:

```javascript
const packageRes = await fetch('/liveplay/package.json');
const packageData = await packageRes.json();
version.value = packageData.version;
```

### Download Links

Links are automatically constructed using the version:

```javascript
const downloadLinks = computed(() => {
  const baseUrl = `https://github.com/tdoukinitsas/liveplay/releases/download/v${version.value}`;
  return {
    windows: `${baseUrl}/LivePlay-Setup-${version.value}.exe`,
    mac: `${baseUrl}/LivePlay-${version.value}.dmg`,
    linux: `${baseUrl}/LivePlay-${version.value}.AppImage`,
    // ... more formats
  };
});
```

### README Rendering

The site fetches and parses the main README.md:

```javascript
const readmeRes = await fetch('/liveplay/README.md');
const readmeText = await readmeRes.text();
readmeHtml.value = parseMarkdown(readmeText);
```

---

## Customization

### Change Colors

Edit `docs-site/app.vue` and search for `#DA1E28` (current red):

```scss
h1 {
  color: #DA1E28;  // Change this
}

.download-button {
  background: #DA1E28;  // And this
}
```

### Update Logo

Replace the logo at:
```
docs-site/public/assets/logo.svg
```

Or update the copy script in `.github/workflows/deploy-docs.yml`

### Modify Layout

The entire page is in `docs-site/app.vue`. Key sections:

- **Header** (`.site-header`) - Logo, title, version
- **Downloads** (`.download-section`) - Platform cards
- **README** (`.readme-section`) - Parsed documentation
- **Footer** (`.site-footer`) - Links and credits

### Add New Sections

Add new sections in `app.vue`:

```vue
<section class="my-section">
  <div class="container">
    <h2>My Section Title</h2>
    <p>Content here...</p>
  </div>
</section>
```

---

## Troubleshooting

### Site Not Loading After Deployment

**Check:**
1. GitHub Pages is enabled (Settings → Pages)
2. Workflow completed successfully (Actions tab)
3. Source is set to "GitHub Actions"
4. Wait 2-3 minutes for GitHub CDN to update

### 404 Errors on Assets

**Problem:** Assets like logo or README not found

**Solution:**
- Check that files exist in `docs-site/public/`
- Verify workflow copied files correctly
- Check browser console for exact 404 paths

### Version Shows as 1.1.3

**Problem:** Old version displayed despite updating package.json

**Solutions:**
1. Clear browser cache (Ctrl+Shift+R)
2. Check `/liveplay/package.json` was deployed
3. Verify GitHub Pages updated (may take 2-3 minutes)

### Download Links 404

**Problem:** Download links return 404

**Causes:**
1. Release not published yet (run release workflow first)
2. Asset names don't match expected format
3. Version in package.json doesn't match release tag

**Solution:**
- Ensure release exists at: `https://github.com/tdoukinitsas/liveplay/releases`
- Check asset names match pattern: `LivePlay-Setup-{version}.exe`
- Update `downloadLinks` in `app.vue` if names differ

### Workflow Fails on Deploy

**Check workflow logs:**
1. Go to Actions tab
2. Click failed workflow run
3. Expand failed step to see error

**Common issues:**
- Missing dependencies (check package.json)
- Build errors (test locally with `npm run generate`)
- Permissions (check Pages settings)

---

## Development Workflow

### Testing Changes Locally

```bash
# 1. Make changes to docs-site/app.vue or other files
# 2. Copy updated assets if needed
cp README.md docs-site/public/README.md
cp package.json docs-site/public/package.json

# 3. Run dev server
cd docs-site
npm run dev

# 4. View at http://localhost:3000
```

### Testing Production Build

```bash
cd docs-site
npm run generate
npm run preview
```

### Deployment Process

```bash
# 1. Update version in main package.json
# 2. Commit changes
git add .
git commit -m "Update docs site"

# 3. Push to GitHub
git push origin main

# 4. Workflow automatically triggers
# 5. Wait 2-3 minutes
# 6. Check https://tdoukinitsas.github.io/liveplay/
```

---

## File Structure

```
liveplay/
├── docs-site/                    # Documentation site
│   ├── app.vue                   # Main page component
│   ├── nuxt.config.ts            # Nuxt configuration
│   ├── package.json              # Site dependencies
│   ├── README.md                 # Site documentation
│   ├── .gitignore                # Git ignore
│   ├── assets/
│   │   └── styles/
│   │       └── main.scss         # Global styles
│   └── public/                   # Static assets (deployed)
│       ├── package.json          # Version info (copied)
│       ├── README.md             # Docs content (copied)
│       └── assets/
│           └── logo.svg          # Logo (copied)
│
├── .github/workflows/
│   └── deploy-docs.yml           # Deployment workflow
│
└── setup-docs-site.ps1           # Setup script
```

---

## Architecture

### Static Site Generation

Nuxt generates a fully static site with no server required:

1. **Build time:** HTML is pre-rendered
2. **Client side:** JavaScript fetches version/README
3. **No API calls:** Everything served from GitHub Pages CDN

### Base URL Configuration

The site uses `/liveplay/` as base URL to work with GitHub Pages:

```typescript
// nuxt.config.ts
app: {
  baseURL: '/liveplay/'
}
```

All asset paths are relative to this base URL.

### Markdown Parsing

Simple client-side markdown parser handles:
- Headers (h1, h2, h3)
- Bold, italic
- Links
- Code blocks, inline code
- Lists
- Horizontal rules

For advanced markdown features, consider adding a library like `marked` or `markdown-it`.

---

## Performance

### Optimizations

- **Static generation:** No server-side rendering overhead
- **Minimal dependencies:** Only Nuxt + Vue
- **CDN delivery:** GitHub Pages uses CloudFlare CDN
- **Lazy loading:** Images load on-demand
- **CSS scoped:** Styles are component-scoped

### Bundle Size

Approximate sizes:
- HTML: ~15 KB
- CSS: ~8 KB
- JS: ~50 KB (Nuxt runtime)
- **Total:** ~75 KB + assets

---

## License

Part of the LivePlay project - AGPL-3.0-only
