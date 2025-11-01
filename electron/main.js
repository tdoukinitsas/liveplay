const { app, BrowserWindow, ipcMain, dialog, Menu, shell } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');
const express = require('express');

let mainWindow = null;
let apiServer = null;
let currentProject = null;

// API Server Setup
function startAPIServer(port = 8080) {
  const apiApp = express();
  apiApp.use(express.json());

  // Trigger item by UUID
  apiApp.get('/api/trigger/uuid/:uuid', (req, res) => {
    const { uuid } = req.params;
    if (mainWindow) {
      mainWindow.webContents.send('trigger-item', { type: 'uuid', value: uuid });
      res.json({ success: true, message: `Triggered item ${uuid}` });
    } else {
      res.status(500).json({ success: false, message: 'Window not available' });
    }
  });

  // Trigger item by index
  apiApp.get('/api/trigger/index/:index', (req, res) => {
    const { index } = req.params;
    if (mainWindow) {
      const indexArray = index.split(',').map(i => parseInt(i.trim()));
      mainWindow.webContents.send('trigger-item', { type: 'index', value: indexArray });
      res.json({ success: true, message: `Triggered item at index ${index}` });
    } else {
      res.status(500).json({ success: false, message: 'Window not available' });
    }
  });

  // Stop item
  apiApp.get('/api/stop/uuid/:uuid', (req, res) => {
    const { uuid } = req.params;
    if (mainWindow) {
      mainWindow.webContents.send('stop-item', { type: 'uuid', value: uuid });
      res.json({ success: true, message: `Stopped item ${uuid}` });
    } else {
      res.status(500).json({ success: false, message: 'Window not available' });
    }
  });

  // Get current project info
  apiApp.get('/api/project/info', (req, res) => {
    if (currentProject) {
      res.json({ success: true, project: currentProject });
    } else {
      res.status(404).json({ success: false, message: 'No project loaded' });
    }
  });

  apiServer = apiApp.listen(port, () => {
    console.log(`LivePlay API Server running on http://localhost:${port}`);
  });
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1400,
    height: 900,
    minWidth: 1200,
    minHeight: 700,
    icon: path.join(__dirname, '../assets/icons/2x/liveplay-icon-lightmode@2x.png'),
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.js'),
      webSecurity: false // Allow loading local files
    },
    show: false
  });

  // Detect if we're in development or production
  // In production (packaged), app.isPackaged will be true
  const isDev = !app.isPackaged;
  
  if (isDev) {
    mainWindow.loadURL('http://localhost:3000');
    // Open DevTools in development
    mainWindow.webContents.openDevTools();
  } else {
    // In production, load the generated static files
    const indexPath = path.join(__dirname, '../.output/public/index.html');
    console.log('Loading production index from:', indexPath);
    console.log('File exists:', fs.existsSync(indexPath));
    
    mainWindow.loadFile(indexPath).catch(err => {
      console.error('Failed to load index.html:', err);
    });
  }

  // Log any loading errors
  mainWindow.webContents.on('did-fail-load', (event, errorCode, errorDescription) => {
    console.error('Failed to load:', errorCode, errorDescription);
  });

  mainWindow.once('ready-to-show', () => {
    mainWindow.show();
  });

  mainWindow.on('closed', () => {
    mainWindow = null;
  });

  createMenu();
  startAPIServer();
}

function createMenu() {
  const template = [
    {
      label: 'File',
      submenu: [
        {
          label: 'New Project',
          accelerator: 'CmdOrCtrl+N',
          click: () => {
            mainWindow.webContents.send('menu-new-project');
          }
        },
        {
          label: 'Open Project',
          accelerator: 'CmdOrCtrl+O',
          click: () => {
            mainWindow.webContents.send('menu-open-project');
          }
        },
        {
          label: 'Save Project',
          accelerator: 'CmdOrCtrl+S',
          click: () => {
            mainWindow.webContents.send('menu-save-project');
          }
        },
        { type: 'separator' },
        {
          label: 'Open Project Folder',
          click: () => {
            mainWindow.webContents.send('menu-open-project-folder');
          }
        },
        { type: 'separator' },
        {
          label: 'Close Project',
          accelerator: 'CmdOrCtrl+W',
          click: () => {
            mainWindow.webContents.send('menu-close-project');
          }
        },
        { type: 'separator' },
        {
          label: 'Exit',
          accelerator: 'CmdOrCtrl+Q',
          click: () => {
            app.quit();
          }
        }
      ]
    },
    {
      label: 'View',
      submenu: [
        {
          label: 'Toggle Dark Mode',
          click: () => {
            mainWindow.webContents.send('menu-toggle-dark-mode');
          }
        },
        {
          label: 'Change Accent Color',
          click: () => {
            mainWindow.webContents.send('menu-change-accent-color');
          }
        },
        { type: 'separator' },
        {
          label: 'Language',
          submenu: [
            {
              label: 'English',
              type: 'radio',
              checked: true,
              click: () => {
                mainWindow.webContents.send('menu-change-language', 'en');
              }
            },
            {
              label: 'Ελληνικά',
              type: 'radio',
              click: () => {
                mainWindow.webContents.send('menu-change-language', 'el');
              }
            }
          ]
        },
        { type: 'separator' },
        { role: 'reload' },
        { role: 'forceReload' },
        { role: 'toggleDevTools' }
      ]
    }
  ];

  const menu = Menu.buildFromTemplate(template);
  Menu.setApplicationMenu(menu);
}

