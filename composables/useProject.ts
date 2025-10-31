import { v4 as uuidv4 } from 'uuid';
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
  const activeCues = useState<Map<string, any>>('activeCues', () => new Map());

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
          currentProject.value = project;
          await window.electronAPI.setCurrentProject(projectFilePath);
          return true;
        }
      }
      return false;
    } catch (error) {
      console.error('Error opening project:', error);
      return false;
    }
  };

  // Save the current project
  const saveProject = async (): Promise<boolean> => {
    try {
      if (!currentProject.value) return false;

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

    return search(currentProject.value.items);
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
    activeCues,
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
