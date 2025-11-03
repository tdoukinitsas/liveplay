export {};

declare global {
  interface Window {
    electronAPI: {
      selectProjectFolder: () => Promise<string | null>;
      selectProjectFile: () => Promise<string | null>;
      selectAudioFiles: () => Promise<string[] | null>;
      readFile: (filePath: string) => Promise<{ success: boolean; data?: string; error?: string }>;
      readAudioFile: (filePath: string) => Promise<{ success: boolean; data?: number[]; error?: string }>;
      writeFile: (filePath: string, data: string) => Promise<{ success: boolean; error?: string }>;
      copyFile: (source: string, destination: string) => Promise<{ success: boolean; error?: string }>;
      ensureDirectory: (dirPath: string) => Promise<{ success: boolean; error?: string }>;
      generateWaveform: (audioPath: string, outputPath: string) => Promise<{ success: boolean; error?: string }>;
      openFolder: (folderPath: string) => Promise<{ success: boolean; error?: string }>;
      setCurrentProject: (projectPath: string) => Promise<{ success: boolean }>;
      getFilePath: (file: File) => string | null;
      checkFfmpeg: () => Promise<{ available: boolean; path: string | null }>;
      searchYouTube: (query: string) => Promise<Array<{
        id: string;
        title: string;
        thumbnail: string;
        channelTitle: string;
        length?: string;
      }>>;
      downloadYouTubeAudio: (
        videoId: string,
        title: string,
        projectFolderPath: string,
        progressCallback?: (progress: { videoId: string; percentage: number; status: string }) => void
      ) => Promise<{ success: boolean; file: string; fileName: string; title: string }>;
      onMenuNewProject: (callback: () => void) => void;
      onMenuOpenProject: (callback: () => void) => void;
      onMenuSaveProject: (callback: () => void) => void;
      onMenuCloseProject: (callback: () => void) => void;
      onMenuOpenProjectFolder: (callback: () => void) => void;
      onMenuToggleDarkMode: (callback: () => void) => void;
      onMenuChangeAccentColor: (callback: () => void) => void;
      onMenuChangeLanguage: (callback: (event: any, locale: string) => void) => void;
      onMenuShowAbout: (callback: () => void) => void;
      openExternal: (url: string) => Promise<void>;
      updateMenuLanguage: (locale: string) => Promise<{ success: boolean }>;
      getSystemLocale: () => Promise<string>;
      checkForUpdates: () => Promise<{ success: boolean; updateInfo?: any; error?: string; isManualUpdate?: boolean }>;
      downloadUpdate: () => Promise<{ success: boolean; error?: string }>;
      installUpdate: () => void;
      getAppVersion: () => Promise<string>;
      onUpdateAvailable: (callback: (event: any, info: { currentVersion: string; newVersion: string; releaseNotes?: string; releaseDate?: string }) => void) => void;
      onUpdateDownloadProgress: (callback: (event: any, progress: { percent: number; transferred: number; total: number }) => void) => void;
      onUpdateDownloaded: (callback: (event: any, info: { version: string }) => void) => void;
      onUpdateError: (callback: (event: any, error: string) => void) => void;
      onManualUpdateAvailable: (callback: (event: any, info: { currentVersion: string; newVersion: string; downloadUrl: string; isManualUpdate: boolean }) => void) => void;
      onTriggerItem: (callback: (event: any, data: any) => void) => void;
      onStopItem: (callback: (event: any, data: any) => void) => void;
      onOpenProjectFile: (callback: (event: any, data: { filePath: string; projectData: any }) => void) => void;
    };
  }

  interface ImportMeta {
    client: boolean;
  }
}
