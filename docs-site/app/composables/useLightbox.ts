import { ref } from 'vue';

const current = ref<{ src: string; alt: string } | null>(null);

// One shared viewer, so any screenshot can be opened at full size.
export const useLightbox = () => ({
  current,
  open: (src: string, alt: string) => { current.value = { src, alt }; },
  close: () => { current.value = null; },
});