// IPC Handlers
ipcMain.handle('select-project-folder', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openDirectory', 'createDirectory']
  });

  if (!result.canceled && result.filePaths.length > 0) {
    return result.filePaths[0];
  }
  return null;
});

ipcMain.handle('select-project-file', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openFile'],
    filters: [{ name: 'LivePlay Project', extensions: ['liveplay'] }]
  });

  if (!result.canceled && result.filePaths.length > 0) {
    return result.filePaths[0];
  }
  return null;
});

ipcMain.handle('select-audio-files', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openFile', 'multiSelections'],
    filters: [
      { name: 'Audio Files', extensions: ['mp3', 'wav', 'ogg', 'flac', 'm4a', 'aac'] }
    ]
  });

  if (!result.canceled && result.filePaths.length > 0) {
    return result.filePaths;
  }
  return null;
});

ipcMain.handle('read-file', async (event, filePath) => {
  try {
    const data = fs.readFileSync(filePath, 'utf8');
    return { success: true, data };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('read-audio-file', async (event, filePath) => {
  try {
    const data = fs.readFileSync(filePath);
    // Convert Node.js Buffer to ArrayBuffer
    const arrayBuffer = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
    return { success: true, data: Array.from(new Uint8Array(arrayBuffer)) };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('write-file', async (event, filePath, data) => {
  try {
    fs.writeFileSync(filePath, data, 'utf8');
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('copy-file', async (event, source, destination) => {
  try {
    // Ensure destination directory exists
    const destDir = path.dirname(destination);
    if (!fs.existsSync(destDir)) {
      fs.mkdirSync(destDir, { recursive: true });
    }
    fs.copyFileSync(source, destination);
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('ensure-directory', async (event, dirPath) => {
  try {
    if (!fs.existsSync(dirPath)) {
      fs.mkdirSync(dirPath, { recursive: true });
    }
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('open-folder', async (event, folderPath) => {
  try {
    shell.openPath(folderPath);
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('set-current-project', async (event, projectPath) => {
  currentProject = projectPath;
  return { success: true };
});

// Generate waveform data using ffmpeg
ipcMain.handle('generate-waveform', async (event, audioPath, outputPath) => {
  return new Promise((resolve) => {
    try {
      // Ensure output directory exists
      const outputDir = path.dirname(outputPath);
      if (!fs.existsSync(outputDir)) {
        fs.mkdirSync(outputDir, { recursive: true });
      }

      // Use ffmpeg to extract audio samples
      // We'll create 1000 samples for the waveform
      const sampleCount = 1000;
      
      // Get audio duration first
      const durationProcess = spawn('ffprobe', [
        '-v', 'error',
        '-show_entries', 'format=duration',
        '-of', 'default=noprint_wrappers=1:nokey=1',
        audioPath
      ]);

      let duration = 0;
      let durationError = '';

      durationProcess.stdout.on('data', (data) => {
        duration = parseFloat(data.toString().trim());
      });

      durationProcess.stderr.on('data', (data) => {
        durationError += data.toString();
      });

      durationProcess.on('close', (code) => {
        if (code !== 0) {
          resolve({ success: false, error: `Failed to get duration: ${durationError}` });
          return;
        }

        // Now extract waveform samples
        const ffmpegProcess = spawn('ffmpeg', [
          '-i', audioPath,
          '-ac', '1',                    // Convert to mono
          '-filter:a', 'aresample=8000', // Resample to 8kHz for faster processing
          '-map', '0:a',
          '-c:a', 'pcm_s16le',           // 16-bit PCM
          '-f', 'data',                  // Raw data output
          '-'
        ]);

        let samples = [];
        let ffmpegError = '';

        ffmpegProcess.stdout.on('data', (data) => {
          // Convert buffer to 16-bit integers
          for (let i = 0; i < data.length; i += 2) {
            if (i + 1 < data.length) {
              const sample = data.readInt16LE(i);
              samples.push(Math.abs(sample));
            }
          }
        });

        ffmpegProcess.stderr.on('data', (data) => {
          ffmpegError += data.toString();
        });

        ffmpegProcess.on('close', (code) => {
          if (code !== 0) {
            resolve({ success: false, error: `FFmpeg error: ${ffmpegError}` });
            return;
          }

          // Downsample to target sample count
          const peaks = [];
          const blockSize = Math.floor(samples.length / sampleCount);
          
          for (let i = 0; i < sampleCount; i++) {
            const start = i * blockSize;
            const end = start + blockSize;
            const block = samples.slice(start, end);
            
            // Find max value in block
            const peak = block.length > 0 ? Math.max(...block) : 0;
            peaks.push(peak);
          }

          // Normalize peaks to 0-1 range
          const maxPeak = Math.max(...peaks, 1);
          const normalizedPeaks = peaks.map(p => p / maxPeak);

          // Create waveform data object
          const waveformData = {
            length: sampleCount,
            duration: duration,
            peaks: normalizedPeaks
          };

          // Write to file
          try {
            fs.writeFileSync(outputPath, JSON.stringify(waveformData, null, 2));
            resolve({ success: true });
          } catch (writeError) {
            resolve({ success: false, error: `Failed to write waveform file: ${writeError.message}` });
          }
        });
      });
    } catch (error) {
      resolve({ success: false, error: error.message });
    }
  });
});

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
  if (apiServer) {
    apiServer.close();
  }
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('activate', () => {
  if (mainWindow === null) {
    createWindow();
  }
});
