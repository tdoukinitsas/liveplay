import { computed, ref } from 'vue';
import contributorData from '../../public/contributors.json';

export interface Credit {
  name: string;
  link: string;
}

// scripts/version.js rewrites this line when the version is bumped. The value
// fetched from public/package.json below is the one CI stamps at deploy time.
const version = ref('2.6.0');
// In the bundle rather than fetched, so the prerendered pages name everyone.
const contributors = ref<Credit[]>(Object.values(contributorData.contributors));
let loaded = false;

export const useSiteInfo = () => {
  // The site is hosted under a base path. Build public-file URLs from Nuxt's
  // resolved baseURL, and bind them dynamically so Vite does not try to import
  // files that only exist in public/ after the build.
  const baseURL = useRuntimeConfig().app.baseURL;
  const asset = (path: string) => `${baseURL}${path.replace(/^\/+/, '')}`;
  const shot = (name: string) => asset(`screenshots/liveplay_screenshot${name ? `_${name}` : ''}.jpg`);

  const shortVersion = computed(() => version.value.split('.').slice(0, 2).join('.'));

  const load = async () => {
    if (loaded) return;
    loaded = true;
    try {
      const pkg = await (await fetch(asset('package.json'))).json();
      if (pkg.version) version.value = pkg.version;
    } catch (error) {
      console.error('Failed to fetch version:', error);
    }
  };

  return { version, shortVersion, contributors, asset, shot, load };
};
