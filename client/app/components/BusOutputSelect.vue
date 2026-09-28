<template>
  <!--
    Where a bus sends its audio — the most consequential control on a strip,
    and the same <select> in the channel view's Sends panel. D29 fixes its
    shape; this component is that shape once, for both places.

    Groups, in order:
      Buses    — the master-role bus first (a sub-mix reaches the house by
                 feeding it), then every other bus except this one, the
                 preview bus (nothing may feed it: PFL summed back into the
                 house is the accident the whole scheme exists to prevent)
                 and any bus whose chain already reaches this one (a loop).
      Outputs  — the built-in names first, then the outputs.json names, plus
                 whatever this bus already targets if it is in neither list,
                 so the select never renders blank. Each shows its mapping.
      Devices  — the machine's enumerated devices, minus any whose name is
                 already a logical output (that row would be the same choice
                 twice). Picking one writes the device name as the target; the
                 server reports it bound while the device is present (D26),
                 and outputs.json is untouched.
      then one action row, "Edit hardware outputs…", which opens the map.

    Role holders are hardware-only: the master bus is the house and the
    preview bus is the phones, and neither may feed another bus. The server
    409s a bus-kind route on either; the Buses group is simply not offered.

    The list is a convenience. The server is the authority on what is legal —
    cycles, the preview bus as a target, a role holder routed to a bus — and
    a 409 reverts the select and surfaces the reason in its tooltip, which is
    why the PATCH goes straight to the server here rather than through the
    fire-and-forget emit('patch') path the other controls use.
  -->
  <select
    class="bos"
    :class="{ 'bos--warn': warn }"
    :value="value"
    :title="title"
    @click.stop
    @mousedown.stop
    @change="onChange"
  >
    <optgroup v-if="busOptions.length" :label="t('mixer.busesGroup')">
      <option v-for="b in busOptions" :key="'bus:' + b.id" :value="'bus:' + b.id">{{ b.name }}</option>
    </optgroup>
    <optgroup :label="t('mixer.outputsGroup')">
      <option v-for="o in outputOptions" :key="'out:' + o.name" :value="'out:' + o.name">{{ o.label }}</option>
    </optgroup>
    <optgroup v-if="deviceOptions.length" :label="t('mixer.devicesGroup')">
      <option v-for="d in deviceOptions" :key="'out:' + d" :value="'out:' + d">{{ d }}</option>
    </optgroup>
    <!-- No output at all: the bus leaves only by its aux sends. Not offered on
         a role holder, where the server refuses it — the master bus is the
         house and the preview bus is the phones. -->
    <option v-if="!bus.master && !bus.preview" :value="VALUE_NONE">{{ t('mixer.outputNone') }}</option>
    <option :value="ACTION_EDIT">{{ t('mixer.editOutputs') }}</option>
  </select>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue';
import type { Bus } from '~/types/project';
import type { OutputMap } from '~/composables/useLiveplayServer';

const props = defineProps<{
  bus: Bus;
  /** Every bus in the project, role holders included. */
  buses: Bus[];
  /** The machine's output map; null while the first fetch is in flight. */
  outputs: OutputMap | null;
}>();

const emit = defineEmits<{
  (e: 'open-output-map'): void;
  /** The server's reason for the last refused route, or '' once cleared. */
  (e: 'error', message: string): void;
}>();

const { t } = useLocalization();
const server = useLiveplayServer();

const ACTION_EDIT = '__edit_outputs__';
const BUILTIN_MAIN = 'Main Out';

// The <select> carries "out:<name>", "bus:<bus id>", or the bare sentinel for
// no output at all — which has no target to carry, so it is not a prefix.
const VALUE_NONE = '__no_output__';

const value = computed(() => {
  const o = props.bus.output;
  if (o.type === 'none') return VALUE_NONE;
  return (o.type === 'bus' ? 'bus:' : 'out:') + o.target;
});

// Does `fromId`'s existing output chain arrive at `toId`? Pointing `toId` at
// `fromId` would then close a loop.
function reachesBus(fromId: string, toId: string, all: Bus[]): boolean {
  const visited = new Set<string>();
  let cur = all.find(b => b.id === fromId);
  while (cur && cur.output.type === 'bus') {
    const nextId = cur.output.target;
    if (nextId === toId) return true;
    if (visited.has(nextId)) break;
    visited.add(nextId);
    cur = all.find(b => b.id === nextId);
  }
  return false;
}

const busOptions = computed<Bus[]>(() => {
  if (props.bus.master || props.bus.preview) return [];
  const all = props.buses;
  const ok = all.filter(b =>
    !b.preview && b.id !== props.bus.id && !reachesBus(b.id, props.bus.id, all));
  // Master first, the rest in rail order.
  return [...ok.filter(b => b.master), ...ok.filter(b => !b.master).sort((a, b) => a.order - b.order)];
});

function deviceLabel(id: string): string {
  return (server.devices ?? []).find(d => d.id === id)?.display_name ?? id;
}

