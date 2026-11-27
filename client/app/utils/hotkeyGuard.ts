// =============================================================================
// utils/hotkeyGuard.ts — when a window-level shortcut must stay out of the way
// -----------------------------------------------------------------------------
// The transport, cart and edit shortcuts listen on `window`. A key that a
// control on the page has already handled, or that belongs to the control
// with focus, must not also reach them: Esc that closes a popover is not
// Stop All, Enter on a tab is not Play Selected, Delete in a list of events
// is not "delete this cue".
// =============================================================================

/** Elements whose keys are their own: typing, choosing, editing. */
const OWN_KEYS_SELECTOR = [
  'input',
  'textarea',
  'select',
  '[contenteditable=""]',
  '[contenteditable="true"]',
  '[role="listbox"]',
  '[role="option"]',
  '[role="combobox"]',
  '[role="textbox"]',
  '[role="slider"]',
  '[role="spinbutton"]',
  '[role="menu"]',
  '[role="menuitem"]',
  // A region that handles its own keys (a popover, a dialog, an editor list)
  // marks itself so.
  '[data-own-keys]',
].join(',');

/** True when the element with focus (or the event's target) takes its own keys. */
export const focusTakesKeys = (e?: Event): boolean => {
  const candidates: Element[] = [];
  const active = typeof document !== 'undefined' ? document.activeElement : null;
  if (active && active !== document.body) candidates.push(active);
  const target = e?.target;
  if (target instanceof Element && target !== document.body) candidates.push(target);
  return candidates.some(el =>
    (el as HTMLElement).isContentEditable || !!el.closest(OWN_KEYS_SELECTOR));
};

/**
 * True when a window-level shortcut should ignore this key: something on the
 * page already handled it (defaultPrevented), or focus is on a control that
 * owns its keys. Enter and Space on a focused button, link or tab activate
 * that control, so they are its keys too.
 */
export const shouldIgnoreHotkey = (e: KeyboardEvent): boolean => {
  if (e.defaultPrevented) return true;
  if (focusTakesKeys(e)) return true;
  if (e.key === 'Enter' || e.key === ' ') {
    const el = (typeof document !== 'undefined' ? document.activeElement : null) as HTMLElement | null;
    if (el && el !== document.body &&
        el.closest('[role="tab"], a[href], [role="button"]:not(button)')) return true;
  }
  return false;
};
