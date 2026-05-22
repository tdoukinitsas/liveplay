// =====================================================================
// plugins/liveplay-server.client.ts
// ---------------------------------------------------------------------
// Nuxt plugin (client-side only) that connects to the LivePlay server
// on app boot and exposes the composable via $liveplay for templates
// that prefer the global form.
// =====================================================================
import { defineNuxtPlugin } from 'nuxt/app';
import { useLiveplayServer } from '~/composables/useLiveplayServer';

export default defineNuxtPlugin(() => {
  const server = useLiveplayServer();
  // Kick off the WebSocket connection — auto-reconnects on failure.
  server.connect();
  return {
    provide: { liveplay: server },
  };
});
