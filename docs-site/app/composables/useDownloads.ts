import { computed, ref } from 'vue';
import { useSiteInfo } from './useSiteInfo';

export type PlatformKey = 'windows' | 'macArm' | 'macIntel' | 'linux';

const allPlatformKeys: PlatformKey[] = ['windows', 'macArm', 'macIntel', 'linux'];
const osPlatformMap: Record<string, PlatformKey[]> = {
  windows: ['windows'],
  mac: ['macArm', 'macIntel'],
  linux: ['linux'],
};

// Approximate installer sizes in MB, from the most recent published release.
export const platformSizes: Record<PlatformKey, number> = {
  windows: 175,
  macArm: 190,
  macIntel: 190,
  linux: 210,
};

const detectedOS = ref<string | null>(null);

const detectOS = (): string | null => {
  if (typeof navigator === 'undefined') return null;
  const uaData = (navigator as any).userAgentData;
  if (uaData?.platform) {
    const p = String(uaData.platform).toLowerCase();
    if (p.includes('win')) return 'windows';
    if (p.includes('mac')) return 'mac';
    if (p.includes('linux') || p.includes('android') || p.includes('chrome os')) return 'linux';
  }
  const ua = (navigator.userAgent || '').toLowerCase();
  const platform = ((navigator as any).platform || '').toLowerCase();
  if (ua.includes('windows') || platform.includes('win')) return 'windows';
  if (ua.includes('mac') || platform.includes('mac')) return 'mac';
  if (ua.includes('linux') || ua.includes('x11') || ua.includes('android') || platform.includes('linux')) return 'linux';
  return null;
};

export const useDownloads = () => {
  const { version } = useSiteInfo();

  const prominentKeys = computed<PlatformKey[]>(() => {
    if (!detectedOS.value) return [...allPlatformKeys];
    return osPlatformMap[detectedOS.value] ?? [...allPlatformKeys];
  });
  const otherKeys = computed<PlatformKey[]>(() =>
    allPlatformKeys.filter((k) => !prominentKeys.value.includes(k)));

  // Release asset names. Windows uses hyphens so the file, the GitHub asset and
  // latest.yml all agree; macOS has two separate per-architecture builds.
  const links = computed(() => {
    const v = version.value;
    const base = `https://github.com/tdoukinitsas/liveplay/releases/download/v${v}`;
    return {
      windows: `${base}/LivePlay-Setup-${v}.exe`,
      macArm: `${base}/LivePlay-${v}-arm64.dmg`,
      macIntel: `${base}/LivePlay-${v}.dmg`,
      linux: `${base}/LivePlay-${v}.AppImage`,
      deb: `${base}/liveplay_${v}_amd64.deb`,
      rpm: `${base}/liveplay-${v}.x86_64.rpm`,
      macArmZip: `${base}/LivePlay-${v}-arm64-mac.zip`,
      macIntelZip: `${base}/LivePlay-${v}-mac.zip`,
    };
  });

  const detect = () => { detectedOS.value = detectOS(); };

  return { detectedOS, prominentKeys, otherKeys, links, detect };
};
