<template>
  <div class="settings-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionUsers') }}</h3>
    <p class="settings-pane-intro">{{ t('settings.sectionUsersHelp') }}</p>

    <!-- ================================================================
         The default posture: no accounts at all. Not an error state and
         not an empty list — it is what every release before 2.5 did, and
         the pane says so rather than looking broken or, worse, looking
         like authentication is on and simply has nobody in it.
         ================================================================ -->
    <section v-if="!server.authRequired" class="settings-field">
      <p class="settings-notice settings-notice--info">
        <span class="material-symbols-rounded">lock_open</span>
        {{ t('users.authOff') }}
      </p>

      <!-- Accounts exist and the login is switched off. A DIFFERENT state from
           having no accounts, and it must not read the same: somebody chose
           this, the team is still here, and an operator looking at an empty
           pane would reasonably assume a password was being asked for. This is
           the branch that tells them it is not. -->
      <template v-if="server.authUserCount > 0">
        <p class="settings-help settings-help--warn">{{ t('users.authOffExplicit') }}</p>
        <button class="settings-btn settings-btn--primary" @click="startAuthChange(true)">
          {{ t('users.authTurnOn') }}
        </button>
      </template>

      <!-- The ordinary state of a fresh installation. -->
      <template v-else>
        <p class="settings-help">{{ t('users.authOffHelp') }}</p>
        <button class="settings-btn settings-btn--primary" @click="showAdd = true">
          {{ t('users.createFirst') }}
        </button>
      </template>
    </section>

    <p v-else-if="loadError" class="settings-help settings-help--warn">{{ loadError }}</p>

    <template v-else>
      <!-- Who this client is signed in as, and the two things a person can do
           about their own account without being an administrator. -->
      <section class="settings-field">
        <label class="settings-label">
          <span class="material-symbols-rounded">badge</span>
          {{ t('users.signedInAs') }}
        </label>
        <p class="settings-help">
          <strong>{{ server.authUser?.name }}</strong>
          — {{ roleLabel(server.authUser?.role) }}
        </p>
        <div class="settings-actions">
          <button class="settings-btn" @click="startPasswordChange(server.authUser?.id)">
            {{ t('users.changePassword') }}
          </button>
          <button class="settings-btn" @click="server.logout()">{{ t('users.signOut') }}</button>
          <button class="settings-btn settings-btn--danger" @click="confirmSignOutEverywhere = true">
            {{ t('users.signOutEverywhere') }}
          </button>
        </div>
        <p class="settings-help settings-help--muted">{{ t('users.signOutEverywhereHelp') }}</p>
      </section>

      <!-- The account list. Admins only — an operator gets the 403 branch
           above, because who else has an account on this rig is the machine's
           business, not the show's. -->
      <template v-if="isAdmin">
        <h4 class="settings-group-title">{{ t('users.accounts') }}</h4>

        <section v-for="u in users" :key="u.id" class="settings-field settings-row">
          <div class="settings-row-main">
            <span class="settings-row-name">
              {{ u.name }}
              <span v-if="u.id === server.authUser?.id" class="settings-badge">
                {{ t('users.you') }}
              </span>
            </span>
            <span class="settings-row-sub">{{ roleLabel(u.role) }}</span>
          </div>
          <div class="settings-actions">
            <select class="settings-select settings-select--inline"
                    :value="u.role" @change="onRoleChange(u, $event)">
              <option value="operator">{{ t('users.roleOperator') }}</option>
              <option value="admin">{{ t('users.roleAdmin') }}</option>
            </select>
            <button class="settings-btn" @click="startPasswordChange(u.id)">
              {{ t('users.changePassword') }}
            </button>
            <button class="settings-btn settings-btn--danger" @click="pendingRemove = u">
              {{ t('users.remove') }}
            </button>
          </div>
        </section>

        <div class="settings-actions">
          <button class="settings-btn settings-btn--primary" @click="showAdd = true">
            {{ t('users.add') }}
          </button>
        </div>

        <!-- Turning the login off. Last on the pane and its own group, because
             it is a posture for the whole server rather than a change to an
             account — and because until now there was NO way back: the store
             refuses to delete the last administrator, so authentication became
             permanent the moment the first account existed, and the recovery on
             the login screen's own note was deleting a file by hand. The
             accounts survive this. -->
        <h4 class="settings-group-title">{{ t('users.authPosture') }}</h4>
        <section class="settings-field">
          <p class="settings-help">{{ t('users.authOnNow') }}</p>
          <p class="settings-help settings-help--muted">{{ t('users.authTurnOffHelp') }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--danger" @click="startAuthChange(false)">
              {{ t('users.authTurnOff') }}
            </button>
          </div>
        </section>
      </template>
      <p v-else class="settings-help">{{ t('users.adminOnly') }}</p>
    </template>

    <p v-if="notice" class="settings-help" :class="noticeIsError ? 'settings-help--warn' : ''">
      {{ notice }}
    </p>

    <!-- ---- Add an account ------------------------------------------- -->
    <div v-if="showAdd" class="settings-inline-form">
      <h4>{{ t('users.addTitle') }}</h4>
      <label class="settings-label">{{ t('users.name') }}</label>
      <input class="settings-input" type="text" v-model="draftName" autocomplete="off" />
      <label class="settings-label">{{ t('users.password') }}</label>
      <input class="settings-input" type="password" v-model="draftPassword"
             autocomplete="new-password" />
      <label class="settings-label">{{ t('users.role') }}</label>
      <!-- Forced to admin, and not offered as a choice, while the store is
           empty: the server overrides the request anyway (a store whose only
           account cannot manage accounts is a locked room with the key inside),
           so offering "operator" here would be a control that lies. -->
      <select v-if="server.authRequired" class="settings-select" v-model="draftRole">
        <option value="operator">{{ t('users.roleOperator') }}</option>
        <option value="admin">{{ t('users.roleAdmin') }}</option>
      </select>
      <p v-else class="settings-help">{{ t('users.firstIsAdmin') }}</p>
      <p class="settings-help">{{ t('users.passwordRule', { min: MIN_PASSWORD_LENGTH }) }}</p>
      <div class="settings-actions">
        <button class="settings-btn settings-btn--primary" :disabled="!canAdd || busy"
                @click="addUser">{{ t('users.add') }}</button>
        <button class="settings-btn" @click="cancelAdd">{{ t('users.cancel') }}</button>
      </div>
    </div>

    <!-- ---- Change a password ---------------------------------------- -->
    <div v-if="passwordFor" class="settings-inline-form">
      <h4>{{ t('users.changePasswordTitle') }}</h4>
      <input class="settings-input" type="password" v-model="draftPassword"
             autocomplete="new-password" />
      <p class="settings-help">{{ t('users.passwordRule', { min: MIN_PASSWORD_LENGTH }) }}</p>
      <p class="settings-help settings-help--muted">{{ t('users.passwordRevokes') }}</p>
      <div class="settings-actions">
        <button class="settings-btn settings-btn--primary"
                :disabled="draftPassword.length < MIN_PASSWORD_LENGTH || busy"
                @click="savePassword">{{ t('users.save') }}</button>
        <button class="settings-btn" @click="cancelPassword">{{ t('users.cancel') }}</button>
      </div>
    </div>

    <!-- ---- Confirmations -------------------------------------------- -->
    <div v-if="pendingRemove" class="settings-inline-form">
      <p>{{ t('users.removeConfirm', { name: pendingRemove.name }) }}</p>
      <p class="settings-help">{{ t('users.removeHelp') }}</p>
      <div class="settings-actions">
        <button class="settings-btn settings-btn--danger" :disabled="busy"
                @click="removeUser">{{ t('users.remove') }}</button>
        <button class="settings-btn" @click="pendingRemove = null">{{ t('users.cancel') }}</button>
      </div>
    </div>

    <!-- ---- Turning the login off, or back on ------------------------- -->
    <!-- The password is asked for in BOTH directions, and that is the server's
         rule rather than this form's caution: while authentication is off there
         is no session to gate the change with, so the password IS the gate.
         Re-entry rather than the token because a token here is long-lived,
         signed, and crosses the LAN with no TLS anywhere in this server.

         The NAME is only asked for when there is no session to take it from —
         which is exactly the case where this check is doing the protecting. -->
    <div v-if="authChange !== null" class="settings-inline-form">
      <h4>{{ authChange ? t('users.authTurnOn') : t('users.authTurnOff') }}</h4>
      <p>{{ authChange ? t('users.authTurnOnConfirm') : t('users.authTurnOffConfirm') }}</p>
      <template v-if="!server.authUser">
        <label class="settings-label">{{ t('users.adminName') }}</label>
        <input class="settings-input" type="text" v-model="authName" autocomplete="username" />
      </template>
      <label class="settings-label">{{ t('users.confirmPassword') }}</label>
      <input class="settings-input" type="password" v-model="authPassword"
             autocomplete="current-password" @keydown.enter="applyAuthChange" />
      <p class="settings-help settings-help--muted">{{ t('users.authKeepsAccounts') }}</p>
      <div class="settings-actions">
        <button class="settings-btn"
                :class="authChange ? 'settings-btn--primary' : 'settings-btn--danger'"
                :disabled="!canApplyAuthChange || busy"
                @click="applyAuthChange">
          {{ authChange ? t('users.authTurnOn') : t('users.authTurnOff') }}
        </button>
        <button class="settings-btn" @click="cancelAuthChange">{{ t('users.cancel') }}</button>
      </div>
    </div>

    <div v-if="confirmSignOutEverywhere" class="settings-inline-form">
      <p>{{ t('users.signOutEverywhereConfirm') }}</p>
      <div class="settings-actions">
        <button class="settings-btn settings-btn--danger" :disabled="busy"
                @click="signOutEverywhere">{{ t('users.signOutEverywhere') }}</button>
        <button class="settings-btn" @click="confirmSignOutEverywhere = false">
          {{ t('users.cancel') }}
        </button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
// The Users pane (P3c).
//
// The /api/users routes have been tested and unreachable since U3 shipped: the
// login screen creates the FIRST account and nothing has been able to create a
// second, change a role, or remove anybody. This is that surface.
//
// Two things shape it more than the CRUD does.
//
// First, the default posture. An installation with no accounts is not an empty
// list — it is the pre-2.5 behaviour, deliberately kept, and the pane says so
// in words rather than rendering a table with nothing in it. Creating the first
// account is a real decision (it turns authentication ON for every client of
// this server), so it reads as one.
//
// Second, every refusal here is the server's. "That name is taken", "you cannot
// remove the last administrator", "that password is too short" — the store has
// better wording for all of them than this pane could invent, and inventing one
// would eventually contradict it. The pane shows what came back.
const { t } = useLocalization();
const server = useLiveplayServer();

// Mirrors kMinPasswordLength in user_store.hpp. Duplicated on purpose rather
// than fetched: it is a floor, the server enforces it regardless, and a form
// that cannot say "too short" until it has round-tripped is a worse form. If
// the two ever disagree the server wins and its message is what is shown.
const MIN_PASSWORD_LENGTH = 8;

const users      = ref<any[]>([]);
const loadError  = ref<string | null>(null);
const notice     = ref('');
const noticeIsError = ref(false);
const busy       = ref(false);

const showAdd       = ref(false);
const draftName     = ref('');
const draftPassword = ref('');
const draftRole     = ref('operator');
const passwordFor   = ref<string | null>(null);
const pendingRemove = ref<any | null>(null);
const confirmSignOutEverywhere = ref(false);

// null = not asking. true/false = the posture being confirmed. Kept separate
// from the add/password drafts so an abandoned half-typed password from one form
// can never be submitted by another.
const authChange   = ref<boolean | null>(null);
const authName     = ref('');
const authPassword = ref('');

const isAdmin = computed(() => server.authUser?.role === 'admin');
const canAdd  = computed(() =>
  draftName.value.trim().length > 0 && draftPassword.value.length >= MIN_PASSWORD_LENGTH);
// No MIN_PASSWORD_LENGTH floor here, deliberately: this is an EXISTING password
// being confirmed, not a new one being chosen, and refusing to submit a short
// one would lock out an account created before the floor existed.
const canApplyAuthChange = computed(() =>
  authPassword.value.length > 0 && (!!server.authUser || authName.value.trim().length > 0));

const roleLabel = (role?: string) =>
  role === 'admin' ? t('users.roleAdmin') : t('users.roleOperator');

// The server's message where there is one, ours only when there is not.
function report(e: any, fallbackKey: string) {
  const msg = String(e?.message ?? e);
  const m = msg.match(/\{"error":"(.*?)"\}/);
  notice.value = m ? m[1] : (msg && msg !== 'undefined' ? msg : t(fallbackKey));
  noticeIsError.value = true;
}

async function load() {
  loadError.value = null;
  if (!server.authRequired) { users.value = []; return; }
  // An operator is refused this list, which is correct and not a fault: who
  // else has an account on this rig belongs to the machine, not to the show.
  if (!isAdmin.value) { users.value = []; return; }
  try {
    users.value = await server.fetchUsers();
  } catch (e: any) {
    loadError.value = /40[13]/.test(String(e?.message ?? e))
      ? t('users.adminOnly') : t('users.loadFailed');
  }
}

function startAuthChange(required: boolean) {
  authChange.value = required;
  authName.value = '';
  authPassword.value = '';
  notice.value = '';
}

function cancelAuthChange() {
  authChange.value = null;
  // Cleared on the way out as well as the way in: a password left in a ref is a
  // password sitting in memory for no reason.
  authName.value = '';
  authPassword.value = '';
}

async function applyAuthChange() {
  if (authChange.value === null || !canApplyAuthChange.value) return;
  const required = authChange.value;
  busy.value = true;
  try {
    await server.setAuthRequired(required, authPassword.value,
                                 server.authUser ? undefined : authName.value.trim());
    cancelAuthChange();
    notice.value = required ? t('users.authTurnedOn') : t('users.authTurnedOff');
    noticeIsError.value = false;
    // The list is admin-only and only exists while authentication is on, so it
    // has to be re-read either way: turning off empties it, turning on fills it.
    await load();
  } catch (e: any) {
    // The password field is cleared but the form stays open — a wrong password
    // should cost a retype, not the whole gesture. The server's own wording is
    // what gets shown, including its 429 when the shared login throttle bites.
    authPassword.value = '';
    report(e, 'users.authChangeFailed');
  } finally {
    busy.value = false;
  }
}

function cancelAdd() {
  showAdd.value = false;
  draftName.value = '';
  draftPassword.value = '';
  draftRole.value = 'operator';
}

async function addUser() {
  busy.value = true;
  notice.value = '';
  try {
    // While the store is empty the server forces admin whatever is asked for,
    // so this asks for it too rather than sending a value that will be ignored.
    const role = server.authRequired ? draftRole.value : 'admin';
    const created = await server.createUser(draftName.value.trim(), draftPassword.value, role);
    const wasBootstrap = !server.authRequired;
    cancelAdd();
    notice.value = wasBootstrap
      ? t('users.authNowOn', { name: created?.name ?? '' })
      : t('users.added', { name: created?.name ?? '' });
    noticeIsError.value = false;
    // Creating the first account turns authentication on for this server, so
    // this client has to re-ask what its posture is — it is currently
    // anonymous and about to need a token.
    if (wasBootstrap) await server.checkAuth();
    await load();
  } catch (e) {
    report(e, 'users.addFailed');
  } finally {
    busy.value = false;
  }
}

function startPasswordChange(id?: string) {
  if (!id) return;
  passwordFor.value = id;
  draftPassword.value = '';
  notice.value = '';
}
function cancelPassword() { passwordFor.value = null; draftPassword.value = ''; }

async function savePassword() {
  busy.value = true;
  try {
    const id = passwordFor.value!;
    const isSelf = id === server.authUser?.id;
    await server.updateUser(id, { password: draftPassword.value });
    cancelPassword();
    notice.value = t('users.passwordChanged');
    noticeIsError.value = false;
    // A password change bumps that user's token epoch, which invalidates every
    // token issued to them — including this client's, if it was their own
    // password. Saying so is better than the session dying on the next request
    // with no explanation.
    if (isSelf) notice.value = t('users.passwordChangedSelf');
    await load();
  } catch (e) {
    report(e, 'users.saveFailed');
  } finally {
    busy.value = false;
  }
}

async function onRoleChange(u: any, e: Event) {
  const role = (e.target as HTMLSelectElement).value;
  if (role === u.role) return;
  busy.value = true;
  notice.value = '';
  try {
    await server.updateUser(u.id, { role });
    notice.value = t('users.roleChanged', { name: u.name, role: roleLabel(role) });
    noticeIsError.value = false;
    // A demotion takes effect on the caller's very next request, because a
    // token carries only an id and an epoch and the role is looked up fresh.
    // Demoting yourself therefore costs you this pane immediately.
    if (u.id === server.authUser?.id) await server.checkAuth();
    await load();
  } catch (err) {
    report(err, 'users.saveFailed');
    await load();   // put the select back to what the server still says
  } finally {
    busy.value = false;
  }
}

async function removeUser() {
  busy.value = true;
  try {
    const u = pendingRemove.value;
    await server.deleteUser(u.id);
    pendingRemove.value = null;
    notice.value = t('users.removed', { name: u.name });
    noticeIsError.value = false;
    await load();
  } catch (e) {
    report(e, 'users.removeFailed');
    pendingRemove.value = null;
  } finally {
    busy.value = false;
  }
}

async function signOutEverywhere() {
  busy.value = true;
  try {
    // Necessarily signs this client out too — that is what "everywhere" means,
    // and the confirmation says so before it runs.
    await server.logoutAll();
    confirmSignOutEverywhere.value = false;
  } catch (e) {
    report(e, 'users.saveFailed');
  } finally {
    busy.value = false;
  }
}

onMounted(load);
// A reconnect can be a different server with a different account list, and a
// sign-in changes what this client is allowed to see.
watch(() => server.connected, (up) => { if (up) void load(); });
watch(() => server.authUser?.id, () => { void load(); });
</script>

<style lang="scss" scoped>
.settings-group-title {
  margin: 32px 0 4px;
  font-size: 15px;
  font-weight: 600;
  padding-top: 20px;
  border-top: 1px solid var(--color-border, #333);
}
.settings-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 10px 0;
  border-bottom: 1px solid var(--color-border, #2a2a2a);
}
.settings-row-main { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.settings-row-name { font-weight: 600; font-size: 14px; }
.settings-row-sub  { font-size: 12px; opacity: 0.6; }
.settings-select--inline { width: auto; min-width: 120px; }
.settings-actions { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.settings-btn {
  padding: 6px 14px;
  border-radius: 4px;
  border: 1px solid var(--color-border, #444);
  background: transparent;
  color: inherit;
  cursor: pointer;
  font-size: 13px;
  &:disabled { opacity: 0.45; cursor: default; }
  &--primary { background: var(--color-accent-custom, #DA1E28); border-color: transparent; color: #fff; }
  &--danger  { border-color: rgba(255, 23, 68, 0.5); color: #ff5a72; }
}
.settings-badge {
  margin-left: 8px;
  padding: 1px 6px;
  border-radius: 3px;
  font-size: 10px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  background: rgba(255, 255, 255, 0.1);
}
.settings-notice {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 12px;
  border-radius: 4px;
  font-size: 13px;
  margin: 8px 0;
  &--info { background: rgba(0, 184, 212, 0.1); color: #00b8d4; }
}
.settings-inline-form {
  margin: 16px 0;
  padding: 16px;
  border: 1px solid var(--color-border, #333);
  border-radius: 6px;
  background: rgba(255, 255, 255, 0.02);
  h4 { margin: 0 0 12px; font-size: 14px; }
}
.settings-help--muted { opacity: 0.6; font-size: 12px; }
.settings-help--warn  { color: #ffc400; }
</style>
