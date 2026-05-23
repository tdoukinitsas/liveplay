<template>
  <div class="welcome-screen">
    <div class="welcome-container">
      <div class="welcome-header">
        <img 
          :src="isDark ? './assets/icons/SVG/liveplay-icon-darkmode@web.svg' : './assets/icons/SVG/liveplay-icon-lightmode@web.svg'"
          alt="LivePlay"
          class="welcome-logo"
        />
        <div class="welcome-text">
          <h1 class="welcome-title">
            {{ t('welcome.title') }}
            <span class="version-badge">v{{ appVersion }}</span>
          </h1>
          <p class="welcome-subtitle">{{ t('welcome.subtitle') }}</p>
        </div>
      </div>
      
      <div class="welcome-actions">
        <button class="welcome-button primary" @click="handleNewProject">
          <span class="button-icon"><span class="material-symbols-rounded">add</span></span>
          <span>{{ t('welcome.newProject') }}</span>
        </button>
        
        <button class="welcome-button" @click="handleOpenProject">
          <span class="button-icon"><span class="material-symbols-rounded">folder</span></span>
          <span>{{ t('welcome.openProject') }}</span>
        </button>
      </div>
    </div>

    <!-- Server-side file picker. Used to choose either the project's parent
         folder (New) or a .liveplay file (Open). The picker browses the
         server's filesystem (drives, network paths, anywhere). -->
    <ServerFilePickerModal
      :open="showPicker"
      :mode="pickerMode"
      :filter="pickerFilter"
      :filter-options="pickerFilterOptions"
      :start-path="pickerStart"
      @pick="onPickerPick"
      @close="showPicker = false"
    />
  </div>
</template>

<script setup lang="ts">
import ServerFilePickerModal from './ServerFilePickerModal.vue';

const { createNewProject, openProject } = useProject();
const { t } = useLocalization();

// Server file picker state — shared by New and Open flows.
const showPicker          = ref(false);
const pickerMode          = ref<'file' | 'directory'>('directory');
const pickerFilter        = ref<string>('.liveplay,.lpa');
const pickerFilterOptions = ref<string[]>(['.liveplay,.lpa', 'all']);
const pickerStart         = ref<string>('');
const pickerIntent        = ref<'new' | 'open'>('open');

// Get app version
const appVersion = ref('1.1.3');
onMounted(async () => {
  if (import.meta.client && window.electronAPI?.getAppVersion) {
    appVersion.value = await window.electronAPI.getAppVersion();
  }
});

// Get theme from app state (works even when no project is open)
const theme = useState('theme', () => 'dark');
const isDark = computed(() => theme.value === 'dark');

const handleNewProject = () => {
  // Browse the server's filesystem for a parent folder to scaffold into.
  pickerIntent.value        = 'new';
  pickerMode.value          = 'directory';
  pickerFilter.value        = 'all';
  pickerFilterOptions.value = ['all'];
  pickerStart.value         = '';
  showPicker.value          = true;
};

const handleOpenProject = () => {
  // Browse for a .liveplay file anywhere on the server's filesystem.
  pickerIntent.value        = 'open';
  pickerMode.value          = 'file';
  pickerFilter.value        = '.liveplay,.lpa';
  pickerFilterOptions.value = ['.liveplay,.lpa', 'all'];
  pickerStart.value         = '';
  showPicker.value          = true;
};

const onPickerPick = async (fullPath: string) => {
  showPicker.value = false;
  if (!fullPath) return;

  if (pickerIntent.value === 'new') {
    const projectName = await getProjectName();
    if (!projectName) return;
    const ok = await createNewProject(projectName, fullPath);
    if (!ok) alert('Failed to create project');
  } else {
    const ok = await openProject(fullPath);
    if (!ok) alert('Failed to open project');
  }
};

