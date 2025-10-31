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
      openFolder: (folderPath: string) => Promise<{ success: boolean; error?: string }>;
      setCurrentProject: (projectPath: string) => Promise<{ success: boolean }>;
      onMenuNewProject: (callback: () => void) => void;
      onMenuOpenProject: (callback: () => void) => void;
      onMenuSaveProject: (callback: () => void) => void;
      onMenuCloseProject: (callback: () => void) => void;
      onMenuOpenProjectFolder: (callback: () => void) => void;
      onMenuToggleDarkMode: (callback: () => void) => void;
      onMenuChangeAccentColor: (callback: () => void) => void;
      onTriggerItem: (callback: (event: any, data: any) => void) => void;
      onStopItem: (callback: (event: any, data: any) => void) => void;
    };
  }

  interface ImportMeta {
    client: boolean;
  }
}
