const { contextBridge, ipcRenderer } = require('electron');

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

  // Project management
  setCurrentProject: (projectPath) => ipcRenderer.invoke('set-current-project', projectPath),

  // Menu events
  onMenuNewProject: (callback) => ipcRenderer.on('menu-new-project', callback),
  onMenuOpenProject: (callback) => ipcRenderer.on('menu-open-project', callback),
  onMenuSaveProject: (callback) => ipcRenderer.on('menu-save-project', callback),
  onMenuCloseProject: (callback) => ipcRenderer.on('menu-close-project', callback),
  onMenuOpenProjectFolder: (callback) => ipcRenderer.on('menu-open-project-folder', callback),
  onMenuToggleDarkMode: (callback) => ipcRenderer.on('menu-toggle-dark-mode', callback),
  onMenuChangeAccentColor: (callback) => ipcRenderer.on('menu-change-accent-color', callback),

  // API triggers
  onTriggerItem: (callback) => ipcRenderer.on('trigger-item', callback),
  onStopItem: (callback) => ipcRenderer.on('stop-item', callback)
});
