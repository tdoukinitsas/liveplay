// =====================================================================
// plugins/liveplay-server.client.ts
// ---------------------------------------------------------------------
// Nuxt plugin (client-side only) that:
//   1. Reads the server-mode config from Electron's main process
//      (local vs remote, persisted in <userData>/liveplay-server.json).
//   2. Points useLiveplayServer at the correct URL.
//   3. Listens for state changes from the main process (e.g. the user
//      flipped to remote mode in the settings modal) and re-targets.
//   4. Kicks off the auto-reconnecting WebSocket.
// =====================================================================
import { defineNuxtPlugin } from 'nuxt/app';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import { useShowControl } from '~/composables/useShowControl';
import { useEvents } from '~/composables/useEvents';
import { useConnectionGuard } from '~/composables/useConnectionGuard';

export default defineNuxtPlugin(async () => {
  const server = useLiveplayServer();

  // Installs the while-disconnected input freeze and the post-reconnect
  // "does the server still have our project?" probe. Wired before the socket
  // opens so the very first reconnect is covered.
  useConnectionGuard();

  // Subscribe to the server-owned operator state (selection / Show Mode /
  // locale) before the socket opens, so the first playback_snapshot is not
  // missed. Wiring it here rather than in a component means it stays live
  // even in windows that never mount the playlist (the detached cart player).
  useShowControl();

  // Event feedback (#84) and the plugin schemas. Here rather than in the
  // Events tab so an event that failed mid-show is already on record when
  // someone opens the tab to find out why.
  useEvents();

  // Electron exposes window.electronAPI.liveplayServer via the preload
  // bridge. In a non-Electron / pure-web context we just fall back to
  // whatever URL the user persisted in localStorage (handled by the
  // composable's defaultUrl logic).
  const ep: any = (globalThis as any).electronAPI?.liveplayServer;

  if (ep) {
    try {
      const cfg = await ep.getConfig();
      const url = cfg.mode === 'remote'
        ? cfg.remoteUrl
        : `http://127.0.0.1:${cfg.localPort ?? 4480}`;
      server.setServerUrl(url);   // also reconnects internally
    } catch (e) {
      console.warn('[liveplay] failed to read Electron config:', e);
      server.connect();
    }

    // Re-target whenever main process tells us the config changed.
    ep.onStateChange?.((payload: any) => {
      const cfg = payload?.config;
      if (!cfg) return;
      const url = cfg.mode === 'remote'
        ? cfg.remoteUrl
        : `http://127.0.0.1:${cfg.localPort ?? 4480}`;
      if (url !== server.serverUrl) server.setServerUrl(url);
    });
  } else {
    server.connect();
  }

  return {
    provide: { liveplay: server },
  };
});
