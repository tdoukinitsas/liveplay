// ============================================================================
// liveplay/core/user_store.hpp
// ----------------------------------------------------------------------------
// Who may talk to this server, and what they may change once they have.
//
// Machine-tier state, like outputs.json and liveplay.json: it lives beside the
// executable, it never travels in a project, and moving a show to another rig
// does not carry its accounts along. A venue owns its own door.
//
// THE DEFAULT IS OPEN. A store with no users in it means "no authentication",
// which is exactly what every release before 2.5 did, and it is what an
// upgrade must keep doing — a server that started demanding a password after a
// point release would lock an operator out of their own rig, quite possibly
// mid-show. Same posture as --fs-root and --cors-origin: the knob appears, the
// behaviour does not change until someone sets it, and the server says which
// posture it is in at boot rather than leaving it to be discovered.
//
// Two roles, and the split is the ownership model's own tier boundary rather
// than an access-control scheme invented alongside it:
//
//   Operator — runs the show. The Project tier and the User tier: cues,
//              buses, transport, project settings, media, their own locale.
//   Admin    — the above, plus the Server tier: the output map, the accounts
//              in this file, and the list of who is connected.
//
// So "what needs admin" has an answer that can be derived rather than
// remembered: it is the state that belongs to the machine rather than to the
// show or to the person.
//
// ---------------------------------------------------------------------------
// Nothing here rolls its own crypto
// ---------------------------------------------------------------------------
// Passwords are hashed with Argon2id (libsodium's crypto_pwhash_str) at the
// INTERACTIVE limits — a login has to complete on the laptop running the show,
// not on a benchmark rig. Verification is libsodium's, so the comparison is
// constant-time and the parameters travel inside the hash string, which means
// raising the limits later does not invalidate existing passwords.
//
// Tokens are stateless and signed (crypto_auth, HMAC-SHA512-256) with a secret
// generated once and kept in this file. Stateless is a deliberate choice and
// not the lazy one: the crash handler AUTO-RESTARTS this server, and tokens
// held only in memory would silently log out every connected surface at the
// exact moment things were already going wrong. A signed token survives the
// restart because the secret does.
//
// The cost of statelessness is that a token cannot be individually withdrawn,
// so each user carries a token_epoch that is stamped into their tokens and
// compared on every verify. Changing a password or deleting a user bumps it,
// which invalidates everything issued to them — the two cases where immediate
// revocation is what anyone actually means.
// ============================================================================
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace liveplay::core {

using json = nlohmann::json;

enum class UserRole { Operator, Admin };

std::string      to_string(UserRole r);
std::optional<UserRole> role_from_string(const std::string& s);

// How long an issued token stays valid. Long, on purpose: this is a LAN tool
// that lives on a desk through load-in, rehearsal and two shows, and an
// operator re-typing a password because a token expired between the matinee
// and the evening is a worse outcome than the one the short expiry prevents.
// One owner for the number, per the O5 rule — the routes and the docs both
// read it from here.
inline constexpr std::int64_t kTokenTtlSeconds = 30LL * 24 * 60 * 60;   // 30 days

// Shortest password the store will accept. Not a policy engine — a floor.
inline constexpr std::size_t kMinPasswordLength = 8;

// What an API token starts with. Recognisable on sight, which is the point: a
// string that says what it is can be spotted in a pasted config, a log line or
// a repository before it is used against the desk it belongs to. `lp1.` remains
// the session token, so one look distinguishes a credential that expires in 30
// days from one that does not expire at all.
inline constexpr std::string_view kApiTokenPrefix = "lpk1_";

class UserStore {
public:
    UserStore();

    // One record in the file. The hash is deliberately not exposed by any
    // accessor: it is read by verify() and written by set_password(), and
    // nothing outside this class has a reason to hold one.
    struct User {
        std::string   id;                 // stable; never reused, never renamed
        std::string   name;               // login name, unique case-insensitively
        UserRole      role = UserRole::Operator;
        std::uint64_t token_epoch = 1;    // bumped to revoke this user's tokens
        std::int64_t  created_at  = 0;    // unix seconds, for display only
    };

    // What kind of thing authenticated. A token issued to Companion is a
    // principal in its own right and NOT a stand-in for the administrator who
    // created it — §6.2 of the ownership model calls these "principals that are
    // not people", and naming the kind is what lets one rule cover them all
    // instead of each route remembering.
    enum class Kind { User, ApiToken };

    // Who a request turned out to be. Copied out of the store at
    // authenticate/verify time, so a handler holding one is not holding a
    // reference into a container another thread may be editing.
    struct Principal {
        std::string id;      // user id, or token id
        std::string name;    // login name, or the token's label
        UserRole    role = UserRole::Operator;
        Kind        kind = Kind::User;

        // An API token is never an administrator, whatever else becomes true of
        // it. Tokens are issued at the operator tier and carry no role of their
        // own; the kind check is here so that stays true if one ever does.
        bool is_admin() const { return kind == Kind::User && role == UserRole::Admin; }
        bool is_api()   const { return kind == Kind::ApiToken; }
    };

    // ---- API tokens: a credential for a thing rather than a person --------
    //
    // A Companion button, a show-control cue, a script that arms the next item.
    // NOT an account: no password, no preferences, no role, and no reach into
    // the filesystem or the Server tier however it is used — the deny list that
    // says so lives beside access_for(), because it is the same kind of rule.
    //
    // The secret is shown ONCE, at creation, and stored only as a BLAKE2b hash.
    // Unsalted, deliberately: a password is low-entropy and needs Argon2id to
    // make guessing expensive, while this is 32 bytes straight from the CSPRNG
    // and guessing it is not a thing anyone can do. What the fast hash buys is
    // verification on EVERY request without spending Argon2id's ~100 ms on each
    // one — which is the whole reason tokens are not just stored passwords.
    //
    // These do not expire. A Companion install runs for seasons, and a token
    // that dies between the matinee and the evening is exactly the failure this
    // exists to prevent; revoking one is immediate and per-token, which is the
    // ending that was actually wanted.
    struct ApiToken {
        std::string  id;             // stable; travels inside the token string
        std::string  name;           // label, unique case-insensitively
        std::string  created_by;     // user id of the administrator who issued it
        std::int64_t created_at   = 0;
        std::int64_t last_used_at = 0;   // 0 = never seen
    };

    enum class Result {
        Ok,
        BadName,          // empty, too long, or contains control characters
        NameTaken,
        WeakPassword,     // shorter than kMinPasswordLength
        NoSuchUser,
        LastAdmin,        // would leave the store with no way in
        NoAccounts,       // cannot demand a login when there is nobody to be
        HashFailed,       // libsodium refused (out of memory, essentially)
        IoError,          // the file could not be written
        NoSuchToken,
        AuthOff,          // issuing a credential through an open door
    };
    static std::string_view describe(Result r);

    // Where users.json lives. Set once at startup, before load().
    void                  set_path(std::filesystem::path p);
    std::filesystem::path path() const;

    // Read from disk. A missing file is not an error — it is the open posture,
    // and it is the ordinary state of an installation nobody has locked down.
    // Returns false only when a file exists and could not be understood, which
    // is worth distinguishing: a corrupt store must NOT silently read as "no
    // users", because that would turn a damaged file into an open door.
    bool load();

    // True when a store file existed but could not be parsed. The server
    // refuses to serve in this state rather than falling open (see main.cpp).
    bool corrupt() const;

    // Is authentication in force? False for an empty store, and false when
    // somebody has explicitly turned it off — see set_auth_required.
    bool        auth_required() const;
    std::size_t user_count() const;
    bool        has_admin() const;

    // Turn authentication off (or back on) WITHOUT touching the accounts.
    //
    // Until this existed there was no route back: auth_required() was simply
    // "the store is not empty", and remove_user refuses to delete the last
    // admin, so the guard that stops an operator locking themselves out of
    // administration also made authentication permanent. The documented recovery
    // was deleting this file by hand, which throws the team away to undo a
    // posture change.
    //
    // A stored `false` is a DECISION and is deliberately distinguishable from
    // the absence of one, the same way U4's preferences treat "system" as
    // different from unset: an installation that has never chosen keeps taking
    // whatever the default is (on, once there are accounts), while one that has
    // chosen keeps its answer even as accounts come and go.
    //
    // Turning it ON with no accounts is refused (NoAccounts) — that state is a
    // server demanding a login nobody can satisfy. Turning it OFF is always
    // allowed, because the open posture is what every release before 2.5 did.
    //
    // NOTE FOR THE ROUTE, NOT FOR THIS CLASS: nothing here authenticates the
    // caller. The store cannot know who is asking, so the admin gate and the
    // password re-entry both live at the REST layer.
    Result set_auth_required(bool required);

    // Has somebody explicitly chosen, either way? Distinct from what
    // auth_required() answers, and the boot warning needs it: "off while
    // accounts exist" is a state an operator must never be in unknowingly.
    bool auth_choice_recorded() const;

    std::vector<User>   users() const;              // never includes hashes
    std::optional<User> find_by_id(const std::string& id) const;
    std::optional<User> find_by_name(const std::string& name) const;

    // Create. The first user in an empty store is forced to Admin regardless
    // of what was asked for: a store whose only account cannot manage accounts
    // is a locked room with the key inside.
    Result add_user(const std::string& name, const std::string& password,
                    UserRole role, std::string* out_id = nullptr);

    // Both bump token_epoch, which is what makes them take effect immediately
    // on every surface already holding a token rather than at the next login.
    Result set_password(const std::string& id, const std::string& password);
    Result remove_user(const std::string& id);

    Result set_role(const std::string& id, UserRole role);
    Result rename_user(const std::string& id, const std::string& name);

    // Invalidate every token issued to this user without changing anything
    // else — "sign me out everywhere".
    Result bump_epoch(const std::string& id);

    // Name + password → principal, or nothing. Takes the same time whether
    // the name exists or not: an unknown name is verified against a fixed
    // decoy hash, so the response cannot be used to enumerate accounts.
    std::optional<Principal> authenticate(const std::string& name,
                                          const std::string& password) const;

    // Mint a token for a principal already established by authenticate().
    std::string mint_token(const Principal& p) const;

    // The reverse, for BOTH kinds of credential — a session token minted above,
    // and an API token issued below. One entry point on purpose: there are two
    // doors into this server (REST and the WebSocket upgrade) and each should
    // ask "who is this?" once rather than once per kind.
    //
    // Returns nothing for a token that is malformed, unsigned by us, expired,
    // issued to a user who has since been deleted, stamped with an epoch the
    // user has moved past, or — for an API token — revoked.
    std::optional<Principal> verify_token(const std::string& token) const;

    // ---- API tokens -------------------------------------------------------
    std::vector<ApiToken> api_tokens() const;        // never includes hashes

    // Issue one. The record comes back through `out` and the ONE AND ONLY copy
    // of the secret through `out_secret`: nothing stores it, so a caller that
    // drops it has destroyed it, and the only way back is another token.
    //
    // Refused while authentication is off (AuthOff), and that is an escalation
    // rather than tidiness: with the door open anyone on the LAN could mint a
    // credential that kept working after an administrator shut it. The STORE
    // refuses it rather than the route, so no future caller can miss the rule.
    Result create_api_token(const std::string& name, const std::string& created_by,
                            ApiToken* out, std::string* out_secret);
    Result rename_api_token(const std::string& id, const std::string& name);
    Result revoke_api_token(const std::string& id);

    // Record that a token was just used, for "is anything still using this?"
    // before someone revokes it.
    //
    // Separate from verify_token so verification stays const and free of disk
    // writes. Called by the one place that admits a request, and it writes the
    // file at most once an hour per token: a credential file rewritten on every
    // automation call would be a far worse trade than a timestamp that lags.
    void note_api_token_use(const std::string& id);

    json to_json() const;   // includes hashes and the secret — for save() only
    bool from_json(const json& j);

private:
    struct Record {
        User        user;
        std::string hash;      // libsodium's self-describing Argon2id string
    };

    struct TokenRecord {
        ApiToken    token;
        std::string hash;      // base64 BLAKE2b of the secret; the secret is gone
        // What last_used_at was when the file was last written. Not serialised:
        // it exists only to rate-limit the writes, and on the next boot it
        // starts from whatever is on disk.
        std::int64_t saved_use = 0;
    };

    // Callers already holding mutex_ use these; the public methods lock and
    // delegate. Kept explicit rather than relying on a recursive mutex, so the
    // lock discipline is visible at every call site.
    bool                save_locked() const;
    json                to_json_locked() const;   // the one serialiser
    const Record*       find_locked(const std::string& id) const;
    Record*             find_locked(const std::string& id);
    std::size_t         admin_count_locked() const;
    bool                auth_required_locked() const;
    const TokenRecord*  find_token_locked(const std::string& id) const;
    TokenRecord*        find_token_locked(const std::string& id);
    std::optional<Principal> verify_api_token(const std::string& token) const;

    mutable std::mutex       mutex_;
    std::filesystem::path    path_;
    std::vector<Record>      users_;
    std::vector<TokenRecord> tokens_;
    std::string           token_secret_;   // base64; generated on first save
    bool                  corrupt_ = false;
    // Unset = nobody has chosen; see set_auth_required. Sparse on disk, so a
    // file written before this field existed reads back identically and a build
    // that predates it ignores the key and keeps authentication ON — the
    // fail-safe direction.
    std::optional<bool>   auth_required_override_;
};

} // namespace liveplay::core