// Simple inline project name dialog
const getProjectName = (): Promise<string | null> => {
  return new Promise((resolve) => {
    const overlay = document.createElement('div');
    overlay.className = 'modal-overlay';

    const dialog = document.createElement('div');
    dialog.className = 'modal-dialog';

    const h3 = document.createElement('h3');
    h3.textContent = t('project.enterName');
    h3.className = 'modal-title';

    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'modal-input';
    input.placeholder = t('project.placeholder');

    const buttonContainer = document.createElement('div');
    buttonContainer.className = 'modal-buttons';

    const cancelBtn = document.createElement('button');
    cancelBtn.className = 'modal-btn modal-btn-cancel';
    cancelBtn.textContent = t('project.cancel');

    const okBtn = document.createElement('button');
    okBtn.className = 'modal-btn modal-btn-primary';
    okBtn.textContent = t('project.ok');

    buttonContainer.appendChild(cancelBtn);
    buttonContainer.appendChild(okBtn);
    
    dialog.appendChild(h3);
    dialog.appendChild(input);
    dialog.appendChild(buttonContainer);
    overlay.appendChild(dialog);
    
    // Append to #app instead of body to inherit theme variables
    const appElement = document.getElementById('app') || document.body;
    appElement.appendChild(overlay);

    input.focus();

    const cleanup = () => {
      appElement.removeChild(overlay);
    };

    okBtn.onclick = () => {
      const value = input.value.trim();
      cleanup();
      resolve(value || null);
    };

    cancelBtn.onclick = () => {
      cleanup();
      resolve(null);
    };

    input.onkeydown = (e) => {
      if (e.key === 'Enter') {
        okBtn.click();
      } else if (e.key === 'Escape') {
        cancelBtn.click();
      }
    };
  });
};

// Listen for menu events
if (import.meta.client && window.electronAPI) {
  window.electronAPI.onMenuNewProject(() => {
    handleNewProject();
  });

  window.electronAPI.onMenuOpenProject(() => {
    handleOpenProject();
  });
}
</script>

<style scoped>
.welcome-screen {
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(135deg, var(--color-background) 0%, var(--color-surface) 100%);
}

.welcome-container {
  text-align: center;
  max-width: 600px;
  padding: var(--spacing-xxl);
}

.welcome-header {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-lg);
  margin-bottom: var(--spacing-xxl);
}

.welcome-logo {
  width: 80px;
  height: 80px;
  object-fit: contain;
}

.welcome-text {
  text-align: left;
}

.welcome-title {
  font-size: 64px;
  font-weight: 600;
  margin-bottom: var(--spacing-xs);
  color: var(--color-text-primary);
  letter-spacing: -2px;
  line-height: 1;
  display: flex;
  align-items: baseline;
  gap: var(--spacing-sm);
}

.version-badge {
  font-size: 16px;
  font-weight: 400;
  color: var(--color-text-secondary);
  opacity: 0.6;
  letter-spacing: 0;
}

.welcome-subtitle {
  font-size: 20px;
  color: var(--color-text-secondary);
  margin: 0;
}

.welcome-actions {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-md);
}

.welcome-button {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-md);
  padding: var(--spacing-lg) var(--spacing-xl);
  font-size: 18px;
  font-weight: 500;
  background-color: var(--color-surface);
  border: 2px solid var(--color-border);
  border-radius: var(--border-radius-lg);
  transition: all var(--transition-base);

  &:hover {
    background-color: var(--color-surface-hover);
    border-color: var(--color-accent);
    transform: translateY(-2px);
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.1);
  }

  &.primary {
    background-color: var(--color-accent);
    border-color: var(--color-accent);
    color: white;

    &:hover {
      background-color: var(--color-accent-hover);
      border-color: var(--color-accent-hover);
    }
  }
}

.button-icon {
  font-size: 24px;
}

/* Modal styles - not scoped since appended to body */
:global(.modal-overlay) {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 10000;
}

:global(.modal-dialog) {
  background: var(--color-surface);
  padding: var(--spacing-xl);
  border-radius: var(--border-radius-lg);
  min-width: 400px;
  border: 1px solid var(--color-border);
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
}

:global(.modal-title) {
  margin: 0 0 var(--spacing-md) 0;
  color: var(--color-text-primary);
  font-size: 18px;
  font-weight: 600;
}

:global(.modal-input) {
  width: 100%;
  padding: var(--spacing-sm) var(--spacing-md);
  margin-bottom: var(--spacing-md);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-primary);
  font-size: 14px;
  box-sizing: border-box;
  outline: none;

  &:focus {
    border-color: var(--color-accent);
  }
}

:global(.modal-buttons) {
  display: flex;
  gap: var(--spacing-sm);
  justify-content: flex-end;
}

:global(.modal-btn) {
  padding: var(--spacing-sm) var(--spacing-md);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
  font-size: 14px;
  transition: all var(--transition-fast);
  outline: none;

  &:active {
    transform: translateY(1px);
  }
}

:global(.modal-btn-cancel) {
  background: var(--color-background);
  border: 1px solid var(--color-border);
  color: var(--color-text-primary);

  &:hover {
    background: var(--color-surface-hover);
  }
}

:global(.modal-btn-primary) {
  background: var(--color-accent);
  border: 1px solid var(--color-accent);
  color: white;
  font-weight: 500;

  &:hover {
    background: var(--color-accent-hover);
    border-color: var(--color-accent-hover);
  }
}
</style>
