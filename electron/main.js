const { app, BrowserWindow, ipcMain, dialog, Menu, shell } = require('electron');
const { autoUpdater } = require('electron-updater');
const path = require('path');
const fs = require('fs');
const { spawn, exec } = require('child_process');
const express = require('express');
const youtubesearchapi = require('youtube-search-api');
const YTDlpWrap = require('yt-dlp-wrap').default;
const ffmpeg = require('fluent-ffmpeg');
const { promisify } = require('util');
const execPromise = promisify(exec);

let ffmpegPath = null;
let ffmpegAvailable = false;

// Check if ffmpeg is available in PATH or use bundled version
async function checkAndSetupFfmpeg() {
  // First, try to find ffmpeg in PATH
  try {
    const command = process.platform === 'win32' ? 'where ffmpeg' : 'which ffmpeg';
    const { stdout } = await execPromise(command);
    const systemFfmpegPath = stdout.trim().split('\n')[0];
    
    // Verify it works
    await execPromise(`"${systemFfmpegPath}" -version`);
    ffmpegPath = systemFfmpegPath;
    ffmpegAvailable = true;
    console.log('Found ffmpeg in system PATH:', ffmpegPath);
    return true;
  } catch (error) {
    console.log('ffmpeg not found in system PATH');
  }

  // If not in PATH, use bundled version
  try {
    const ffmpegStatic = require('@ffmpeg-installer/ffmpeg');
    ffmpegPath = ffmpegStatic.path;
    
    // Verify bundled version works
    await execPromise(`"${ffmpegPath}" -version`);
    ffmpegAvailable = true;
    console.log('Using bundled ffmpeg:', ffmpegPath);
    return true;
  } catch (error) {
    console.error('Failed to setup ffmpeg:', error);
    return false;
  }
}

// Show ffmpeg installation dialog
async function promptFfmpegInstallation(mainWindow) {
  const result = await dialog.showMessageBox(mainWindow, {
    type: 'warning',
    title: 'FFmpeg Required',
    message: 'FFmpeg is required for audio processing and YouTube downloads',
    detail: 'LivePlay can use a bundled version of FFmpeg, or you can install it system-wide for better performance.\n\nWould you like to use the bundled version now?',
    buttons: ['Use Bundled FFmpeg', 'Download System FFmpeg', 'Cancel'],
    defaultId: 0,
    cancelId: 2
  });

  if (result.response === 0) {
    // Use bundled version
    console.log('User chose to use bundled ffmpeg');
    return true;
  } else if (result.response === 1) {
    // Open ffmpeg download page
    shell.openExternal('https://www.ffmpeg.org/download.html');
    await dialog.showMessageBox(mainWindow, {
      type: 'info',
      title: 'FFmpeg Installation',
      message: 'Please download and install FFmpeg, then restart LivePlay',
      detail: 'After installing FFmpeg, make sure to add it to your system PATH.',
      buttons: ['OK']
    });
    return false;
  }

  return false;
}

// Initialize yt-dlp wrapper
let ytDlpPath;
let ytDlpReady = false;

async function initializeYtDlp() {
  try {
    // Set up download directory in user data folder
    const binDir = path.join(app.getPath('userData'), 'bin');
    if (!fs.existsSync(binDir)) {
      fs.mkdirSync(binDir, { recursive: true });
    }
    
    const exeName = process.platform === 'win32' ? 'yt-dlp.exe' : 'yt-dlp';
    const binaryPath = path.join(binDir, exeName);
    
    console.log('Initializing yt-dlp...');
    console.log('Binary directory:', binDir);
    console.log('Binary path:', binaryPath);
    
    // Check if binary already exists
    if (fs.existsSync(binaryPath)) {
      console.log('yt-dlp binary already exists');
      ytDlpPath = binaryPath;
      ytDlpReady = true;
      return true;
    }
    
    // Download yt-dlp binary - pass the full file path
    console.log('Downloading yt-dlp binary...');
    ytDlpPath = await YTDlpWrap.downloadFromGithub(binaryPath);
    
    // Verify the download
    if (fs.existsSync(binaryPath)) {
      ytDlpPath = binaryPath;
      ytDlpReady = true;
      console.log('yt-dlp binary downloaded successfully:', ytDlpPath);
      return true;
    } else {
      throw new Error('Failed to download yt-dlp binary');
    }
  } catch (error) {
    console.error('Failed to initialize yt-dlp:', error);
    ytDlpReady = false;
    return false;
  }
}

