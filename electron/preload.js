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
  onMenuCloseProject: (callback) => ipcRenderer.on('menu-close-project', callback),
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

  // Auto-updater
  checkForUpdates: () => ipcRenderer.invoke('check-for-updates'),
  downloadUpdate: () => ipcRenderer.invoke('download-update'),
  installUpdate: () => ipcRenderer.invoke('install-update'),
  getAppVersion: () => ipcRenderer.invoke('get-app-version'),
  onUpdateAvailable: (callback) => ipcRenderer.on('update-available', callback),
  onUpdateDownloadProgress: (callback) => ipcRenderer.on('update-download-progress', callback),
  onUpdateDownloaded: (callback) => ipcRenderer.on('update-downloaded', callback),
  onUpdateError: (callback) => ipcRenderer.on('update-error', callback),

  // API triggers
  onTriggerItem: (callback) => ipcRenderer.on('trigger-item', callback),
  onStopItem: (callback) => ipcRenderer.on('stop-item', callback),
  
  // File association - opening project files
  onOpenProjectFile: (callback) => ipcRenderer.on('open-project-file', callback)
});
