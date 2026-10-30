<template>
  <div class="schema-form" :class="{ 'schema-form--nested': nested }">
    <template v-for="field in visibleFields" :key="field.id">
      <!-- Checkbox: the label sits beside the box, the way every other toggle
           in the app reads. -->
      <div v-if="field.type === 'boolean'" class="sf-field sf-field--check">
        <label class="sf-check">
          <input
            type="checkbox"
            :checked="!!valueOf(field)"
            :disabled="disabled"
            @change="set(field, ($event.target as HTMLInputElement).checked)"
          />
          <span>{{ field.label }}</span>
        </label>
        <p v-if="field.help" class="sf-help">{{ field.help }}</p>
      </div>

      <div v-else class="sf-field" :class="{ 'sf-field--wide': isWide(field) }">
        <label class="sf-label" :for="inputId(field)">
          {{ field.label }}<span v-if="field.required" class="sf-required" aria-hidden="true">*</span>
        </label>

        <input
          v-if="field.type === 'string'"
          :id="inputId(field)"
          type="text"
          :value="valueOf(field) ?? ''"
          :placeholder="field.placeholder ?? ''"
          :disabled="disabled"
          :class="{ invalid: isMissing(field) }"
          @change="set(field, textOrUndefined(($event.target as HTMLInputElement).value))"
        />

        <textarea
          v-else-if="field.type === 'text'"
          :id="inputId(field)"
          rows="3"
          :value="(valueOf(field) as string) ?? ''"
          :placeholder="field.placeholder ?? ''"
          :disabled="disabled"
          :class="{ invalid: isMissing(field) }"
          @change="set(field, textOrUndefined(($event.target as HTMLTextAreaElement).value))"
        />

        <input
          v-else-if="field.type === 'number' || field.type === 'integer'"
          :id="inputId(field)"
          type="number"
          :value="valueOf(field) ?? ''"
          :min="field.min"
          :max="field.max"
          :step="field.step ?? (field.type === 'integer' ? 1 : 'any')"
          :placeholder="field.placeholder ?? ''"
          :disabled="disabled"
          :class="{ invalid: isMissing(field) }"
          @change="onNumber(field, $event)"
        />

        <!-- allowCustom: a typed value has to be possible as well as a picked
             one — a MIDI port that is not plugged in right now is still the
             right port for the show. -->
        <template v-else-if="field.type === 'enum' && field.allowCustom">
          <input
            :id="inputId(field)"
            type="text"
            :list="inputId(field) + '-list'"
            :value="valueOf(field) ?? ''"
            :placeholder="field.placeholder ?? ''"
            :disabled="disabled"
            :class="{ invalid: isMissing(field) }"
            @change="onEnum(field, ($event.target as HTMLInputElement).value)"
          />
          <datalist :id="inputId(field) + '-list'">
            <option v-for="o in field.options ?? []" :key="String(o.value)" :value="o.value">{{ o.label }}</option>
          </datalist>
        </template>

        <select
          v-else-if="field.type === 'enum'"
          :id="inputId(field)"
          :value="enumSelectValue(field)"
          :disabled="disabled"
          :class="{ invalid: isMissing(field) }"
          @change="onEnum(field, ($event.target as HTMLSelectElement).value)"
        >
          <option v-if="!field.required || valueOf(field) === undefined" value="">{{ t('events.form.none') }}</option>
          <option v-for="o in field.options ?? []" :key="String(o.value)" :value="String(o.value)">{{ o.label }}</option>
          <!-- A saved value the schema no longer offers stays visible rather
               than silently reading as "none". -->
          <option v-if="enumUnknown(field)" :value="String(valueOf(field))">{{ String(valueOf(field)) }}</option>
        </select>

        <select
          v-else-if="field.type === 'item'"
          :id="inputId(field)"
          :value="(valueOf(field) as string) ?? ''"
          :disabled="disabled"
          :class="{ invalid: isMissing(field) }"
          @change="set(field, textOrUndefined(($event.target as HTMLSelectElement).value))"
        >
          <option value="">{{ t('events.form.none') }}</option>
          <option v-for="o in itemOptions" :key="o.uuid" :value="o.uuid">{{ o.label }}</option>
          <option v-if="itemUnknown(field)" :value="String(valueOf(field))">{{ t('events.form.missingItem') }}</option>
        </select>

        <div v-else-if="field.type === 'list'" class="sf-list">
          <div v-for="(row, i) in rowsOf(field)" :key="i" class="sf-list-row">
            <span class="sf-list-index">{{ i + 1 }}</span>
            <SchemaForm
              class="sf-list-form"
              :fields="field.fields ?? []"
              :model-value="row"
              :disabled="disabled"
              nested
              @update:model-value="(v: Record<string, unknown>) => setRow(field, i, v)"
            />
            <div class="sf-list-actions">
              <button type="button" class="sf-icon-btn" :disabled="disabled || i === 0"
                      :title="t('events.form.moveUp')" @click="moveRow(field, i, -1)">
                <span class="material-symbols-rounded">arrow_upward</span>
              </button>
              <button type="button" class="sf-icon-btn" :disabled="disabled || i === rowsOf(field).length - 1"
                      :title="t('events.form.moveDown')" @click="moveRow(field, i, 1)">
                <span class="material-symbols-rounded">arrow_downward</span>
              </button>
              <button type="button" class="sf-icon-btn" :disabled="disabled"
                      :title="t('events.form.removeRow')" @click="removeRow(field, i)">
                <span class="material-symbols-rounded">close</span>
              </button>
            </div>
          </div>
          <button type="button" class="sf-add-row" :disabled="disabled" @click="addRow(field)">
            <span class="material-symbols-rounded">add</span>
            <span>{{ t('events.form.addRow') }}</span>
          </button>
        </div>

        <!-- A type this build doesn't know, from a newer server: show the raw
             value read-only rather than dropping the field or guessing. -->
        <input v-else :id="inputId(field)" type="text" readonly :value="JSON.stringify(valueOf(field) ?? null)" />

        <p v-if="field.help" class="sf-help">{{ field.help }}</p>
      </div>
    </template>
  </div>