// Start initialization immediately
initializeYtDlp();

let mainWindow = null;
let apiServer = null;
let currentProject = null;
let fileToOpen = null; // Store file path if app is opened with a file

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

// Configure auto-updater
autoUpdater.autoDownload = false; // Don't auto-download, ask user first
autoUpdater.autoInstallOnAppQuit = true;

// Auto-updater event handlers
autoUpdater.on('checking-for-update', () => {
  console.log('Checking for updates...');
});

autoUpdater.on('update-available', (info) => {
  console.log('Update available:', info.version);
  if (mainWindow) {
    mainWindow.webContents.send('update-available', {
      currentVersion: app.getVersion(),
      newVersion: info.version,
      releaseNotes: info.releaseNotes,
      releaseDate: info.releaseDate
    });
  }
});

autoUpdater.on('update-not-available', (info) => {
  console.log('Update not available. Current version is latest:', info.version);
});

autoUpdater.on('error', (err) => {
  console.error('Error in auto-updater:', err);
  if (mainWindow) {
    mainWindow.webContents.send('update-error', err.message);
  }
});

autoUpdater.on('download-progress', (progressObj) => {
  console.log(`Download speed: ${progressObj.bytesPerSecond} - Downloaded ${progressObj.percent}%`);
  if (mainWindow) {
    mainWindow.webContents.send('update-download-progress', {
      percent: progressObj.percent,
      transferred: progressObj.transferred,
      total: progressObj.total
    });
  }
});

autoUpdater.on('update-downloaded', (info) => {
  console.log('Update downloaded:', info.version);
  if (mainWindow) {
    mainWindow.webContents.send('update-downloaded', {
      version: info.version
    });
  }
});

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1400,
    height: 900,
    minWidth: 1200,
    minHeight: 700,
    icon: path.join(__dirname, '../assets/icons/2x/app_icon_darkmode@2x.png'),
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
    
    // Check for updates only in production
    if (!isDev) {
      // Wait a bit for the window to fully load before checking updates
      setTimeout(() => {
        autoUpdater.checkForUpdates().catch(err => {
          console.error('Failed to check for updates:', err);
        });
      }, 3000);
    }
  });

  mainWindow.on('closed', () => {
    mainWindow = null;
  });

  createMenu('en', isDev);
  startAPIServer();
}

// Translation strings for menu (default: English)
// Load all locale files dynamically
const localeFiles = {
  en: require('../locales/en.json'),
  el: require('../locales/el.json'),
  fr: require('../locales/fr.json'),
  es: require('../locales/es.json'),
  it: require('../locales/it.json'),
  pt: require('../locales/pt.json'),
  ar: require('../locales/ar.json'),
  fa: require('../locales/fa.json'),
  de: require('../locales/de.json'),
  sv: require('../locales/sv.json'),
  no: require('../locales/no.json'),
  ru: require('../locales/ru.json'),
  ja: require('../locales/ja.json'),
  zh: require('../locales/zh.json'),
  hi: require('../locales/hi.json'),
  bn: require('../locales/bn.json'),
  tr: require('../locales/tr.json'),
  ko: require('../locales/ko.json'),
  sq: require('../locales/sq.json'),
  ur: require('../locales/ur.json')
};

// Build menu translations from locale files
const menuTranslations = Object.entries(localeFiles).reduce((acc, [code, data]) => {
  acc[code] = {
    file: data.menu.file,
    newProject: data.menu.newProject,
    openProject: data.menu.openProject,
    saveProject: data.menu.saveProject,
    closeProject: data.menu.closeProject,
    openProjectFolder: data.menu.openProjectFolder,
    exit: data.menu.exit,
    view: data.menu.view,
    toggleDarkMode: data.menu.toggleDarkMode,
    changeAccentColor: data.menu.changeAccentColor,
    fullscreen: data.menu.fullscreen,
    language: data.menu.language,
    help: data.menu.help,
    about: data.menu.about
  };
  return acc;
}, {});

