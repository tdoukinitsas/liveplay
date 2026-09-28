// =====================================================================
// useMixerView.ts
// ---------------------------------------------------------------------
// The mixer's layout as the one value the Mixer settings pane offers:
// docked beside the other panes, filling the workspace, or in its own
// window.
//
// This used to be a separate, separately-stored mode that only the mixer
// had. It is now a view over useWorkspaceLayout, which applies the same
// three states to every pane — so choosing "fill the workspace" here is
// exactly pressing the mixer's Expand button, and what is remembered
// between launches is the workspace layout as a whole.
// =====================================================================

export type MixerMode = 'side' | 'full';

export const useMixerView = () => {
  const layout = useWorkspaceLayout();
  const { mixerDetached, flexPane, dockedPanes } = layout;

  const canDetach = computed(() => layout.canDetach('mixer'));

  /**
   * 'full' whenever the mixer is the pane that stretches — expanded, or simply
   * the only thing docked. That is also what MixerPanel needs to know: the
   * channel view wants the whole row and is only offered when it has it.
   */
  const mixerMode = computed<MixerMode>(() => (flexPane.value === 'mixer' ? 'full' : 'side'));

  /** Detached wins: a mixer in a window is not rendering the docked pane at all. */
  const mixerView = computed<'side' | 'full' | 'window'>(() =>
    mixerDetached.value ? 'window' : mixerMode.value);

  const setMixerView = (view: 'side' | 'full' | 'window') => {
    if (view === 'window') { layout.detachPane('mixer'); return; }
    // Coming back from a window: close it, and its `closed` event docks the
    // pane. Nothing more can be done synchronously — until that event lands
    // the pane still reads as detached, and showPane would re-raise the window.
    if (mixerDetached.value) { layout.attachPane('mixer'); return; }
    if (view === 'full') { layout.expandPane('mixer'); return; }
    // Docked beside something. Put back whatever an expand hid; if nothing was
    // hidden and the mixer is alone, the playlist is the natural neighbour.
    if (layout.isExpanded('mixer')) { layout.restorePanes(); return; }
    layout.showPane('mixer');
    if (dockedPanes.value.length <= 1) layout.showPane('playlist');
  };

  return { mixerMode, mixerView, mixerDetached, canDetach, setMixerView };
};