</template>

<script setup lang="ts">
// Generic form over a server-published field schema (#84). The client has no
// plugin-specific UI at all: OSC, MIDI, HTTP and the internal LivePlay plugin
// all render through here, so a plugin added to the server needs no client
// change. Emits a fresh object on every commit; text commits on change (blur
// or Enter), not per keystroke, like the rest of the Properties panel.
import { useId } from 'vue';
import type { FieldSchema } from '~/types/events';
import { defaultsFor, isFieldVisible } from '~/utils/events';

const props = withDefaults(defineProps<{
  fields: FieldSchema[];
  modelValue: Record<string, unknown> | null | undefined;
  disabled?: boolean;
  /** A row inside a list field: tighter, and no outer gap. */
  nested?: boolean;
}>(), {
  disabled: false,
  nested: false,
});

const emit = defineEmits<{ 'update:modelValue': [value: Record<string, unknown>] }>();

const { t } = useLocalization();
const { currentProject, getAllItemsFlat, formatItemIndex } = useProject();

// For label/for and the datalist link. Rows of a list nest forms with the
// same field ids, so the prefix has to be per instance.
const uid = useId();
const inputId = (f: FieldSchema) => `${uid}-${f.id}`;

const values = computed<Record<string, unknown>>(() => props.modelValue ?? {});

const visibleFields = computed(() =>
  props.fields.filter(f => isFieldVisible(f, props.fields, values.value)));

const valueOf = (f: FieldSchema): any => values.value[f.id];

const isWide = (f: FieldSchema) => f.type === 'text' || f.type === 'list';

const isMissing = (f: FieldSchema) => {
  if (!f.required) return false;
  const v = valueOf(f);
  return v === undefined || v === null || v === '';
};

function set(f: FieldSchema, v: unknown) {
  const next = { ...values.value };
  if (v === undefined) delete next[f.id];
  else next[f.id] = v;
  emit('update:modelValue', next);
}

const textOrUndefined = (s: string) => (s === '' ? undefined : s);

function onNumber(f: FieldSchema, e: Event) {
  const input = e.target as HTMLInputElement;
  if (input.value.trim() === '') { set(f, undefined); return; }
  let n = Number(input.value);
  if (!Number.isFinite(n)) { input.value = String(valueOf(f) ?? ''); return; }
  if (f.type === 'integer') n = Math.round(n);
  // The browser's min/max only constrain the spinner, not what is typed.
  if (typeof f.min === 'number') n = Math.max(f.min, n);
  if (typeof f.max === 'number') n = Math.min(f.max, n);
  input.value = String(n);
  set(f, n);
}

// Options carry their own type (a MIDI channel may be the number 1), but a
// select or a text box only hands back strings — map back to the option's.
function onEnum(f: FieldSchema, raw: string) {
  if (raw === '') { set(f, undefined); return; }
  const opt = (f.options ?? []).find(o => String(o.value) === raw);
  set(f, opt ? opt.value : raw);
}

const enumSelectValue = (f: FieldSchema) => {
  const v = valueOf(f);
  return v === undefined || v === null ? '' : String(v);
};

const enumUnknown = (f: FieldSchema) => {
  const v = valueOf(f);
  if (v === undefined || v === null || v === '') return false;
  return !(f.options ?? []).some(o => String(o.value) === String(v));
};

