const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
  onStateUpdate: (callback) => ipcRenderer.on('state-update', callback)
});
