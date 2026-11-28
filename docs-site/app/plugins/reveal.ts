// v-reveal: content rises in as it scrolls into view. The server renders it
// visible, so crawlers and visitors without JavaScript always get the content.
// Anything already on screen is left alone, and so is anyone who prefers
// reduced motion.
export default defineNuxtPlugin((nuxtApp) => {
  if (import.meta.server) {
    nuxtApp.vueApp.directive('reveal', { getSSRProps: () => ({}) });
    return;
  }

  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const canObserve = 'IntersectionObserver' in window;

  const observer = canObserve && !reduced
    ? new IntersectionObserver((entries) => {
        for (const entry of entries) {
          if (!entry.isIntersecting) continue;
          entry.target.classList.add('is-in');
          observer && observer.unobserve(entry.target);
        }
      }, { rootMargin: '0px 0px -8% 0px', threshold: 0.05 })
    : null;

  nuxtApp.vueApp.directive('reveal', {
    mounted(el: HTMLElement) {
      if (!observer || el.getBoundingClientRect().top < window.innerHeight) return;
      el.classList.add('reveal');
      observer.observe(el);
    },
    unmounted(el: HTMLElement) {
      observer?.unobserve(el);
    },
  });
});
