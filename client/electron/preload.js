const { contextBridge, ipcRenderer } = require('electron');

// Try to import webUtils, but it may not be available in all Electron versions
let webUtils;
try {
  webUtils = require('electron').webUtils;
} catch (e) {
  console.warn('webUtils not available:', e);
}

contextBridge.exposeInMainWorld('electronAPI', {
  // File dialogs
  selectProjectFolder: () => ipcRenderer.invoke('select-project-folder'),
  selectProjectFile: () => ipcRenderer.invoke('select-project-file'),
  selectAudioFiles: () => ipcRenderer.invoke('select-audio-files'),

  // File operations
  readFile: (filePath) => ipcRenderer.invoke('read-file', filePath),
  readAudioFile: (filePath) => ipcRenderer.invoke('read-audio-file', filePath),
  loadAudioBuffer: async (filePath) => {
    const result = await ipcRenderer.invoke('read-audio-file', filePath);
    if (result.success) {
      return new Uint8Array(result.data).buffer;
    }
    throw new Error(result.error || 'Failed to load audio');
  },
  writeFile: (filePath, data) => ipcRenderer.invoke('write-file', filePath, data),
  // Dialogs + binary helpers used by the dual-dialog import/export flows.
  showSaveArchiveDialog: (defaultName) => ipcRenderer.invoke('show-save-archive-dialog', defaultName),
  showOpenArchiveDialog: () => ipcRenderer.invoke('show-open-archive-dialog'),
  writeBinaryFile: (filePath, data) => ipcRenderer.invoke('write-binary-file', filePath, data),
  copyFile: (source, destination) => ipcRenderer.invoke('copy-file', source, destination),
  ensureDirectory: (dirPath) => ipcRenderer.invoke('ensure-directory', dirPath),
  openFolder: (folderPath) => ipcRenderer.invoke('open-folder', folderPath),
  
  // Get file path from dropped file
  getFilePath: (file) => {
    try {
      if (webUtils && webUtils.getPathForFile) {
        return webUtils.getPathForFile(file);
      }
      // Fallback: try to get path from file object directly (may not work in all cases)
      return file.path || null;
    } catch (error) {
      console.error('Error getting file path:', error);
      return null;
    }
  },

  // Project management
  setCurrentProject: (projectPath) => ipcRenderer.invoke('set-current-project', projectPath),
  exportProject: (projectFolderPath, projectName) => ipcRenderer.invoke('export-project', projectFolderPath, projectName),
  importProject: () => ipcRenderer.invoke('import-project'),
  importLpaFile: (lpaPath) => ipcRenderer.invoke('import-lpa-file', lpaPath),
  onExportProgress: (callback) => ipcRenderer.on('export-progress', callback),
  onImportProgress: (callback) => ipcRenderer.on('import-progress', callback),
  removeExportProgressListener: (callback) => ipcRenderer.removeListener('export-progress', callback),
  removeImportProgressListener: (callback) => ipcRenderer.removeListener('import-progress', callback),

  // Waveform generation
  generateWaveform: (audioPath, outputPath) => ipcRenderer.invoke('generate-waveform', audioPath, outputPath),

  // FFmpeg check
  checkFfmpeg: () => ipcRenderer.invoke('check-ffmpeg'),

  // YouTube features
  searchYouTube: (query) => ipcRenderer.invoke('search-youtube', query),
  downloadYouTubeAudio: (videoId, title, projectFolderPath, progressCallback) => {
    // Set up progress listener
    const progressListener = (event, progress) => {
      if (progress.videoId === videoId && progressCallback) {
        progressCallback(progress);
      }
    };
    ipcRenderer.on('youtube-download-progress', progressListener);
    
    // Start download and clean up listener when done
    return ipcRenderer.invoke('download-youtube-audio', videoId, title, projectFolderPath)
      .finally(() => {
        ipcRenderer.removeListener('youtube-download-progress', progressListener);
      });
  },

  // Menu events
  onMenuNewProject: (callback) => ipcRenderer.on('menu-new-project', callback),
  onMenuOpenProject: (callback) => ipcRenderer.on('menu-open-project', callback),
  onMenuSaveProject: (callback) => ipcRenderer.on('menu-save-project', callback),
  onMenuExportProject: (callback) => ipcRenderer.on('menu-export-project', callback),
  onMenuImportProject: (callback) => ipcRenderer.on('menu-import-project', callback),
  onMenuCloseProject: (callback) => ipcRenderer.on('menu-close-project', callback),
  onMenuOpenRecentProject: (callback) => ipcRenderer.on('menu-open-recent-project', callback),
  onMenuOpenProjectFolder: (callback) => ipcRenderer.on('menu-open-project-folder', callback),
  onMenuToggleDarkMode: (callback) => ipcRenderer.on('menu-toggle-dark-mode', callback),
  onMenuChangeAccentColor: (callback) => ipcRenderer.on('menu-change-accent-color', callback),
  onMenuChangeLanguage: (callback) => ipcRenderer.on('menu-change-language', callback),
  onMenuShowAbout: (callback) => ipcRenderer.on('menu-show-about', callback),

  // External links
  openExternal: (url) => ipcRenderer.invoke('open-external', url),
  
  // Update menu language
  updateMenuLanguage: (locale) => ipcRenderer.invoke('update-menu-language', locale),
  
  // Get system locale
  getSystemLocale: () => ipcRenderer.invoke('get-system-locale'),
  
  // Get available locales and locale data
  getAvailableLocales: () => ipcRenderer.invoke('get-available-locales'),
  getLocaleData: (localeCode) => ipcRenderer.invoke('get-locale-data', localeCode),

  // Auto-updater
  checkForUpdates: () => ipcRenderer.invoke('check-for-updates'),
  downloadUpdate: () => ipcRenderer.invoke('download-update'),
  installUpdate: () => ipcRenderer.invoke('install-update'),
  getAppVersion: () => ipcRenderer.invoke('get-app-version'),
  onUpdateAvailable: (callback) => ipcRenderer.on('update-available', callback),
  onUpdateDownloadProgress: (callback) => ipcRenderer.on('update-download-progress', callback),
  onUpdateDownloaded: (callback) => ipcRenderer.on('update-downloaded', callback),
  onUpdateError: (callback) => ipcRenderer.on('update-error', callback),
  onManualUpdateAvailable: (callback) => ipcRenderer.on('manual-update-available', callback),

  // API triggers
  onTriggerItem: (callback) => ipcRenderer.on('trigger-item', callback),
  onStopItem: (callback) => ipcRenderer.on('stop-item', callback),
  onTriggerCartSlot: (callback) => ipcRenderer.on('trigger-cart-slot', callback),
  onStopAllCues: (callback) => ipcRenderer.on('stop-all-cues', callback),

  // HTTP API — project data sync and PATCH round-trips
  syncProjectData: (data) => ipcRenderer.send('sync-project-data', data),
  sendApiResponse: (data) => ipcRenderer.send('api-response', data),
  onApiUpdateItem: (callback) => ipcRenderer.on('api-update-item', callback),
  onApiUpdateCartItem: (callback) => ipcRenderer.on('api-update-cart-item', callback),
  
  // File association - opening project files (.liveplay / .lpa).
  // Push: main → renderer for warm-start / macOS open-file events.
  onOpenFileAssociation: (callback) => ipcRenderer.on('open-file-association', callback),
  // Pull: renderer asks on mount for any file queued before it was ready
  // (cold start). Returns { filePath, kind } | null.
  getPendingOpenFile: () => ipcRenderer.invoke('get-pending-open-file'),
  
  // Cart player window — detach/attach
  openCartPlayerWindow: (projectFolderPath) => ipcRenderer.invoke('open-cart-player-window', projectFolderPath),
  attachCartPlayerWindow: () => ipcRenderer.send('cart-player-window-attach'),
  getCartWindowProjectData: () => ipcRenderer.invoke('get-cart-window-project-data'),
  onCartPlayerWindowOpened: (callback) => ipcRenderer.on('cart-player-window-opened', callback),
  onCartPlayerWindowClosed: (callback) => ipcRenderer.on('cart-player-window-closed', callback),
  onCartWindowProjectUpdate: (callback) => ipcRenderer.on('cart-window-project-update', callback),

  // State viewer - send state updates to main process
  updateAppState: (state) => ipcRenderer.send('update-app-state', state),
  
  // Check if dev mode is enabled
  isDevMode: () => ipcRenderer.invoke('is-dev-mode'),

  // MIDI config
  readMidiConfig: () => ipcRenderer.invoke('read-midi-config'),
  writeMidiConfig: (config) => ipcRenderer.invoke('write-midi-config', config),

  // LivePlay audio server lifecycle (C++ server spawned by main process)
  liveplayServer: {
    getConfig: () => ipcRenderer.invoke('liveplay-server:get-config'),
    setConfig: (cfg) => ipcRenderer.invoke('liveplay-server:set-config', cfg),
    getStatus: () => ipcRenderer.invoke('liveplay-server:get-status'),
    restart:   () => ipcRenderer.invoke('liveplay-server:restart'),
    shutdown:  () => ipcRenderer.invoke('liveplay-server:shutdown'),
    ensureRunning: () => ipcRenderer.invoke('liveplay-server:ensure-running'),
    onStateChange: (callback) => {
      const listener = (_e, payload) => callback(payload);
      ipcRenderer.on('liveplay-server:state', listener);
      return () => ipcRenderer.removeListener('liveplay-server:state', listener);
    },
  },

  // Top-level app lifecycle (used by the connection-lost modal and the
  // quit-confirmation flow).
  app: {
    relaunch: () => ipcRenderer.invoke('app:relaunch'),
    exit:     () => ipcRenderer.invoke('app:exit'),
    // Quit confirmation: main vetoes the window close and pushes
    // `app:request-quit`; the renderer shows its dialogs then calls
    // confirmQuit({ stopServer }) to actually quit (optionally shutting
    // the local audio server down first).
    confirmQuit: (opts) => ipcRenderer.invoke('app:confirm-quit', opts),
    onRequestQuit: (callback) => {
      const listener = () => callback();
      ipcRenderer.on('app:request-quit', listener);
      return () => ipcRenderer.removeListener('app:request-quit', listener);
    },
  },

  // LAN auto-discovery of other LivePlay servers (UDP beacons on the LAN).
  liveplayDiscovery: {
    start:   () => ipcRenderer.invoke('liveplay-discovery:start'),
    list:    () => ipcRenderer.invoke('liveplay-discovery:list'),
    solicit: () => ipcRenderer.invoke('liveplay-discovery:solicit'),
    onServers: (callback) => {
      const listener = (_e, servers) => callback(servers);
      ipcRenderer.on('liveplay-discovery:servers', listener);
      return () => ipcRenderer.removeListener('liveplay-discovery:servers', listener);
    },
    // Recent-servers history (persisted) — robust reconnect fallback.
    recentList:   ()    => ipcRenderer.invoke('liveplay-discovery:recent-list'),
    recentAdd:    (e)   => ipcRenderer.invoke('liveplay-discovery:recent-add', e),
    recentRemove: (url) => ipcRenderer.invoke('liveplay-discovery:recent-remove', url),
  },

  // Recent-projects history (persisted) — last 10 .liveplay files opened on
  // this client. Surfaced in the File > Open Recent menu.
  liveplayProjects: {
    recentList:   ()     => ipcRenderer.invoke('liveplay-projects:recent-list'),
    recentAdd:    (e)    => ipcRenderer.invoke('liveplay-projects:recent-add', e),
    recentRemove: (path) => ipcRenderer.invoke('liveplay-projects:recent-remove', path),
    recentClear:  ()     => ipcRenderer.invoke('liveplay-projects:recent-clear'),
  },
});
