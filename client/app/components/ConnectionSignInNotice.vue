<template>
  <Teleport to="body">
    <div v-if="needing.length && !dismissed" class="signin-notice" role="alert">
      <span class="material-symbols-rounded" aria-hidden="true">lock</span>
      <p>{{ message }}</p>
      <button type="button" class="signin-notice-go" @click="signIn">{{ t('events.connections.signIn') }}</button>
      <button type="button" class="signin-notice-close" :aria-label="t('actions.close')" @click="dismissed = true">
        <span class="material-symbols-rounded" aria-hidden="true">close</span>
      </button>
    </div>
  </Teleport>
</template>

<script setup lang="ts">
// A connection needs its credentials entered again (F-23): the device refused
// them (HTTP 401/403), or the show came from another machine and this one
// can't open the ones it brought. Like a missing output device, it says so
// and takes the operator to the place to fix it.
const { t } = useLocalization();
const { connections } = useEvents();
const { open: openSettings } = useSettingsPage();

const needing = computed(() => connections.value.filter(c => c.auth));
const signature = computed(() => needing.value.map(c => `${c.id}:${c.auth}`).join(','));

// Dismissed until something else needs a sign-in.
const dismissed = ref(false);
watch(signature, () => { dismissed.value = false; });

const message = computed(() => {
  const first = needing.value[0];
  if (!first) return '';
  if (needing.value.length > 1) return t('events.connections.signInMany', { count: needing.value.length });
  return first.auth === 'rejected'
    ? t('events.connections.signInRejected', { name: first.name })
    : t('events.connections.signInMissing', { name: first.name });
});

const signIn = () => {
  const first = needing.value[0];
  if (!first) return;
  useState<string | null>('connections.signIn', () => null).value = first.id;
  openSettings('connections');
  dismissed.value = true;
};
</script>

<style scoped>
.signin-notice {
  position: fixed;
  inset-block-end: 24px;
  inset-inline: 16px;
  margin-inline: auto;
  z-index: 10001;
  display: flex;
  align-items: center;
  gap: var(--spacing-sm, 8px);
  width: fit-content;
  max-width: 640px;
  padding: 10px 12px 10px 16px;
  background: var(--color-surface);
  border: 1px solid var(--color-warning);
  border-radius: 8px;
  box-shadow: 0 12px 36px rgba(0, 0, 0, 0.5);
  color: var(--color-text-primary);
  font-size: 14px;
  line-height: 1.4;
}
.signin-notice > .material-symbols-rounded {
  flex-shrink: 0;
  color: var(--color-warning);
  font-size: 20px;
}
.signin-notice p {
  margin: 0;
  min-width: 0;
  overflow-wrap: anywhere;
}
.signin-notice-go {
  flex-shrink: 0;
  padding: 4px 10px;
  background: var(--color-accent);
  border: 0;
  border-radius: 4px;
  color: var(--color-on-accent, #fff);
  font: inherit;
  font-size: 13px;
  cursor: pointer;
}
.signin-notice-close {
  display: inline-flex;
  flex-shrink: 0;
  padding: 4px;
  background: transparent;
  border: 0;
  border-radius: 4px;
  color: inherit;
  cursor: pointer;
}
.signin-notice-close:hover {
  background: var(--color-surface-hover);
}
.signin-notice-close .material-symbols-rounded {
  font-size: 18px;
}
</style>
