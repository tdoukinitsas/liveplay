<template>
  <!--
    Aux sends (M1): where this bus sends a COPY of itself, at what level, and
    whether that copy is taken before or after its own fader.

    Deliberately not the same control as the output picker above it. An output
    is one destination for the whole signal and reads as a choice between
    alternatives, which is why it is a select. A send is a list that grows, each
    row carrying its own level and tap — so it is a list, and adding one is an
    explicit act rather than a side effect of changing a dropdown.

    The server is the authority on what is legal — cycles through either kind of
    edge, the preview bus at either end, a bus sending to itself — and this asks
    rather than predicts: the whole list is PATCHed and a 409 reverts nothing
    locally (the refetch restores the truth) and surfaces the server's own words
    inline. Same contract BusOutputSelect uses, and for the same reason: two
    implementations of the rules would eventually disagree about them.
  -->
  <div class="bsl">
    <ul v-if="sends.length" class="bsl__list">
      <li v-for="s in sends" :key="s.id" class="bsl__row">
        <span class="bsl__name" :style="{ '--dot': colorOf(s.id) }" :title="nameOf(s.id)">
          {{ nameOf(s.id) }}
        </span>

        <!-- Typed, not a slider: a send level is set to a number the operator
             has in mind, and there is no room here for a fader worth dragging. -->
        <span class="bsl__level">
          <input
            type="text"
            inputmode="decimal"
            class="bsl__db"
            :value="fmtDb(s.levelDb)"
            :aria-label="t('mixer.sendLevel', { name: nameOf(s.id) })"
            @focus="($event.target as HTMLInputElement).select()"
            @keydown.enter="($event.target as HTMLInputElement).blur()"
            @keydown.esc="revertDb($event, s)"
            @change="onLevel($event, s)"
          />
          <span class="bsl__unit">dB</span>
        </span>

        <!-- Two states, both named. A toggle labelled only by its current value
             cannot say what pressing it would do, and pre/post is the one thing
             on this row somebody will get wrong in a hurry. -->
        <button
          type="button"
          class="bsl__tap"
          :class="{ 'bsl__tap--pre': s.tap === 'pre' }"
          :title="s.tap === 'pre' ? t('mixer.sendTapPreHelp') : t('mixer.sendTapPostHelp')"
          @click="onTap(s)"
        >
          {{ s.tap === 'pre' ? t('mixer.sendTapPre') : t('mixer.sendTapPost') }}
        </button>

        <button
          type="button"
          class="bsl__del"
          :title="t('mixer.sendRemove', { name: nameOf(s.id) })"
          :aria-label="t('mixer.sendRemove', { name: nameOf(s.id) })"
          @click="onRemove(s)"
        >
          <span class="material-symbols-rounded">close</span>
        </button>
      </li>
    </ul>
    <p v-else class="bsl__none">{{ t('mixer.sendsNone') }}</p>

    <!-- Hidden rather than disabled when there is nowhere left to send: a
         disabled control invites the operator to work out why, and the answer
         ("every other bus already has one") is not worth their time mid-show. -->
    <select v-if="candidates.length" class="bsl__add" :value="''" @change="onAdd">
      <option value="" disabled>{{ t('mixer.sendAdd') }}</option>
      <option v-for="b in candidates" :key="b.id" :value="b.id">{{ b.name }}</option>
    </select>

    <p v-if="errorMsg" class="bsl__err">{{ errorMsg }}</p>
  </div>
</template>

<script setup lang="ts">
import type { Bus, BusSend } from '~/types/project';

const props = defineProps<{
  bus: Bus;
  /** Every bus, so a destination can be named and the candidates worked out. */
  buses: Bus[];
}>();

const { t } = useLocalization();
const server = useLiveplayServer();

const sends = computed<BusSend[]>(() => props.bus.sends ?? []);

const nameOf = (id: string) => props.buses.find(b => b.id === id)?.name ?? id;
const colorOf = (id: string) =>
  props.buses.find(b => b.id === id)?.color || 'var(--color-accent)';

// What is offered. Only the rules that are FLAT and certain are applied here —
// not itself, not the preview bus (nothing may feed it), and not a bus it
// already sends to. Cycles are deliberately NOT predicted: that needs a walk of
// both edge kinds, the server does it exactly, and a second implementation here
// would eventually disagree with it. Offering one and being refused costs a
// click and tells the truth; hiding one wrongly is invisible.
const candidates = computed(() =>
  props.buses
    .filter(b => b.id !== props.bus.id && !b.preview)
    .filter(b => !sends.value.some(s => s.id === b.id))
    .sort((a, b) => a.order - b.order));

const errorMsg = ref('');
let errorTimer: ReturnType<typeof setTimeout> | null = null;
onBeforeUnmount(() => { if (errorTimer) clearTimeout(errorTimer); });

function setError(msg: string) {
  errorMsg.value = msg;
  if (errorTimer) clearTimeout(errorTimer);
  if (msg) errorTimer = setTimeout(() => { errorMsg.value = ''; }, 6000);
}

