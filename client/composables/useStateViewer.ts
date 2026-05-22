// State viewer for development mode
// Sends application state to the state viewer window in real-time

let updateInterval: NodeJS.Timeout | null = null;

export const useStateViewer = () => {
  const { currentProject } = useProject();
  const { activeCues, activeGroups, masterOutputLevel, masterPeakLevel } = useAudioEngine();
  const { cartOnlyItems } = useCartItems();
  const { currentLocale } = useLocalization();

  const startStateUpdates = () => {
    if (updateInterval) return; // Already running

    // Update every 500ms
    updateInterval = setInterval(() => {
      if (!window.electronAPI) return;
      
      const electronAPI = window.electronAPI as any;
      if (!electronAPI.updateAppState) return;

      // Collect all relevant state (serialize to remove non-cloneable objects)
      const state = {
        project: currentProject.value ? JSON.parse(JSON.stringify(currentProject.value)) : { message: 'No project loaded' },
        
        audioEngine: {
          activeCues: activeCues.value ? Array.from(activeCues.value.entries()).map(([uuid, cue]) => ({
            uuid,
            displayName: cue.displayName,
            duration: cue.duration,
            currentTime: cue.currentTime,
            volume: cue.volume,
            isDucked: cue.isDucked,
            originalVolume: cue.originalVolume,
            color: cue.color,
            inPoint: cue.inPoint,
            outPoint: cue.outPoint,
            currentLevel: cue.currentLevel,
            peakLevel: cue.peakLevel
            // Exclude howl and progressInterval as they can't be cloned
          })) : [],
          activeGroups: activeGroups.value ? Array.from(activeGroups.value) : [],
          masterOutputLevel: masterOutputLevel.value,
          masterPeakLevel: masterPeakLevel.value
        },
        
        cartItems: cartOnlyItems.value ? JSON.parse(JSON.stringify(cartOnlyItems.value)) : [],
        
        locale: {
          currentLocale: currentLocale.value
        },
        
        metadata: {
          timestamp: new Date().toISOString(),
          activeCueCount: activeCues.value?.size || 0
        }
      };

      // Send to main process
      electronAPI.updateAppState(state);
    }, 500);
  };

  const stopStateUpdates = () => {
    if (updateInterval) {
      clearInterval(updateInterval);
      updateInterval = null;
    }
  };

  // Check if dev mode and start automatically
  onMounted(async () => {
    if (import.meta.client && window.electronAPI) {
      const electronAPI = window.electronAPI as any;
      if (electronAPI.isDevMode) {
        const isDevMode = await electronAPI.isDevMode();
        if (isDevMode) {
          startStateUpdates();
        }
      }
    }
  });

  onUnmounted(() => {
    stopStateUpdates();
  });

  return {
    startStateUpdates,
    stopStateUpdates
  };
};
