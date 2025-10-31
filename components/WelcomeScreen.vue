<template>
  <div class="welcome-screen">
    <div class="welcome-container">
      <h1 class="welcome-title">LivePlay</h1>
      <p class="welcome-subtitle">Audio Cue Playback System</p>
      
      <div class="welcome-actions">
        <button class="welcome-button primary" @click="handleNewProject">
          <span class="button-icon">+</span>
          <span>Create New Project</span>
        </button>
        
        <button class="welcome-button" @click="handleOpenProject">
          <span class="button-icon">📁</span>
          <span>Open Existing Project</span>
        </button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const { createNewProject, openProject } = useProject();

const handleNewProject = async () => {
  if (!import.meta.client || !window.electronAPI) return;

  const folderPath = await window.electronAPI.selectProjectFolder();
  if (!folderPath) return;

  // Get project name from user - using a simple component since prompt() doesn't work in Electron
  const projectName = await getProjectName();
  if (!projectName) return;

  const success = await createNewProject(projectName, folderPath);
  if (!success) {
    alert('Failed to create project');
  }
};

const handleOpenProject = async () => {
  if (!import.meta.client || !window.electronAPI) return;

  const projectFilePath = await window.electronAPI.selectProjectFile();
  if (!projectFilePath) return;

  const success = await openProject(projectFilePath);
  if (!success) {
    alert('Failed to open project');
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
    h3.textContent = 'Enter Project Name';
    h3.className = 'modal-title';

    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'modal-input';
    input.placeholder = 'My Project';

    const buttonContainer = document.createElement('div');
    buttonContainer.className = 'modal-buttons';

    const cancelBtn = document.createElement('button');
    cancelBtn.className = 'modal-btn modal-btn-cancel';
    cancelBtn.textContent = 'Cancel';

    const okBtn = document.createElement('button');
    okBtn.className = 'modal-btn modal-btn-primary';
    okBtn.textContent = 'OK';

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

.welcome-title {
  font-size: 64px;
  font-weight: 600;
  margin-bottom: var(--spacing-md);
  color: var(--color-text-primary);
  letter-spacing: -2px;
}

.welcome-subtitle {
  font-size: 20px;
  color: var(--color-text-secondary);
  margin-bottom: var(--spacing-xxl);
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
