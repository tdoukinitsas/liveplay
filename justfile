# LivePlay development recipes

# List available recipes
default:
    @just --list

# Install dependencies
install:
    npm install

# Ensure node_modules are installed
_ensure-deps:
    @[ -d node_modules ] || npm install

# Start dev mode (Nuxt + Electron)
dev: _ensure-deps
    npx concurrently "npm run dev:nuxt" "npm run dev:electron"

# Start dev mode with a project auto-opened and CDP debugging enabled
dev-debug project_path="/Users/steve/Documents/test-liveplay.liveplay" cdp_port="9222": _ensure-deps
    npx concurrently \
        "npm run dev:nuxt" \
        "wait-on http://localhost:3000 && LIVEPLAY_PROJECT={{project_path}} npx electron . --remote-debugging-port={{cdp_port}}"

# Start only the Nuxt dev server
dev-nuxt:
    npm run dev:nuxt

# Start only Electron (requires Nuxt already running)
dev-electron:
    npm run dev:electron

# Build Nuxt static output (production)
build:
    npm run build

# Build distributable Electron app
build-electron:
    npm run build:electron

# Generate Nuxt static output
generate:
    npm run generate

# Preview Nuxt build
preview:
    npm run preview
