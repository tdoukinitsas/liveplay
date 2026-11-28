// =============================================================================
// utils/hotkeyGuard.ts — when a window-level shortcut must stay out of the way
// -----------------------------------------------------------------------------
// The transport, cart and edit shortcuts listen on `window`. A key that a
// control on the page has already handled, or that belongs to the control
// with focus, must not also reach them: Esc that closes a popover is not
// Stop All, Enter on a tab is not Play Selected, Delete in a list of events
// is not "delete this cue".
//
// But the transport keys are the operator's way out, and a control keeps
// focus after a mouse click (Chromium leaves it on a <select> after a pick, on
// a row after a click). So which control "owns" a key depends on the key
// (R2-06, R2-14):
//
//   * Esc reaches Stop All from anywhere except while typing in a field. A
//     control that uses Esc (a popover, a menu, a selection) handles it and
//     stops it, which counts as handled (defaultPrevented / not propagated).
//   * Space and Enter go to GO and Play Selected except while typing, inside a
//     region that handles its own keys, on an open menu, or on a focused
//     tab, link or role="button" element (which Space/Enter activate). A
//     closed <select>, a checkbox, a slider or a list row doesn't take them:
//     the window handler prevents the native action, as 2.5 did.
//   * The arrows move the selection except while typing, on a slider, in a
//     region that handles its own keys or on a menu. A closed <select> doesn't
//     take them either, so a stray ↓ can't re-route the selected cue to
//     another bus. (Alt+↓ still opens it, and rows that use the arrows stop
//     them themselves.)
//   * Every other key (Delete, Backspace, letters, Ctrl+C…) is the focused
//     control's whenever that control takes keys at all, as before.
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

/** Where the operator is typing: every key, Esc included, is the field's. */
const TEXT_ENTRY_SELECTOR = [
  'textarea',
  '[contenteditable=""]',
  '[contenteditable="true"]',
  '[role="textbox"]',
  '[role="combobox"]',
  '[role="spinbutton"]',
].join(',');

/** input types that take typing (the rest are buttons, boxes and sliders). */
const TEXT_INPUT_TYPES = new Set([
  '', 'text', 'search', 'number', 'email', 'password', 'url', 'tel',
  'date', 'time', 'datetime-local', 'month', 'week',
]);

/** Regions that handle Space, Enter and the arrows themselves. */
const OWN_REGION_SELECTOR = '[data-own-keys], [role="menu"], [role="menuitem"]';

/** Controls that use the arrows for their own value. */
const ARROW_CONTROL_SELECTOR = 'input[type="range"], [role="slider"]';

const focusCandidates = (e?: Event): Element[] => {
  const candidates: Element[] = [];
  const doc = typeof document !== 'undefined' ? document : null;
  const active = doc ? doc.activeElement : null;
  if (active && active !== doc?.body) candidates.push(active);
  const target = e?.target;
  if (typeof Element !== 'undefined' && target instanceof Element && target !== doc?.body)
    candidates.push(target);
  return candidates;
};

const isTextEntry = (el: Element): boolean => {
  if ((el as HTMLElement).isContentEditable) return true;
  if (el.tagName === 'INPUT') {
    const type = ((el as HTMLInputElement).type || el.getAttribute('type') || '').toLowerCase();
    return TEXT_INPUT_TYPES.has(type);
  }
  return !!el.closest(TEXT_ENTRY_SELECTOR);
};

/** True when the element with focus (or the event's target) takes its own keys. */
export const focusTakesKeys = (e?: Event): boolean =>
  focusCandidates(e).some(el =>
    (el as HTMLElement).isContentEditable || !!el.closest(OWN_KEYS_SELECTOR));

/** True while the operator is typing in a field (focus or the event's target). */
export const typingInField = (e?: Event): boolean => focusCandidates(e).some(isTextEntry);

const ARROWS = new Set(['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight']);

/**
 * True when a window-level shortcut should ignore this key: something on the
 * page already handled it (defaultPrevented), or the control with focus owns
 * this key (see the rules above).
 */
export const shouldIgnoreHotkey = (e: KeyboardEvent): boolean => {
  if (e.defaultPrevented) return true;
  const key = e.key;
  const modified = e.altKey || e.ctrlKey || e.metaKey;
  const transportKey = !modified && (key === 'Escape' || key === ' ' || key === 'Enter' || ARROWS.has(key));
  if (!transportKey) return focusTakesKeys(e);

  if (typingInField(e)) return true;
  if (key === 'Escape') return false;

  const els = focusCandidates(e);
  if (els.some(el => !!el.closest(OWN_REGION_SELECTOR))) return true;
  if (ARROWS.has(key)) return els.some(el => !!el.closest(ARROW_CONTROL_SELECTOR));
  // Space / Enter activate a focused tab, link or button-like element.
  const active = (typeof document !== 'undefined' ? document.activeElement : null) as HTMLElement | null;
  if (active && active !== document.body &&
      active.closest('[role="tab"], a[href], [role="button"]:not(button)')) return true;
  return false;
};
