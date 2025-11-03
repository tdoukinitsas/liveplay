import { v4 as uuidv4 } from 'uuid';
import { triggerRef } from 'vue';
import type { 
  Project, 
  AudioItem, 
  GroupItem, 
  BaseItem,
  Theme,
  CartItem
} from '~/types/project';
import { DEFAULT_THEME } from '~/types/project';

export const useProject = () => {
  const currentProject = useState<Project | null>('currentProject', () => null);
  const selectedItem = useState<BaseItem | null>('selectedItem', () => null);
  const selectedItems = useState<Set<string>>('selectedItems', () => new Set()); // Track multiple selections by UUID
  const activeCues = useState<Map<string, any>>('activeCues', () => new Map());
  const waveformUpdateKey = useState<number>('waveformUpdateKey', () => 0);

  // Force UI update for waveforms
  const triggerWaveformUpdate = () => {
    waveformUpdateKey.value++;
  };

  // Multi-select helpers
  const toggleItemSelection = (uuid: string, isCtrlKey: boolean, isShiftKey: boolean) => {
    if (!currentProject.value) return;

    if (isShiftKey && selectedItems.value.size > 0) {
      // Shift-click: select range
      const allItems = getAllItemsFlat(currentProject.value.items);
      const lastSelectedUuid = Array.from(selectedItems.value).pop();
      const lastIndex = allItems.findIndex(item => item.uuid === lastSelectedUuid);
      const currentIndex = allItems.findIndex(item => item.uuid === uuid);
      
      if (lastIndex !== -1 && currentIndex !== -1) {
        const start = Math.min(lastIndex, currentIndex);
        const end = Math.max(lastIndex, currentIndex);
        
        for (let i = start; i <= end; i++) {
          selectedItems.value.add(allItems[i].uuid);
        }
      }
    } else if (isCtrlKey) {
      // Ctrl-click: toggle individual item
      if (selectedItems.value.has(uuid)) {
        selectedItems.value.delete(uuid);
      } else {
        selectedItems.value.add(uuid);
      }
    } else {
      // Normal click: select only this item
      selectedItems.value.clear();
      selectedItems.value.add(uuid);
    }

    // Update single selectedItem for backward compatibility
    if (selectedItems.value.size === 1) {
      const uuid = Array.from(selectedItems.value)[0];
      selectedItem.value = findItemByUuid(uuid);
    } else if (selectedItems.value.size > 1) {
      // Multiple selection - keep selectedItem for properties panel
      selectedItem.value = findItemByUuid(uuid); // Use the last clicked item
    } else {
      selectedItem.value = null;
    }
  };

  // Get all items as a flat array (for shift-select)
  const getAllItemsFlat = (items: (AudioItem | GroupItem)[]): BaseItem[] => {
    const result: BaseItem[] = [];
    for (const item of items) {
      result.push(item);
      if (item.type === 'group') {
        result.push(...getAllItemsFlat(item.children));
      }
    }
    return result;
  };

  // Get all selected items
  const getSelectedItems = (): BaseItem[] => {
    return Array.from(selectedItems.value)
      .map(uuid => findItemByUuid(uuid))
      .filter(item => item !== null) as BaseItem[];
  };

  // Create a new project
  const createNewProject = async (name: string, folderPath: string): Promise<boolean> => {
    try {
      // Create project structure
      const mediaPath = `${folderPath}/media`;
      const waveformsPath = `${folderPath}/waveforms`;

      if (import.meta.client && window.electronAPI) {
        await window.electronAPI.ensureDirectory(mediaPath);
        await window.electronAPI.ensureDirectory(waveformsPath);
      }

      const newProject: Project = {
        name,
        version: '1.0.0',
        folderPath,
        items: [],
        cartItems: [],
        cartOnlyItems: [],
        theme: { ...DEFAULT_THEME },
        createdAt: new Date().toISOString(),
        lastModified: new Date().toISOString()
      };

      // Save project file
      const projectFilePath = `${folderPath}/${name}.liveplay`;
      if (import.meta.client && window.electronAPI) {
        await window.electronAPI.writeFile(
          projectFilePath,
          JSON.stringify(newProject, null, 2)
        );
        await window.electronAPI.setCurrentProject(projectFilePath);
      }

      currentProject.value = newProject;
      return true;
    } catch (error) {
      console.error('Error creating project:', error);
      return false;
    }
  };

  // Open an existing project
  const openProject = async (projectFilePath: string): Promise<boolean> => {
    try {
      if (import.meta.client && window.electronAPI) {
        const result = await window.electronAPI.readFile(projectFilePath);
        if (result.success) {
          const project: Project = JSON.parse(result.data);
          
          // Set folderPath from the project file location
          // Extract the directory path from the .liveplay file path
          // Handle both forward slashes (Unix) and backslashes (Windows)
          const normalizedPath = projectFilePath.replace(/\\/g, '/');
          const folderPath = normalizedPath.substring(0, normalizedPath.lastIndexOf('/'));
          project.folderPath = folderPath;
          
          console.log('Opening project from:', projectFilePath);
          console.log('Project folder path set to:', folderPath);
          
          // Migrate project to ensure new properties exist
          migrateProject(project);
          
          currentProject.value = project;
          await window.electronAPI.setCurrentProject(projectFilePath);
          
          // Restore cart-only items to memory
          const { clearCartOnlyItems, addCartOnlyItem } = useCartItems();
          clearCartOnlyItems();
          if (project.cartOnlyItems && project.cartOnlyItems.length > 0) {
            for (const item of project.cartOnlyItems) {
              addCartOnlyItem(item);
            }
          }
          
          // Load waveforms from disk asynchronously for all audio items
          loadWaveformsAsync(project);
          
          return true;
        }
      }
      return false;
    } catch (error) {
      console.error('Error opening project:', error);
      return false;
    }
  };

  // Migrate project to add new properties for backwards compatibility
  const migrateProject = (project: Project) => {
    // Add cartOnlyItems if missing
    if (!project.cartOnlyItems) {
      project.cartOnlyItems = [];
    }
    
    const migrateItem = (item: BaseItem) => {
      if (item.type === 'audio') {
        const audioItem = item as AudioItem;
        
        // Add fadeOutDuration if missing
        if (audioItem.fadeOutDuration === undefined) {
          audioItem.fadeOutDuration = 1.0;
        }
        
        // Add ducking fade times if missing
        if (audioItem.duckingBehavior) {
          if (audioItem.duckingBehavior.duckFadeIn === undefined) {
            audioItem.duckingBehavior.duckFadeIn = 0.25;
          }
          if (audioItem.duckingBehavior.duckFadeOut === undefined) {
            audioItem.duckingBehavior.duckFadeOut = 1.0;
          }
        }
      } else if (item.type === 'group') {
        const groupItem = item as GroupItem;
        for (const child of groupItem.children) {
          migrateItem(child);
        }
      }
    };
    
    // Migrate all items
    for (const item of project.items) {
      migrateItem(item);
    }
  };

  // Load waveforms from disk asynchronously
  const loadWaveformsAsync = async (project: Project) => {
    const loadWaveformForItem = async (item: BaseItem) => {
      if (item.type === 'audio') {
        const audioItem = item as AudioItem;
        
        // Skip if waveform already loaded and valid
        if (audioItem.waveform && audioItem.waveform.peaks && audioItem.waveform.peaks.length > 0) {
          return;
        }
        
        try {
          const result = await window.electronAPI.readFile(audioItem.waveformPath);
          if (result.success && result.data) {
            const waveformData = JSON.parse(result.data);
            
            // Validate waveform format
            if (waveformData.peaks && waveformData.peaks.length && waveformData.duration) {
              audioItem.waveform = waveformData;
            } else {
              console.warn(`Invalid waveform format for ${audioItem.displayName}, regenerating...`);
              regenerateWaveform(audioItem, project);
            }
          } else {
            // No waveform file exists, generate it
            regenerateWaveform(audioItem, project);
          }
        } catch (error) {
          console.warn(`Failed to load waveform for ${audioItem.displayName}`, error);
          regenerateWaveform(audioItem, project);
        }
      } else if (item.type === 'group') {
        // Recursively load waveforms for group children
        const groupItem = item as GroupItem;
        for (const child of groupItem.children) {
          await loadWaveformForItem(child);
        }
      }
    };
    
    // Load all waveforms
    for (const item of project.items) {
      loadWaveformForItem(item);
    }
  };

  // Regenerate waveform using ffmpeg
  const regenerateWaveform = async (audioItem: AudioItem, project: Project) => {
    try {
      // Check if generateWaveform is available
      if (!window.electronAPI.generateWaveform) {
        console.warn('generateWaveform not implemented yet - waveform will not be regenerated');
        return;
      }

      const mediaPath = `${project.folderPath}/media/${audioItem.mediaFileName}`;
      
      // Generate waveform using ffmpeg (non-blocking)
      const result = await window.electronAPI.generateWaveform(mediaPath, audioItem.waveformPath);
      
      if (result.success) {
        // Load the generated waveform
        const waveformFile = await window.electronAPI.readFile(audioItem.waveformPath);
        if (waveformFile.success && waveformFile.data) {
          audioItem.waveform = JSON.parse(waveformFile.data);
          console.log(`Regenerated waveform for ${audioItem.displayName}`);
          
          // Manually trigger reactivity on the project ref
          if (currentProject.value === project) {
            triggerRef(currentProject);
          }
        }
      } else {
        console.error(`Failed to generate waveform for ${audioItem.displayName}:`, result.error);
      }
    } catch (error) {
      console.error(`Failed to regenerate waveform for ${audioItem.displayName}:`, error);
    }
  };

  // Save the current project
  const saveProject = async (): Promise<boolean> => {
    try {
      if (!currentProject.value) return false;

      // Save cart-only items from memory to project
      const { cartOnlyItems } = useCartItems();
      currentProject.value.cartOnlyItems = Array.from(cartOnlyItems.value.values());

      currentProject.value.lastModified = new Date().toISOString();
      const projectFilePath = `${currentProject.value.folderPath}/${currentProject.value.name}.liveplay`;

      if (import.meta.client && window.electronAPI) {
        const result = await window.electronAPI.writeFile(
          projectFilePath,
          JSON.stringify(currentProject.value, null, 2)
        );
        return result.success;
      }
      return false;
    } catch (error) {
      console.error('Error saving project:', error);
      return false;
    }
  };

  // Close the current project
  const closeProject = () => {
    currentProject.value = null;
    selectedItem.value = null;
    activeCues.value.clear();
    
    // Clear cart-only items from memory
    const { clearCartOnlyItems } = useCartItems();
    clearCartOnlyItems();
  };

  // Update item indices recursively
  const updateIndices = (items: (AudioItem | GroupItem)[], parentIndex: number[] = []): void => {
    items.forEach((item, i) => {
      item.index = [...parentIndex, i];
      if (item.type === 'group') {
        updateIndices(item.children, item.index);
      }
    });
  };

  // Add an item to the project
  const addItem = (item: AudioItem | GroupItem, parentIndex?: number[]) => {
    if (!currentProject.value) return;

    if (parentIndex && parentIndex.length > 0) {
      // Add to a group
      const parent = findItemByIndex(parentIndex);
      if (parent && parent.type === 'group') {
        parent.children.push(item);
        updateIndices(parent.children, parentIndex);
      }
    } else {
      // Add to root
      currentProject.value.items.push(item);
      updateIndices(currentProject.value.items);
    }
  };

  // Remove an item
  const removeItem = (uuid: string) => {
    if (!currentProject.value) return;

    const removeFromArray = (items: (AudioItem | GroupItem)[]): boolean => {
      const index = items.findIndex(item => item.uuid === uuid);
      if (index !== -1) {
        items.splice(index, 1);
        updateIndices(items);
        return true;
      }

      for (const item of items) {
        if (item.type === 'group') {
          if (removeFromArray(item.children)) {
            updateIndices(item.children, item.index);
            return true;
          }
        }
      }
      return false;
    };

    removeFromArray(currentProject.value.items);
    if (selectedItem.value?.uuid === uuid) {
      selectedItem.value = null;
    }
  };

  // Find item by UUID
  const findItemByUuid = (uuid: string): AudioItem | GroupItem | null => {
    if (!currentProject.value) return null;

    const search = (items: (AudioItem | GroupItem)[]): AudioItem | GroupItem | null => {
      for (const item of items) {
        if (item.uuid === uuid) return item;
        if (item.type === 'group') {
          const found = search(item.children);
          if (found) return found;
        }
      }
      return null;
    };

    // First search in project items
    const found = search(currentProject.value.items);
    if (found) return found;

    // Also check cart-only items
    const { getCartOnlyItem } = useCartItems();
    return getCartOnlyItem(uuid);
  };

  // Find item by index
  const findItemByIndex = (index: number[]): AudioItem | GroupItem | null => {
    if (!currentProject.value) return null;

    let items: (AudioItem | GroupItem)[] = currentProject.value.items;
    let currentItem: AudioItem | GroupItem | null = null;

    for (const idx of index) {
      if (idx >= items.length) return null;
      currentItem = items[idx];
      if (currentItem.type === 'group') {
        items = currentItem.children;
      }
    }

    return currentItem;
  };

  // Move item
  const moveItem = (fromIndex: number[], toIndex: number[]) => {
    if (!currentProject.value) return;

    const item = findItemByIndex(fromIndex);
    if (!item) return;

    // Remove from current position
    removeItem(item.uuid);

    // Add to new position
    if (toIndex.length === 1) {
      currentProject.value.items.splice(toIndex[0], 0, item);
      updateIndices(currentProject.value.items);
    } else {
      const parentIndex = toIndex.slice(0, -1);
      const parent = findItemByIndex(parentIndex);
      if (parent && parent.type === 'group') {
        parent.children.splice(toIndex[toIndex.length - 1], 0, item);
        updateIndices(parent.children, parentIndex);
      }
    }
  };

  return {
    currentProject,
    selectedItem,
    selectedItems,
    activeCues,
    waveformUpdateKey,
    triggerWaveformUpdate,
    toggleItemSelection,
    getSelectedItems,
    createNewProject,
    openProject,
    saveProject,
    closeProject,
    addItem,
    removeItem,
    findItemByUuid,
    findItemByIndex,
    moveItem,
    updateIndices
  };
};
