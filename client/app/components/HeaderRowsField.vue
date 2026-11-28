<template>
  <div class="hdr" :id="id">
    <div v-for="(row, i) in rows" :key="i" class="hdr-row">
      <template v-if="row.raw !== undefined">
        <!-- A line with no colon: kept as it is; it may be a pasted secret. -->
        <input class="hdr-raw" type="password" :value="row.raw" readonly
               :aria-label="t('events.headers.rawLine')" />
      </template>
      <template v-else>
        <input
          class="hdr-name"
          type="text"
          :value="row.name"
          :placeholder="t('events.headers.name')"
          :aria-label="t('events.headers.name')"
          :disabled="disabled"
          spellcheck="false"
          autocomplete="off"
          @change="setName(i, ($event.target as HTMLInputElement).value)"
        />
        <!-- A credential is typed and shown as dots, and a stored one is never
             sent here at all: the server sends dots in its place, and leaving
             them keeps what it has (F-23). -->
        <input
          class="hdr-value"
          :type="isSecret(row) ? 'password' : 'text'"
          :value="row.value"
          :placeholder="isSecret(row) ? t('events.headers.secretPlaceholder') : t('events.headers.value')"
          :aria-label="t('events.headers.valueFor', { name: row.name || '?' })"
          :disabled="disabled"
          spellcheck="false"
          :autocomplete="isSecret(row) ? 'new-password' : 'off'"
          @focus="onFocusValue($event, row)"
          @change="setValue(i, ($event.target as HTMLInputElement).value)"
        />
        <span v-if="isSecret(row)" class="material-symbols-rounded hdr-lock" role="img"
              :aria-label="t('events.headers.secret')" :title="t('events.headers.secret')">lock</span>
      </template>
      <button type="button" class="hdr-remove" :disabled="disabled"
              :aria-label="t('events.headers.remove')" :title="t('events.headers.remove')" @click="remove(i)">
        <span class="material-symbols-rounded" aria-hidden="true">close</span>
      </button>
    </div>
    <button type="button" class="hdr-add" :disabled="disabled" @click="add">
      <span class="material-symbols-rounded" aria-hidden="true">add</span>
      <span>{{ t('events.headers.add') }}</span>
    </button>
  </div>
</template>

<script setup lang="ts">
// The headers of an HTTP connection, preset or event, as name/value rows. The
// stored form stays the "Name: value" lines it has always been.
import { SECRET_MASK, formatHeaders, isSecretHeader, parseHeaders, type HeaderRow } from '~/utils/secretHeaders';

const props = withDefaults(defineProps<{
  modelValue: string | null | undefined;
  disabled?: boolean;
  id?: string;
}>(), { disabled: false, id: undefined });

const emit = defineEmits<{ 'update:modelValue': [value: string] }>();
const { t } = useLocalization();

// Rows being edited, including a new one with nothing typed yet (which the
// stored text can't hold).
const rows = ref<HeaderRow[]>(parseHeaders(props.modelValue ?? ''));
watch(() => props.modelValue, (v) => {
  if ((v ?? '') !== formatHeaders(rows.value)) rows.value = parseHeaders(v ?? '');
});

const isSecret = (row: HeaderRow) => isSecretHeader(row.name) || row.value === SECRET_MASK;

const commit = () => emit('update:modelValue', formatHeaders(rows.value));
// A stored credential is restored by its header's name (and, for a name used
// twice, in order). So a stored value never moves to another row (R2-18):
//   * renamed, it can't follow the new name: its value is cleared, to be
//     entered again, rather than saved as literal dots;
//   * one of two same-named stored values removed, the other's would be
//     restored from the first stored line, the removed one: it is cleared too.
const setName = (i: number, name: string) => {
  const row = rows.value[i]!;
  if (row.value === SECRET_MASK && name.trim().toLowerCase() !== row.name.trim().toLowerCase()) row.value = '';
  row.name = name;
  commit();
};
const setValue = (i: number, value: string) => { rows.value[i]!.value = value; commit(); };
const remove = (i: number) => {
  const gone = rows.value[i]!;
  rows.value.splice(i, 1);
  if (gone.value === SECRET_MASK && gone.raw === undefined) {
    const name = gone.name.trim().toLowerCase();
    for (const r of rows.value) {
      if (r.raw === undefined && r.value === SECRET_MASK && r.name.trim().toLowerCase() === name) r.value = '';
    }
  }
  commit();
};
const add = () => { rows.value.push({ name: '', value: '' }); };

// Typing into a stored secret replaces it: select the dots so the first key
// does that, rather than appending to them.
const onFocusValue = (e: FocusEvent, row: HeaderRow) => {
  if (row.value === SECRET_MASK) (e.target as HTMLInputElement).select();
};
</script>

<style scoped>
.hdr {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.hdr-row {
  display: flex;
  align-items: center;
  gap: 6px;
}
.hdr input {
  padding: var(--spacing-xs) var(--spacing-sm);
  min-height: 30px;
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  color: var(--color-text);
  font-size: 12px;
  font-family: var(--font-mono, ui-monospace, monospace);
}
.hdr input:focus {
  outline: none;
  border-color: var(--color-accent);
}
.hdr input[readonly] {
  opacity: 0.6;
}
.hdr-name {
  flex: 0 0 38%;
  min-width: 0;
}
.hdr-value,
.hdr-raw {
  flex: 1 1 auto;
  min-width: 0;
}
.hdr-lock {
  font-size: 16px;
  color: var(--color-text-secondary);
}
.hdr-remove,
.hdr-add {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 4px;
  background: transparent;
  border: 0;
  border-radius: 4px;
  color: var(--color-text-secondary);
  cursor: pointer;
}
.hdr-add {
  align-self: flex-start;
  padding: 4px 8px;
  font-size: 13px;
}
.hdr-remove:hover:not(:disabled),
.hdr-add:hover:not(:disabled) {
  background: var(--color-surface-hover);
  color: var(--color-text-primary);
}
.hdr-remove .material-symbols-rounded,
.hdr-add .material-symbols-rounded {
  font-size: 18px;
}
</style>
