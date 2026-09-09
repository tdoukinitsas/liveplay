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

    // Who a request turned out to be. Copied out of the store at
    // authenticate/verify time, so a handler holding one is not holding a
    // reference into a container another thread may be editing.
    struct Principal {
        std::string id;
        std::string name;
        UserRole    role = UserRole::Operator;

        bool is_admin() const { return role == UserRole::Admin; }
    };

    enum class Result {
        Ok,
        BadName,          // empty, too long, or contains control characters
        NameTaken,
        WeakPassword,     // shorter than kMinPasswordLength
        NoSuchUser,
        LastAdmin,        // would leave the store with no way in
        HashFailed,       // libsodium refused (out of memory, essentially)
        IoError,          // the file could not be written
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

    // Is authentication in force? False for an empty store.
    bool        auth_required() const;
    std::size_t user_count() const;
    bool        has_admin() const;

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

    // The reverse. Returns nothing for a token that is malformed, unsigned by
    // us, expired, issued to a user who has since been deleted, or stamped
    // with an epoch the user has moved past.
    std::optional<Principal> verify_token(const std::string& token) const;

    json to_json() const;   // includes hashes and the secret — for save() only
    bool from_json(const json& j);

private:
    struct Record {
        User        user;
        std::string hash;      // libsodium's self-describing Argon2id string
    };

    // Callers already holding mutex_ use these; the public methods lock and
    // delegate. Kept explicit rather than relying on a recursive mutex, so the
    // lock discipline is visible at every call site.
    bool                save_locked() const;
    json                to_json_locked() const;   // the one serialiser
    const Record*       find_locked(const std::string& id) const;
    Record*             find_locked(const std::string& id);
    std::size_t         admin_count_locked() const;

    mutable std::mutex    mutex_;
    std::filesystem::path path_;
    std::vector<Record>   users_;
    std::string           token_secret_;   // base64; generated on first save
    bool                  corrupt_ = false;
};

} // namespace liveplay::core
