# LivePlay Auto-Update System

## Overview

LivePlay includes a comprehensive auto-update system that automatically checks for new versions and allows users to download and install updates seamlessly without leaving the application.

## How It Works

### 1. Update Detection

When LivePlay starts in production mode, it automatically:
- Checks the GitHub repository for new releases (after a 3-second delay to allow the UI to load)
- Compares the current version with the latest published release
- Notifies the user if a new version is available

### 2. User Experience

When an update is available:
1. A modal dialog appears showing:
   - Current version
   - New version
   - Release notes (what's new)
   - Release date

2. User options:
   - **Download and Install**: Downloads the update immediately
   - **Later**: Dismisses the dialog (will check again on next launch)

3. During download:
   - Progress bar shows download percentage
   - User cannot close the dialog during download

4. After download completes:
   - **Install Now**: Quits the app and installs the update immediately
   - **Install on Exit**: Update will be installed when the user closes the app normally

## Technical Implementation

### Components

1. **electron-updater**: Handles the auto-update logic
2. **GitHub Releases**: Source for update packages
3. **UpdateModal.vue**: User interface for update notifications
4. **IPC Handlers**: Communication between main and renderer processes

### Files Modified

- `package.json`: Added electron-updater dependency and publish configuration
- `electron/main.js`: Auto-updater configuration and event handlers
- `electron/preload.js`: Exposed update APIs to renderer
- `types/global.d.ts`: TypeScript definitions for update APIs
- `components/UpdateModal.vue`: Update notification UI component
- `app.vue`: Integration of update modal and event listeners
- `locales/en.json` & `locales/el.json`: Translations for update messages

## Publishing Updates

To publish a new version that users can auto-update to:

### 1. Update Version Number

Edit `package.json`:
```json
{
  "version": "1.2.0"
}
```

### 2. Build the Application

```bash
npm run build:electron
```

This creates platform-specific installers in the `dist-electron` directory.

### 3. Create a GitHub Release

1. Go to https://github.com/tdoukinitsas/liveplay/releases/new
2. Create a new tag matching the version (e.g., `v1.2.0`)
3. Title the release (e.g., "LivePlay v1.2.0")
4. Add release notes describing what's new
5. Upload the generated installer files from `dist-electron`:
   - Windows: `LivePlay Setup X.X.X.exe` and `latest.yml`
   - macOS: `LivePlay-X.X.X.dmg` and `latest-mac.yml`
   - Linux: `LivePlay-X.X.X.AppImage` and `latest-linux.yml`

### 4. Publish the Release

Once published, all users running LivePlay will be notified of the update on their next launch.

## Important Notes

### Development vs Production

- Auto-updates only work in **production** builds (packaged apps)
- During development (`npm run dev`), updates are not checked
- This prevents false update notifications during development

### Update Files

The `latest.yml`, `latest-mac.yml`, and `latest-linux.yml` files are critical:
- They contain metadata about the release (version, download URLs, checksums)
- Must be uploaded alongside the installers
- Generated automatically by electron-builder

### Code Signing (Recommended)

For production apps, code signing is recommended:
- **Windows**: Use a code signing certificate
- **macOS**: Use Apple Developer ID
- Without signing, users may see security warnings

To configure code signing, add to `package.json`:
```json
{
  "build": {
    "win": {
      "certificateFile": "path/to/certificate.pfx",
      "certificatePassword": "password"
    },
    "mac": {
      "identity": "Developer ID Application: Your Name"
    }
  }
}
```

## Security

The auto-updater:
- Uses HTTPS for all downloads
- Verifies checksums to ensure file integrity
- Only downloads from the configured GitHub repository
- Requires user confirmation before downloading
- Downloads are performed by Electron's native update system

## Troubleshooting

### Updates Not Appearing

1. Check that the app is running in production mode (packaged)
2. Verify internet connection
3. Check console logs for error messages
4. Ensure the GitHub release is published (not draft)

### Download Failures

1. Check internet connection stability
2. Verify the release files are correctly uploaded
3. Ensure `latest.yml` files are present in the release
4. Check file permissions on GitHub repository

### Install Failures

1. Ensure user has write permissions to the installation directory
2. Check available disk space
3. Close all instances of the app before installing
4. Review system logs for error details

## API Reference

### Electron Main Process

```javascript
autoUpdater.checkForUpdates()    // Manually check for updates
autoUpdater.downloadUpdate()      // Download available update
autoUpdater.quitAndInstall()      // Install downloaded update
```

### Renderer Process (Vue)

```javascript
window.electronAPI.checkForUpdates()       // Check for updates
window.electronAPI.downloadUpdate()        // Download update
window.electronAPI.installUpdate()         // Install and restart
window.electronAPI.getAppVersion()         // Get current version

// Event listeners
window.electronAPI.onUpdateAvailable(callback)
window.electronAPI.onUpdateDownloadProgress(callback)
window.electronAPI.onUpdateDownloaded(callback)
window.electronAPI.onUpdateError(callback)
```

## Future Enhancements

Possible improvements to the update system:
- Delta updates (only download changed files)
- Background downloads
- Scheduled update checks
- Update rollback functionality
- Beta/alpha release channels
- Automatic updates option (install without prompting)

## Support

For issues with the update system:
1. Check the console logs in developer tools
2. Review GitHub release configuration
3. Open an issue at https://github.com/tdoukinitsas/liveplay/issues
