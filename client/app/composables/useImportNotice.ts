import { ref } from 'vue';
import { isPlayableAudio } from '~/utils/audioFormats';

const NOTICE_MS = 9000;

// Shared so a drop on any pane reports in the same place.
const rejected = ref<string[]>([]);
let timer: ReturnType<typeof setTimeout> | undefined;

const baseName = (path: string) => path.split(/[\\/]/).pop() || path;

export const useImportNotice = () => {
  const dismiss = () => {
    clearTimeout(timer);
    rejected.value = [];
  };

  /** The items the engine can play. Anything else is named in a notice. */
  const keepPlayable = <T>(items: T[], nameOf: (item: T) => string): T[] => {
    const refused = items.filter(item => !isPlayableAudio(nameOf(item)));
    if (refused.length) {
      rejected.value = refused.map(item => baseName(nameOf(item)));
      clearTimeout(timer);
      timer = setTimeout(dismiss, NOTICE_MS);
    }
    return items.filter(item => isPlayableAudio(nameOf(item)));
  };

  return { rejected, keepPlayable, dismiss };
};