let currentLocale = 'en';

function createMenu(locale = 'en', isDev = false) {
  currentLocale = locale;
  const t = menuTranslations[locale] || menuTranslations.en;
  
  const template = [
    {
      label: t.file,
      submenu: [
        {
          label: t.newProject,
          accelerator: 'CmdOrCtrl+N',
          click: () => {
            mainWindow.webContents.send('menu-new-project');
          }
        },
        {
          label: t.openProject,
          accelerator: 'CmdOrCtrl+O',
          click: () => {
            mainWindow.webContents.send('menu-open-project');
          }
        },
        {
          label: t.saveProject,
          accelerator: 'CmdOrCtrl+S',
          click: () => {
            mainWindow.webContents.send('menu-save-project');
          }
        },
        { type: 'separator' },
        {
          label: t.openProjectFolder,
          click: () => {
            mainWindow.webContents.send('menu-open-project-folder');
          }
        },
        { type: 'separator' },
        {
          label: t.closeProject,
          accelerator: 'CmdOrCtrl+W',
          click: () => {
            mainWindow.webContents.send('menu-close-project');
          }
        },
        { type: 'separator' },
        {
          label: t.exit,
          accelerator: 'CmdOrCtrl+Q',
          click: () => {
            app.quit();
          }
        }
      ]
    },
    {
      label: t.view,
      submenu: [
        {
          label: t.toggleDarkMode,
          click: () => {
            mainWindow.webContents.send('menu-toggle-dark-mode');
          }
        },
        {
          label: t.changeAccentColor,
          click: () => {
            mainWindow.webContents.send('menu-change-accent-color');
          }
        },
        { type: 'separator' },
        {
          label: t.fullscreen,
          accelerator: 'F11',
          click: () => {
            const isFullScreen = mainWindow.isFullScreen();
            mainWindow.setFullScreen(!isFullScreen);
          }
        },
        { type: 'separator' },
        {
          label: t.language,
          submenu: Object.values(localeFiles).map((localeData) => ({
            label: localeData._metadata.nativeName,
            type: 'radio',
            checked: locale === localeData._metadata.code,
            click: () => {
              mainWindow.webContents.send('menu-change-language', localeData._metadata.code);
              createMenu(localeData._metadata.code, isDev);
            }
          }))
        },
        ...(isDev ? [
          { type: 'separator' },
          { role: 'reload' },
          { role: 'forceReload' },
          { 
          label: 'Toggle Developer Tools',
          accelerator: process.platform === 'darwin' ? 'Alt+Command+I' : 'Ctrl+Shift+I',
          click: () => {
            if (mainWindow && mainWindow.webContents) {
              mainWindow.webContents.toggleDevTools();
            }
          }
        }
        ] : [])
      ]
    },
    {
      label: t.help,
      submenu: [
        {
          label: t.about,
          click: () => {
            mainWindow.webContents.send('menu-show-about');
          }
        }
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

// Open external URL in default browser
ipcMain.handle('open-external', async (event, url) => {
  try {
    await shell.openExternal(url);
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

// Update menu language from renderer
ipcMain.handle('update-menu-language', async (event, locale) => {
  const isDev = !app.isPackaged;
  createMenu(locale, isDev);
  return { success: true };
});

// Auto-updater IPC handlers
ipcMain.handle('check-for-updates', async () => {
  try {
    const result = await autoUpdater.checkForUpdates();
    return { success: true, updateInfo: result?.updateInfo };
  } catch (error) {
    console.error('Check for updates error:', error);
    return { success: false, error: error.message };
  }
});

ipcMain.handle('download-update', async () => {
  try {
    await autoUpdater.downloadUpdate();
    return { success: true };
  } catch (error) {
    console.error('Download update error:', error);
    return { success: false, error: error.message };
  }
});

ipcMain.handle('install-update', () => {
  autoUpdater.quitAndInstall(false, true);
});

ipcMain.handle('get-app-version', () => {
  return app.getVersion();
});

ipcMain.handle('get-system-locale', () => {
  // Get the system locale from Electron
  const systemLocale = app.getLocale(); // Returns locale like 'en-US', 'es-ES', 'fr-FR', etc.
  
  // Extract just the language code (e.g., 'en' from 'en-US')
  const languageCode = systemLocale.split('-')[0].toLowerCase();
  
  return languageCode;
});

ipcMain.handle('set-current-project', async (event, projectPath) => {
  currentProject = projectPath;
  return { success: true };
});

// Check FFmpeg availability
ipcMain.handle('check-ffmpeg', async () => {
  return {
    available: ffmpegAvailable,
    path: ffmpegPath || null
  };
});

// Waveform generation
ipcMain.handle('generate-waveform', async (event, audioFilePath, outputPath) => {
  return new Promise((resolve, reject) => {
    console.log('Generating waveform for:', audioFilePath);
    console.log('Output path:', outputPath);
    
    // Ensure output directory exists
    const outputDir = path.dirname(outputPath);
    if (!fs.existsSync(outputDir)) {
      fs.mkdirSync(outputDir, { recursive: true });
    }
    
    // Use the detected ffmpeg path
    if (ffmpegPath) {
      ffmpeg.setFfmpegPath(ffmpegPath);
    }
    
    // First, get the duration
    ffmpeg.ffprobe(audioFilePath, (err, metadata) => {
      if (err) {
        console.error('FFprobe error:', err);
        reject(err);
        return;
      }
      
      const duration = metadata.format.duration;
      if (!duration) {
        reject(new Error('Could not determine audio duration'));
        return;
      }
      
      // Calculate samples: 10 per second
      const targetSamples = Math.ceil(duration * 10);
      const samples = [];
      const tempOutput = outputPath + '.temp.wav';
      
      // Extract raw audio data
      ffmpeg(audioFilePath)
        .audioChannels(1)
        .audioFrequency(8000) // Lower frequency for smaller data
        .format('s16le')
        .on('error', (err) => {
          console.error('FFmpeg waveform error:', err);
          reject(err);
        })
        .on('end', () => {
          // Read the temp file and process samples
          try {
            if (fs.existsSync(tempOutput)) {
              const buffer = fs.readFileSync(tempOutput);
              
              // Process samples to get exactly 10 per second
              const sampleInterval = Math.floor(buffer.length / (targetSamples * 2)); // 2 bytes per sample
              
              for (let i = 0; i < buffer.length - 1 && samples.length < targetSamples; i += sampleInterval * 2) {
                const sample = buffer.readInt16LE(i) / 32768.0; // Normalize to -1 to 1
                samples.push(Math.abs(sample));
              }
              
              // Clean up temp file
              fs.unlinkSync(tempOutput);
              
              // Save waveform data with duration
              const waveformData = {
                peaks: samples,
                duration: duration,
                sampleRate: 10 // 10 samples per second
              };
              
              fs.writeFileSync(outputPath, JSON.stringify(waveformData));
              console.log('Waveform generated successfully:', samples.length, 'samples @10/sec for', duration.toFixed(2), 'seconds');
              
              resolve({ success: true, duration: duration });
            } else {
              reject(new Error('Temporary audio file not created'));
            }
          } catch (error) {
            console.error('Error processing waveform:', error);
            reject(error);
          }
        })
        .save(tempOutput);
    });
  });
});

// YouTube Search Handler
ipcMain.handle('search-youtube', async (event, query) => {
  try {
    const result = await youtubesearchapi.GetListByKeyword(query, false, 20, [{ type: 'video' }]);
    
    // Format results
    const videos = result.items.map(item => ({
      id: item.id,
      title: item.title,
      thumbnail: item.thumbnail.thumbnails[item.thumbnail.thumbnails.length - 1].url,
      channelTitle: item.channelTitle,
      length: item.length?.simpleText || ''
    }));
    
    return videos;
  } catch (error) {
    console.error('YouTube search error:', error);
    throw new Error('Failed to search YouTube');
  }
});

// YouTube Download Handler
ipcMain.handle('download-youtube-audio', async (event, videoId, title, projectFolderPath) => {
  return new Promise(async (resolve, reject) => {
    console.log('YouTube download - Project folder path:', projectFolderPath);
    
    const outputPath = path.join(projectFolderPath, 'media');
    console.log('YouTube download - Output path:', outputPath);
    
    // Ensure output directory exists
    if (!fs.existsSync(outputPath)) {
      fs.mkdirSync(outputPath, { recursive: true });
      console.log('Created media directory:', outputPath);
    }
    
    // Clean filename
    const sanitizedTitle = title.replace(/[<>:"/\\|?*]/g, '').substring(0, 200);
    const fileName = `${sanitizedTitle}.mp3`;
    const outputTemplate = path.join(outputPath, sanitizedTitle);
    
    console.log('YouTube download - Output template:', outputTemplate);
    
    const videoUrl = `https://www.youtube.com/watch?v=${videoId}`;
    
    console.log(`Starting YouTube download: ${videoId} -> ${fileName}`);
    console.log(`Video URL: ${videoUrl}`);
    
    // Wait for yt-dlp to be ready (with timeout)
    if (!ytDlpReady) {
      console.log('Waiting for yt-dlp to initialize...');
      let attempts = 0;
      while (!ytDlpReady && attempts < 30) { // Wait up to 30 seconds
        await new Promise(resolve => setTimeout(resolve, 1000));
        attempts++;
      }
      
      if (!ytDlpReady) {
        reject(new Error('yt-dlp initialization timed out. Please try again.'));
        return;
      }
    }
    
    if (!ytDlpPath) {
      reject(new Error('yt-dlp binary path not available. Please restart the application.'));
      return;
    }
    
    if (!ffmpegAvailable) {
      reject(new Error('FFmpeg is required for YouTube downloads. Please install FFmpeg and restart the application.'));
      return;
    }
    
    try {
      // Create YTDlpWrap instance with the binary path
      const ytDlp = new YTDlpWrap(ytDlpPath);
      
      // Build yt-dlp arguments
      const args = [
        videoUrl,
        '-f', 'bestaudio',
        '--extract-audio',
        '--audio-format', 'mp3',
        '--audio-quality', '0', // Best quality
        '-o', outputTemplate + '.%(ext)s',
        '--no-playlist',
        '--progress',
        '--newline' // Force progress on new lines for easier parsing
      ];
      
      // Add ffmpeg path if we have it
      if (ffmpegPath) {
        args.push('--ffmpeg-location', ffmpegPath);
      }
      
      console.log('Running yt-dlp with args:', args);
      console.log('yt-dlp path:', ytDlpPath);
      
      // Use spawn to get a proper ChildProcess
      const { spawn } = require('child_process');
      const downloadProcess = spawn(ytDlpPath, args);
      
      // Check if downloadProcess is valid
      if (!downloadProcess || !downloadProcess.stdout) {
        throw new Error('Failed to start yt-dlp process');
      }
      
      let lastProgress = 0;
      
      // Track progress by parsing stdout
      downloadProcess.stdout.on('data', (data) => {
        const output = data.toString();
        
        // Parse download progress
        const downloadMatch = output.match(/\[download\]\s+(\d+\.?\d*)%/);
        if (downloadMatch) {
          const percentage = parseFloat(downloadMatch[1]);
          if (percentage > lastProgress) {
            lastProgress = percentage;
            event.sender.send('youtube-download-progress', {
              videoId,
              percentage: percentage,
              status: percentage < 100 ? 'downloading' : 'converting'
            });
          }
        }
        
        // Check for post-processing
        if (output.includes('[ExtractAudio]') || output.includes('Destination:')) {
          event.sender.send('youtube-download-progress', {
            videoId,
            percentage: 95,
            status: 'converting'
          });
        }
      });
      
      downloadProcess.stderr.on('data', (data) => {
        const errorOutput = data.toString();
        // yt-dlp uses stderr for normal output, only log actual errors
        if (errorOutput.includes('ERROR')) {
          console.error('yt-dlp error:', errorOutput);
        }
      });
      
      downloadProcess.on('error', (error) => {
        console.error('yt-dlp process error:', error);
        reject(new Error(`Download process failed: ${error.message}`));
      });
      
      downloadProcess.on('close', (code) => {
        console.log(`yt-dlp process closed with code: ${code}`);
        
        if (code !== 0) {
          reject(new Error(`yt-dlp exited with code ${code}`));
          return;
        }
        
        console.log(`Download completed: ${fileName}`);
        
        // Find the actual downloaded file (yt-dlp might use URL encoding)
        const expectedFile = path.join(outputPath, fileName);
        let actualFile = expectedFile;
        
        // Check if file exists with expected name
        if (!fs.existsSync(expectedFile)) {
          // Try to find it with URL-encoded name or other variations
          const files = fs.readdirSync(outputPath);
          const baseName = sanitizedTitle;
          
          // Look for files that match the base name (case-insensitive, with any encoding)
          const matchingFile = files.find(f => {
            const decoded = decodeURIComponent(f);
            return decoded.toLowerCase().startsWith(baseName.toLowerCase()) && f.endsWith('.mp3');
          });
          
          if (matchingFile) {
            actualFile = path.join(outputPath, matchingFile);
            console.log('Found downloaded file:', matchingFile);
            
            // Rename to expected filename if different
            if (matchingFile !== fileName) {
              try {
                fs.renameSync(actualFile, expectedFile);
                actualFile = expectedFile;
                console.log('Renamed file to:', fileName);
              } catch (renameError) {
                console.error('Failed to rename file:', renameError);
              }
            }
          } else {
            console.error('Could not find downloaded file. Files in directory:', files);
            reject(new Error('Downloaded file not found in expected location'));
            return;
          }
        }
        
        // Send 100% progress
        event.sender.send('youtube-download-progress', {
          videoId,
          percentage: 100,
          status: 'completed'
        });
        
        resolve({
          success: true,
          file: actualFile,
          fileName: path.basename(actualFile),
          title: sanitizedTitle
        });
      });
      
    } catch (error) {
      console.error('YouTube download error:', error);
      console.error('Error stack:', error.stack);
      
      // Clean up partial file
      const outputFile = path.join(outputPath, fileName);
      if (fs.existsSync(outputFile)) {
        try {
          fs.unlinkSync(outputFile);
        } catch (e) {
          console.error('Failed to clean up file:', e);
        }
      }
      
      reject(new Error(`Download failed: ${error.message}`));
    }
  });
});

app.whenReady().then(async () => {
  // Check and setup ffmpeg before creating window
  const ffmpegReady = await checkAndSetupFfmpeg();
  
  createWindow();
  
  // If a file was opened before the app was ready, open it now
  if (fileToOpen && mainWindow) {
    mainWindow.once('ready-to-show', () => {
      openFile(fileToOpen);
      fileToOpen = null;
    });
  }
  
  // If ffmpeg is not available, prompt user after window is shown
  if (!ffmpegReady && mainWindow) {
    mainWindow.once('ready-to-show', async () => {
      const shouldContinue = await promptFfmpegInstallation(mainWindow);
      if (shouldContinue) {
        // Try setup again with bundled version
        await checkAndSetupFfmpeg();
      }
    });
  }
});

// Handle file opening on Windows/Linux (when file is double-clicked)
app.on('open-file', (event, filePath) => {
  event.preventDefault();
  
  if (mainWindow && mainWindow.webContents) {
    // Window is ready, open the file immediately
    openFile(filePath);
  } else {
    // Window not ready yet, store the file path
    fileToOpen = filePath;
  }
});

// Handle command line arguments (Windows/Linux)
if (process.platform === 'win32' || process.platform === 'linux') {
  // Check if a file was passed as argument
  const fileArg = process.argv.find(arg => arg.endsWith('.liveplay'));
  if (fileArg) {
    fileToOpen = fileArg;
  }
}

// Helper function to open a project file
function openFile(filePath) {
  if (!mainWindow) return;
  
  try {
    // Read the file
    const fileContent = fs.readFileSync(filePath, 'utf-8');
    const projectData = JSON.parse(fileContent);
    
    // Send the project data to the renderer
    mainWindow.webContents.send('open-project-file', {
      filePath: filePath,
      projectData: projectData
    });
    
    console.log('Opened project file:', filePath);
  } catch (error) {
    console.error('Failed to open project file:', error);
    
    if (mainWindow) {
      dialog.showErrorBox(
        'Failed to Open Project',
        `Could not open the project file:\n${error.message}`
      );
    }
  }
}

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
