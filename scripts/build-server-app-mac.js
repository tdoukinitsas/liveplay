#!/usr/bin/env node
/**
 * Builds a macOS .app bundle for the LivePlay Server.
 * This creates the proper application structure that can be bundled in the DMG.
 * Only runs on macOS; no-ops silently on other platforms.
 */

const path = require('path');
const fs = require('fs');
const { execSync } = require('child_process');

// Only run on macOS
if (process.platform !== 'darwin') {
  console.log('[build-server-app-mac] Skipping on non-macOS platform');
  process.exit(0);
}

const projectRoot = path.join(__dirname, '..');
const serverBinaryPath = path.join(projectRoot, 'server', 'build', 'liveplay-server');
const serverIconPath = path.join(projectRoot, 'client', 'assets', 'icons', '4092w', 'liveplay-server4092w.png');
const appBundlePath = path.join(projectRoot, 'server', 'build', 'LivePlay Server.app');
const contentsPath = path.join(appBundlePath, 'Contents');
const macOsPath = path.join(contentsPath, 'MacOS');
const resourcesPath = path.join(contentsPath, 'Resources');

console.log('[build-server-app-mac] Building LivePlay Server.app bundle...');
console.log('[build-server-app-mac] Source binary:', serverBinaryPath);
console.log('[build-server-app-mac] Target bundle:', appBundlePath);

try {
  // Check if source binary exists
  if (!fs.existsSync(serverBinaryPath)) {
    console.error(`[build-server-app-mac] ERROR: Server binary not found at ${serverBinaryPath}`);
    console.error('[build-server-app-mac] Make sure to run "npm run server:build" first');
    process.exit(1);
  }

  // Check if icon exists
  if (!fs.existsSync(serverIconPath)) {
    console.error(`[build-server-app-mac] ERROR: Server icon not found at ${serverIconPath}`);
    process.exit(1);
  }

  // Remove existing bundle if present
  if (fs.existsSync(appBundlePath)) {
    console.log('[build-server-app-mac] Removing existing bundle...');
    execSync(`rm -rf "${appBundlePath}"`, { stdio: 'inherit' });
  }

  // Create directory structure
  console.log('[build-server-app-mac] Creating directory structure...');
  fs.mkdirSync(macOsPath, { recursive: true });
  fs.mkdirSync(resourcesPath, { recursive: true });

  // Copy server binary to MacOS/
  console.log('[build-server-app-mac] Copying server binary...');
  fs.copyFileSync(serverBinaryPath, path.join(macOsPath, 'liveplay-server'));
  execSync(`chmod +x "${path.join(macOsPath, 'liveplay-server')}"`, { stdio: 'inherit' });

  // Create the launcher shell script.
  // When the user double-clicks the .app, macOS runs this script rather than
  // the server binary directly. The script tells Terminal.app to open a new
  // window and run the binary there, then exits immediately. This gives the
  // user a visible, interactive terminal showing the startup banner, logs,
  // and the Ctrl-C prompt — identical to the Electron-spawned experience.
  console.log('[build-server-app-mac] Creating launcher script...');
  const launcherScript = `#!/bin/bash
# LivePlay Server launcher — opens a visible Terminal window running the server.
BINARY_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$BINARY_DIR/liveplay-server"
# Shell-quote the path for AppleScript in case it contains spaces.
QUOTED="'$(echo "$BINARY" | sed "s/'/'\\\\''/g")'"
osascript \\
  -e 'tell application "Terminal"' \\
  -e "  do script $QUOTED" \\
  -e '  activate' \\
  -e 'end tell'
`;
  const launcherPath = path.join(macOsPath, 'launcher');
  fs.writeFileSync(launcherPath, launcherScript, { mode: 0o755 });

  // Create Info.plist
  // CFBundleExecutable points to the launcher script, not the binary.
  // LSUIElement keeps the launcher itself out of the Dock — it exits in under
  // a second after handing off to Terminal, so a Dock icon would just flicker.
  console.log('[build-server-app-mac] Creating Info.plist...');
  const infoPlist = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key>
  <string>com.liveplay.server</string>
  <key>CFBundleName</key>
  <string>LivePlay Server</string>
  <key>CFBundleDisplayName</key>
  <string>LivePlay Server</string>
  <key>CFBundleExecutable</key>
  <string>launcher</string>
  <key>CFBundleIconFile</key>
  <string>liveplay-server</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleVersion</key>
  <string>2.0.0</string>
  <key>CFBundleShortVersionString</key>
  <string>2.0.0</string>
  <key>LSUIElement</key>
  <true/>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>`;

  fs.writeFileSync(path.join(contentsPath, 'Info.plist'), infoPlist);

  // Convert PNG icon to ICNS
  console.log('[build-server-app-mac] Converting icon to ICNS format...');
  const tempIconDir = path.join(projectRoot, 'server', 'build', 'liveplay-server.iconset');
  const outputIconPath = path.join(resourcesPath, 'liveplay-server.icns');

  // Create iconset directory
  if (fs.existsSync(tempIconDir)) {
    execSync(`rm -rf "${tempIconDir}"`, { stdio: 'inherit' });
  }
  fs.mkdirSync(tempIconDir, { recursive: true });

  // Generate icon sizes using sips
  const iconSizes = [16, 32, 64, 128, 256, 512];
  for (const size of iconSizes) {
    const outputSize = `${size}x${size}`;
    const outputFile = path.join(tempIconDir, `icon_${outputSize}.png`);
    console.log(`[build-server-app-mac] Creating ${outputSize} icon...`);
    execSync(`sips -z ${size} ${size} "${serverIconPath}" --out "${outputFile}"`, { stdio: 'inherit' });

    // For retina versions (2x)
    if (size <= 256) {
      const retinaSizeInt = size * 2;
      const retinaOutputSize = `${retinaSizeInt}x${retinaSizeInt}`;
      const retinaOutputFile = path.join(tempIconDir, `icon_${outputSize}@2x.png`);
      console.log(`[build-server-app-mac] Creating ${retinaOutputSize} icon (retina)...`);
      execSync(`sips -z ${retinaSizeInt} ${retinaSizeInt} "${serverIconPath}" --out "${retinaOutputFile}"`, { stdio: 'inherit' });
    }
  }

  // Convert iconset to ICNS
  console.log('[build-server-app-mac] Converting iconset to ICNS...');
  execSync(`iconutil -c icns -o "${outputIconPath}" "${tempIconDir}"`, { stdio: 'inherit' });

  // Clean up temporary iconset directory
  execSync(`rm -rf "${tempIconDir}"`, { stdio: 'inherit' });

  // Create PkgInfo file (optional but good practice)
  fs.writeFileSync(path.join(contentsPath, 'PkgInfo'), 'APPLcom.liveplay.server');

  console.log('[build-server-app-mac] ✓ LivePlay Server.app bundle created successfully');
  console.log(`[build-server-app-mac] Bundle location: ${appBundlePath}`);
} catch (error) {
  console.error('[build-server-app-mac] ERROR: Failed to build server app bundle');
  console.error(error.message);
  process.exit(1);
}
