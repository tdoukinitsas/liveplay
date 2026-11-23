import type { RouterConfig } from '@nuxt/schema';

// The header is sticky, so a jump to a section stops just below it.
export default <RouterConfig>{
  scrollBehavior(to, from, saved) {
    if (saved) return saved;
    if (to.hash) {
      // Arriving from another page, go straight there rather than animating
      // down a page the visitor hasn't seen yet.
      const behavior = to.path === from.path ? 'smooth' : 'instant';
      return new Promise((resolve) => {
        // A jump from another page has to wait for this one to render.
        setTimeout(() => resolve({ el: to.hash, top: 76, behavior }), 120);
      });
    }
    return { top: 0 };
  },
};
