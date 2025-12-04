const { app, BrowserWindow, ipcMain, dialog, shell, Menu, protocol } = require('electron');
const { autoUpdater } = require('electron-updater');
const path = require('path');
const fs = require('fs');
const { spawn, exec } = require('child_process');
const express = require('express');
const youtubesearchapi = require('youtube-search-api');
const YTDlpWrap = require('yt-dlp-wrap').default;
const ffmpeg = require('fluent-ffmpeg');
const { promisify } = require('util');
const https = require('https');
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
let stateViewerWindow = null; // Debug state viewer window

// Check if --dev flag is present in command line arguments
const isDevMode = process.argv.includes('--dev') || !app.isPackaged;

// API Server Setup
function startAPIServer(port = 8080, maxAttempts = 10) {
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

  // Try to start server, incrementing port if already in use
  const tryListen = (currentPort, attemptsLeft) => {
    const server = apiApp.listen(currentPort)
      .on('listening', () => {
        apiServer = server;
        console.log(`LivePlay API Server running on http://localhost:${currentPort}`);
      })
      .on('error', (err) => {
        if (err.code === 'EADDRINUSE' && attemptsLeft > 0) {
          console.log(`Port ${currentPort} is in use, trying ${currentPort + 1}...`);
          tryListen(currentPort + 1, attemptsLeft - 1);
        } else if (err.code === 'EADDRINUSE') {
          console.error(`Failed to start API server after ${maxAttempts} attempts. Ports ${port}-${currentPort} are all in use.`);
        } else {
          console.error('Failed to start API server:', err);
        }
      });
  };

  tryListen(port, maxAttempts - 1);
}

// Configure auto-updater
autoUpdater.autoDownload = false; // Don't auto-download, ask user first
autoUpdater.autoInstallOnAppQuit = true;

// Configure update feed URL to point to GitHub releases
autoUpdater.setFeedURL({
  provider: 'github',
  owner: 'tdoukinitsas',
  repo: 'liveplay',
  private: false
});

console.log('Auto-updater configured for:', autoUpdater.getFeedURL());

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
  console.log('Falling back to manual update check...');
  
  // Fallback to manual update check
  checkForManualUpdate().then(updateInfo => {
    if (updateInfo && mainWindow) {
      mainWindow.webContents.send('manual-update-available', updateInfo);
    }
  }).catch(fallbackErr => {
    console.error('Fallback update check also failed:', fallbackErr);
    if (mainWindow) {
      mainWindow.webContents.send('update-error', err.message);
    }
  });
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

// Fallback manual update checker using GitHub Pages hosted package.json
async function checkForManualUpdate() {
  return new Promise((resolve, reject) => {
    const currentVersion = app.getVersion();
    const packageJsonUrl = 'https://tdoukinitsas.github.io/liveplay/package.json';
    
    console.log('Checking for updates manually at:', packageJsonUrl);
    console.log('Current version:', currentVersion);
    
    https.get(packageJsonUrl, (res) => {
      let data = '';
      
      res.on('data', (chunk) => {
        data += chunk;
      });
      
      res.on('end', () => {
        try {
          const packageData = JSON.parse(data);
          const latestVersion = packageData.version;
          
          console.log('Latest version from package.json:', latestVersion);
          
          // Simple version comparison
          if (compareVersions(latestVersion, currentVersion) > 0) {
            console.log('New version available:', latestVersion);
            resolve({
              currentVersion,
              newVersion: latestVersion,
              downloadUrl: 'https://tdoukinitsas.github.io/liveplay/',
              isManualUpdate: true
            });
          } else {
            console.log('No update available');
            resolve(null);
          }
        } catch (error) {
          console.error('Error parsing package.json:', error);
          reject(error);
        }
      });
    }).on('error', (error) => {
      console.error('Error fetching package.json:', error);
      reject(error);
    });
  });
}

