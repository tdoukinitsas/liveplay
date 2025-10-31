import type { AudioItem, DuckingBehavior } from '~/types/project';
import { Howl } from 'howler';

// Active cue tracking with Howler instances
interface ActiveCueState {
  uuid: string;
  displayName: string;
  duration: number;
  currentTime: number;
  volume: number;
  isDucked: boolean;
  originalVolume: number;
  howl: Howl;
  progressInterval?: any;
}

export const useAudioEngine = () => {
  const { currentProject, findItemByUuid, findItemByIndex } = useProject();
  const activeCues = useState<Map<string, ActiveCueState>>('activeCues', () => new Map());

  // Apply ducking behavior (using Howler volume control)
  const applyDucking = (newCueUuid: string, behavior: DuckingBehavior) => {
    const cues = Array.from(activeCues.value.values());

    if (behavior.mode === 'stop-all') {
      // Stop all other cues
      for (const cue of cues) {
        if (cue.uuid !== newCueUuid) {
          stopCue(cue.uuid);
        }
      }
    } else if (behavior.mode === 'duck-others' && behavior.duckLevel !== undefined) {
      // Lower volume of other cues
      for (const cue of cues) {
        if (cue.uuid !== newCueUuid) {
          if (!cue.isDucked) {
            cue.originalVolume = cue.volume;
            cue.isDucked = true;
          }
          const duckedVolume = cue.originalVolume * behavior.duckLevel;
          cue.howl.volume(duckedVolume);
          cue.volume = duckedVolume;
        }
      }
    }
    // 'no-ducking' mode does nothing
  };

  // Restore volumes after ducking
  const restoreDuckedVolumes = (endingCueUuid: string) => {
    const cues = Array.from(activeCues.value.values());
    const hasDuckingCues = cues.some(cue => cue.uuid !== endingCueUuid);

    if (!hasDuckingCues) {
      // No more ducking cues, restore all volumes
      for (const cue of cues) {
        if (cue.isDucked) {
          cue.howl.volume(cue.originalVolume);
          cue.volume = cue.originalVolume;
          cue.isDucked = false;
        }
      }
    }
  };

  // Play an audio item (using Howler.js in renderer)
  const playCue = async (item: AudioItem): Promise<boolean> => {
    try {
      if (!import.meta.client || !currentProject.value) return false;

      // Check if already playing
      if (activeCues.value.has(item.uuid)) {
        console.warn('Cue already playing:', item.uuid);
        return false;
      }

      const audioPath = `${currentProject.value.folderPath}/media/${item.mediaFileName}`;
      
      // Convert file path to file:// URL
      const fileUrl = 'file:///' + audioPath.replace(/\\/g, '/');
      
      // Create Howler instance
      const howl = new Howl({
        src: [fileUrl],
        html5: true, // Use HTML5 Audio for better file system access
        volume: item.volume,
        loop: item.endBehavior.action === 'loop',
        sprite: item.inPoint || item.outPoint ? {
          main: [
            (item.inPoint || 0) * 1000,
            ((item.outPoint || item.duration) - (item.inPoint || 0)) * 1000
          ]
        } : undefined,
        onload: () => {
          const cue = activeCues.value.get(item.uuid);
          if (cue) {
            cue.duration = howl.duration();
            
            // Start progress tracking
            cue.progressInterval = setInterval(() => {
              if (!activeCues.value.has(item.uuid)) {
                clearInterval(cue.progressInterval);
                return;
              }
              
              const currentTime = howl.seek() as number;
              cue.currentTime = currentTime;
              
            }, 100); // Update every 100ms
          }
        },
        onend: () => {
          const cue = activeCues.value.get(item.uuid);
          if (cue && cue.progressInterval) {
            clearInterval(cue.progressInterval);
          }
          activeCues.value.delete(item.uuid);
          restoreDuckedVolumes(item.uuid);
          handleEndBehavior(item);
        },
        onloaderror: (id, error) => {
          console.error('Error loading audio:', error);
          activeCues.value.delete(item.uuid);
        },
        onplayerror: (id, error) => {
          console.error('Error playing audio:', error);
          activeCues.value.delete(item.uuid);
        }
      });

      // Create active cue state
      const activeCue: ActiveCueState = {
        uuid: item.uuid,
        displayName: item.displayName,
        currentTime: 0,
        duration: item.duration,
        volume: item.volume,
        isDucked: false,
        originalVolume: item.volume,
        howl
      };

      activeCues.value.set(item.uuid, activeCue);

      // Apply ducking before playback
      applyDucking(item.uuid, item.duckingBehavior);

      // Start playback (use sprite if in/out points defined)
      if (item.inPoint || item.outPoint) {
        howl.play('main');
      } else {
        howl.play();
      }

      // Handle start behavior
      handleStartBehavior(item);

      // Schedule custom actions
      scheduleCustomActions(item);

      return true;
    } catch (error) {
      console.error('Error playing cue:', error);
      activeCues.value.delete(item.uuid);
      return false;
    }
  };

  // Stop a cue
  const stopCue = async (uuid: string) => {
    if (!import.meta.client) return;

    const cue = activeCues.value.get(uuid);
    if (cue) {
      try {
        if (cue.progressInterval) {
          clearInterval(cue.progressInterval);
        }
        cue.howl.stop();
        cue.howl.unload();
        activeCues.value.delete(uuid);
        restoreDuckedVolumes(uuid);
      } catch (error) {
        console.error('Error stopping cue:', error);
      }
    }
  };

  // Stop all cues
  const stopAllCues = async () => {
    if (!import.meta.client) return;

    try {
      for (const [uuid, cue] of activeCues.value.entries()) {
        if (cue.progressInterval) {
          clearInterval(cue.progressInterval);
        }
        cue.howl.stop();
        cue.howl.unload();
      }
      activeCues.value.clear();
    } catch (error) {
      console.error('Error stopping all cues:', error);
    }
  };

  // Pause a cue
  const pauseCue = async (uuid: string) => {
    if (!import.meta.client) return;

    const cue = activeCues.value.get(uuid);
    if (cue) {
      try {
        cue.howl.pause();
      } catch (error) {
        console.error('Error pausing cue:', error);
      }
    }
  };

  // Handle end behavior
  const handleEndBehavior = (item: AudioItem) => {
    const behavior = item.endBehavior;

    switch (behavior.action) {
      case 'next':
        playNextItem(item.index);
        break;
      case 'goto-item':
        if (behavior.targetUuid) {
          triggerByUuid(behavior.targetUuid);
        }
        break;
      case 'goto-index':
        if (behavior.targetIndex) {
          triggerByIndex(behavior.targetIndex);
        }
        break;
      case 'loop':
        playCue(item);
        break;
      case 'nothing':
      default:
        break;
    }
  };

  // Handle start behavior
  const handleStartBehavior = (item: AudioItem) => {
    const behavior = item.startBehavior;

    switch (behavior.action) {
      case 'play-next':
        playNextItem(item.index);
        break;
      case 'play-item':
        if (behavior.targetUuid) {
          triggerByUuid(behavior.targetUuid);
        }
        break;
      case 'play-index':
        if (behavior.targetIndex) {
          triggerByIndex(behavior.targetIndex);
        }
        break;
      case 'nothing':
      default:
        break;
    }
  };

  // Schedule custom actions
  const scheduleCustomActions = (item: AudioItem) => {
    item.customActions.forEach(customAction => {
      const timeoutMs = (customAction.timePoint - (item.inPoint || 0)) * 1000;
      
      if (timeoutMs > 0) {
        setTimeout(() => {
          executeCustomAction(customAction.action);
        }, timeoutMs);
      }
    });
  };

  // Execute custom action
  const executeCustomAction = async (action: any) => {
    switch (action.type) {
      case 'play-item':
        triggerByUuid(action.uuid);
        break;
      case 'play-index':
        triggerByIndex(action.index);
        break;
      case 'stop-all':
        stopAllCues();
        break;
      case 'http-request':
        await executeHttpRequest(action.request);
        break;
    }
  };

  // Execute HTTP request
  const executeHttpRequest = async (request: any) => {
    try {
      const options: RequestInit = {
        method: request.method,
        headers: {}
      };

      if (request.body) {
        if (request.contentType === 'json') {
          options.headers = { 'Content-Type': 'application/json' };
          options.body = JSON.stringify(request.body);
        } else {
          options.headers = { 'Content-Type': 'application/x-www-form-urlencoded' };
          options.body = new URLSearchParams(request.body).toString();
        }
      }

      await fetch(request.url, options);
    } catch (error) {
      console.error('Error executing HTTP request:', error);
    }
  };

  // Play next item in sequence
  const playNextItem = (currentIndex: number[]) => {
    const nextIndex = [...currentIndex];
    nextIndex[nextIndex.length - 1]++;

    const nextItem = findItemByIndex(nextIndex);
    if (nextItem) {
      if (nextItem.type === 'audio') {
        playCue(nextItem);
      } else if (nextItem.type === 'group') {
        triggerGroup(nextItem);
      }
    }
  };

  // Trigger by UUID
  const triggerByUuid = (uuid: string) => {
    const item = findItemByUuid(uuid);
    if (item) {
      if (item.type === 'audio') {
        playCue(item);
      } else if (item.type === 'group') {
        triggerGroup(item);
      }
    }
  };

  // Trigger by index
  const triggerByIndex = (index: number[]) => {
    const item = findItemByIndex(index);
    if (item) {
      if (item.type === 'audio') {
        playCue(item);
      } else if (item.type === 'group') {
        triggerGroup(item);
      }
    }
  };

  // Trigger a group
  const triggerGroup = (group: any) => {
    if (group.startBehavior.action === 'play-first') {
      if (group.children.length > 0) {
        const firstChild = group.children[0];
        if (firstChild.type === 'audio') {
          playCue(firstChild);
        } else if (firstChild.type === 'group') {
          triggerGroup(firstChild);
        }
      }
    } else if (group.startBehavior.action === 'play-all') {
      group.children.forEach((child: any) => {
        if (child.type === 'audio') {
          playCue(child);
        } else if (child.type === 'group') {
          triggerGroup(child);
        }
      });
    }
  };

  return {
    activeCues,
    playCue,
    stopCue,
    stopAllCues,
    pauseCue,
    triggerByUuid,
    triggerByIndex,
    triggerGroup
  };
};
