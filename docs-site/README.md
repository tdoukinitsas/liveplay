# LivePlay Documentation Site

This is the static documentation and download site for LivePlay, built with Nuxt 3.

## Features

- **Dynamic Version Detection**: Automatically fetches version from parent `package.json`
- **Download Links**: Direct links to all platform installers (Windows, macOS, Linux)
- **README Integration**: Displays main project README with markdown parsing
- **Responsive Design**: Mobile-friendly layout
- **Auto-deployment**: GitHub Actions automatically builds and deploys to GitHub Pages

## Development

```bash
# Install dependencies
cd docs-site
npm install

# Start development server
npm run dev
```

Visit `http://localhost:3000`

## Build

```bash
# Generate static site
npm run generate
```

Output will be in `.output/public/`

## Deployment

The site is automatically deployed to GitHub Pages when:
- Changes are pushed to `docs-site/` directory
- `README.md` is updated
- Parent `package.json` is updated (version change)

The site will be available at: `https://tdoukinitsas.github.io/liveplay/`

## How It Works

1. **Version Detection**: Fetches `/liveplay/package.json` and reads the version
2. **Download Links**: Constructs GitHub release URLs using the version
3. **README Parsing**: Fetches `/liveplay/README.md` and converts markdown to HTML
4. **Static Generation**: Nuxt generates a fully static site with no server required

## GitHub Pages Setup

To enable GitHub Pages for your repository:

1. Go to repository **Settings** → **Pages**
2. Under **Source**, select **GitHub Actions**
3. The workflow will automatically deploy on the next push

## File Structure

```
docs-site/
├── app.vue              # Main page component
├── nuxt.config.ts       # Nuxt configuration
├── package.json         # Dependencies
├── assets/
│   └── styles/
│       └── main.scss    # Global styles
└── public/              # Static assets (auto-copied from parent)
    ├── package.json     # Version info
    ├── README.md        # Documentation content
    └── assets/
        └── logo.svg     # LivePlay logo
```

## Customization

### Change Base URL

Edit `nuxt.config.ts`:

```typescript
app: {
  baseURL: '/your-repo-name/',  // Change this
}
```

### Update Colors

Edit `app.vue` styles section to change the color scheme. Current primary color is `#DA1E28` (red).

### Modify Download Links

The download links are automatically constructed from the version number. They follow this pattern:

```
https://github.com/tdoukinitsas/liveplay/releases/download/v{version}/LivePlay-Setup-{version}.exe
```

If your release asset names differ, update the `downloadLinks` computed property in `app.vue`.

## License

Part of the LivePlay project - AGPL-3.0-only
