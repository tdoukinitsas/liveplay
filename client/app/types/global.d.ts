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
      writeBinaryFile: (filePath: string, data: ArrayBuffer | Uint8Array | number[]) => Promise<{ success: boolean; error?: string }>;
      showSaveArchiveDialog: (defaultName?: string) => Promise<string | null>;
      showOpenArchiveDialog: () => Promise<string | null>;
      copyFile: (source: string, destination: string) => Promise<{ success: boolean; error?: string }>;
      ensureDirectory: (dirPath: string) => Promise<{ success: boolean; error?: string }>;
      generateWaveform: (audioPath: string, outputPath: string) => Promise<{ success: boolean; error?: string }>;
      openFolder: (folderPath: string) => Promise<{ success: boolean; error?: string }>;
      setCurrentProject: (projectPath: string) => Promise<{ success: boolean }>;
      exportProject: (projectFolderPath: string, projectName?: string) => Promise<{ success: boolean; path?: string; size?: number; canceled?: boolean; error?: string }>;
      importProject: () => Promise<{ 
        success: boolean; 
        projectPath?: string; 
        extractPath?: string; 
        multipleProjects?: boolean;
        projectFiles?: string[];
        canceled?: boolean; 
        error?: string 
      }>;
      importLpaFile: (lpaPath: string) => Promise<{ 
        success: boolean; 
        projectPath?: string; 
        extractPath?: string; 
        multipleProjects?: boolean;
        projectFiles?: string[];
        canceled?: boolean; 
        error?: string 
      }>;
      onExportProgress: (callback: (event: any, data: { percentage: number; fileName: string }) => void) => void;
      onImportProgress: (callback: (event: any, data: { percentage: number; fileName: string }) => void) => void;
      removeExportProgressListener: (callback: (event: any, data: { percentage: number; fileName: string }) => void) => void;
      removeImportProgressListener: (callback: (event: any, data: { percentage: number; fileName: string }) => void) => void;
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
      onMenuExportProject: (callback: () => void) => void;
      onMenuImportProject: (callback: () => void) => void;
      onMenuCloseProject: (callback: () => void) => void;
      onMenuOpenRecentProject: (callback: (event: any, projectPath: string) => void) => void;
      onMenuOpenProjectFolder: (callback: () => void) => void;
      onMenuToggleDarkMode: (callback: () => void) => void;
      onMenuChangeAccentColor: (callback: () => void) => void;
      onMenuChangeLanguage: (callback: (event: any, locale: string) => void) => void;
      onMenuShowAbout: (callback: () => void) => void;
      openExternal: (url: string) => Promise<void>;
      updateMenuLanguage: (locale: string) => Promise<{ success: boolean }>;
      getSystemLocale: () => Promise<string>;
      getAvailableLocales: () => Promise<Array<{ code: string; name: string; direction: string }>>;
      getLocaleData: (localeCode: string) => Promise<any>;
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
      onTriggerCartSlot: (callback: (event: any, data: { slot: number }) => void) => void;
      onStopAllCues: (callback: (event: any, data: any) => void) => void;
      syncProjectData: (data: any) => void;
      sendApiResponse: (data: any) => void;
      onApiUpdateItem: (callback: (event: any, data: { requestId: string; id: string; updates: Record<string, any> }) => void) => void;
      onApiUpdateCartItem: (callback: (event: any, data: { requestId: string; slot: number; updates: Record<string, any> }) => void) => void;
      onOpenFileAssociation: (callback: (event: any, data: { filePath: string; kind: 'liveplay' | 'lpa' }) => void) => void;
      getPendingOpenFile: () => Promise<{ filePath: string; kind: 'liveplay' | 'lpa' } | null>;
      readMidiConfig: () => Promise<Record<string, any>>;
      writeMidiConfig: (config: Record<string, any>) => Promise<{ success: boolean }>;
      // Cart player window
      openCartPlayerWindow: (projectFolderPath: string) => Promise<void>;
      attachCartPlayerWindow: () => void;
      getCartWindowProjectData: () => Promise<any>;
      onCartPlayerWindowOpened: (callback: () => void) => void;
      onCartPlayerWindowClosed: (callback: () => void) => void;
      onCartWindowProjectUpdate: (callback: (event: any, projectData: any) => void) => void;
      // Recent-projects history (last 10 .liveplay files opened on this client).
      liveplayProjects?: {
        recentList: () => Promise<Array<{ path: string; name: string; folderPath: string; lastOpened: number }>>;
        recentAdd: (entry: { path: string; name?: string; folderPath?: string }) => Promise<Array<{ path: string; name: string; folderPath: string; lastOpened: number }>>;
        recentRemove: (path: string) => Promise<Array<{ path: string; name: string; folderPath: string; lastOpened: number }>>;
        recentClear: () => Promise<Array<never>>;
      };
    };
  }

  interface ImportMeta {
    client: boolean;
  }
}
