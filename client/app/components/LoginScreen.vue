<template>
  <transition name="fade">
    <div v-if="visible" class="lgn-overlay">
      <div class="lgn-dialog">
        <div class="lgn-header">
          <span class="material-symbols-rounded lgn-icon">
            {{ isSetup ? 'person_add' : 'lock' }}
          </span>
          <h2>{{ isSetup ? t('login.setupTitle') : t('login.title') }}</h2>
        </div>

        <p class="lgn-body">
          {{ isSetup ? t('login.setupMessage') : t('login.message', { url: server.serverUrl }) }}
        </p>

        <form class="lgn-form" @submit.prevent="submit">
          <label class="lgn-field">
            <span>{{ t('login.name') }}</span>
            <input
              ref="nameInput"
              v-model="name"
              type="text"
              autocomplete="username"
              :disabled="busy"
              required
            />
          </label>

          <label class="lgn-field">
            <span>{{ t('login.password') }}</span>
            <input
              v-model="password"
              type="password"
              :autocomplete="isSetup ? 'new-password' : 'current-password'"
              :disabled="busy"
              required
            />
          </label>

          <!-- Setup only. A typo in the password that creates the rig's only
               administrator locks the machine, and the recovery is still
               deleting a file on it by hand — the Accounts pane can now turn
               authentication off without losing the accounts, but that itself
               needs the password nobody can type. So it is worth typing twice
               exactly once. -->
          <label v-if="isSetup" class="lgn-field">
            <span>{{ t('login.confirmPassword') }}</span>
            <input
              v-model="confirm"
              type="password"
              autocomplete="new-password"
              :disabled="busy"
              required
            />
          </label>

          <p v-if="isSetup" class="lgn-note">
            {{ t('login.setupNote', { min: MIN_PASSWORD_LENGTH }) }}
          </p>

          <p v-if="error" class="lgn-error">{{ error }}</p>

          <div class="lgn-actions">
            <button class="lgn-btn primary" type="submit" :disabled="busy || !canSubmit">
              <span v-if="busy" class="material-symbols-rounded lgn-spinner">progress_activity</span>
              <span v-else class="material-symbols-rounded">{{ isSetup ? 'check' : 'login' }}</span>
              <span>{{ isSetup ? t('login.createAccount') : t('login.signIn') }}</span>
            </button>
            <!-- The way out that does not need a password: point at a
                 different server. Without it, an operator who has locked
                 themselves out of one rig cannot reach any other. -->
            <button class="lgn-btn" type="button" :disabled="busy" @click="openServerSettings">
              <span class="material-symbols-rounded">dns</span>
              <span>{{ t('login.changeServer') }}</span>
            </button>
          </div>
        </form>

        <p class="lgn-hint">
          {{ isSetup ? t('login.setupHint') : t('login.hint') }}
        </p>
      </div>
    </div>
  </transition>
</template>

<script setup lang="ts">
const { t } = useLocalization();
const server = useLiveplayServer();

// Mirrors the server's kMinPasswordLength. The server is the authority and
// will refuse a short one regardless; this only saves a round trip and lets
// the form say why the button is dead.
const MIN_PASSWORD_LENGTH = 8;

const name     = ref('');
const password = ref('');
const confirm  = ref('');
const busy     = ref(false);
const localError = ref<string | null>(null);
const nameInput  = ref<HTMLInputElement | null>(null);

const visible = computed(() => !!server.needsLogin);
// The same screen does both jobs: a server with no accounts is asking to be
// set up, not asking who you are. Splitting them into two components would
// duplicate the form and the plumbing to say one different sentence.
const isSetup = computed(() => !!server.needsSetup);

const error = computed(() => localError.value || server.authError || null);

const canSubmit = computed(() => {
  if (!name.value.trim() || !password.value) return false;
  if (!isSetup.value) return true;
  return password.value.length >= MIN_PASSWORD_LENGTH && password.value === confirm.value;
});

watch(visible, (shown) => {
  if (!shown) return;
  localError.value = null;
  password.value = '';
  confirm.value = '';
  void nextTick(() => nameInput.value?.focus());
});