// Best-effort extraction of the server's `{"error":"..."}` out of the Error
// rest() throws (`"<status> <statusText> — <raw body>"`). Lifted from
// BusOutputSelect, which reads the same shape off the same endpoint.
function serverErrorText(err: unknown): string {
  const msg = err instanceof Error ? err.message : String(err);
  const body = msg.slice(msg.indexOf('—') + 1).trim();
  try {
    const parsed = JSON.parse(body);
    if (parsed && typeof parsed.error === 'string') return parsed.error;
  } catch { /* not JSON; fall through */ }
  return body || msg;
}

const fmtDb = (db: number) => (db <= -120 ? '-inf' : db.toFixed(1));

// The whole list every time, because that is what the endpoint takes: a send
// has no identity apart from its destination, so there is nothing to address a
// delta to. It also means a refused patch leaves the bus exactly as it was.
async function commit(next: BusSend[]) {
  try {
    await server.patchBus(props.bus.id, { sends: next });
    setError('');
  } catch (e) {
    setError(serverErrorText(e));
    // No local revert: the list is rendered from the server's copy, and
    // patchBus refetches, so what is on screen is already the truth.
    await server.fetchBuses().catch(() => { /* offline; the next broadcast fixes it */ });
  }
}

function onAdd(e: Event) {
  const el = e.target as HTMLSelectElement;
  const id = el.value;
  el.value = '';                      // the row is an action, not a selection
  if (!id) return;
  // 0 dB and post-fader: the same signal the output edge would carry, at the
  // same point. A send that starts silent looks broken, and one that starts
  // pre-fader would be a surprising default for anything but a foldback.
  void commit([...sends.value, { id, levelDb: 0, tap: 'post' }]);
}

function onRemove(s: BusSend) {
  void commit(sends.value.filter(x => x.id !== s.id));
}

function onTap(s: BusSend) {
  void commit(sends.value.map(x =>
    x.id === s.id ? { ...x, tap: x.tap === 'pre' ? 'post' as const : 'pre' as const } : x));
}

function revertDb(e: Event, s: BusSend) {
  const el = e.target as HTMLInputElement;
  el.value = fmtDb(s.levelDb);
  el.blur();
}

function onLevel(e: Event, s: BusSend) {
  const el = e.target as HTMLInputElement;
  const raw = el.value.trim().toLowerCase().replace(/db$/, '').trim();
  const db = /^-?inf/.test(raw) ? -120 : parseFloat(raw);
  if (Number.isNaN(db)) { el.value = fmtDb(s.levelDb); return; }
  // Matches the server's own clamp, so the field cannot show a value the
  // document will not hold.
  const clamped = Math.max(-120, Math.min(12, db));
  el.value = fmtDb(clamped);
  if (clamped === s.levelDb) return;
  void commit(sends.value.map(x => (x.id === s.id ? { ...x, levelDb: clamped } : x)));
}
</script>

<style scoped>
.bsl {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.bsl__list {
  display: flex;
  flex-direction: column;
  gap: 3px;
  list-style: none;
  margin: 0;
  padding: 0;
}

.bsl__row {
  display: flex;
  align-items: center;
  gap: 4px;
  min-width: 0;
}

/* The destination's own colour as a leading bar, the same way the strip
   carries it across its top — a send is found by which bus it feeds. */
.bsl__name {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: 11px;
  color: var(--color-text-primary);
  border-left: 3px solid var(--dot);
  padding-left: 5px;
}

.bsl__level {
  display: flex;
  align-items: baseline;
  gap: 2px;
  flex: 0 0 auto;
}

.bsl__db {
  width: 46px;
  padding: 2px 3px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-primary);
  font-size: 11px;
  font-variant-numeric: tabular-nums;
  text-align: right;
}
.bsl__db:focus {
  outline: none;
  border-color: var(--color-accent);
}

.bsl__unit {
  font-size: 9px;
  color: var(--color-text-secondary);
}

.bsl__tap {
  flex: 0 0 auto;
  min-width: 38px;
  padding: 2px 5px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-secondary);
  font-size: 9px;
  font-weight: 600;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  cursor: pointer;
}
.bsl__tap:hover { border-color: var(--color-accent); color: var(--color-text-primary); }
/* Pre-fader is the one that keeps playing when the house fader comes down, so
   it is the state worth being able to spot without reading. */
.bsl__tap--pre {
  background: var(--color-accent);
  border-color: var(--color-accent);
  color: #fff;
}

.bsl__del {
  flex: 0 0 auto;
  display: flex;
  align-items: center;
  justify-content: center;
  width: 18px;
  height: 18px;
  padding: 0;
  background: none;
  border: none;
  border-radius: var(--border-radius-sm);
  color: var(--color-text-disabled);
  cursor: pointer;
}
.bsl__del:hover { background: var(--color-surface-hover); color: var(--color-danger); }
.bsl__del .material-symbols-rounded { font-size: 14px; }

.bsl__none {
  margin: 0;
  font-size: 11px;
  color: var(--color-text-disabled);
}

.bsl__add {
  width: 100%;
  padding: 3px 4px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-secondary);
  font-size: 11px;
}
.bsl__add:focus { outline: none; border-color: var(--color-accent); }

.bsl__err {
  margin: 0;
  font-size: 10px;
  color: var(--color-warning);
}
</style>
