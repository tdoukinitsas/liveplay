import type { AudioItem, DuckingBehavior, GroupItem } from '~/types/project';
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
  duckedBy: Set<string>; // Track which cues are ducking this one
  howl: Howl;
  progressInterval?: any;
  color?: string;
  inPoint?: number; // Store inPoint for seek operations
  outPoint?: number; // Store outPoint for reference
  currentLevel?: number; // Current audio level in dB (-60 to 0)
  peakLevel?: number; // Peak audio level in dB
}

// Active group tracking for progress indicators
interface ActiveGroupState {
  uuid: string;
  displayName: string;
  totalDuration: number; // Total duration of all auto-playing items
  currentTime: number; // Current progress through the sequence
  playbackChain: string[]; // UUIDs of items that will play in sequence
  currentItemIndex: number; // Index in the playback chain
  lastPlayedItem: string | null; // Last item that played in this group
}

export const useAudioEngine = () => {
  const { currentProject, findItemByUuid, findItemByIndex } = useProject();
  const activeCues = useState<Map<string, ActiveCueState>>('activeCues', () => new Map());
  const activeGroups = useState<Map<string, ActiveGroupState>>('activeGroups', () => new Map());
  const masterOutputLevel = useState<number>('masterOutputLevel', () => -60); // Master output level in dB
  const masterPeakLevel = useState<number>('masterPeakLevel', () => -60); // Master peak level in dB

  // Helper function to apply -10dB offset to volume
  // This allows the UI to show 0dB as "normal" while actually playing at -10dB
  // Giving us +10dB headroom for louder playback
  const applyVolumeOffset = (uiVolume: number): number => {
    // Convert UI volume (linear) to dB
    const uiDB = uiVolume <= 0 ? -60 : 20 * Math.log10(uiVolume);
    
    // Apply -10dB offset
    const actualDB = uiDB - 10;
    
    // Convert back to linear and clamp to Howler's 0-1 range
    const actualVolume = actualDB <= -60 ? 0 : Math.pow(10, actualDB / 20);
    return Math.max(0, Math.min(1, actualVolume));
  };

  // Estimate audio level based on volume and waveform data
  // This samples the waveform at the current playback position
  const estimateAudioLevel = (volume: number, item: AudioItem, currentTime: number): number => {
    if (volume <= 0) return -60;
    
    // Convert volume to dB as base level
    const baseDB = 20 * Math.log10(volume);
    
    // Try to get waveform data for more accurate level
    let waveformMultiplier = 0.7; // Default if no waveform (assume 70% average)
    
    if (item.waveform && item.waveform.peaks && item.waveform.peaks.length > 0) {
      const peaks = item.waveform.peaks;
      const duration = item.duration;
      
      // Calculate which peak index corresponds to current time
      // Account for inPoint offset
      const absoluteTime = currentTime + (item.inPoint || 0);
      const progress = absoluteTime / duration;
      const peakIndex = Math.floor(progress * peaks.length);
      
      // Get peak value (0-1) at current position
      if (peakIndex >= 0 && peakIndex < peaks.length) {
        waveformMultiplier = peaks[peakIndex];
        
        // Add slight variation to adjacent peaks for smoother animation
        if (peakIndex > 0 && peakIndex < peaks.length - 1) {
          const avg = (peaks[peakIndex - 1] + peaks[peakIndex] + peaks[peakIndex + 1]) / 3;
          waveformMultiplier = avg;
        }
      }
    }
    
    // Convert waveform multiplier to dB reduction
    // 0.0 = -60dB, 0.5 = -6dB, 1.0 = 0dB
    const waveformDB = waveformMultiplier <= 0 ? -60 : 20 * Math.log10(waveformMultiplier);
    
    // Combine base volume and waveform level
    const totalDB = baseDB + waveformDB;
    
    return Math.max(-60, Math.min(0, totalDB));
  };

  // Update all cue levels and calculate master output
  const updateAudioLevels = () => {
    let sumLinear = 0; // Sum of linear amplitudes for mixing
    
    for (const cue of activeCues.value.values()) {
      // Find the item to get waveform data
      const item = findItemByUuid(cue.uuid);
      if (!item || item.type !== 'audio') continue;
      
      // Estimate current level based on volume and waveform
      const currentLevel = estimateAudioLevel(cue.volume, item as AudioItem, cue.currentTime);
      cue.currentLevel = currentLevel;
      
      // Update peak hold (decays slowly)
      if (!cue.peakLevel || currentLevel > cue.peakLevel) {
        cue.peakLevel = currentLevel;
      } else {
        // Decay peak hold by 1dB per update cycle
        cue.peakLevel = Math.max(currentLevel, cue.peakLevel - 1);
      }
      
      // Convert dB to linear amplitude for mixing
      const linearAmplitude = currentLevel <= -60 ? 0 : Math.pow(10, currentLevel / 20);
      sumLinear += linearAmplitude;
    }
    
    // Convert summed linear amplitude back to dB for master meter
    const masterLevel = sumLinear <= 0 ? -60 : 20 * Math.log10(sumLinear);
    masterOutputLevel.value = Math.max(-60, Math.min(0, masterLevel));
    
    // Update master peak hold
    if (masterLevel > masterPeakLevel.value) {
      masterPeakLevel.value = masterLevel;
    } else {
      masterPeakLevel.value = Math.max(masterLevel, masterPeakLevel.value - 0.5);
    }
    
    // Reset to -60 if no active cues
    if (activeCues.value.size === 0) {
      masterOutputLevel.value = -60;
      masterPeakLevel.value = -60;
    }
  };

  // Apply ducking behavior (using Howler volume control)
  const applyDucking = (newCueUuid: string, behavior: DuckingBehavior) => {
    const cues = Array.from(activeCues.value.values());

    if (behavior.mode === 'stop-all') {
      // Stop all other cues with fade out
      for (const cue of cues) {
        if (cue.uuid !== newCueUuid) {
          stopCue(cue.uuid);
        }
      }
    } else if (behavior.mode === 'duck-others' && behavior.duckLevel !== undefined) {
      // Lower volume of other cues with fade
      const fadeInDuration = (behavior.duckFadeIn ?? 0.25) * 1000; // Convert to ms
      
      for (const cue of cues) {
        if (cue.uuid !== newCueUuid) {
          if (!cue.isDucked) {
            cue.originalVolume = cue.volume;
            cue.isDucked = true;
          }
          
          // Track that this cue is being ducked by the new cue
          cue.duckedBy.add(newCueUuid);
          
          const duckedVolume = cue.originalVolume * behavior.duckLevel;
          
          // Fade to ducked volume
          cue.howl.fade(cue.volume, duckedVolume, fadeInDuration);
          cue.volume = duckedVolume;
        }
      }
    }
    // 'no-ducking' mode does nothing
  };

  // Restore volumes after ducking
  const restoreDuckedVolumes = (endingCueUuid: string) => {
    const cues = Array.from(activeCues.value.values());
    
    // Check if the ending cue was a ducking cue
    const endingItem = findItemByUuid(endingCueUuid);
    if (!endingItem || endingItem.type !== 'audio') return;
    
    const audioItem = endingItem as AudioItem;
    
    // Only restore if the ending cue had duck-others behavior
    if (audioItem.duckingBehavior.mode === 'duck-others') {
      const fadeOutDuration = (audioItem.duckingBehavior.duckFadeOut ?? 1.0) * 1000; // Convert to ms
      
      // Remove this cue from all duckedBy sets and restore volumes if no other ducking cues remain
      for (const cue of cues) {
        if (cue.duckedBy.has(endingCueUuid)) {
          cue.duckedBy.delete(endingCueUuid);
          
          // If no other cues are ducking this one, restore its volume
          if (cue.duckedBy.size === 0 && cue.isDucked) {
            cue.howl.fade(cue.volume, cue.originalVolume, fadeOutDuration);
            cue.volume = cue.originalVolume;
            cue.isDucked = false;
          }
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
        volume: applyVolumeOffset(item.volume), // Apply -10dB offset
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
            // For trimmed items (with sprites), duration should be the trimmed duration
            // Howler's duration() returns the sprite duration when using sprites
            cue.duration = item.inPoint || item.outPoint 
              ? (item.outPoint || item.duration) - (item.inPoint || 0)
              : howl.duration();
            
            // Start progress tracking
            cue.progressInterval = setInterval(() => {
              if (!activeCues.value.has(item.uuid)) {
                clearInterval(cue.progressInterval);
                return;
              }
              
              // When using sprites, seek() returns absolute position in file
              // We need to subtract inPoint to get position relative to sprite start
              const absoluteTime = howl.seek() as number;
              const inPoint = item.inPoint || 0;
              const currentTime = absoluteTime - inPoint;
              
              // Clamp to valid range (0 to trimmed duration)
              cue.currentTime = Math.max(0, Math.min(currentTime, cue.duration));
              
              // Update audio levels for VU meters
              updateAudioLevels();
              
              // Safety check: if we've somehow passed the end point, stop playback
              // This shouldn't happen with sprites, but ensures clean behavior
              if (currentTime >= cue.duration) {
                howl.stop();
                clearInterval(cue.progressInterval);
                return;
              }
              
              // Update group progress
              const parentGroup = findParentGroup(item.uuid);
              if (parentGroup) {
                const groupState = activeGroups.value.get(parentGroup.uuid);
                if (groupState) {
                  // Calculate accumulated time up to current item
                  let accumulatedTime = 0;
                  for (let i = 0; i < groupState.currentItemIndex; i++) {
                    const uuid = groupState.playbackChain[i];
                    const prevItem = findItemByUuid(uuid);
                    if (prevItem && prevItem.type === 'audio') {
                      const audioItem = prevItem as AudioItem;
                      accumulatedTime += audioItem.outPoint - audioItem.inPoint;
                    }
                  }
                  // Add current item's progress
                  groupState.currentTime = accumulatedTime + currentTime;
                }
              }
              
            }, 100); // Update every 100ms
          }
        },
        onend: () => {
          const cue = activeCues.value.get(item.uuid);
          
          // Don't handle end behavior for looping items - Howler handles loop internally
          // Only clean up and handle end behavior for non-looping items
          if (item.endBehavior.action !== 'loop') {
            if (cue && cue.progressInterval) {
              clearInterval(cue.progressInterval);
            }
            activeCues.value.delete(item.uuid);
            restoreDuckedVolumes(item.uuid);
            
            // Check if we need to stop group tracking
            const parentGroup = findParentGroup(item.uuid);
            if (parentGroup) {
              const groupState = activeGroups.value.get(parentGroup.uuid);
              if (groupState) {
                const itemIndex = groupState.playbackChain.indexOf(item.uuid);
                // If this is the last item in the chain, stop group tracking
                if (itemIndex === groupState.playbackChain.length - 1) {
                  stopGroupTracking(parentGroup.uuid);
                }
              }
            }
            
            handleEndBehavior(item);
          }
          // For looping items, keep the progress interval running
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
        duration: item.inPoint || item.outPoint 
          ? (item.outPoint || item.duration) - (item.inPoint || 0)
          : item.duration,
        volume: applyVolumeOffset(item.volume), // Apply -10dB offset
        isDucked: false,
        originalVolume: applyVolumeOffset(item.volume), // Apply -10dB offset
        duckedBy: new Set<string>(),
        howl,
        color: item.color, // Pass item color to active cue
        inPoint: item.inPoint, // Store for seek operations
        outPoint: item.outPoint, // Store for reference
        currentLevel: -60, // Initialize level meter
        peakLevel: -60 // Initialize peak hold
      };

      activeCues.value.set(item.uuid, activeCue);

      // Apply ducking before playback
      applyDucking(item.uuid, item.duckingBehavior);
      
      // Update group progress tracking
      updateGroupProgress(item.uuid);

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
        // Get the audio item to retrieve fade out duration
        const item = findItemByUuid(uuid);
        const fadeOutDuration = (item && item.type === 'audio') 
          ? ((item as AudioItem).fadeOutDuration ?? 1.0) * 1000 
          : 1000; // Default 1 second in ms

        // Clear progress interval immediately
        if (cue.progressInterval) {
          clearInterval(cue.progressInterval);
        }

        // Restore ducked volumes BEFORE fading out (so restoration happens in parallel)
        restoreDuckedVolumes(uuid);

        // Fade out before stopping
        cue.howl.fade(cue.volume, 0, fadeOutDuration);
        
        // Remove from active cues immediately (so other functions know it's stopping)
        activeCues.value.delete(uuid);
        
        // Check if this was part of a tracked group
        const parentGroup = findParentGroup(uuid);
        if (parentGroup && activeGroups.value.has(parentGroup.uuid)) {
          // Check if there are any other items from this group still playing
          const groupState = activeGroups.value.get(parentGroup.uuid);
          if (groupState) {
            const anyGroupItemPlaying = groupState.playbackChain.some(itemUuid => 
              activeCues.value.has(itemUuid)
            );
            
            // If no items from this group are playing, stop group tracking
            if (!anyGroupItemPlaying) {
              stopGroupTracking(parentGroup.uuid);
            }
          }
        }
        
        // Wait for fade to complete, then stop and unload
        setTimeout(() => {
          cue.howl.stop();
          cue.howl.unload();
        }, fadeOutDuration);
        
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

  // Panic stop - fade out all cues over 0.5 seconds then stop
  const panicStop = async () => {
    if (!import.meta.client) return;

    try {
      const fadeOutDuration = 500; // 0.5 seconds in ms
      
      for (const [uuid, cue] of activeCues.value.entries()) {
        // Fade out to 0
        cue.howl.fade(cue.volume, 0, fadeOutDuration);
      }
      
      // Wait for fade to complete, then stop all
      setTimeout(() => {
        for (const [uuid, cue] of activeCues.value.entries()) {
          if (cue.progressInterval) {
            clearInterval(cue.progressInterval);
          }
          cue.howl.stop();
          cue.howl.unload();
        }
        activeCues.value.clear();
      }, fadeOutDuration);
    } catch (error) {
      console.error('Error panic stopping cues:', error);
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
        // Loop is handled by Howler's loop setting, no need to manually re-trigger
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

  // Calculate the playback chain for a group (items that will play automatically)
  const calculateGroupPlaybackChain = (group: GroupItem, startingItemUuid?: string): string[] => {
    const chain: string[] = [];
    const visited = new Set<string>(); // Prevent infinite loops
    
    // Find starting item
    let currentItem: AudioItem | null = null;
    
    if (startingItemUuid) {
      // Start from specified item
      const item = findItemByUuid(startingItemUuid);
      if (item && item.type === 'audio') {
        currentItem = item as AudioItem;
      }
    } else {
      // Start from first audio item in group based on start behavior
      if (group.startBehavior.action === 'play-first') {
        const firstAudio = findFirstAudioInGroup(group);
        if (firstAudio) currentItem = firstAudio;
      } else {
        // For 'play-all', we don't track group progress
        return [];
      }
    }
    
    if (!currentItem) return [];
    
    // Build the chain by following end behaviors
    while (currentItem && !visited.has(currentItem.uuid)) {
      visited.add(currentItem.uuid);
      chain.push(currentItem.uuid);
      
      const endBehavior = currentItem.endBehavior;
      
      if (endBehavior.action === 'next') {
        // Find next item in the group
        const nextItem = findNextItemInGroup(group, currentItem);
        if (nextItem && nextItem.type === 'audio') {
          currentItem = nextItem as AudioItem;
        } else {
          break; // No more items
        }
      } else if (endBehavior.action === 'goto-item' && endBehavior.targetUuid) {
        const targetItem = findItemByUuid(endBehavior.targetUuid);
        
        // Check if target is in the same group
        if (targetItem && targetItem.type === 'audio' && isItemInGroup(group, targetItem.uuid)) {
          // Check if we're going forward or backward
          const targetIndex = chain.indexOf(targetItem.uuid);
          if (targetIndex === -1) {
            // Going forward - add to chain
            currentItem = targetItem as AudioItem;
          } else {
            // Going backward (loop) - reset chain
            return [targetItem.uuid];
          }
        } else {
          // Jumping outside group - stop tracking
          break;
        }
      } else if (endBehavior.action === 'goto-index' && endBehavior.targetIndex) {
        const targetItem = findItemByIndex(endBehavior.targetIndex);
        
        if (targetItem && targetItem.type === 'audio' && isItemInGroup(group, targetItem.uuid)) {
          const targetIndex = chain.indexOf(targetItem.uuid);
          if (targetIndex === -1) {
            currentItem = targetItem as AudioItem;
          } else {
            return [targetItem.uuid];
          }
        } else {
          break;
        }
      } else if (endBehavior.action === 'loop') {
        // Single item loop - don't continue chain
        break;
      } else {
        // 'nothing' or unknown - stop chain
        break;
      }
    }
    
    return chain;
  };
  
  // Helper: Find first audio item in group (recursively)
  const findFirstAudioInGroup = (group: GroupItem): AudioItem | null => {
    for (const child of group.children) {
      if (child.type === 'audio') {
        return child as AudioItem;
      } else if (child.type === 'group') {
        const found = findFirstAudioInGroup(child as GroupItem);
        if (found) return found;
      }
    }
    return null;
  };
  
  // Helper: Find next item in group
  const findNextItemInGroup = (group: GroupItem, currentItem: AudioItem): AudioItem | GroupItem | null => {
    const flatten = (items: any[]): any[] => {
      const result: any[] = [];
      for (const item of items) {
        result.push(item);
        if (item.type === 'group') {
          result.push(...flatten(item.children));
        }
      }
      return result;
    };
    
    const allItems = flatten(group.children);
    const currentIndex = allItems.findIndex(item => item.uuid === currentItem.uuid);
    
    if (currentIndex >= 0 && currentIndex < allItems.length - 1) {
      return allItems[currentIndex + 1];
    }
    
    return null;
  };
  
  // Helper: Check if item is in group
  const isItemInGroup = (group: GroupItem, itemUuid: string): boolean => {
    for (const child of group.children) {
      if (child.uuid === itemUuid) return true;
      if (child.type === 'group') {
        if (isItemInGroup(child as GroupItem, itemUuid)) return true;
      }
    }
    return false;
  };
  
  // Find parent group of an item
  const findParentGroup = (itemUuid: string): GroupItem | null => {
    if (!currentProject.value) return null;
    
    const searchInGroup = (group: GroupItem): GroupItem | null => {
      for (const child of group.children) {
        if (child.uuid === itemUuid) return group;
        if (child.type === 'group') {
          const found = searchInGroup(child as GroupItem);
          if (found) return found;
        }
      }
      return null;
    };
    
    for (const item of currentProject.value.items) {
      if (item.type === 'group') {
        const found = searchInGroup(item as GroupItem);
        if (found) return found;
      }
    }
    
    return null;
  };
  
  // Start tracking a group
  const startGroupTracking = (group: GroupItem, startingItemUuid?: string) => {
    const chain = calculateGroupPlaybackChain(group, startingItemUuid);
    
    if (chain.length === 0) return; // Can't track 'play-all' groups
    
    // Calculate total duration
    let totalDuration = 0;
    for (const itemUuid of chain) {
      const item = findItemByUuid(itemUuid);
      if (item && item.type === 'audio') {
        const audioItem = item as AudioItem;
        const duration = audioItem.outPoint - audioItem.inPoint;
        totalDuration += duration;
      }
    }
    
    activeGroups.value.set(group.uuid, {
      uuid: group.uuid,
      displayName: group.displayName,
      totalDuration,
      currentTime: 0,
      playbackChain: chain,
      currentItemIndex: 0,
      lastPlayedItem: null
    });
  };
  
  // Update group progress when an item plays
  const updateGroupProgress = (itemUuid: string) => {
    // Find which group this item belongs to
    const parentGroup = findParentGroup(itemUuid);
    if (!parentGroup) return;
    
    const groupState = activeGroups.value.get(parentGroup.uuid);
    if (!groupState) {
      // Start tracking if not already
      startGroupTracking(parentGroup, itemUuid);
      return;
    }
    
    // Check if this item is in the playback chain
    const itemIndex = groupState.playbackChain.indexOf(itemUuid);
    
    if (itemIndex === -1) {
      // Item not in chain - recalculate from this item
      const newChain = calculateGroupPlaybackChain(parentGroup, itemUuid);
      if (newChain.length > 0) {
        let totalDuration = 0;
        for (const uuid of newChain) {
          const item = findItemByUuid(uuid);
          if (item && item.type === 'audio') {
            const audioItem = item as AudioItem;
            totalDuration += audioItem.outPoint - audioItem.inPoint;
          }
        }
        
        groupState.playbackChain = newChain;
        groupState.totalDuration = totalDuration;
        groupState.currentItemIndex = 0;
        groupState.currentTime = 0;
        groupState.lastPlayedItem = itemUuid;
      }
    } else if (groupState.lastPlayedItem) {
      // Check if we're going backward
      const lastIndex = groupState.playbackChain.indexOf(groupState.lastPlayedItem);
      if (lastIndex > itemIndex) {
        // Going backward - reset and recalculate
        const newChain = calculateGroupPlaybackChain(parentGroup, itemUuid);
        if (newChain.length > 0) {
          let totalDuration = 0;
          for (const uuid of newChain) {
            const item = findItemByUuid(uuid);
            if (item && item.type === 'audio') {
              const audioItem = item as AudioItem;
              totalDuration += audioItem.outPoint - audioItem.inPoint;
            }
          }
          
          groupState.playbackChain = newChain;
          groupState.totalDuration = totalDuration;
          groupState.currentItemIndex = 0;
          groupState.currentTime = 0;
        }
      } else {
        // Going forward - update position
        groupState.currentItemIndex = itemIndex;
        
        // Calculate current time (sum of durations of previous items)
        let accumulatedTime = 0;
        for (let i = 0; i < itemIndex; i++) {
          const uuid = groupState.playbackChain[i];
          const item = findItemByUuid(uuid);
          if (item && item.type === 'audio') {
            const audioItem = item as AudioItem;
            accumulatedTime += audioItem.outPoint - audioItem.inPoint;
          }
        }
        groupState.currentTime = accumulatedTime;
      }
    }
    
    groupState.lastPlayedItem = itemUuid;
  };
  
  // Stop tracking a group
  const stopGroupTracking = (groupUuid: string) => {
    activeGroups.value.delete(groupUuid);
  };

  // Trigger a group
  const triggerGroup = (group: any) => {
    // Start tracking group progress
    startGroupTracking(group);
    
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
    activeGroups,
    masterOutputLevel,
    masterPeakLevel,
    playCue,
    stopCue,
    stopAllCues,
    panicStop,
    pauseCue,
    triggerByUuid,
    triggerByIndex,
    triggerGroup
  };
};