// Simple version comparison (e.g., "1.2.3" vs "1.2.4")
function compareVersions(v1, v2) {
  const parts1 = v1.split('.').map(Number);
  const parts2 = v2.split('.').map(Number);
  
  for (let i = 0; i < Math.max(parts1.length, parts2.length); i++) {
    const num1 = parts1[i] || 0;
    const num2 = parts2[i] || 0;
    
    if (num1 > num2) return 1;
    if (num1 < num2) return -1;
  }
  
  return 0;
}

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

  // Use the global isDevMode flag
  if (isDevMode) {
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
    if (!isDevMode) {
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

  createMenu('en', isDevMode);
  startAPIServer();
}

// Create state viewer window for debugging
function createStateViewerWindow() {
  if (stateViewerWindow) {
    stateViewerWindow.focus();
    return;
  }

  stateViewerWindow = new BrowserWindow({
    width: 1200,
    height: 800,
    title: 'LivePlay - Current State Viewer',
    icon: path.join(__dirname, '../assets/icons/2x/app_icon_darkmode@2x.png'),
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload-state-viewer.js')
    }
  });

  // Create a simple HTML page for the state viewer
  const stateViewerHTML = `
    <!DOCTYPE html>
    <html>
    <head>
      <meta charset="UTF-8">
      <title>LivePlay State Viewer</title>
      <style>
        * {
          margin: 0;
          padding: 0;
          box-sizing: border-box;
        }
        
        body {
          font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
          background: #1e1e1e;
          color: #d4d4d4;
          overflow: hidden;
          display: flex;
          flex-direction: column;
          height: 100vh;
        }
        
        .header {
          background: #252526;
          padding: 12px 20px;
          border-bottom: 1px solid #3e3e42;
          flex-shrink: 0;
        }
        
        h1 {
          font-size: 16px;
          font-weight: 600;
          color: #cccccc;
        }
        
        .container {
          flex: 1;
          overflow-y: auto;
          padding: 20px;
        }
        
        .state-section {
          margin-bottom: 24px;
          background: #252526;
          border: 1px solid #3e3e42;
          border-radius: 4px;
          overflow: hidden;
        }
        
        .section-header {
          background: #2d2d30;
          padding: 10px 16px;
          font-weight: 600;
          font-size: 13px;
          color: #cccccc;
          border-bottom: 1px solid #3e3e42;
          cursor: pointer;
          user-select: none;
          display: flex;
          justify-content: space-between;
          align-items: center;
        }
        
        .section-header:hover {
          background: #3e3e42;
        }
        
        .collapse-icon {
          font-size: 12px;
          transition: transform 0.2s;
        }
        
        .collapse-icon.collapsed {
          transform: rotate(-90deg);
        }
        
        .section-content {
          padding: 16px;
          overflow: hidden;
        }
        
        .section-content.collapsed {
          display: none;
        }
        
        pre {
          font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
          font-size: 12px;
          line-height: 1.5;
          overflow-x: auto;
          white-space: pre;
        }
        
        /* JSON Syntax Highlighting */
        .json-key {
          color: #9cdcfe;
        }
        
        .json-string {
          color: #ce9178;
        }
        
        .json-number {
          color: #b5cea8;
        }
        
        .json-boolean {
          color: #569cd6;
        }
        
        .json-null {
          color: #569cd6;
        }
        
        .update-time {
          font-size: 11px;
          color: #858585;
          margin-top: 8px;
        }
        
        ::-webkit-scrollbar {
          width: 10px;
          height: 10px;
        }
        
        ::-webkit-scrollbar-track {
          background: #1e1e1e;
        }
        
        ::-webkit-scrollbar-thumb {
          background: #424242;
          border-radius: 5px;
        }
        
        ::-webkit-scrollbar-thumb:hover {
          background: #4e4e4e;
        }
      </style>
    </head>
    <body>
      <div class="header">
        <h1>LivePlay - Current State Viewer (Development Mode)</h1>
      </div>
      <div class="container" id="container"></div>
      
      <script>
        const collapsedSections = new Set();
        const scrollPositions = new Map();
        
        function syntaxHighlight(json) {
          json = JSON.stringify(json, null, 2);
          json = json.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
          return json.replace(/("(\\\\u[a-zA-Z0-9]{4}|\\\\[^u]|[^\\\\"])*"(\\s*:)?|\\b(true|false|null)\\b|-?\\d+(?:\\.\\d*)?(?:[eE][+\\-]?\\d+)?)/g, function (match) {
            let cls = 'json-number';
            if (/^"/.test(match)) {
              if (/:$/.test(match)) {
                cls = 'json-key';
              } else {
                cls = 'json-string';
              }
            } else if (/true|false/.test(match)) {
              cls = 'json-boolean';
            } else if (/null/.test(match)) {
              cls = 'json-null';
            }
            return '<span class="' + cls + '">' + match + '</span>';
          });
        }
        
        function toggleSection(sectionId) {
          const section = document.getElementById(sectionId);
          const icon = document.getElementById(sectionId + '-icon');
          const content = document.getElementById(sectionId + '-content');
          
          if (collapsedSections.has(sectionId)) {
            collapsedSections.delete(sectionId);
            content.classList.remove('collapsed');
            icon.classList.remove('collapsed');
          } else {
            // Save scroll position before collapsing
            scrollPositions.set(sectionId, content.scrollTop);
            collapsedSections.add(sectionId);
            content.classList.add('collapsed');
            icon.classList.add('collapsed');
          }
        }
        
        function updateState(state) {
          console.log('[State Viewer] updateState called with:', Object.keys(state));
          const container = document.getElementById('container');
          if (!container) {
            console.error('[State Viewer] Container not found!');
            return;
          }
          
          const currentScroll = container.scrollTop;
          
          // Save scroll positions for each section
          const sections = container.querySelectorAll('.section-content');
          sections.forEach(section => {
            if (section.id) {
              scrollPositions.set(section.id, section.scrollTop);
            }
          });
          
          let html = '';
          
          for (const [key, value] of Object.entries(state)) {
            const sectionId = 'section-' + key;
            const isCollapsed = collapsedSections.has(sectionId);
            
            html += \`
              <div class="state-section">
                <div class="section-header" onclick="toggleSection('\${sectionId}')">
                  <span>\${key.replace(/([A-Z])/g, ' $1').replace(/^./, str => str.toUpperCase())}</span>
                  <span class="collapse-icon \${isCollapsed ? 'collapsed' : ''}" id="\${sectionId}-icon">▼</span>
                </div>
                <div class="section-content \${isCollapsed ? 'collapsed' : ''}" id="\${sectionId}-content">
                  <pre>\${syntaxHighlight(value)}</pre>
                  <div class="update-time">Last updated: \${new Date().toLocaleTimeString()}</div>
                </div>
              </div>
            \`;
          }
          
          container.innerHTML = html;
          console.log('[State Viewer] Updated DOM with', Object.keys(state).length, 'sections');
          
          // Restore scroll positions
          container.scrollTop = currentScroll;
          scrollPositions.forEach((scrollTop, sectionId) => {
            const section = document.getElementById(sectionId);
            if (section) {
              section.scrollTop = scrollTop;
            }
          });
        }
        
        // Listen for state updates
        window.electronAPI.onStateUpdate((event, state) => {
          console.log('[State Viewer] Received state update:', Object.keys(state));
          updateState(state);
        });
        
        // Initial message
        console.log('[State Viewer] Initialized, waiting for updates...');
        updateState({
          message: 'Waiting for state updates from main application...'
        });
      </script>
    </body>
    </html>
  `;

  stateViewerWindow.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(stateViewerHTML));

  // Make sure the window is ready before we start receiving updates
  stateViewerWindow.webContents.once('did-finish-load', () => {
    console.log('[Main] State viewer window loaded and ready');
  });

  stateViewerWindow.on('closed', () => {
    stateViewerWindow = null;
  });
}

