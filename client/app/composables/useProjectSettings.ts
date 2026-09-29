// Read and write `project.settings`, for the Settings panes.
//
// This was inline in ProjectSettingsModal; three panes now need the same two
// things, so it lives here rather than three times over.
//
// The write is optimistic — the local object updates first so a checkbox does
// not lag a round trip — and then PATCHes. The server validates against its
// settings registry and drops anything it does not recognise, so a pane cannot
// put a stray key into the show document even by accident; what it will not do
// is tell the pane it dropped something, which is why panes should send the
// keys the registry knows and nothing else.
import type { ProjectSettings } from '~/types/project';

export const useProjectSettings = () => {
  const server = useLiveplayServer();
  const { currentProject } = useProject();

  const settings = computed<ProjectSettings>(
    () => ((currentProject.value as any)?.settings ?? {}) as ProjectSettings
  );

  async function applyPatch(patch: Record<string, any>) {
    if (currentProject.value) {
      const current = (currentProject.value as any).settings ?? {};
      (currentProject.value as any).settings = { ...current, ...patch };
    }
    try {
      await server.patchSettings(patch);
    } catch (e) {
      console.warn('[settings] patch failed:', e);
    }
  }

  return { settings, applyPatch };
};