// "Name — Device 3/4", "Name — default device" for an unmapped Main Out,
// "Name — unmapped" otherwise: the mapping is the part of an output you
// actually need to know, and it lived only in the modal before.
function describe(name: string): string {
  const entry = props.outputs?.outputs.find(o => o.name === name);
  const chans = entry?.channels ?? [];
  // Channels 1-based, as they are printed on the interface.
  const pair = `${t('mixer.outputChannelsShort')} ${chans.map(c => c.hwChannel + 1).join('/')}`;
  if (chans.length) {
    const dev = deviceLabel(chans[0]!.device);
    // An entry named after its own device (what picking a device creates)
    // would otherwise print the device name twice.
    if (chans.every(c => c.device === name)) return `${name} — ${pair}`;
    return `${name} — ${dev} ${pair}`;
  }
  if (name === BUILTIN_MAIN) return `${name} — ${t('mixer.outputDefaultDevice')}`;
  // The bus's own target, resolved by the server: a device this machine has
  // under a renumbered name is found, not "unmapped".
  const resolved = name === props.bus.output.target ? (props.bus.outputDevices ?? []).filter(Boolean) : [];
  if (resolved.length) return `${name} — ${resolved[0]}`;
  return `${name} — ${t('mixer.outputUnmappedShort')}`;
}

const outputNames = computed<string[]>(() => {
  const names: string[] = [];
  for (const n of props.outputs?.builtin ?? []) if (!names.includes(n)) names.push(n);
  for (const o of props.outputs?.outputs ?? []) if (!names.includes(o.name)) names.push(o.name);
  return names;
});

const outputOptions = computed(() => {
  const names = [...outputNames.value];
  const target = props.bus.output.target;
  const isDevice = (server.devices ?? []).some(d => d.display_name === target);
  // The current target, if it is neither a known output nor a device this
  // machine can see — a name from another venue, or an unplugged device.
  if (props.bus.output.type === 'output' && target && !names.includes(target) && !isDevice) {
    names.push(target);
  }
  return names.map(name => ({ name, label: describe(name) }));
});

const deviceOptions = computed<string[]>(() => {
  const taken = new Set(outputNames.value);
  const out: string[] = [];
  for (const d of server.devices ?? []) {
    if (!d.display_name || taken.has(d.display_name) || out.includes(d.display_name)) continue;
    out.push(d.display_name);
  }
  return out;
});

const unmapped = computed(() =>
  props.bus.output.type === 'output' && props.bus.bound === false);
const warn = computed(() => props.bus.bound === false);

// Transient: set on a 409, cleared after a few seconds or on the next
// successful change. Shown in the tooltip so the reason is readable without
// a blocking dialog; also emitted, for the channel view's inline line.
const errorMsg = ref('');
let errorTimer: ReturnType<typeof setTimeout> | null = null;
onBeforeUnmount(() => { if (errorTimer) clearTimeout(errorTimer); });

const title = computed(() => {
  if (errorMsg.value) return errorMsg.value;
  if (props.bus.output.type === 'bus') {
    return warn.value ? t('mixer.busRouteUnbound') : t('mixer.output');
  }
  if (props.bus.output.type === 'none') {
    // Unbound here does not mean a mapping is missing; it means nothing is
    // carrying the bus onward, sends included.
    return warn.value ? t('mixer.outputNoneSilent') : t('mixer.outputNoneHelp');
  }
  // Unmapped means something different on the preview bus. Every other bus
  // falls back to treating the name as a device and usually still plays; the
  // preview bus never falls back, so unmapped means PFL and pre-listen are
  // inaudible — worth saying outright.
  if (unmapped.value) {
    return props.bus.preview
      ? t('mixer.previewUnmapped')
      : t('mixer.outputUnmapped', { name: props.bus.output.target });
  }
  return t('mixer.output');
});

// Best-effort extraction of the server's `{"error":"..."}` body out of the
// Error thrown by rest() (`"<status> <statusText> — <raw body>"`).
function serverErrorText(err: unknown): string {
  const msg = err instanceof Error ? err.message : String(err);
  const body = msg.slice(msg.indexOf('—') + 1).trim();
  try {
    const parsed = JSON.parse(body);
    if (parsed && typeof parsed.error === 'string') return parsed.error;
  } catch { /* not JSON; fall through */ }
  return body || msg;
}

function setError(msg: string) {
  errorMsg.value = msg;
  emit('error', msg);
  if (errorTimer) clearTimeout(errorTimer);
  if (msg) errorTimer = setTimeout(() => { errorMsg.value = ''; emit('error', ''); }, 6000);
}

async function onChange(e: Event) {
  const el = e.target as HTMLSelectElement;
  const v = el.value;
  const prev = value.value;
  // The action row is not a destination: put the select back and open the
  // editor.
  if (v === ACTION_EDIT) {
    el.value = prev;
    emit('open-output-map');
    return;
  }
  const patch = v === VALUE_NONE
    ? { output: { type: 'none' as const, target: '' } }
    : v.startsWith('bus:')
      ? { output: { type: 'bus' as const, target: v.slice(4) } }
      : { output: { type: 'output' as const, target: v.slice(4) } };
  try {
    await server.patchBus(props.bus.id, patch);
    // Where a bus sends its audio is part of the show. This picker patches the
    // server directly rather than emitting up to MixerPanel, so it has to ask
    // for the save itself — otherwise a route change reached disk only if some
    // other edit happened to save afterwards.
    void useProject().saveProject();
    setError('');
  } catch (err) {
    // Rejected: put the select back where the server still has it. Vue won't
    // force this by itself — the bound value hasn't changed from its own
    // point of view, only the browser's live selection has.
    el.value = prev;
    setError(serverErrorText(err));
  }
}
</script>

<style scoped>
/* Bare: the strip and the channel view each size and frame it themselves
   (see .strip__output and .det__field select in their files). */
.bos {
  min-width: 0;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
}
.bos--warn { border-color: var(--color-warning); }
</style>