// Translation strings for menu (default: English)
// Dynamically load all locale files from the locales directory
function loadLocaleFiles() {
  const localesDir = path.join(__dirname, '../locales');
  const localeFiles = {};
  
  try {
    // Read all files in the locales directory
    const files = fs.readdirSync(localesDir);
    
    // Filter for JSON files and load them
    files.forEach(file => {
      if (file.endsWith('.json')) {
        const code = file.replace('.json', '');
        try {
          localeFiles[code] = require(path.join(localesDir, file));
          console.log(`Loaded locale: ${code}`);
        } catch (error) {
          console.error(`Failed to load locale ${code}:`, error);
        }
      }
    });
    
    console.log(`Loaded ${Object.keys(localeFiles).length} locale files`);
  } catch (error) {
    console.error('Failed to read locales directory:', error);
    // Fallback to English if directory read fails
    localeFiles.en = require('../locales/en.json');
  }
  
  return localeFiles;
}

const localeFiles = loadLocaleFiles();

// Build menu translations from locale files
const menuTranslations = Object.entries(localeFiles).reduce((acc, [code, data]) => {
  acc[code] = {
    file: data.menu.file,
    newProject: data.menu.newProject,
    openProject: data.menu.openProject,
    saveProject: data.menu.saveProject,
    exportProject: data.menu.exportProject,
    importProject: data.menu.importProject,
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
          label: t.exportProject,
          enabled: currentProject !== null,
          click: () => {
            mainWindow.webContents.send('menu-export-project');
          }
        },
        {
          label: t.importProject,
          click: () => {
            mainWindow.webContents.send('menu-import-project');
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
          {
            label: 'Show Current State',
            accelerator: 'CmdOrCtrl+Shift+D',
            click: () => {
              createStateViewerWindow();
            }
          },
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
  createMenu(locale, isDevMode);
  return { success: true };
});

// Auto-updater IPC handlers
ipcMain.handle('check-for-updates', async () => {
  try {
    console.log('Manual update check requested');
    const result = await autoUpdater.checkForUpdates();
    return { success: true, updateInfo: result?.updateInfo };
  } catch (error) {
    console.error('Check for updates error:', error);
    console.log('Attempting fallback manual update check...');
    
    // Try fallback method
    try {
      const manualUpdateInfo = await checkForManualUpdate();
      if (manualUpdateInfo) {
        return { 
          success: true, 
          isManualUpdate: true,
          updateInfo: manualUpdateInfo 
        };
      } else {
        return { success: true, updateInfo: null };
      }
    } catch (fallbackError) {
      console.error('Fallback update check error:', fallbackError);
      return { success: false, error: error.message };
    }
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

ipcMain.handle('get-available-locales', () => {
  // Return list of available locale codes and metadata
  return Object.keys(localeFiles).map(code => ({
    code,
    name: localeFiles[code]._metadata.nativeName,
    direction: localeFiles[code]._metadata.direction
  }));
});

ipcMain.handle('get-locale-data', (event, localeCode) => {
  // Return the full locale data for a specific locale
  if (localeCode in localeFiles) {
    return localeFiles[localeCode];
  }
  // Fallback to English if locale not found
  return localeFiles.en;
});

ipcMain.handle('set-current-project', async (event, projectPath) => {
  currentProject = projectPath;
  // Rebuild menu to update enabled/disabled state of menu items
  createMenu(currentLocale, isDevMode);
  return { success: true };
});

// Export project to .lpa archive
ipcMain.handle('export-project', async (event, projectFolderPath, projectName = null) => {
  try {
    const archiver = require('archiver');
    // Use provided project name or fall back to folder name
    const defaultName = projectName || path.basename(projectFolderPath);
    
    // Show save dialog for .lpa file
    const result = await dialog.showSaveDialog(mainWindow, {
      title: 'Export Project',
      defaultPath: `${defaultName}.lpa`,
      filters: [
        { name: 'LivePlay Archive', extensions: ['lpa'] }
      ]
    });

    if (result.canceled || !result.filePath) {
      return { success: false, canceled: true };
    }

    const outputPath = result.filePath;
    const fileName = path.basename(outputPath);
    const output = fs.createWriteStream(outputPath);
    const archive = archiver('zip', { zlib: { level: 9 } });

    return new Promise((resolve, reject) => {
      let totalBytes = 0;
      let processedBytes = 0;

      output.on('close', () => {
        event.sender.send('export-progress', { percentage: 100, fileName });
        resolve({
          success: true,
          path: outputPath,
          size: archive.pointer()
        });
      });

      archive.on('error', (err) => {
        reject({ success: false, error: err.message });
      });

      // Track progress by monitoring data being written
      archive.on('data', (chunk) => {
        processedBytes += chunk.length;
        if (totalBytes > 0) {
          const percentage = Math.min(99, Math.round((processedBytes / totalBytes) * 100));
          event.sender.send('export-progress', { percentage, fileName });
        }
      });

      archive.pipe(output);
      
      // Calculate total size first
      const calculateSize = (dirPath) => {
        let size = 0;
        const files = fs.readdirSync(dirPath);
        for (const file of files) {
          const filePath = path.join(dirPath, file);
          const stats = fs.statSync(filePath);
          if (stats.isDirectory()) {
            size += calculateSize(filePath);
          } else {
            size += stats.size;
          }
        }
        return size;
      };
      
      totalBytes = calculateSize(projectFolderPath);
      event.sender.send('export-progress', { percentage: 0, fileName });
      
      // Add the entire project folder to the archive
      archive.directory(projectFolderPath, false);
      
      archive.finalize();
    });
  } catch (error) {
    console.error('Export error:', error);
    return { success: false, error: error.message };
  }
});

// Import project from .lpa archive
ipcMain.handle('import-project', async (event) => {
  try {
    const extractZip = require('extract-zip');
    
    // Show open dialog for .lpa file
    const fileResult = await dialog.showOpenDialog(mainWindow, {
      title: 'Import Project',
      properties: ['openFile'],
      filters: [
        { name: 'LivePlay Archive', extensions: ['lpa'] }
      ]
    });

    if (fileResult.canceled || fileResult.filePaths.length === 0) {
      return { success: false, canceled: true };
    }

    const archivePath = fileResult.filePaths[0];
    const fileName = path.basename(archivePath);

    // Show folder dialog for extraction location
    const folderResult = await dialog.showOpenDialog(mainWindow, {
      title: 'Select Extraction Location',
      properties: ['openDirectory', 'createDirectory']
    });

    if (folderResult.canceled || folderResult.filePaths.length === 0) {
      return { success: false, canceled: true };
    }

    const extractPath = folderResult.filePaths[0];

    // Send initial progress
    event.sender.send('import-progress', { percentage: 0, fileName });

    // Extract the archive with progress updates
    await extractZip(archivePath, { 
      dir: extractPath,
      onEntry: (entry, zipfile) => {
        const percentage = Math.round((zipfile.entriesRead / zipfile.entryCount) * 100);
        event.sender.send('import-progress', { percentage, fileName });
      }
    });

    // Send completion
    event.sender.send('import-progress', { percentage: 100, fileName });

    // Find all .liveplay files in the extracted folder
    const files = fs.readdirSync(extractPath);
    const projectFiles = files.filter(file => file.endsWith('.liveplay'));

    if (projectFiles.length === 0) {
      return { success: false, error: 'No .liveplay file found in archive' };
    }

    // If multiple project files found, return them for user selection
    if (projectFiles.length > 1) {
      return {
        success: true,
        multipleProjects: true,
        projectFiles,
        extractPath
      };
    }

    // Single project file - return its path directly
    const projectPath = path.join(extractPath, projectFiles[0]);

    return {
      success: true,
      projectPath,
      extractPath
    };
  } catch (error) {
    console.error('Import error:', error);
    return { success: false, error: error.message };
  }
});

// Import project from specific .lpa file (for double-click file association)
ipcMain.handle('import-lpa-file', async (event, archivePath) => {
  try {
    const extractZip = require('extract-zip');
    const fileName = path.basename(archivePath);

    // Show folder dialog for extraction location
    const folderResult = await dialog.showOpenDialog(mainWindow, {
      title: 'Select Extraction Location',
      properties: ['openDirectory', 'createDirectory']
    });

    if (folderResult.canceled || folderResult.filePaths.length === 0) {
      return { success: false, canceled: true };
    }

    const extractPath = folderResult.filePaths[0];

    // Send initial progress
    event.sender.send('import-progress', { percentage: 0, fileName });

    // Extract the archive with progress updates
    await extractZip(archivePath, { 
      dir: extractPath,
      onEntry: (entry, zipfile) => {
        const percentage = Math.round((zipfile.entriesRead / zipfile.entryCount) * 100);
        event.sender.send('import-progress', { percentage, fileName });
      }
    });

    // Send completion
    event.sender.send('import-progress', { percentage: 100, fileName });

    // Find all .liveplay files in the extracted folder
    const files = fs.readdirSync(extractPath);
    const projectFiles = files.filter(file => file.endsWith('.liveplay'));

    if (projectFiles.length === 0) {
      return { success: false, error: 'No .liveplay file found in archive' };
    }

    // If multiple project files found, return them for user selection
    if (projectFiles.length > 1) {
      return {
        success: true,
        multipleProjects: true,
        projectFiles,
        extractPath
      };
    }

    // Single project file - return its path directly
    const projectPath = path.join(extractPath, projectFiles[0]);

    return {
      success: true,
      projectPath,
      extractPath
    };
  } catch (error) {
    console.error('Import LPA file error:', error);
    return { success: false, error: error.message };
  }
});

// State viewer: Receive state updates from renderer and forward to state viewer window
ipcMain.on('update-app-state', (event, state) => {
  //console.log('[Main] Received state update, viewer window exists:', !!stateViewerWindow);
  if (stateViewerWindow && !stateViewerWindow.isDestroyed()) {
    // Make sure webContents is ready
    if (stateViewerWindow.webContents && !stateViewerWindow.webContents.isDestroyed()) {
      console.log('[Main] Forwarding state to viewer window');
      stateViewerWindow.webContents.send('state-update', state);
    }
  }
});

// Check if dev mode is enabled
ipcMain.handle('is-dev-mode', () => {
  return isDevMode;
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
              
              // Save waveform data (including duration for convenience)
              const waveformData = {
                peaks: samples,
                sampleRate: 10, // 10 samples per second
                duration: duration // Include duration in seconds
              };
              
              fs.writeFileSync(outputPath, JSON.stringify(waveformData));
              console.log('Waveform generated successfully:', samples.length, 'samples @10/sec for', duration.toFixed(2), 'seconds');
              
              resolve({ success: true });
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

// Register custom protocol for app
if (process.defaultApp) {
  if (process.argv.length >= 2) {
    app.setAsDefaultProtocolClient('liveplay', process.execPath, [path.resolve(process.argv[1])]);
  }
} else {
  app.setAsDefaultProtocolClient('liveplay');
}

// For Windows, we need to handle the protocol differently
const gotTheLock = app.requestSingleInstanceLock();

if (!gotTheLock) {
  app.quit();
} else {
  app.on('second-instance', (event, commandLine) => {
    // Someone tried to run a second instance, we should focus our window
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore();
      mainWindow.focus();
    }
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
}

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
  const fileArg = process.argv.find(arg => arg.endsWith('.liveplay') || arg.endsWith('.lpa'));
  if (fileArg) {
    fileToOpen = fileArg;
  }
}

// Helper function to open a project file
function openFile(filePath) {
  if (!mainWindow) return;
  
  try {
    // Check if it's an .lpa archive file
    if (filePath.endsWith('.lpa')) {
      // Trigger import process for .lpa files
      mainWindow.webContents.send('open-lpa-file', { lpaPath: filePath });
      console.log('Triggering import for .lpa file:', filePath);
      return;
    }
    
    // Handle .liveplay project files
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
