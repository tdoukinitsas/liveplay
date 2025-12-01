<template>
  <div id="app" :data-theme="theme">
    <WelcomeScreen v-if="!currentProject" />
    <MainWorkspace v-else />
    
    <!-- Accent Color Picker Modal -->
    <div v-if="showColorPicker" class="color-picker-overlay" @click="showColorPicker = false">
      <div class="color-picker-dialog" @click.stop>
        <h3>Choose Accent Color</h3>
        <div class="color-grid">
          <button
            v-for="color in accentColors"
            :key="color"
            class="color-option"
            :style="{ backgroundColor: color }"
            @click="changeAccentColor(color)"
          ></button>
        </div>
        <button class="close-dialog" @click="showColorPicker = false">Cancel</button>
      </div>
    </div>
    
    <!-- About Modal -->
    <AboutModal v-if="showAboutModal" @close="showAboutModal = false" />
    
    <!-- Update Modal -->
    <UpdateModal
      v-if="showUpdateModal"
      :current-version="updateInfo.currentVersion"
      :new-version="updateInfo.newVersion"
      :release-notes="updateInfo.releaseNotes"
      :release-date="updateInfo.releaseDate"
      :is-manual-update="updateInfo.isManualUpdate"
      :download-url="updateInfo.downloadUrl"
      @close="showUpdateModal = false"
    />
    
    <!-- Progress Modal for Import/Export -->
    <ProgressModal
      :visible="progressModal.visible"
      :title="progressModal.title"
      :message="progressModal.message"
      :percentage="progressModal.percentage"
    />
    
    <!-- Project Selection Modal -->
    <ProjectSelectionModal
      :visible="showProjectSelection"
      :projects="availableProjects"
      @select="handleProjectSelection"
      @cancel="handleProjectSelectionCancel"
    />
  </div>
</template>

<script setup lang="ts">
import 'material-symbols';

const { currentProject, saveProject, openProject } = useProject();
const { currentLocale, setLocale, getDirection, t } = useLocalization();
const theme = useState('theme', () => 'dark');

// Initialize state viewer for dev mode
useStateViewer();

// Progress modal state
const progressModal = ref({
  visible: false,
  title: '',
  message: '',
  percentage: 0
});

// Project selection modal state
const showProjectSelection = ref(false);
const availableProjects = ref<string[]>([]);
const pendingImportPath = ref<string>('');

// Color picker for accent color
const showColorPicker = ref(false);

// About modal
const showAboutModal = ref(false);

// Update modal
const showUpdateModal = ref(false);
const updateInfo = ref({
  currentVersion: '',
  newVersion: '',
  releaseNotes: '',
  releaseDate: '',
  isManualUpdate: false,
  downloadUrl: ''
});

const accentColors = [
  '#0f62fe', '#0353e9', '#002d9c', // Blues
  '#da1e28', '#a2191f', '#750e13', // Reds
  '#24a148', '#198038', '#0e6027', // Greens
  '#f1c21b', '#d2a106', '#b28600', // Yellows
  '#8a3ffc', '#6929c4', '#491d8b', // Purples
  '#ff7eb6', '#ee5396', '#d02670', // Pinks
];

