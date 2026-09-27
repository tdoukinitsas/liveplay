// user_store.cpp — see user_store.hpp.
#include "liveplay/core/user_store.hpp"

#include "liveplay/logger.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#if !defined(_WIN32)
#  include <sys/stat.h>
#endif

namespace liveplay::core {

namespace fs = std::filesystem;

namespace {

constexpr int kUserStoreSchemaVersion = 1;

// libsodium has to be initialised once per process before any other call. It
// is safe to call repeatedly and from multiple threads since 1.0.12, so the
// store simply asks on construction rather than making main.cpp remember.
bool ensure_sodium() {
    static const bool ok = (sodium_init() >= 0);
    return ok;
}

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// Case-insensitive over ASCII, which is what login names are compared as.
// Deliberately NOT a full Unicode casefold: two names that differ only by a
// Turkish dotless i should be two names, not a collision nobody can see.
std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// URL-safe and unpadded, so a token can sit in a query string (which is how
// the WebSocket upgrade carries it — a browser cannot set headers there)
// without any escaping at all.
std::string b64_encode(const unsigned char* data, std::size_t len) {
    const std::size_t max = sodium_base64_ENCODED_LEN(
        len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::string out(max, '\0');
    sodium_bin2base64(out.data(), out.size(), data, len,
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::strlen(out.c_str()));
    return out;
}

bool b64_decode(const std::string& text, std::vector<unsigned char>& out) {
    out.assign(text.size(), 0);   // decoded is always shorter than encoded
    std::size_t written = 0;
    if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(),
                          nullptr, &written, nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        return false;
    }
    out.resize(written);
    return true;
}

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    randombytes_buf(buf.data(), buf.size());
    std::string out(bytes * 2 + 1, '\0');
    sodium_bin2hex(out.data(), out.size(), buf.data(), buf.size());
    out.resize(bytes * 2);
    return out;
}

// A valid Argon2id string for a password nobody knows, used to spend the same
// time on an unknown user as on a known one. Generated once per process from
// random bytes so it is not a constant anyone can recognise in a core dump.
const std::string& decoy_hash() {
    static const std::string h = [] {
        char out[crypto_pwhash_STRBYTES] = {};
        const std::string pw = random_hex(32);
        if (crypto_pwhash_str(out, pw.data(), pw.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            return std::string{};
        }
        return std::string{out};
    }();
    return h;
}

} // namespace

std::string to_string(UserRole r) {
    return r == UserRole::Admin ? "admin" : "operator";
}

std::optional<UserRole> role_from_string(const std::string& s) {
    if (s == "admin")    return UserRole::Admin;
    if (s == "operator") return UserRole::Operator;
    return std::nullopt;
}

std::string_view UserStore::describe(Result r) {
    switch (r) {
        case Result::Ok:           return "ok";
        case Result::BadName:      return "a user name must be 1-64 characters and printable";
        case Result::NameTaken:    return "a user with that name already exists";
        case Result::WeakPassword: return "the password is too short";
        case Result::NoSuchUser:   return "no such user";
        case Result::LastAdmin:    return "this is the only administrator — promote another first";
        case Result::NoAccounts:   return "create an account before requiring a login";
        case Result::HashFailed:   return "the password could not be hashed";
        case Result::IoError:      return "the user store could not be written";
    }
    return "the request was refused";
}

UserStore::UserStore() {
    if (!ensure_sodium()) {
        // Nothing this class offers is safe without it, and pretending
        // otherwise would mean hashing with an uninitialised CSPRNG.
        Logger::error("libsodium failed to initialise — authentication is unavailable");
    }
}

void UserStore::set_path(fs::path p) {
    std::lock_guard lock{mutex_};
    path_ = std::move(p);
}

fs::path UserStore::path() const {
    std::lock_guard lock{mutex_};
    return path_;
}

bool UserStore::load() {
    fs::path p = path();
    if (p.empty()) return false;
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        // The ordinary state of an installation nobody has locked down.
        Logger::debug("UserStore: no store at '{}' — authentication is off",
                      util::path_to_utf8(p));
        return false;
    }
    try {
        std::ifstream in{p, std::ios::binary};
        json j = json::parse(in, nullptr, false);
        if (j.is_discarded() || !from_json(j)) {
            // Loud, and sticky. A store that cannot be read must never be
            // mistaken for an empty one: "no users" means "let everyone in",
            // so a corrupt file that fell back to it would turn a damaged
            // disk into an unlocked door. corrupt() keeps the distinction and
            // main.cpp refuses to serve while it is set.
            std::lock_guard lock{mutex_};
            corrupt_ = true;
            Logger::error("UserStore: '{}' could not be read. The server will NOT "
                          "fall back to open access — fix or remove the file.",
                          util::path_to_utf8(p));
            return false;
        }
        {
            std::lock_guard lock{mutex_};
            corrupt_ = false;
        }
        Logger::info("UserStore: loaded {} account(s) from '{}'",
                     user_count(), util::path_to_utf8(p));
        return true;
    } catch (const std::exception& e) {
        std::lock_guard lock{mutex_};
        corrupt_ = true;
        Logger::error("UserStore: failed to read '{}': {}", util::path_to_utf8(p), e.what());
        return false;
    }
}