async function submit() {
  if (busy.value || !canSubmit.value) return;
  localError.value = null;
  if (isSetup.value && password.value !== confirm.value) {
    localError.value = t('login.passwordsDiffer');
    return;
  }
  busy.value = true;
  try {
    const ok = isSetup.value
      ? await server.setupFirstUser(name.value.trim(), password.value)
      : await server.login(name.value.trim(), password.value);
    // Never keep the password in a reactive ref past the attempt, successful
    // or not — the screen stays mounted and this is the one string on it worth
    // not leaving lying around.
    password.value = '';
    confirm.value = '';
    if (!ok) void nextTick(() => nameInput.value?.focus());
  } finally {
    busy.value = false;
  }
}

function openServerSettings() {
  // Same hash route the Settings page uses elsewhere, so there is exactly one
  // way to reach the server pane rather than a second entry point to keep in
  // step with it.
  if (typeof window !== 'undefined') window.location.hash = '#/settings/server';
}
</script>

<style scoped>
.lgn-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.78);
  display: flex;
  align-items: center;
  justify-content: center;
  /* Above the connection-lost modal (9500). Both can be true at once — a
     server that wants a login is also a server this client is not connected
     to — and the login is the one the operator can actually act on. */
  z-index: 9600;
}
.lgn-dialog {
  background: var(--color-surface);
  color: var(--color-text-primary);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  padding: 24px 28px;
  max-width: 420px;
  width: 90%;
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
}
.lgn-header {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 8px;
}
.lgn-header h2 { margin: 0; font-size: 18px; }
.lgn-icon { color: var(--color-accent, #0f62fe); font-size: 26px; }
.lgn-body {
  color: var(--color-text-secondary);
  font-size: 14px;
  line-height: 1.5;
  margin: 0 0 16px;
}
.lgn-form { display: flex; flex-direction: column; gap: 12px; }
.lgn-field { display: flex; flex-direction: column; gap: 5px; }
.lgn-field span { font-size: 12px; color: var(--color-text-secondary); }
.lgn-field input {
  padding: 9px 10px;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-background);
  color: var(--color-text-primary);
  font-size: 14px;
}
.lgn-field input:focus {
  outline: none;
  border-color: var(--color-accent, #0f62fe);
}
.lgn-field input:disabled { opacity: 0.6; }
.lgn-note {
  font-size: 11px;
  color: var(--color-text-secondary);
  margin: -4px 0 0;
  line-height: 1.4;
}
.lgn-error {
  font-size: 12px;
  color: #da1e28;
  background: rgba(218, 30, 40, 0.1);
  padding: 8px 10px;
  border-radius: 6px;
  border: 1px solid rgba(218, 30, 40, 0.3);
  margin: 0;
}
.lgn-actions { display: flex; gap: 8px; margin-top: 4px; }
.lgn-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  flex: 1;
  justify-content: center;
  padding: 10px 12px;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-background);
  color: var(--color-text-primary);
  font-size: 13px;
  cursor: pointer;
}
.lgn-btn:hover:not(:disabled) {
  background: var(--color-surface-hover);
  border-color: var(--color-accent);
}
.lgn-btn.primary {
  background: var(--color-accent, #0f62fe);
  color: #fff;
  border-color: transparent;
}
.lgn-btn.primary:hover:not(:disabled) { filter: brightness(1.1); }
.lgn-btn:disabled { opacity: 0.5; cursor: default; }
.lgn-btn .material-symbols-rounded { font-size: 16px; }
.lgn-spinner { animation: lgn-spin 1.1s linear infinite; }
@keyframes lgn-spin { to { transform: rotate(360deg); } }
.lgn-hint {
  font-size: 11px;
  color: var(--color-text-secondary);
  margin-top: 14px;
  line-height: 1.4;
}

.fade-enter-active, .fade-leave-active { transition: opacity 0.15s ease; }
.fade-enter-from, .fade-leave-to { opacity: 0; }
</style>
