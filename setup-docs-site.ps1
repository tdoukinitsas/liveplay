# LivePlay Documentation Site Setup

Write-Host "Setting up LivePlay documentation site..." -ForegroundColor Cyan

# Check if we're in the right directory
if (-not (Test-Path "docs-site")) {
    Write-Host "Error: docs-site directory not found. Please run from the project root." -ForegroundColor Red
    exit 1
}

# Navigate to docs-site
Set-Location docs-site

# Install dependencies
Write-Host "`nInstalling dependencies..." -ForegroundColor Yellow
npm install

# Create public directory structure
Write-Host "`nSetting up public assets..." -ForegroundColor Yellow
if (-not (Test-Path "public")) {
    New-Item -ItemType Directory -Path "public" -Force | Out-Null
}
if (-not (Test-Path "public/assets")) {
    New-Item -ItemType Directory -Path "public/assets" -Force | Out-Null
}

# Copy assets from parent directory
Write-Host "Copying assets from parent directory..." -ForegroundColor Yellow

# Copy logo
if (Test-Path "../assets/icons/SVG/liveplay-icon-darkmode@web.svg") {
    Copy-Item "../assets/icons/SVG/liveplay-icon-darkmode@web.svg" "public/assets/logo.svg" -Force
    Write-Host "✓ Logo copied" -ForegroundColor Green
} else {
    Write-Host "⚠ Logo not found at ../assets/icons/SVG/liveplay-icon-darkmode@web.svg" -ForegroundColor Yellow
}

# Copy README
if (Test-Path "../README.md") {
    Copy-Item "../README.md" "public/README.md" -Force
    Write-Host "✓ README copied" -ForegroundColor Green
} else {
    Write-Host "⚠ README.md not found" -ForegroundColor Yellow
}

# Copy package.json
if (Test-Path "../package.json") {
    Copy-Item "../package.json" "public/package.json" -Force
    Write-Host "✓ package.json copied" -ForegroundColor Green
} else {
    Write-Host "⚠ package.json not found" -ForegroundColor Yellow
}

Write-Host "`n✓ Setup complete!" -ForegroundColor Green
Write-Host "`nTo start the development server:" -ForegroundColor Cyan
Write-Host "  cd docs-site" -ForegroundColor White
Write-Host "  npm run dev" -ForegroundColor White
Write-Host "`nTo build for production:" -ForegroundColor Cyan
Write-Host "  cd docs-site" -ForegroundColor White
Write-Host "  npm run generate" -ForegroundColor White

Set-Location ..