bool UserStore::corrupt() const {
    std::lock_guard lock{mutex_};
    return corrupt_;
}

bool UserStore::save_locked() const {
    if (path_.empty()) return false;
    try {
        std::error_code ec;
        fs::create_directories(path_.parent_path(), ec);
        const fs::path tmp = path_.string() + ".tmp";
        {
            std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
            if (!out) return false;
            out << to_json_locked().dump(2);
            out.flush();
            if (!out) return false;
        }
#if !defined(_WIN32)
        // Owner read/write only, set on the temp file BEFORE the rename so the
        // store is never briefly world-readable at its real name. This file
        // holds password hashes and the token-signing secret; anyone who can
        // read the secret can mint a token for any account in it.
        //
        // No equivalent here on Windows: doing it properly means a DACL, and a
        // half-done ACL that looks like protection is worse than an honest
        // note that the file inherits the directory's permissions. Documented
        // in server/README.md rather than faked.
        ::chmod(tmp.string().c_str(), S_IRUSR | S_IWUSR);
#endif
        fs::rename(tmp, path_, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        Logger::warn("UserStore: failed to write '{}': {}",
                     util::path_to_utf8(path_), e.what());
        return false;
    }
}

bool UserStore::auth_required() const {
    std::lock_guard lock{mutex_};
    // An empty store is open, unconditionally and before the override is even
    // consulted. This ordering is what makes the feature lockout-proof: a
    // stored `true` over an empty store would be a server demanding a login
    // that nobody on earth could satisfy, and it is reachable by deleting
    // accounts after choosing "on". set_auth_required refuses to create that
    // state; this makes it unrepresentable.
    if (users_.empty()) return false;
    return auth_required_override_.value_or(true);
}

UserStore::Result UserStore::set_auth_required(bool required) {
    std::lock_guard lock{mutex_};
    // See the header: on with nobody to be is the one combination that bricks
    // the server, so it is refused here rather than merely rendered harmless.
    if (required && users_.empty()) return Result::NoAccounts;

    const auto previous = auth_required_override_;
    auth_required_override_ = required;
    if (!save_locked()) { auth_required_override_ = previous; return Result::IoError; }

    // Tokens are NOT invalidated either way, and both directions are deliberate.
    // Turning it off: every token stops being asked for, so revoking them would
    // be busywork. Turning it on: the sessions that were open while the door was
    // unlocked were never anybody in particular — they hold no token at all, so
    // there is nothing to revoke and they will be asked to log in on their next
    // request like any other unauthenticated caller.
    Logger::warn("UserStore: authentication explicitly turned {} ({} account(s) kept)",
                 required ? "ON" : "OFF", users_.size());
    return Result::Ok;
}

bool UserStore::auth_choice_recorded() const {
    std::lock_guard lock{mutex_};
    return auth_required_override_.has_value();
}

std::size_t UserStore::user_count() const {
    std::lock_guard lock{mutex_};
    return users_.size();
}

bool UserStore::has_admin() const {
    std::lock_guard lock{mutex_};
    return admin_count_locked() > 0;
}

std::size_t UserStore::admin_count_locked() const {
    return static_cast<std::size_t>(
        std::count_if(users_.begin(), users_.end(), [](const Record& r) {
            return r.user.role == UserRole::Admin;
        }));
}

const UserStore::Record* UserStore::find_locked(const std::string& id) const {
    for (const auto& r : users_) if (r.user.id == id) return &r;
    return nullptr;
}

UserStore::Record* UserStore::find_locked(const std::string& id) {
    for (auto& r : users_) if (r.user.id == id) return &r;
    return nullptr;
}

std::vector<UserStore::User> UserStore::users() const {
    std::lock_guard lock{mutex_};
    std::vector<User> out;
    out.reserve(users_.size());
    for (const auto& r : users_) out.push_back(r.user);
    std::sort(out.begin(), out.end(), [](const User& a, const User& b) {
        return lower_ascii(a.name) < lower_ascii(b.name);
    });
    return out;
}

std::optional<UserStore::User> UserStore::find_by_id(const std::string& id) const {
    std::lock_guard lock{mutex_};
    if (const auto* r = find_locked(id)) return r->user;
    return std::nullopt;
}

std::optional<UserStore::User> UserStore::find_by_name(const std::string& name) const {
    std::lock_guard lock{mutex_};
    const std::string key = lower_ascii(trim(name));
    for (const auto& r : users_)
        if (lower_ascii(r.user.name) == key) return r.user;
    return std::nullopt;
}

// A name has to be something an operator can type and read back in a list:
// non-empty, bounded, and free of control characters that would make two
// different names print identically.
static bool name_is_valid(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (const unsigned char c : n)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

UserStore::Result UserStore::add_user(const std::string& name_in,
                                      const std::string& password,
                                      UserRole role,
                                      std::string* out_id) {
    const std::string name = trim(name_in);
    if (!name_is_valid(name))                 return Result::BadName;
    if (password.size() < kMinPasswordLength) return Result::WeakPassword;
    if (!ensure_sodium())                     return Result::HashFailed;

    std::lock_guard lock{mutex_};
    const std::string key = lower_ascii(name);
    for (const auto& r : users_)
        if (lower_ascii(r.user.name) == key) return Result::NameTaken;

    char hash[crypto_pwhash_STRBYTES] = {};
    if (crypto_pwhash_str(hash, password.data(), password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        return Result::HashFailed;
    }

    Record rec;
    rec.user.id          = random_hex(16);
    rec.user.name        = name;
    // The first account in an empty store is an administrator whatever was
    // asked for. An operator-only store is a room locked from the inside: no
    // one in it can create the admin who could let anyone else in, and the
    // only way out is deleting this file by hand.
    rec.user.role        = users_.empty() ? UserRole::Admin : role;
    rec.user.token_epoch = 1;
    rec.user.created_at  = now_unix();
    rec.hash             = hash;
    sodium_memzero(hash, sizeof(hash));

    if (token_secret_.empty()) {
        unsigned char key_buf[crypto_auth_KEYBYTES];
        randombytes_buf(key_buf, sizeof(key_buf));
        token_secret_ = b64_encode(key_buf, sizeof(key_buf));
        sodium_memzero(key_buf, sizeof(key_buf));
    }

    users_.push_back(rec);
    if (!save_locked()) {
        users_.pop_back();
        return Result::IoError;
    }
    if (out_id) *out_id = rec.user.id;
    Logger::info("UserStore: created {} '{}'", to_string(rec.user.role), rec.user.name);
    return Result::Ok;
}

UserStore::Result UserStore::set_password(const std::string& id,
                                          const std::string& password) {
    if (password.size() < kMinPasswordLength) return Result::WeakPassword;
    if (!ensure_sodium())                     return Result::HashFailed;

    std::lock_guard lock{mutex_};
    Record* r = find_locked(id);
    if (!r) return Result::NoSuchUser;

    char hash[crypto_pwhash_STRBYTES] = {};
    if (crypto_pwhash_str(hash, password.data(), password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        return Result::HashFailed;
    }

    const std::string    previous_hash  = r->hash;
    const std::uint64_t  previous_epoch = r->user.token_epoch;
    r->hash = hash;
    sodium_memzero(hash, sizeof(hash));
    // Changing a password is the one moment someone means "and stop whatever
    // is already logged in" — a stolen token is exactly what they are usually
    // reacting to. Bumping the epoch is what makes that true rather than
    // merely intended.
    r->user.token_epoch = previous_epoch + 1;

    if (!save_locked()) {
        r->hash             = previous_hash;
        r->user.token_epoch = previous_epoch;
        return Result::IoError;
    }
    return Result::Ok;
}

UserStore::Result UserStore::set_role(const std::string& id, UserRole role) {
    std::lock_guard lock{mutex_};
    Record* r = find_locked(id);
    if (!r) return Result::NoSuchUser;
    if (r->user.role == role) return Result::Ok;
    if (r->user.role == UserRole::Admin && admin_count_locked() == 1)
        return Result::LastAdmin;

    const UserRole previous = r->user.role;
    r->user.role = role;
    if (!save_locked()) { r->user.role = previous; return Result::IoError; }
    return Result::Ok;
}

UserStore::Result UserStore::rename_user(const std::string& id,
                                         const std::string& name_in) {
    const std::string name = trim(name_in);
    if (!name_is_valid(name)) return Result::BadName;

    std::lock_guard lock{mutex_};
    Record* r = find_locked(id);
    if (!r) return Result::NoSuchUser;
    const std::string key = lower_ascii(name);
    for (const auto& other : users_)
        if (other.user.id != id && lower_ascii(other.user.name) == key)
            return Result::NameTaken;

    const std::string previous = r->user.name;
    r->user.name = name;
    if (!save_locked()) { r->user.name = previous; return Result::IoError; }
    return Result::Ok;
}

UserStore::Result UserStore::remove_user(const std::string& id) {
    std::lock_guard lock{mutex_};
    auto it = std::find_if(users_.begin(), users_.end(),
                           [&](const Record& r) { return r.user.id == id; });
    if (it == users_.end()) return Result::NoSuchUser;
    if (it->user.role == UserRole::Admin && admin_count_locked() == 1)
        return Result::LastAdmin;

    const Record removed = *it;
    users_.erase(it);
    if (!save_locked()) {
        users_.push_back(removed);
        return Result::IoError;
    }
    // Deleting the record is what invalidates their tokens — verify_token
    // looks the user up on every call, so there is no epoch to bump here and
    // no window in which a token outlives the account.
    Logger::info("UserStore: removed user '{}'", removed.user.name);
    return Result::Ok;
}

UserStore::Result UserStore::bump_epoch(const std::string& id) {
    std::lock_guard lock{mutex_};
    Record* r = find_locked(id);
    if (!r) return Result::NoSuchUser;
    const std::uint64_t previous = r->user.token_epoch;
    r->user.token_epoch = previous + 1;
    if (!save_locked()) { r->user.token_epoch = previous; return Result::IoError; }
    return Result::Ok;
}

std::optional<UserStore::Principal> UserStore::authenticate(
        const std::string& name, const std::string& password) const {
    if (!ensure_sodium()) return std::nullopt;

    std::string hash;
    std::optional<Principal> found;
    {
        std::lock_guard lock{mutex_};
        const std::string key = lower_ascii(trim(name));
        for (const auto& r : users_) {
            if (lower_ascii(r.user.name) != key) continue;
            hash  = r.hash;
            found = Principal{r.user.id, r.user.name, r.user.role};
            break;
        }
    }

    // No early return for an unknown name. Argon2id at these limits takes on
    // the order of 100 ms, which is plainly measurable over a LAN, so
    // answering an unknown name instantly would let anyone map the account
    // list by timing alone. Spending the same work on a decoy costs one login
    // attempt and removes the signal.
    const bool have_user = found.has_value();
    if (!have_user) hash = decoy_hash();
    if (hash.empty()) return std::nullopt;

    const bool ok = crypto_pwhash_str_verify(hash.c_str(),
                                             password.data(),
                                             password.size()) == 0;
    if (!have_user || !ok) return std::nullopt;
    return found;
}

std::string UserStore::mint_token(const Principal& p) const {
    std::string secret;
    std::uint64_t epoch = 0;
    {
        std::lock_guard lock{mutex_};
        secret = token_secret_;
        if (const auto* r = find_locked(p.id)) epoch = r->user.token_epoch;
    }
    if (secret.empty() || epoch == 0) return {};

    std::vector<unsigned char> key;
    if (!b64_decode(secret, key) || key.size() != crypto_auth_KEYBYTES) return {};

    // Compact on purpose: this string is carried in a query parameter on the
    // WebSocket upgrade, and every byte here is a byte in a URL.
    const json payload{
        {"u", p.id},
        {"e", epoch},
        {"x", now_unix() + kTokenTtlSeconds},
    };
    const std::string body = payload.dump();
    const std::string body_b64 =
        b64_encode(reinterpret_cast<const unsigned char*>(body.data()), body.size());

    unsigned char mac[crypto_auth_BYTES];
    crypto_auth(mac, reinterpret_cast<const unsigned char*>(body_b64.data()),
                body_b64.size(), key.data());
    sodium_memzero(key.data(), key.size());

    // Signed over the ENCODED body, not the raw JSON, so verification never
    // has to re-serialise anything to check the signature — the bytes that
    // were signed are exactly the bytes on the wire.
    return "lp1." + body_b64 + "." + b64_encode(mac, sizeof(mac));
}

std::optional<UserStore::Principal> UserStore::verify_token(
        const std::string& token) const {
    if (token.size() < 8 || token.rfind("lp1.", 0) != 0) return std::nullopt;

    const auto first  = token.find('.');
    const auto second = token.find('.', first + 1);
    if (second == std::string::npos) return std::nullopt;
    const std::string body_b64 = token.substr(first + 1, second - first - 1);
    const std::string mac_b64  = token.substr(second + 1);
    if (body_b64.empty() || mac_b64.empty()) return std::nullopt;

    std::string secret;
    {
        std::lock_guard lock{mutex_};
        secret = token_secret_;
    }
    if (secret.empty()) return std::nullopt;

    std::vector<unsigned char> key, mac;
    if (!b64_decode(secret, key) || key.size() != crypto_auth_KEYBYTES) return std::nullopt;
    if (!b64_decode(mac_b64, mac) || mac.size() != crypto_auth_BYTES) {
        sodium_memzero(key.data(), key.size());
        return std::nullopt;
    }

    // Signature first, before the payload is parsed at all. Anything else
    // means feeding attacker-chosen bytes to the JSON parser on every
    // unauthenticated request. crypto_auth_verify is constant-time.
    const bool signature_ok =
        crypto_auth_verify(mac.data(),
                           reinterpret_cast<const unsigned char*>(body_b64.data()),
                           body_b64.size(), key.data()) == 0;
    sodium_memzero(key.data(), key.size());
    if (!signature_ok) return std::nullopt;

    std::vector<unsigned char> body;
    if (!b64_decode(body_b64, body)) return std::nullopt;
    json payload = json::parse(std::string(body.begin(), body.end()), nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) return std::nullopt;

    const std::string  id    = payload.value("u", std::string{});
    const std::uint64_t epoch = payload.value("e", std::uint64_t{0});
    const std::int64_t expiry = payload.value("x", std::int64_t{0});
    if (id.empty() || expiry <= now_unix()) return std::nullopt;

    std::lock_guard lock{mutex_};
    const Record* r = find_locked(id);
    // A deleted user's token stops working here, and a bumped epoch is what
    // makes a password change retroactive. Both are the reason the token is
    // not simply trusted once its signature checks out.
    if (!r || r->user.token_epoch != epoch) return std::nullopt;
    return Principal{r->user.id, r->user.name, r->user.role};
}

json UserStore::to_json() const {
    std::lock_guard lock{mutex_};
    return to_json_locked();
}

json UserStore::to_json_locked() const {
    json arr = json::array();
    for (const auto& r : users_) {
        arr.push_back(json{
            {"id",         r.user.id},
            {"name",       r.user.name},
            {"role",       to_string(r.user.role)},
            {"hash",       r.hash},
            {"tokenEpoch", r.user.token_epoch},
            {"createdAt",  r.user.created_at},
        });
    }
    json out{
        {"schema_version", kUserStoreSchemaVersion},
        {"tokenSecret",    token_secret_},
        {"users",          std::move(arr)},
    };
    // Written only when somebody has chosen. Sparse for the same reason U4's
    // profiles are: absent has its own meaning here ("never decided, keep taking
    // the default"), and a file that has never been touched by this feature must
    // round-trip byte-identically. No schema_version bump either — the key is
    // additive, and a build that predates it ignores an unknown field and keeps
    // authentication ON, which is the fail-safe direction to be wrong in.
    if (auth_required_override_) out["authRequired"] = *auth_required_override_;
    return out;
}

bool UserStore::from_json(const json& j) {
    if (!j.is_object()) return false;
    const auto users_it = j.find("users");
    if (users_it == j.end() || !users_it->is_array()) return false;

    std::vector<Record> parsed;
    for (const auto& e : *users_it) {
        if (!e.is_object()) return false;
        Record r;
        r.user.id   = e.value("id",   std::string{});
        r.user.name = e.value("name", std::string{});
        r.hash      = e.value("hash", std::string{});
        // Every field is load-bearing. A record missing any of them cannot be
        // repaired by guessing — an id we invent would not match the tokens
        // already issued against it, and a missing hash would be an account
        // with no password rather than one nobody can use.
        if (r.user.id.empty() || r.user.name.empty() || r.hash.empty()) return false;
        const auto role = role_from_string(e.value("role", std::string{"operator"}));
        if (!role) return false;
        r.user.role        = *role;
        r.user.token_epoch = e.value("tokenEpoch", std::uint64_t{1});
        r.user.created_at  = e.value("createdAt",  std::int64_t{0});
        parsed.push_back(std::move(r));
    }

    std::string secret = j.value("tokenSecret", std::string{});
    // A store with accounts and no usable secret cannot verify anything it
    // issues. Refusing is right: silently minting a new one would invalidate
    // every live session without saying so, and reading it as "open" would
    // unlock the server.
    if (!parsed.empty()) {
        std::vector<unsigned char> key;
        if (!b64_decode(secret, key) || key.size() != crypto_auth_KEYBYTES) return false;
        sodium_memzero(key.data(), key.size());
    }

    // Only a real boolean counts as a choice. Anything else — a string "false",
    // a number, null — is treated as "never chose", because the alternative is
    // letting a malformed value decide whether the door is locked, and the
    // truthiness of `"false"` is exactly the accident that would open it.
    std::optional<bool> auth_override;
    const auto auth_it = j.find("authRequired");
    if (auth_it != j.end() && auth_it->is_boolean()) auth_override = auth_it->get<bool>();

    std::lock_guard lock{mutex_};
    users_        = std::move(parsed);
    token_secret_ = std::move(secret);
    auth_required_override_ = auth_override;
    return true;
}

} // namespace liveplay::core
