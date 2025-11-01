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

  // Menu events
  onMenuNewProject: (callback) => ipcRenderer.on('menu-new-project', callback),
  onMenuOpenProject: (callback) => ipcRenderer.on('menu-open-project', callback),
  onMenuSaveProject: (callback) => ipcRenderer.on('menu-save-project', callback),
  onMenuCloseProject: (callback) => ipcRenderer.on('menu-close-project', callback),
  onMenuOpenProjectFolder: (callback) => ipcRenderer.on('menu-open-project-folder', callback),
  onMenuToggleDarkMode: (callback) => ipcRenderer.on('menu-toggle-dark-mode', callback),
  onMenuChangeAccentColor: (callback) => ipcRenderer.on('menu-change-accent-color', callback),
  onMenuChangeLanguage: (callback) => ipcRenderer.on('menu-change-language', callback),

  // API triggers
  onTriggerItem: (callback) => ipcRenderer.on('trigger-item', callback),
  onStopItem: (callback) => ipcRenderer.on('stop-item', callback)
});