// Listen to menu events
onMounted(() => {
  if (import.meta.client && window.electronAPI) {
    window.electronAPI.onMenuToggleDarkMode(() => {
      theme.value = theme.value === 'dark' ? 'light' : 'dark';
      if (currentProject.value) {
        currentProject.value.theme.mode = theme.value as 'dark' | 'light';
        saveProject();
      }
    });

    window.electronAPI.onMenuChangeAccentColor(() => {
      showColorPicker.value = true;
    });

    window.electronAPI.onMenuChangeLanguage((event: any, locale: string) => {
      setLocale(locale);
    });
    
    window.electronAPI.onMenuShowAbout(() => {
      showAboutModal.value = true;
    });
    
    // Handle import project (works even on welcome screen)
    window.electronAPI.onMenuImportProject(async () => {
      try {
        // Set up progress listener
        const progressListener = (_event: any, data: { percentage: number; fileName: string }) => {
          progressModal.value = {
            visible: true,
            title: t('importProgress.title'),
            message: `${t('importProgress.message')} ${data.fileName}...`,
            percentage: data.percentage
          };
        };
        
        window.electronAPI.onImportProgress(progressListener);
        
        const result = await window.electronAPI.importProject();
        
        // Clean up listener
        window.electronAPI.removeImportProgressListener(progressListener);
        
        // Hide modal
        progressModal.value.visible = false;
        
        if (result.success) {
          // Handle multiple projects
          if (result.multipleProjects && result.projectFiles) {
            availableProjects.value = result.projectFiles;
            pendingImportPath.value = result.extractPath;
            showProjectSelection.value = true;
          } else if (result.projectPath) {
            // Single project - open directly
            await openProject(result.projectPath);
          }
        }
      } catch (error) {
        console.error('Import failed:', error);
        progressModal.value.visible = false;
      }
    });

    // Listen for update events
    window.electronAPI.onUpdateAvailable((event: any, info: any) => {
      updateInfo.value = info;
      showUpdateModal.value = true;
    });
    
    // Listen for manual update events (fallback)
    window.electronAPI.onManualUpdateAvailable((event: any, info: any) => {
      updateInfo.value = info;
      showUpdateModal.value = true;
    });
    
    // Listen for project file opening (from file association)
    window.electronAPI.onOpenProjectFile((event: any, data: { filePath: string, projectData: any }) => {
      try {
        currentProject.value = data.projectData;
        console.log('Opened project from file association:', data.filePath);
      } catch (error) {
        console.error('Failed to open project file:', error);
      }
    });

    // Sync menu with current UI language on startup
    window.electronAPI.updateMenuLanguage(currentLocale.value);
  }
});

const changeAccentColor = (color: string) => {
  if (currentProject.value) {
    currentProject.value.theme.accentColor = color;
    document.documentElement.style.setProperty('--color-accent-custom', color);
    saveProject();
    showColorPicker.value = false;
  }
};

// Handle project selection from multiple projects
const handleProjectSelection = async (projectName: string) => {
  showProjectSelection.value = false;
  const projectPath = `${pendingImportPath.value}/${projectName}`;
  await openProject(projectPath);
  pendingImportPath.value = '';
  availableProjects.value = [];
};

const handleProjectSelectionCancel = () => {
  showProjectSelection.value = false;
  pendingImportPath.value = '';
  availableProjects.value = [];
};

// Set initial theme from project
watch(currentProject, (project) => {
  if (project) {
    theme.value = project.theme.mode;
    
    // Set accent color
    if (import.meta.client && project.theme.accentColor) {
      document.documentElement.style.setProperty('--color-accent-custom', project.theme.accentColor);
    }
  }
}, { immediate: true });

// Apply RTL direction when locale changes
watch(currentLocale, () => {
  if (import.meta.client) {
    const direction = getDirection();
    document.documentElement.setAttribute('dir', direction);
  }
}, { immediate: true });

// Prevent default drag and drop behavior globally
// (except where specifically handled by components)
onMounted(() => {
  if (import.meta.client) {
    // Prevent default drag/drop on document to avoid "stop" cursor
    // But allow it if a component has already handled it
    document.addEventListener('dragover', (e) => {
      // Only prevent if the event hasn't been handled by a component
      if (e.defaultPrevented) return;
      e.preventDefault();
      e.dataTransfer!.dropEffect = 'none';
    }, true); // Use capture phase
    
    document.addEventListener('drop', (e) => {
      // Only prevent if the event hasn't been handled by a component
      if (e.defaultPrevented) return;
      e.preventDefault();
    }, true); // Use capture phase
  }
});
</script>

<style scoped>
#app {
  width: 100%;
  height: 100%;
  overflow: hidden;
}

.color-picker-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: var(--z-modal);
}

.color-picker-dialog {
  background: var(--color-surface);
  padding: var(--spacing-xl);
  border-radius: var(--border-radius-lg);
  min-width: 400px;
  color: var(--color-text-primary);
}

.color-picker-dialog h3 {
  margin-bottom: var(--spacing-md);
  color: var(--color-text-primary);
}

.color-grid {
  display: grid;
  grid-template-columns: repeat(6, 1fr);
  gap: var(--spacing-sm);
  margin-bottom: var(--spacing-md);
}

.color-option {
  width: 50px;
  height: 50px;
  border: 2px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
  transition: transform var(--transition-fast);
}

.color-option:hover {
  transform: scale(1.1);
  border-color: var(--color-text-primary);
}

.close-dialog {
  width: 100%;
  padding: var(--spacing-sm) var(--spacing-md);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-primary);
}

.close-dialog:hover {
  background: var(--color-surface-hover);
}
</style>
