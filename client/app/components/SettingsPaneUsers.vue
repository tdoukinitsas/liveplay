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

        <!-- Turning the login back ON.
             The password is asked for in BOTH directions, and that is the
             server's rule rather than this form's caution: while authentication
             is off there is no session to gate the change with, so the password
             IS the gate. Re-entry rather than the token because a token here is
             long-lived, signed, and crosses the LAN with no TLS anywhere in this
             server. The NAME is asked for the same way and for the same reason —
             there is no session to take it from. -->
        <div v-if="authChange === true" v-reveal class="settings-inline-form">
          <h4>{{ t('users.authTurnOn') }}</h4>
          <p>{{ t('users.authTurnOnConfirm') }}</p>
          <template v-if="!server.authUser">
            <label class="settings-label">{{ t('users.adminName') }}</label>
            <input class="settings-input" type="text" v-model="authName" autocomplete="username" />
          </template>
          <label class="settings-label">{{ t('users.confirmPassword') }}</label>
          <input class="settings-input" type="password" v-model="authPassword"
                 autocomplete="current-password" @keydown.enter="applyAuthChange" />
          <p class="settings-help settings-help--muted">{{ t('users.authKeepsAccounts') }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--primary"
                    :disabled="!canApplyAuthChange || busy"
                    @click="applyAuthChange">{{ t('users.authTurnOn') }}</button>
            <button class="settings-btn" @click="cancelAuthChange">{{ t('users.cancel') }}</button>
          </div>
        </div>

        <!-- Tokens outlive the posture: turning the login off does not revoke
             them, and turning it back on makes every one of them live again.
             An administrator standing in this state needs to know they are
             there. No controls here — issuing is refused while the door is
             open (a credential minted through it would survive it closing),
             and the rest belongs on the pane this state does not show. -->
        <p v-if="apiTokens.length" class="settings-help settings-help--muted">
          {{ t('apiTokens.dormant', { count: apiTokens.length }) }}
        </p>
      </template>

      <!-- The ordinary state of a fresh installation. -->
      <template v-else>
        <p class="settings-help">{{ t('users.authOffHelp') }}</p>
        <div class="settings-actions">
          <button class="settings-btn settings-btn--primary" @click="showAdd = true">
            {{ t('users.createFirst') }}
          </button>
          <!-- The other way to set up a fresh machine: take the team from the
               old one. Offered here, with nobody signed in, for the same reason
               "Create First Account" is — there is no administrator yet, and the
               server opens exactly this window until the first account lands. -->
          <button class="settings-btn" @click="pickImportFile">{{ t('accountFile.import') }}</button>
        </div>
        <p class="settings-help settings-help--muted">{{ t('accountFile.firstImport') }}</p>

        <!-- The FIRST account. A different form from "add a colleague" below,
             and not only in where it sits: creating this one turns
             authentication ON for every client of this server, and no role is
             offered because the server overrides the request anyway — a store
             whose only account cannot manage accounts is a locked room with the
             key inside. -->
        <div v-if="showAdd" v-reveal class="settings-inline-form">
          <h4>{{ t('users.addTitle') }}</h4>
          <label class="settings-label">{{ t('users.name') }}</label>
          <input class="settings-input" type="text" v-model="draftName" autocomplete="off" />
          <label class="settings-label">{{ t('users.password') }}</label>
          <input class="settings-input" type="password" v-model="draftPassword"
                 autocomplete="new-password" />
          <p class="settings-help">{{ t('users.firstIsAdmin') }}</p>
          <p class="settings-help">{{ t('users.passwordRule', { min: MIN_PASSWORD_LENGTH }) }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--primary" :disabled="!canAdd || busy"
                    @click="addUser">{{ t('users.add') }}</button>
            <button class="settings-btn" @click="cancelAdd">{{ t('users.cancel') }}</button>
          </div>
        </div>
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
        <div class="settings-me">
          <UserAvatar :name="server.authUser?.name" :src="server.authUser?.avatar" :size="48"
                      :label="t('avatar.alt', { name: server.authUser?.name ?? '' })" />
          <p class="settings-help">
            <strong>{{ server.authUser?.name }}</strong>
            — {{ roleLabel(server.authUser?.role) }}
          </p>
        </div>
        <div class="settings-actions">
          <!-- Your own picture is yours to change whatever your role, like your
               own password. It goes through /api/auth/me/avatar, which acts
               only on the caller, so an operator gets this and nothing more. -->
          <button class="settings-btn" @click="pickAvatar(server.authUser?.id, true)">
            {{ server.authUser?.avatar ? t('avatar.change') : t('avatar.add') }}
          </button>
          <button v-if="server.authUser?.avatar" class="settings-btn"
                  :disabled="busy" @click="clearAvatar(server.authUser?.id, true)">
            {{ t('avatar.remove') }}
          </button>
          <button class="settings-btn" @click="startPasswordChange(server.authUser?.id)">
            {{ t('users.changePassword') }}
          </button>
          <button class="settings-btn" @click="server.logout()">{{ t('users.signOut') }}</button>
          <button class="settings-btn settings-btn--danger" @click="confirmSignOutEverywhere = true">
            {{ t('users.signOutEverywhere') }}
          </button>
        </div>
        <p class="settings-help settings-help--muted">{{ t('users.signOutEverywhereHelp') }}</p>

        <div v-if="confirmSignOutEverywhere" v-reveal class="settings-inline-form">
          <p>{{ t('users.signOutEverywhereConfirm') }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--danger" :disabled="busy"
                    @click="signOutEverywhere">{{ t('users.signOutEverywhere') }}</button>
            <button class="settings-btn" @click="confirmSignOutEverywhere = false">
              {{ t('users.cancel') }}
            </button>
          </div>
        </div>

        <!-- Changing a password, for this account and for anybody else's.
             It lives HERE rather than beside the account list because an
             operator has no list — they reach this form from the button above
             and nowhere else — so the one copy has to sit outside the admin
             branch. That makes it the one form on the pane that can be opened
             from somewhere other than where it appears, which is why it names
             the account it is about. -->
        <div v-if="passwordFor" v-reveal class="settings-inline-form">
          <h4>{{ t('users.changePasswordTitle') }}</h4>
          <p class="settings-help"><strong>{{ passwordForName }}</strong></p>
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
      </section>

      <!-- The account list. Admins only — an operator gets the 403 branch
           above, because who else has an account on this rig is the machine's
           business, not the show's. -->
      <template v-if="isAdmin">
        <h4 class="settings-group-title">{{ t('users.accounts') }}</h4>

        <section v-for="u in users" :key="u.id" class="settings-field settings-row">
          <div class="settings-row-lead">
            <UserAvatar :name="u.name" :src="u.avatar" :size="36"
                        :label="t('avatar.alt', { name: u.name })" />
            <div class="settings-row-main">
              <span class="settings-row-name">
                {{ u.name }}
                <span v-if="u.id === server.authUser?.id" class="settings-badge">
                  {{ t('users.you') }}
                </span>
              </span>
              <span class="settings-row-sub">{{ roleLabel(u.role) }}</span>
            </div>
          </div>
          <div class="settings-actions">
            <select class="settings-select settings-select--inline"
                    :value="u.role" @change="onRoleChange(u, $event)">
              <option value="operator">{{ t('users.roleOperator') }}</option>
              <option value="admin">{{ t('users.roleAdmin') }}</option>
            </select>
            <button class="settings-btn" @click="pickAvatar(u.id, false)">
              {{ u.avatar ? t('avatar.change') : t('avatar.add') }}
            </button>
            <button v-if="u.avatar" class="settings-btn" :disabled="busy"
                    @click="clearAvatar(u.id, false)">
              {{ t('avatar.remove') }}
            </button>
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

        <!-- Adding a colleague. The role IS offered here, unlike the first
             account above: authentication is already on, so the server has no
             reason to override the request. -->
        <div v-if="showAdd" v-reveal class="settings-inline-form">
          <h4>{{ t('users.addTitle') }}</h4>
          <label class="settings-label">{{ t('users.name') }}</label>
          <input class="settings-input" type="text" v-model="draftName" autocomplete="off" />
          <label class="settings-label">{{ t('users.password') }}</label>
          <input class="settings-input" type="password" v-model="draftPassword"
                 autocomplete="new-password" />
          <label class="settings-label">{{ t('users.role') }}</label>
          <select class="settings-select" v-model="draftRole">
            <option value="operator">{{ t('users.roleOperator') }}</option>
            <option value="admin">{{ t('users.roleAdmin') }}</option>
          </select>
          <p class="settings-help">{{ t('users.passwordRule', { min: MIN_PASSWORD_LENGTH }) }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--primary" :disabled="!canAdd || busy"
                    @click="addUser">{{ t('users.add') }}</button>
            <button class="settings-btn" @click="cancelAdd">{{ t('users.cancel') }}</button>
          </div>
        </div>

        <div v-if="pendingRemove" v-reveal class="settings-inline-form">
          <p>{{ t('users.removeConfirm', { name: pendingRemove.name }) }}</p>
          <p class="settings-help">{{ t('users.removeHelp') }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--danger" :disabled="busy"
                    @click="removeUser">{{ t('users.remove') }}</button>
            <button class="settings-btn" @click="pendingRemove = null">{{ t('users.cancel') }}</button>
          </div>
        </div>

        <!-- API tokens. Below the accounts because they answer the same
             question — who may talk to this server — for the half of the
             answer that is not a person: a Companion instance, a cue in
             somebody else's show-control system, a script. They are NOT
             accounts, and the help text says what they may do rather than
             leaving somebody to assume a token is an operator with no face. -->
        <h4 class="settings-group-title">{{ t('apiTokens.title') }}</h4>
        <p class="settings-help">{{ t('apiTokens.help') }}</p>
        <p class="settings-help settings-help--muted">{{ t('apiTokens.scope') }}</p>

        <section v-for="k in apiTokens" :key="k.id" class="settings-field settings-row">
          <div class="settings-row-main">
            <span class="settings-row-name">{{ k.name }}</span>
            <span class="settings-row-sub">
              {{ t('apiTokens.created', { date: shortDate(k.createdAt) }) }}
              ·
              {{ k.lastUsedAt
                   ? t('apiTokens.lastUsed', { date: shortDate(k.lastUsedAt) })
                   : t('apiTokens.neverUsed') }}
            </span>
          </div>
          <div class="settings-actions">
            <button class="settings-btn" @click="startTokenRename(k)">
              {{ t('apiTokens.rename') }}
            </button>
            <button class="settings-btn settings-btn--danger" @click="pendingRevoke = k">
              {{ t('apiTokens.revoke') }}
            </button>
          </div>
        </section>
        <p v-if="!apiTokens.length" class="settings-help settings-help--muted">
          {{ t('apiTokens.none') }}
        </p>

        <div class="settings-actions">
          <button class="settings-btn settings-btn--primary" @click="showAddToken = true">
            {{ t('apiTokens.add') }}
          </button>
        </div>

        <!-- ---- Issue an API token ---------------------------------------- -->
        <div v-if="showAddToken" v-reveal class="settings-inline-form">
          <h4>{{ t('apiTokens.addTitle') }}</h4>
          <!-- users.name, not a key of its own: the same word on the same pane,
               already settled in 21 languages. -->
          <label class="settings-label">{{ t('users.name') }}</label>
          <input class="settings-input" type="text" v-model="tokenName" autocomplete="off" />
          <p class="settings-help">{{ t('apiTokens.nameHelp') }}</p>
          <label class="settings-label">{{ t('users.confirmPassword') }}</label>
          <input class="settings-input" type="password" v-model="tokenPassword"
                 autocomplete="current-password" @keydown.enter="issueToken" />
          <p class="settings-help settings-help--muted">{{ t('apiTokens.passwordWhy') }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--primary" :disabled="!canIssueToken || busy"
                    @click="issueToken">{{ t('apiTokens.issue') }}</button>
            <button class="settings-btn" @click="cancelAddToken">{{ t('users.cancel') }}</button>
          </div>
        </div>

        <!-- ---- The one and only sight of the secret -------------------- -->
        <!-- Deliberately not a toast and not dismissed by clicking away: the
             string exists in exactly one response and the server kept only a
             hash of it, so losing this dialogue loses the token. It stays until
             the person says they have it. -->
        <div v-if="issuedToken" v-reveal class="settings-inline-form settings-inline-form--secret">
          <h4>{{ t('apiTokens.issuedTitle', { name: issuedName }) }}</h4>
          <p class="settings-help settings-help--warn">{{ t('apiTokens.shownOnce') }}</p>
          <input ref="secretInput" class="settings-input settings-input--secret" type="text"
                 readonly :value="issuedToken" @focus="selectSecret" />
          <div class="settings-actions">
            <button class="settings-btn settings-btn--primary" @click="copyToken">
              {{ copied ? t('apiTokens.copied') : t('apiTokens.copy') }}
            </button>
            <button class="settings-btn" @click="dismissIssued">{{ t('apiTokens.done') }}</button>
          </div>
        </div>

        <!-- ---- Rename a token ------------------------------------------ -->
        <div v-if="renameFor" v-reveal class="settings-inline-form">
          <h4>{{ t('apiTokens.renameTitle') }}</h4>
          <input class="settings-input" type="text" v-model="renameDraft" autocomplete="off"
                 @keydown.enter="saveTokenRename" />
          <p class="settings-help settings-help--muted">{{ t('apiTokens.renameHelp') }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--primary"
                    :disabled="!renameDraft.trim() || busy"
                    @click="saveTokenRename">{{ t('users.save') }}</button>
            <button class="settings-btn" @click="renameFor = null">{{ t('users.cancel') }}</button>
          </div>
        </div>

        <div v-if="pendingRevoke" v-reveal class="settings-inline-form">
          <p>{{ t('apiTokens.revokeConfirm', { name: pendingRevoke.name }) }}</p>
          <p class="settings-help">{{ t('apiTokens.revokeHelp') }}</p>
          <div class="settings-actions">
            <button class="settings-btn settings-btn--danger" :disabled="busy"
                    @click="revokeToken">{{ t('apiTokens.revoke') }}</button>
            <button class="settings-btn" @click="pendingRevoke = null">{{ t('users.cancel') }}</button>
          </div>
        </div>

        <!-- Turning the login off. Last of the account groups, and its own, because
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

          <!-- Turning the login OFF. The other half of the form at the top of
               this pane, and a separate one rather than a ternary: the password
               is asked for in both directions, but only that much is shared —
               the wording, the danger the button carries, and whether there is
               a session to take a name from all differ. There always is one
               here, since this group only exists for a signed-in administrator. -->
          <div v-if="authChange === false" v-reveal class="settings-inline-form">
            <h4>{{ t('users.authTurnOff') }}</h4>
            <p>{{ t('users.authTurnOffConfirm') }}</p>
            <label class="settings-label">{{ t('users.confirmPassword') }}</label>
            <input class="settings-input" type="password" v-model="authPassword"
                   autocomplete="current-password" @keydown.enter="applyAuthChange" />
            <p class="settings-help settings-help--muted">{{ t('users.authKeepsAccounts') }}</p>
            <div class="settings-actions">
              <button class="settings-btn settings-btn--danger"
                      :disabled="!canApplyAuthChange || busy"
                      @click="applyAuthChange">{{ t('users.authTurnOff') }}</button>
              <button class="settings-btn" @click="cancelAuthChange">{{ t('users.cancel') }}</button>
            </div>
          </div>
        </section>

        <!-- Moving the team to another server. Last, because it is the one
             group here that is about a different machine, and because its
             import form renders just below this pane's branches (the fresh-
             install state opens the same form from its own button). The
             warning is not small print: the file holds every password hash on
             this server, and the server gives it only to a signed-in
             administrator for that reason. -->
        <h4 class="settings-group-title">{{ t('accountFile.title') }}</h4>
        <section class="settings-field">
          <p class="settings-help">{{ t('accountFile.help') }}</p>
          <p class="settings-help settings-help--warn">{{ t('accountFile.warning') }}</p>
          <div class="settings-actions">
            <button class="settings-btn" :disabled="busy" @click="exportAccounts">
              {{ t('accountFile.export') }}
            </button>
            <button class="settings-btn" :disabled="busy" @click="pickImportFile">
              {{ t('accountFile.import') }}
            </button>
          </div>
        </section>
      </template>
      <p v-else class="settings-help">{{ t('users.adminOnly') }}</p>
    </template>

    <!-- ---- Import: what is in the file, and what to do with it ---------- -->
    <!-- Nothing is sent until a mode is chosen, and Replace asks twice: it is
         the one action on this pane that removes every account at once,
         including, possibly, the one doing it. -->
    <div v-if="importData" v-reveal class="settings-inline-form">
      <h4>{{ t('accountFile.previewTitle') }}</h4>
      <p>{{ t('accountFile.previewCounts', { users: importCounts.users, tokens: importCounts.tokens }) }}</p>
      <p v-if="importData.exportedAt" class="settings-help settings-help--muted">
        {{ t('accountFile.previewDate', { date: shortDate(importData.exportedAt) }) }}
      </p>
      <template v-if="!importConfirmReplace">
        <p class="settings-help">{{ t('accountFile.mergeHelp') }}</p>
        <p v-if="server.authUserCount > 0" class="settings-help">{{ t('accountFile.replaceHelp') }}</p>
        <div class="settings-actions">
          <button class="settings-btn settings-btn--primary" :disabled="busy"
                  @click="runImport('merge')">{{ t('accountFile.merge') }}</button>
          <!-- Replacing an empty server is the same as adding to it, so the
               dangerous button is only offered when there is something to lose. -->
          <button v-if="server.authUserCount > 0" class="settings-btn settings-btn--danger"
                  :disabled="busy" @click="importConfirmReplace = true">
            {{ t('accountFile.replace') }}
          </button>
          <button class="settings-btn" @click="cancelImport">{{ t('users.cancel') }}</button>
        </div>
      </template>
      <template v-else>
        <p class="settings-help settings-help--warn">{{ t('accountFile.replaceConfirm') }}</p>
        <div class="settings-actions">
          <button class="settings-btn settings-btn--danger" :disabled="busy"
                  @click="runImport('replace')">{{ t('accountFile.replaceConfirmButton') }}</button>
          <button class="settings-btn" @click="importConfirmReplace = false">{{ t('users.cancel') }}</button>
        </div>
      </template>
    </div>

    <!-- The two file pickers. Hidden, and opened by the buttons above, so one
         input serves every row's "Change Picture" and both "Import…" buttons. -->
    <input ref="avatarInput" type="file" accept="image/*" hidden @change="onAvatarFile" />
    <input ref="importInput" type="file" accept=".json,application/json" hidden @change="onImportFile" />

    <!-- What the last action did. Above the connected list rather than below
         it, because it answers something the person just did and the list
         below reflows every five seconds. -->
    <p v-if="notice" class="settings-help" :class="noticeIsError ? 'settings-help--warn' : ''">
      {{ notice }}
    </p>

    <!-- ================================================================
         Who is connected right now.
         Outside the authentication split on purpose: "is the tablet still
         on?" is the same question whether or not this server asks for a
         login, and with no accounts every session is anonymous rather than
         absent. Hidden from an operator, because /api/clients reports remote
         addresses and that is the machine's business (U3 made it admin-only
         for exactly that reason).
         ================================================================ -->
    <template v-if="clients.length">
      <h4 class="settings-group-title">{{ t('clients.title') }}</h4>
      <p class="settings-help">{{ t('clients.help') }}</p>

      <section v-for="c in clients" :key="c.id" class="settings-field settings-row">
        <div class="settings-row-lead">
          <!-- The face comes from the account list rather than from this poll:
               both are administrator-only, so whoever can see this list already
               has the pictures, and five-second polls need not carry them. -->
          <UserAvatar :name="c.user || '?'" :src="c.userId ? avatarById[c.userId] : null" :size="28"
                      :label="c.user || t('clients.anonymous')" />
          <div class="settings-row-main">
            <span class="settings-row-name">
              {{ c.user || t('clients.anonymous') }}
              <span v-if="c.kind === 'token'" class="settings-badge">{{ t('clients.token') }}</span>
              <span v-else-if="c.userId && c.userId === server.authUser?.id" class="settings-badge">
                {{ t('users.you') }}
              </span>
            </span>
            <span class="settings-row-sub">
              {{ c.remoteIp }}
              ·
              {{ t('clients.connected', { when: sinceLabel(c.connectedSeconds) }) }}
              ·
              {{ c.locale }}
            </span>
          </div>
        </div>
      </section>
    </template>
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
const { t, currentLocale } = useLocalization();
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

// ---- API tokens ------------------------------------------------------
// Same rule as above about separate drafts, and it matters more here: this
// form asks for the administrator's OWN password, so sharing a ref with the
// "set someone's new password" form would be a way to send one as the other.
const apiTokens     = ref<any[]>([]);
const showAddToken  = ref(false);
const tokenName     = ref('');
const tokenPassword = ref('');
const renameFor     = ref<any | null>(null);
const renameDraft   = ref('');
const pendingRevoke = ref<any | null>(null);
// The secret, for as long as it is on screen and no longer.
const issuedToken   = ref('');
const issuedName    = ref('');
const copied        = ref(false);
const secretInput   = ref<HTMLInputElement | null>(null);

// ---- Pictures ------------------------------------------------------------
// Whose picture the hidden file input is about to set. `self` chooses the
// route: a person's own goes through /api/auth/me/avatar (no admin needed), an
// administrator changing somebody else's goes through PATCH /api/users/<id>.
const avatarInput  = ref<HTMLInputElement | null>(null);
const avatarTarget = ref<{ id: string; self: boolean } | null>(null);
// Square, and small on purpose: every picture travels inline in the account
// list and in users.json (the server caps them at 128 KB), and this is drawn at
// 48px at most. Twice that is sharp on a high-density screen.
const AVATAR_PX = 128;

// For the connected list, which carries a user id but not a picture.
const avatarById = computed<Record<string, string | null>>(() => {
  const out: Record<string, string | null> = {};
  for (const u of users.value) out[u.id] = u.avatar ?? null;
  if (server.authUser?.id) out[server.authUser.id] = server.authUser.avatar ?? null;
  return out;
});

// ---- Moving accounts ---------------------------------------------------------
const importInput = ref<HTMLInputElement | null>(null);
// The parsed file, held between choosing it and choosing what to do with it.
const importData  = ref<any | null>(null);
const importConfirmReplace = ref(false);
const importCounts = computed(() => ({
  users:  Array.isArray(importData.value?.users) ? importData.value.users.length : 0,
  tokens: Array.isArray(importData.value?.apiTokens) ? importData.value.apiTokens.length : 0,
}));

// ---- Who is connected -------------------------------------------------
// Polled while this pane is open, because there is no push for it: the server
// broadcasts meters, cue state and document patches, and nothing for a socket
// opening or closing. Adding one would mean a fan-out to every client each
// time any client connects, which is a real cost for a list almost nobody is
// looking at — whereas this costs one request every few seconds, and only
// while somebody has the pane in front of them.
const CLIENTS_POLL_MS = 5000;
const clients = ref<any[]>([]);
let clientsTimer: ReturnType<typeof setInterval> | null = null;

// A form that opens out of sight reads as a button that did nothing.
//
// Every form on this pane used to be rendered together at the FOOT of it, which
// was reasonable while the pane was an account list and stopped being reasonable
// the moment it grew a token group, a posture group and a live list of
// connections below them: "Issue a token" opened a form a screen and a half
// away, and the button looked broken. The forms now sit where their control is.
//
// This is the other half of that, for the case where the control itself is near
// the bottom of a short window: bring the form into view and put the cursor in
// its first field. `nearest` scrolls the least that will do, so a form already
// on screen does not move. Focus first with the scroll suppressed, because
// focus() does its own scrolling and the two fight.
const vReveal = {
  mounted(el: HTMLElement) {
    (el.querySelector('input, select') as HTMLElement | null)?.focus({ preventScroll: true });
    el.scrollIntoView({
      block: 'nearest',
      behavior: window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth',
    });
  },
};

const isAdmin = computed(() => server.authUser?.role === 'admin');
// Whose password is being changed. The password form is the one on this pane
// that can be opened from somewhere other than where it appears (an operator
// has no account list to open it from), so it says the name rather than leaving
// an administrator to remember which row they clicked.
const passwordForName = computed(() => {
  const id = passwordFor.value;
  if (!id) return '';
  if (id === server.authUser?.id) return server.authUser?.name ?? '';
  return users.value.find((u: any) => u.id === id)?.name ?? '';
});
const canAdd  = computed(() =>
  draftName.value.trim().length > 0 && draftPassword.value.length >= MIN_PASSWORD_LENGTH);
// No MIN_PASSWORD_LENGTH floor here, deliberately: this is an EXISTING password
// being confirmed, not a new one being chosen, and refusing to submit a short
// one would lock out an account created before the floor existed.
const canApplyAuthChange = computed(() =>
  authPassword.value.length > 0 && (!!server.authUser || authName.value.trim().length > 0));
const canIssueToken = computed(() =>
  tokenName.value.trim().length > 0 && tokenPassword.value.length > 0);

// Unix seconds from the server, in the browser's own format for the language
// in force. A date format is one of the few things every locale already has an
// opinion about, so inventing one here would be 21 wrong answers.
function shortDate(seconds?: number | null): string {
  if (!seconds) return '';
  return new Date(seconds * 1000).toLocaleDateString(currentLocale.value, {
    year: 'numeric', month: 'short', day: 'numeric',
  });
}

// How long ago a session started, as "5 minutes ago" in the language in force.
// Intl does the words, for the same reason toLocaleDateString does the dates:
// "minute", "hour" and their plurals are grammar, and twenty-one hand-written
// unit strings is twenty-one chances to get one wrong. The server sends
// ELAPSED seconds rather than a timestamp (it survives a clock step), so the
// sign is negative here — the instant being described is in the past.
function sinceLabel(elapsed?: number): string {
  const s = Math.max(0, Math.floor(elapsed ?? 0));
  const rtf = new Intl.RelativeTimeFormat(currentLocale.value, { numeric: 'auto' });
  if (s < 60)   return rtf.format(-s, 'second');
  if (s < 3600) return rtf.format(-Math.round(s / 60), 'minute');
  return rtf.format(-Math.round(s / 3600), 'hour');
}

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
  // Before the early return below, not after: tokens exist and matter in the
  // authentication-off state too, where the accounts list does not.
  await loadTokens();
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

// ---- API tokens ------------------------------------------------------

async function loadTokens() {
  // Admin-gated while the login is on; open while it is off, along with every
  // other admin route — which is exactly why issuing one is refused in that
  // state. Reading the list is not the danger, and hiding it would be.
  if (server.authRequired && !isAdmin.value) { apiTokens.value = []; return; }
  try {
    apiTokens.value = await server.fetchApiTokens();
  } catch {
    // An operator being refused is not a fault, and this pane already has a
    // branch that says so. Nothing to report here.
    apiTokens.value = [];
  }
}

function cancelAddToken() {
  showAddToken.value = false;
  tokenName.value = '';
  // On the way out as well as the way in: an administrator's password left in
  // a ref is a password sitting in memory for no reason.
  tokenPassword.value = '';
}

async function issueToken() {
  if (!canIssueToken.value) return;
  busy.value = true;
  notice.value = '';
  try {
    const created = await server.createApiToken(tokenName.value.trim(), tokenPassword.value);
    issuedName.value  = created?.name ?? tokenName.value.trim();
    issuedToken.value = created?.token ?? '';
    copied.value = false;
    cancelAddToken();
    await loadTokens();
  } catch (e) {
    // The form stays open with the password cleared — a wrong password should
    // cost a retype, not the name that was typed with it. The server's wording
    // is what gets shown, including its 429 when the shared throttle bites.
    tokenPassword.value = '';
    report(e, 'apiTokens.issueFailed');
  } finally {
    busy.value = false;
  }
}

function selectSecret(e: Event) { (e.target as HTMLInputElement).select(); }

async function copyToken() {
  try {
    await navigator.clipboard.writeText(issuedToken.value);
    copied.value = true;
  } catch {
    // Clipboard access can be refused, and losing this string is expensive —
    // so select it instead and let the person copy it themselves rather than
    // reporting a failure they can do nothing about.
    secretInput.value?.select();
  }
}

function dismissIssued() {
  issuedToken.value = '';
  issuedName.value = '';
  copied.value = false;
}

function startTokenRename(k: any) {
  renameFor.value = k;
  renameDraft.value = k.name;
  notice.value = '';
}

async function saveTokenRename() {
  const k = renameFor.value;
  if (!k || !renameDraft.value.trim()) return;
  busy.value = true;
  try {
    await server.renameApiToken(k.id, renameDraft.value.trim());
    renameFor.value = null;
    await loadTokens();
  } catch (e) {
    report(e, 'users.saveFailed');
  } finally {
    busy.value = false;
  }
}

async function revokeToken() {
  busy.value = true;
  try {
    const k = pendingRevoke.value;
    await server.revokeApiToken(k.id);
    pendingRevoke.value = null;
    notice.value = t('apiTokens.revoked', { name: k.name });
    noticeIsError.value = false;
    await loadTokens();
  } catch (e) {
    report(e, 'apiTokens.revokeFailed');
    pendingRevoke.value = null;
  } finally {
    busy.value = false;
  }
}

async function loadClients() {
  // Admin-gated while the login is on. An operator being refused is not a
  // fault — remote addresses belong to the machine — so the section simply
  // does not appear rather than showing them a refusal they cannot act on.
  if (server.authRequired && !isAdmin.value) { clients.value = []; return; }
  try {
    clients.value = await server.fetchClients();
  } catch {
    // A poll that fails leaves the list as it was rather than blanking it:
    // one dropped request during a reconnect should not make the room look
    // empty for five seconds.
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

// ---- Pictures ------------------------------------------------------------

function pickAvatar(id: string | undefined, self: boolean) {
  if (!id) return;
  avatarTarget.value = { id, self };
  notice.value = '';
  avatarInput.value?.click();
}

// Any image the browser can decode, cropped to its centre square and redrawn at
// AVATAR_PX. Re-encoding is the point, not a side effect: whatever was picked —
// a 12-megapixel photo, an animated GIF, a PNG with a colour profile — what
// leaves here is one small still image in a format the server accepts. WebP
// where the browser can write it (Chromium, so Electron always can), PNG where
// it cannot; PNG rather than JPEG so a transparent logo stays transparent.
function toAvatarDataUrl(file: File): Promise<string> {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => {
      try {
        const side = Math.min(img.naturalWidth, img.naturalHeight);
        if (!side) throw new Error('empty image');
        const canvas = document.createElement('canvas');
        canvas.width = canvas.height = AVATAR_PX;
        const ctx = canvas.getContext('2d');
        if (!ctx) throw new Error('no canvas');
        ctx.imageSmoothingQuality = 'high';
        ctx.drawImage(img,
          (img.naturalWidth - side) / 2, (img.naturalHeight - side) / 2, side, side,
          0, 0, AVATAR_PX, AVATAR_PX);
        let out = canvas.toDataURL('image/webp', 0.85);
        if (!out.startsWith('data:image/webp')) out = canvas.toDataURL('image/png');
        resolve(out);
      } catch (e) {
        reject(e);
      } finally {
        URL.revokeObjectURL(url);
      }
    };
    img.onerror = () => { URL.revokeObjectURL(url); reject(new Error('not an image')); };
    img.src = url;
  });
}

async function onAvatarFile(e: Event) {
  const input = e.target as HTMLInputElement;
  const file = input.files?.[0];
  // Cleared at once so choosing the same file twice still fires a change.
  input.value = '';
  const target = avatarTarget.value;
  avatarTarget.value = null;
  if (!file || !target) return;

  let dataUrl: string;
  try {
    dataUrl = await toAvatarDataUrl(file);
  } catch {
    notice.value = t('avatar.notImage');
    noticeIsError.value = true;
    return;
  }
  busy.value = true;
  try {
    if (target.self) await server.setOwnAvatar(dataUrl);
    else             await server.updateUser(target.id, { avatar: dataUrl });
    notice.value = t('avatar.changed');
    noticeIsError.value = false;
    await afterAvatarChange(target.id);
  } catch (err) {
    report(err, 'avatar.failed');
  } finally {
    busy.value = false;
  }
}

async function clearAvatar(id: string | undefined, self: boolean) {
  if (!id) return;
  busy.value = true;
  notice.value = '';
  try {
    if (self) await server.clearOwnAvatar();
    else      await server.updateUser(id, { avatar: null });
    notice.value = t('avatar.removed');
    noticeIsError.value = false;
    await afterAvatarChange(id);
  } catch (err) {
    report(err, 'avatar.failed');
  } finally {
    busy.value = false;
  }
}

// The signed-in person's own picture lives on authUser, which only /api/auth/me
// refreshes — so a change to it (from either route: an administrator can reach
// their own row in the list too) re-asks, and the list is re-read either way.
async function afterAvatarChange(id: string) {
  if (id === server.authUser?.id) await server.checkAuth();
  await load();
}

// ---- Moving accounts ---------------------------------------------------------

// Local date, not UTC: the name is for the person looking at a folder of these,
// and an export made in the evening should not be dated tomorrow.
function exportFileName(): string {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, '0');
  return `liveplay-users-${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}.json`;
}

async function exportAccounts() {
  busy.value = true;
  notice.value = '';
  try {
    const data = await server.exportUsers();
    const text = JSON.stringify(data, null, 2);
    const name = exportFileName();
    // In the desktop app, a real save dialog and a real file. In a browser,
    // the ordinary download — which is also the fallback for a desktop shell
    // older than the dialog.
    const api = typeof window !== 'undefined' ? window.electronAPI : undefined;
    if (api?.showSaveJsonDialog) {
      const dest = await api.showSaveJsonDialog(name, t('accountFile.saveTitle'));
      if (!dest) return;   // cancelled — not an error, and nothing to say
      const w = await api.writeFile(dest, text);
      if (!w.success) throw new Error(w.error || 'write failed');
      notice.value = t('accountFile.exportedTo', { file: dest });
    } else {
      const url = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
      const a = document.createElement('a');
      a.href = url;
      a.download = name;
      document.body.appendChild(a);
      a.click();
      a.remove();
      // Revoked on the next tick, not immediately: some browsers start the
      // download asynchronously and a revoked URL downloads nothing.
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      notice.value = t('accountFile.exported');
    }
    noticeIsError.value = false;
  } catch (e) {
    report(e, 'accountFile.exportFailed');
  } finally {
    busy.value = false;
  }
}

function pickImportFile() {
  notice.value = '';
  importInput.value?.click();
}

// Read and sanity-check the file HERE, before anything is sent, so a wrong
// file is caught with a plain message rather than a round trip — but only
// enough to show a preview. The server validates every record again and is
// the one whose refusal counts.
async function onImportFile(e: Event) {
  const input = e.target as HTMLInputElement;
  const file = input.files?.[0];
  input.value = '';
  if (!file) return;
  try {
    const data = JSON.parse(await file.text());
    if (data?.format !== 'liveplay-users' || !Array.isArray(data.users)) throw new Error('format');
    importData.value = data;
    importConfirmReplace.value = false;
  } catch {
    importData.value = null;
    notice.value = t('accountFile.notAFile');
    noticeIsError.value = true;
  }
}

function cancelImport() {
  // Dropped rather than kept: the file holds password hashes, and there is no
  // reason for them to sit in memory once the person has said no.
  importData.value = null;
  importConfirmReplace.value = false;
}

async function runImport(mode: 'merge' | 'replace') {
  if (!importData.value) return;
  busy.value = true;
  notice.value = '';
  try {
    const res = await server.importUsers(mode, importData.value);
    cancelImport();
    notice.value = mode === 'replace'
      ? t('accountFile.resultReplace', { count: res?.userCount ?? 0 })
      : t('accountFile.resultMerge', {
          added:   res?.usersAdded?.length ?? 0,
          skipped: res?.usersSkipped?.length ?? 0,
          tokens:  res?.tokensAdded?.length ?? 0,
        });
    noticeIsError.value = false;
    // A replace can remove the account doing it, or give it a different
    // password; the server says which. Signing out now shows the login screen
    // straight away, instead of leaving the pane to fail on its next request.
    if (res?.sessionValid === false) {
      notice.value = t('accountFile.signedOut');
      server.logout();
      return;
    }
    // Re-ask the posture either way: importing into an empty server turns the
    // login on (and this client, anonymous until now, will need to sign in),
    // and a replace can change the caller's role or picture.
    await server.checkAuth();
    await load();
  } catch (e) {
    report(e, 'accountFile.importFailed');
  } finally {
    busy.value = false;
  }
}

onMounted(() => {
  void load();
  void loadClients();
  clientsTimer = setInterval(() => { void loadClients(); }, CLIENTS_POLL_MS);
});
onBeforeUnmount(() => {
  if (clientsTimer) clearInterval(clientsTimer);
  clientsTimer = null;
});
// A reconnect can be a different server with a different account list, and a
// sign-in changes what this client is allowed to see.
watch(() => server.connected, (up) => { if (up) { void load(); void loadClients(); } });
watch(() => server.authUser?.id, () => { void load(); void loadClients(); });
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
// The picture beside the name and role, as one unit that wraps as one.
.settings-row-lead { display: flex; align-items: center; gap: 12px; min-width: 0; }
.settings-me {
  display: flex;
  align-items: center;
  gap: 12px;
  margin: 4px 0 8px;
  .settings-help { margin: 0; }
}
.settings-row-name { font-weight: 600; font-size: 14px; }
.settings-row-sub  { font-size: 12px; opacity: 0.6; }
// SettingsPage's shared `.settings-field` (a column) and full-width
// `.settings-select` win over the two rules above on source order alone, which
// stacked every row — name centred, the role select a full-width bar, buttons
// on a line of their own. A little more specificity puts the row back.
.settings-pane .settings-field.settings-row { flex-direction: row; }
.settings-pane .settings-actions .settings-select--inline { width: auto; min-width: 120px; padding: 6px 10px; }
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
// The secret's own form is marked out, because it is the one thing on this
// pane that cannot be recovered by looking again.
.settings-inline-form--secret { border-color: rgba(255, 196, 0, 0.5); }
.settings-input--secret {
  font-family: ui-monospace, "Cascadia Mono", Consolas, monospace;
  font-size: 12px;
  // A token is 80-odd characters with no spaces, so it cannot fit and cannot
  // wrap inside an input. It scrolls, and selecting the whole string on focus
  // is what makes copying it by hand reliable.
  text-overflow: ellipsis;
}
</style>