// ---- item picker -------------------------------------------------------
// Every playlist item and group, flattened in playlist order with the number
// the operator knows it by, then the cart's own cues by slot.
const itemOptions = computed(() => {
  const p = currentProject.value;
  if (!p) return [];
  const out: { uuid: string; label: string }[] = [];
  for (const it of getAllItemsFlat(p.items)) {
    if (it.type !== 'audio' && it.type !== 'group') continue;
    out.push({ uuid: it.uuid, label: `${formatItemIndex(it.index)} · ${it.displayName}` });
  }
  const onPlaylist = new Set(out.map(o => o.uuid));
  const slots = [...(p.cartItems ?? [])].sort((a, b) => a.slot - b.slot);
  const cartByUuid = new Map((p.cartOnlyItems ?? []).map(i => [i.uuid, i]));
  for (const c of slots) {
    const it = cartByUuid.get(c.itemUuid);
    if (!it || onPlaylist.has(it.uuid)) continue;
    out.push({ uuid: it.uuid, label: t('events.form.cartItem', { slot: c.slot + 1, name: it.displayName }) });
  }
  return out;
});

const itemUnknown = (f: FieldSchema) => {
  const v = valueOf(f);
  return typeof v === 'string' && v !== '' && !itemOptions.value.some(o => o.uuid === v);
};

// ---- list rows ----------------------------------------------------------
const rowsOf = (f: FieldSchema): Record<string, unknown>[] => {
  const v = valueOf(f);
  return Array.isArray(v) ? v : [];
};

function setRows(f: FieldSchema, rows: Record<string, unknown>[]) {
  set(f, rows);
}
function setRow(f: FieldSchema, i: number, row: Record<string, unknown>) {
  const rows = [...rowsOf(f)];
  rows[i] = row;
  setRows(f, rows);
}
function addRow(f: FieldSchema) {
  setRows(f, [...rowsOf(f), defaultsFor(f.fields)]);
}
function removeRow(f: FieldSchema, i: number) {
  const rows = [...rowsOf(f)];
  rows.splice(i, 1);
  setRows(f, rows);
}
function moveRow(f: FieldSchema, i: number, delta: number) {
  const rows = [...rowsOf(f)];
  const j = i + delta;
  if (j < 0 || j >= rows.length) return;
  [rows[i], rows[j]] = [rows[j]!, rows[i]!];
  setRows(f, rows);
}
</script>

<style scoped>
.schema-form {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: var(--spacing-sm) var(--spacing-md);
  align-items: start;
}

.schema-form--nested {
  gap: var(--spacing-xs) var(--spacing-sm);
}

.sf-field {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  min-width: 0;
  color: var(--color-text-secondary);
}

.sf-field--wide {
  grid-column: 1 / -1;
}

.sf-field--check {
  justify-content: flex-end;
  align-self: end;
  min-height: 34px;
}

.sf-label {
  font-size: 12px;
  font-weight: 500;
}

.sf-required {
  color: var(--color-accent);
  margin-left: 2px;
}

.sf-field input[type="text"],
.sf-field input[type="number"],
.sf-field select,
.sf-field textarea {
  width: 100%;
  padding: var(--spacing-xs) var(--spacing-sm);
  min-height: 30px;
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  color: var(--color-text);
  font-size: 13px;
  font-family: inherit;

  &:focus {
    outline: none;
    border-color: var(--color-accent);
  }

  &.invalid {
    border-color: #e53e3e;
  }

  &[readonly] {
    opacity: 0.6;
  }
}

.sf-field textarea {
  resize: vertical;
  font-family: var(--font-mono, ui-monospace, monospace);
  font-size: 12px;
}

.sf-check {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  font-size: 13px;
  color: var(--color-text-primary);
  cursor: pointer;

  input {
    width: 16px;
    height: 16px;
    accent-color: var(--color-accent);
    cursor: pointer;
  }
}

.sf-help {
  margin: 0;
  font-size: 11px;
  color: var(--color-text-secondary);
}

.sf-list {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
}

.sf-list-row {
  display: flex;
  align-items: flex-start;
  gap: var(--spacing-sm);
  padding: var(--spacing-xs) var(--spacing-sm);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  background: var(--color-surface);
}

.sf-list-index {
  flex: 0 0 auto;
  min-width: 16px;
  padding-top: 6px;
  font-size: 11px;
  font-variant-numeric: tabular-nums;
  color: var(--color-text-secondary);
}

.sf-list-form {
  flex: 1;
  min-width: 0;
}

.sf-list-actions {
  display: flex;
  gap: 2px;
  flex: 0 0 auto;
}

.sf-icon-btn,
.sf-add-row {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-xs);
  background: none;
  border: 1px solid transparent;
  border-radius: 4px;
  color: var(--color-text-secondary);
  cursor: pointer;

  .material-symbols-rounded {
    font-size: 16px;
  }

  &:hover:not(:disabled) {
    color: var(--color-text-primary);
    background-color: var(--color-surface-hover);
  }

  &:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }
}

.sf-icon-btn {
  width: 24px;
  height: 24px;
}

.sf-add-row {
  align-self: flex-start;
  padding: var(--spacing-xs) var(--spacing-sm);
  border-color: var(--color-border);
  font-size: 12px;
}
</style>
