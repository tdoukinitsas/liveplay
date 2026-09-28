// user_store.cpp — see user_store.hpp.
#include "liveplay/core/user_store.hpp"

#include "liveplay/logger.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <sodium.h>

#include <algorithm>
#include <cctype>
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

// The one place a session-signing key is made. Shared by the first add_user
// and by an import into an empty store, which are the two ways a store can
// acquire its first account — and a store with accounts and no key cannot
// verify anything it issues (from_json refuses to load one).
std::string new_token_secret() {
    unsigned char key_buf[crypto_auth_KEYBYTES];
    randombytes_buf(key_buf, sizeof(key_buf));
    std::string out = b64_encode(key_buf, sizeof(key_buf));
    sodium_memzero(key_buf, sizeof(key_buf));
    return out;
}

// How stale a token's "last used" stamp is allowed to get on disk. The value
// is written at most this often per token, because the alternative is
// rewriting the file that holds every password hash on this machine each time
// a Companion button is pressed.
constexpr std::int64_t kApiTokenUseWriteSeconds = 3600;

// 32 bytes from the CSPRNG. Long enough that the hash below needs no salt and
// no work factor, short enough to paste into a Companion field by hand.
constexpr std::size_t kApiTokenSecretBytes = 32;

std::string blake2b_b64(const unsigned char* data, std::size_t len) {
    unsigned char out[crypto_generichash_BYTES];
    crypto_generichash(out, sizeof(out), data, len, nullptr, 0);
    return b64_encode(out, sizeof(out));
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
        case Result::NoSuchToken:  return "no such API token";
        case Result::AuthOff:      return "turn the login on before issuing an API token — "
                                          "while it is off, anyone on the network could issue "
                                          "one and keep it";
        case Result::BadAvatar:    return "the picture must be a PNG, JPEG or WebP image";
        case Result::AvatarTooLarge: return "the picture is too large";
        case Result::BadImport:    return "that is not a LivePlay account file this server can read";
        case Result::NoAdmin:      return "the result would have no administrator — nobody could "
                                          "manage the accounts afterwards";
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
    return auth_required_locked();
}

bool UserStore::auth_required_locked() const {
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

const UserStore::TokenRecord* UserStore::find_token_locked(const std::string& id) const {
    for (const auto& t : tokens_) if (t.token.id == id) return &t;
    return nullptr;
}

UserStore::TokenRecord* UserStore::find_token_locked(const std::string& id) {
    for (auto& t : tokens_) if (t.token.id == id) return &t;
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

    if (token_secret_.empty()) token_secret_ = new_token_secret();

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

UserStore::Result UserStore::validate_avatar(const std::string& avatar) {
    if (avatar.empty()) return Result::Ok;   // "no picture" is always valid

    // An allow-list of three raster types, spelled exactly as a canvas writes
    // them. Not SVG, ever: an SVG is a document that can carry script, and this
    // string is rendered on every surface that lists accounts.
    static constexpr std::string_view kPrefixes[] = {
        "data:image/png;base64,", "data:image/jpeg;base64,", "data:image/webp;base64,",
    };
    std::string_view kind;
    for (const auto p : kPrefixes)
        if (avatar.rfind(p, 0) == 0) { kind = p; break; }
    if (kind.empty()) return Result::BadAvatar;

    // Refuse on the ENCODED length before decoding anything, so an oversized
    // upload costs a comparison rather than an allocation of its own size.
    const std::string_view b64 = std::string_view{avatar}.substr(kind.size());
    if (b64.size() > (kMaxAvatarBytes + 2) / 3 * 4) return Result::AvatarTooLarge;
    if (b64.empty()) return Result::BadAvatar;

    std::vector<unsigned char> raw(b64.size());
    std::size_t written = 0;
    if (sodium_base642bin(raw.data(), raw.size(), b64.data(), b64.size(),
                          nullptr, &written, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return Result::BadAvatar;
    }
    raw.resize(written);
    if (raw.size() > kMaxAvatarBytes) return Result::AvatarTooLarge;

    // The bytes have to be what the label says. Cheap, and it is what stops a
    // data URL that claims to be a PNG from being anything else at all.
    const auto starts = [&](std::initializer_list<unsigned char> sig, std::size_t at = 0) {
        if (raw.size() < at + sig.size()) return false;
        return std::equal(sig.begin(), sig.end(), raw.begin() + static_cast<std::ptrdiff_t>(at));
    };
    bool magic = false;
    if (kind == kPrefixes[0]) magic = starts({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
    if (kind == kPrefixes[1]) magic = starts({0xFF, 0xD8, 0xFF});
    if (kind == kPrefixes[2]) magic = starts({'R', 'I', 'F', 'F'}) && starts({'W', 'E', 'B', 'P'}, 8);
    return magic ? Result::Ok : Result::BadAvatar;
}

UserStore::Result UserStore::set_avatar(const std::string& id, const std::string& avatar) {
    if (const auto v = validate_avatar(avatar); v != Result::Ok) return v;

    std::lock_guard lock{mutex_};
    Record* r = find_locked(id);
    if (!r) return Result::NoSuchUser;
    if (r->user.avatar == avatar) return Result::Ok;

    std::string previous = std::move(r->user.avatar);
    r->user.avatar = avatar;
    if (!save_locked()) { r->user.avatar = std::move(previous); return Result::IoError; }
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
    // Which kind of credential this is, decided by its prefix and nowhere else.
    // The two verifications share no code and must not: one checks a signature
    // over a payload we minted, the other a hash of a secret we never kept.
    if (token.rfind(kApiTokenPrefix, 0) == 0) return verify_api_token(token);

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

// ---------------------------------------------------------------------------
// API tokens — principals that are not people
// ---------------------------------------------------------------------------

std::vector<UserStore::ApiToken> UserStore::api_tokens() const {
    std::lock_guard lock{mutex_};
    std::vector<ApiToken> out;
    out.reserve(tokens_.size());
    for (const auto& t : tokens_) out.push_back(t.token);
    std::sort(out.begin(), out.end(), [](const ApiToken& a, const ApiToken& b) {
        return lower_ascii(a.name) < lower_ascii(b.name);
    });
    return out;
}

UserStore::Result UserStore::create_api_token(const std::string& name_in,
                                              const std::string& created_by,
                                              ApiToken* out,
                                              std::string* out_secret) {
    const std::string name = trim(name_in);
    if (!name_is_valid(name)) return Result::BadName;
    if (!ensure_sodium())     return Result::HashFailed;

    std::lock_guard lock{mutex_};
    // See the header: issuing a credential through an open door would survive
    // the door being shut, which is the one way a token can be an escalation
    // rather than a convenience.
    if (!auth_required_locked()) return Result::AuthOff;

    // Names are unique so that revoking one from a list is unambiguous. Two
    // tokens called "Companion" is exactly the state in which somebody revokes
    // the wrong one during a show.
    const std::string key = lower_ascii(name);
    for (const auto& t : tokens_)
        if (lower_ascii(t.token.name) == key) return Result::NameTaken;

    unsigned char secret[kApiTokenSecretBytes];
    randombytes_buf(secret, sizeof(secret));

    TokenRecord rec;
    rec.token.id         = random_hex(16);
    rec.token.name       = name;
    rec.token.created_by = created_by;
    rec.token.created_at = now_unix();
    rec.hash             = blake2b_b64(secret, sizeof(secret));

    // The id travels in the clear inside the token. That is what makes
    // verification a lookup plus one hash rather than a scan over every record,
    // and it gives away nothing: the id is already in every listing.
    const std::string presented = std::string{kApiTokenPrefix} + rec.token.id + "_" +
                                  b64_encode(secret, sizeof(secret));
    sodium_memzero(secret, sizeof(secret));

    tokens_.push_back(rec);
    if (!save_locked()) { tokens_.pop_back(); return Result::IoError; }

    if (out)        *out = rec.token;
    if (out_secret) *out_secret = presented;
    // warn, not info: a long-lived credential now exists on this machine, and
    // that belongs in a log an administrator reads rather than one they enable.
    Logger::warn("UserStore: API token '{}' issued ({} in total)", name, tokens_.size());
    return Result::Ok;
}

UserStore::Result UserStore::rename_api_token(const std::string& id,
                                              const std::string& name_in) {
    const std::string name = trim(name_in);
    if (!name_is_valid(name)) return Result::BadName;

    std::lock_guard lock{mutex_};
    TokenRecord* r = find_token_locked(id);
    if (!r) return Result::NoSuchToken;

    const std::string key = lower_ascii(name);
    for (const auto& t : tokens_)
        if (t.token.id != id && lower_ascii(t.token.name) == key) return Result::NameTaken;

    const std::string previous = r->token.name;
    r->token.name = name;
    if (!save_locked()) { r->token.name = previous; return Result::IoError; }
    Logger::info("UserStore: API token '{}' renamed to '{}'", previous, name);
    return Result::Ok;
}

UserStore::Result UserStore::revoke_api_token(const std::string& id) {
    std::lock_guard lock{mutex_};
    const auto it = std::find_if(tokens_.begin(), tokens_.end(),
                                 [&](const TokenRecord& t) { return t.token.id == id; });
    if (it == tokens_.end()) return Result::NoSuchToken;

    const TokenRecord removed = *it;
    tokens_.erase(it);
    if (!save_locked()) { tokens_.push_back(removed); return Result::IoError; }
    // Erasing the record IS the revocation, and it is immediate: verification
    // looks the token up every time, so there is no epoch to bump and no window
    // in which a revoked token still answers. Unlike a user's session tokens,
    // there is also nothing left behind to revoke later.
    Logger::warn("UserStore: API token '{}' revoked", removed.token.name);
    return Result::Ok;
}

std::optional<UserStore::Principal> UserStore::verify_api_token(
        const std::string& token) const {
    // lpk1_<id>_<secret>
    const auto sep = token.find('_', kApiTokenPrefix.size());
    if (sep == std::string::npos) return std::nullopt;
    const std::string id     = token.substr(kApiTokenPrefix.size(),
                                            sep - kApiTokenPrefix.size());
    const std::string secret = token.substr(sep + 1);
    if (id.empty() || secret.empty()) return std::nullopt;

    std::vector<unsigned char> raw;
    if (!b64_decode(secret, raw) || raw.size() != kApiTokenSecretBytes) return std::nullopt;
    const std::string presented = blake2b_b64(raw.data(), raw.size());
    sodium_memzero(raw.data(), raw.size());

    std::lock_guard lock{mutex_};
    const TokenRecord* r = find_token_locked(id);
    if (!r) return std::nullopt;
    // Constant time. Both sides are a base64 BLAKE2b digest, so a length
    // mismatch means the stored value is not one and there is nothing to
    // compare — sodium_memcmp requires equal lengths to be meaningful.
    if (r->hash.size() != presented.size()) return std::nullopt;
    if (sodium_memcmp(r->hash.data(), presented.data(), presented.size()) != 0)
        return std::nullopt;

    // Operator tier, always. A token has no role of its own to raise.
    return Principal{r->token.id, r->token.name, UserRole::Operator, Kind::ApiToken};
}

void UserStore::note_api_token_use(const std::string& id) {
    std::lock_guard lock{mutex_};
    TokenRecord* r = find_token_locked(id);
    if (!r) return;
    const std::int64_t now = now_unix();
    r->token.last_used_at = now;
    // In memory every time, on disk at most hourly. A timestamp that lags by an
    // hour still answers the question it exists for — "is anything still using
    // this?" — and the write it avoids is to the file holding every password
    // hash on the machine.
    if (now - r->saved_use < kApiTokenUseWriteSeconds) return;
    r->saved_use = now;
    // Best effort: failing a request because a timestamp could not be written
    // would turn a full disk into an outage of the thing automation depends on.
    save_locked();
}

// ---------------------------------------------------------------------------
// Export / import — moving the team to another rig
// ---------------------------------------------------------------------------

namespace {

// Ids are ours (random hex), but an import is somebody else's file. Anything
// outside plain alphanumerics is refused rather than escaped: a user id travels
// inside every session token and a token id is spelled between the two
// underscores of an API token, so an '_' there would split it in the wrong place.
bool import_id_is_valid(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(),
                       [](unsigned char c) { return std::isalnum(c) != 0; });
}

// libsodium's own reader is the judge of an Argon2id string, not a regex:
// needs_rehash parses every parameter and answers -1 for anything it cannot.
// The prefix check keeps out Argon2i (which it would also accept) — this store
// has only ever written Argon2id, so an Argon2i hash is a sign of a file that
// did not come from here.
bool hash_is_argon2id(const std::string& h) {
    if (h.size() >= crypto_pwhash_STRBYTES) return false;
    if (h.rfind("$argon2id$", 0) != 0) return false;
    for (const unsigned char c : h)
        if (c < 0x21 || c > 0x7E) return false;
    return crypto_pwhash_str_needs_rehash(h.c_str(),
                                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != -1;
}

// Type-checked reads. nlohmann's value() throws on a type mismatch, and an
// import must say WHICH record was wrong rather than surface a library message.
bool read_string(const json& o, const char* key, std::string& out) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}
// Absent is fine (the default stands); present and not a whole number >= min
// is not.
bool read_int(const json& o, const char* key, std::int64_t min, std::int64_t& out) {
    const auto it = o.find(key);
    if (it == o.end() || it->is_null()) return true;
    if (!it->is_number_integer()) return false;
    const auto v = it->get<std::int64_t>();
    if (v < min) return false;
    out = v;
    return true;
}

} // namespace

json UserStore::export_json() const {
    std::lock_guard lock{mutex_};
    json users = json::array();
    for (const auto& r : users_) {
        users.push_back(json{
            {"id",         r.user.id},
            {"name",       r.user.name},
            {"role",       to_string(r.user.role)},
            {"hash",       r.hash},
            {"tokenEpoch", r.user.token_epoch},
            {"createdAt",  r.user.created_at},
            {"avatar",     r.user.avatar.empty() ? json(nullptr) : json(r.user.avatar)},
        });
    }
    json tokens = json::array();
    for (const auto& t : tokens_) {
        tokens.push_back(json{
            {"id",         t.token.id},
            {"name",       t.token.name},
            {"hash",       t.hash},
            {"createdBy",  t.token.created_by},
            {"createdAt",  t.token.created_at},
            {"lastUsedAt", t.token.last_used_at},
        });
    }
    // No tokenSecret and no authRequired — see the header for why each one
    // belongs to this machine rather than to the people on it.
    return json{
        {"format",     std::string{kUserExportFormat}},
        {"version",    kUserExportVersion},
        {"exportedAt", now_unix()},
        {"users",      std::move(users)},
        {"apiTokens",  std::move(tokens)},
    };
}

UserStore::Result UserStore::import_json(const json& file, ImportMode mode,
                                         ImportReport* report, std::string* detail) {
    const auto fail = [&](std::string why) {
        if (detail) *detail = std::move(why);
        return Result::BadImport;
    };
    if (!ensure_sodium()) return Result::HashFailed;
    if (!file.is_object()) return fail("the file is not a JSON object");

    std::string format;
    if (!read_string(file, "format", format) || format != kUserExportFormat)
        return fail("this is not a LivePlay account file");
    const auto ver = file.find("version");
    if (ver == file.end() || !ver->is_number_integer() ||
        ver->get<std::int64_t>() != kUserExportVersion)
        return fail("this account file was made by a version of LivePlay this server cannot read");

    // ---- Validate every record before touching anything ----------------
    const auto users_it = file.find("users");
    if (users_it == file.end() || !users_it->is_array())
        return fail("the file has no account list");

    std::vector<Record> in_users;
    std::vector<std::string> seen_ids, seen_names;
    const auto seen = [](const std::vector<std::string>& v, const std::string& k) {
        return std::find(v.begin(), v.end(), k) != v.end();
    };
    std::size_t index = 0;
    for (const auto& e : *users_it) {
        ++index;
        std::string where = "account " + std::to_string(index);
        if (!e.is_object()) return fail(where + " is not an object");
        Record r;
        if (!read_string(e, "id", r.user.id) || !import_id_is_valid(r.user.id))
            return fail(where + " has no valid id");
        std::string name;
        if (!read_string(e, "name", name)) return fail(where + " has no name");
        r.user.name = trim(name);
        if (!name_is_valid(r.user.name))
            return fail(where + ": the name is empty, too long or unprintable");
        where = "account '" + r.user.name + "'";

        std::string role_s;
        const auto role = read_string(e, "role", role_s) ? role_from_string(role_s)
                                                          : std::nullopt;
        if (!role) return fail(where + " has no valid role");
        r.user.role = *role;

        if (!read_string(e, "hash", r.hash) || !hash_is_argon2id(r.hash))
            return fail(where + " does not have a valid password hash");

        std::int64_t epoch = 1, created = 0;
        if (!read_int(e, "tokenEpoch", 1, epoch)) return fail(where + " has a bad tokenEpoch");
        if (!read_int(e, "createdAt",  0, created)) return fail(where + " has a bad createdAt");
        r.user.token_epoch = static_cast<std::uint64_t>(epoch);
        r.user.created_at  = created;

        // Unlike load(), a bad picture here IS a refusal: this is a file somebody
        // chose to import, it is better to say it is damaged than to take most
        // of it and quietly drop a face.
        if (const auto a = e.find("avatar"); a != e.end() && !a->is_null()) {
            if (!a->is_string()) return fail(where + " has a picture that is not an image");
            r.user.avatar = a->get<std::string>();
            if (validate_avatar(r.user.avatar) != Result::Ok)
                return fail(where + " has a picture that is not a small PNG, JPEG or WebP image");
        }

        if (seen(seen_ids, r.user.id)) return fail(where + " repeats an id already in the file");
        const std::string key = lower_ascii(r.user.name);
        if (seen(seen_names, key)) return fail(where + " appears in the file twice");
        seen_ids.push_back(r.user.id);
        seen_names.push_back(key);
        in_users.push_back(std::move(r));
    }

    std::vector<TokenRecord> in_tokens;
    seen_ids.clear();
    seen_names.clear();
    if (const auto tokens_it = file.find("apiTokens");
        tokens_it != file.end() && !tokens_it->is_null()) {
        if (!tokens_it->is_array()) return fail("the API token list is not a list");
        index = 0;
        for (const auto& e : *tokens_it) {
            ++index;
            std::string where = "API token " + std::to_string(index);
            if (!e.is_object()) return fail(where + " is not an object");
            TokenRecord t;
            if (!read_string(e, "id", t.token.id) || !import_id_is_valid(t.token.id))
                return fail(where + " has no valid id");
            std::string name;
            if (!read_string(e, "name", name)) return fail(where + " has no name");
            t.token.name = trim(name);
            if (!name_is_valid(t.token.name))
                return fail(where + ": the name is empty, too long or unprintable");
            where = "API token '" + t.token.name + "'";

            // A base64 BLAKE2b digest, which is all this store ever keeps of a
            // token. Anything else is a record no presented token could match.
            std::vector<unsigned char> digest;
            if (!read_string(e, "hash", t.hash) || !b64_decode(t.hash, digest) ||
                digest.size() != crypto_generichash_BYTES)
                return fail(where + " does not have a valid hash");

            const auto by = e.find("createdBy");
            if (by != e.end() && !by->is_null()) {
                if (!by->is_string()) return fail(where + " has a bad createdBy");
                t.token.created_by = by->get<std::string>();
            }
            std::int64_t created = 0, used = 0;
            if (!read_int(e, "createdAt",  0, created)) return fail(where + " has a bad createdAt");
            if (!read_int(e, "lastUsedAt", 0, used))    return fail(where + " has a bad lastUsedAt");
            t.token.created_at   = created;
            t.token.last_used_at = used;
            t.saved_use          = used;

            if (seen(seen_ids, t.token.id)) return fail(where + " repeats an id already in the file");
            const std::string key = lower_ascii(t.token.name);
            if (seen(seen_names, key)) return fail(where + " appears in the file twice");
            seen_ids.push_back(t.token.id);
            seen_names.push_back(key);
            in_tokens.push_back(std::move(t));
        }
    }

    // ---- Work out the result, still without touching anything ----------
    std::lock_guard lock{mutex_};
    ImportReport rep;
    std::vector<Record>      users_next;
    std::vector<TokenRecord> tokens_next;

    if (mode == ImportMode::Replace) {
        // An empty file would leave an empty store, and an empty store is an
        // OPEN one. Restoring a backup must never quietly unlock the building;
        // turning the login off is its own act, with its own password prompt.
        if (in_users.empty())
            return fail("the file has no accounts — replacing with it would switch the login off");

        for (auto& r : in_users) {
            // Same person, same machine (restoring a backup here): carry the
            // epoch FORWARD, never back, or a session revoked since the backup
            // was taken would come back to life. And if the password is not the
            // one they have now, bump it — the set_password rule, for the same
            // reason: a different password means the sessions opened with the
            // current one should end.
            if (const Record* cur = find_locked(r.user.id)) {
                r.user.token_epoch = std::max(cur->user.token_epoch, r.user.token_epoch) +
                                     (cur->hash != r.hash ? 1 : 0);
            }
            rep.users_added.push_back(r.user.name);
        }
        for (const auto& cur : users_)
            if (std::none_of(in_users.begin(), in_users.end(),
                             [&](const Record& r) { return r.user.id == cur.user.id; }))
                rep.users_removed.push_back(cur.user.id);
        for (const auto& t : in_tokens) rep.tokens_added.push_back(t.token.name);
        users_next  = std::move(in_users);
        tokens_next = std::move(in_tokens);
    } else {
        users_next  = users_;
        tokens_next = tokens_;
        const auto name_taken = [&](const std::string& n) {
            const std::string key = lower_ascii(n);
            return std::any_of(users_next.begin(), users_next.end(),
                               [&](const Record& x) { return lower_ascii(x.user.name) == key; });
        };
        const auto id_taken = [&](const std::string& id) {
            return std::any_of(users_next.begin(), users_next.end(),
                               [&](const Record& x) { return x.user.id == id; });
        };
        // old id → new id, for the few users whose id collided here. Tokens'
        // createdBy points at a user id, so it follows the rename.
        std::vector<std::pair<std::string, std::string>> renamed;
        for (auto& r : in_users) {
            if (name_taken(r.user.name)) { rep.users_skipped.push_back(r.user.name); continue; }
            if (id_taken(r.user.id)) {
                std::string fresh;
                do { fresh = random_hex(16); } while (id_taken(fresh));
                renamed.emplace_back(r.user.id, fresh);
                r.user.id = fresh;
                ++rep.ids_regenerated;
            }
            rep.users_added.push_back(r.user.name);
            users_next.push_back(std::move(r));
        }
        for (auto& t : in_tokens) {
            const std::string key = lower_ascii(t.token.name);
            const bool clash = std::any_of(tokens_next.begin(), tokens_next.end(),
                [&](const TokenRecord& x) {
                    return x.token.id == t.token.id || lower_ascii(x.token.name) == key;
                });
            // Skipped, NOT given a new id: the id is spelled inside the token
            // string the Companion install holds, so a renumbered record would
            // be a token nothing can ever present.
            if (clash) { rep.tokens_skipped.push_back(t.token.name); continue; }
            for (const auto& [from, to] : renamed)
                if (t.token.created_by == from) t.token.created_by = to;
            rep.tokens_added.push_back(t.token.name);
            tokens_next.push_back(std::move(t));
        }
    }

    // The LastAdmin rule, for a whole file at once.
    if (!users_next.empty() &&
        std::none_of(users_next.begin(), users_next.end(),
                     [](const Record& r) { return r.user.role == UserRole::Admin; }))
        return Result::NoAdmin;

    // The AuthOff rule, for the same reason create_api_token has it: a token
    // added while the door is open survives the door being shut. An import
    // that ENDS with the login on — the ordinary case, including a fresh
    // machine taking its first accounts — is not that.
    const bool auth_after = !users_next.empty() && auth_required_override_.value_or(true);
    if (!auth_after && !rep.tokens_added.empty()) return Result::AuthOff;

    // ---- Commit, or put everything back ------------------------------------
    std::vector<Record>      users_prev  = std::move(users_);
    std::vector<TokenRecord> tokens_prev = std::move(tokens_);
    const std::string        secret_prev = token_secret_;
    users_  = std::move(users_next);
    tokens_ = std::move(tokens_next);
    // The first accounts on this machine need a signing key of their own —
    // the exporting machine's was deliberately left behind.
    if (!users_.empty() && token_secret_.empty()) token_secret_ = new_token_secret();
    if (!save_locked()) {
        users_        = std::move(users_prev);
        tokens_       = std::move(tokens_prev);
        token_secret_ = secret_prev;
        return Result::IoError;
    }

    Logger::warn("UserStore: {} import — {} account(s) added, {} skipped, {} removed; "
                 "{} API token(s) added, {} skipped",
                 mode == ImportMode::Replace ? "replacing" : "merging",
                 rep.users_added.size(), rep.users_skipped.size(), rep.users_removed.size(),
                 rep.tokens_added.size(), rep.tokens_skipped.size());
    if (report) *report = std::move(rep);
    return Result::Ok;
}

json UserStore::to_json() const {
    std::lock_guard lock{mutex_};
    return to_json_locked();
}

json UserStore::to_json_locked() const {
    json arr = json::array();
    for (const auto& r : users_) {
        json u{
            {"id",         r.user.id},
            {"name",       r.user.name},
            {"role",       to_string(r.user.role)},
            {"hash",       r.hash},
            {"tokenEpoch", r.user.token_epoch},
            {"createdAt",  r.user.created_at},
        };
        // Sparse, like authRequired below: a store nobody has given a picture
        // round-trips byte-identically, and a build that predates the key
        // simply ignores it.
        if (!r.user.avatar.empty()) u["avatar"] = r.user.avatar;
        arr.push_back(std::move(u));
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

    // Same rule, same reasoning: absent means no tokens have ever been issued,
    // and a build that predates this key ignores it — which stops honouring
    // those tokens rather than honouring something it does not understand.
    // Refusing a credential is the safe direction to be wrong in.
    if (!tokens_.empty()) {
        json tarr = json::array();
        for (const auto& t : tokens_) {
            tarr.push_back(json{
                {"id",         t.token.id},
                {"name",       t.token.name},
                {"hash",       t.hash},
                {"createdBy",  t.token.created_by},
                {"createdAt",  t.token.created_at},
                {"lastUsedAt", t.token.last_used_at},
            });
        }
        out["apiTokens"] = std::move(tarr);
    }
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
        // A picture that does not validate is DROPPED rather than failing the
        // load. Everything else in this record decides who gets in, which is
        // why a malformed field there refuses the whole file; a bad avatar
        // decides nothing, and refusing to serve over one would turn a
        // cosmetic fault into a locked-out rig.
        if (const auto a = e.find("avatar"); a != e.end() && a->is_string()) {
            std::string avatar = a->get<std::string>();
            if (validate_avatar(avatar) == Result::Ok) r.user.avatar = std::move(avatar);
            else Logger::warn("UserStore: ignoring an unreadable picture for '{}'", r.user.name);
        }
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

    // API tokens. Absent is the ordinary state; present and malformed is a
    // refusal, because this whole function's failure means "corrupt store, do
    // not serve" — and a token array we could not read is a set of credentials
    // whose membership we do not know. Dropping the ones we could not parse
    // would silently revoke them; guessing would be worse.
    std::vector<TokenRecord> parsed_tokens;
    const auto tokens_it = j.find("apiTokens");
    if (tokens_it != j.end()) {
        if (!tokens_it->is_array()) return false;
        for (const auto& e : *tokens_it) {
            if (!e.is_object()) return false;
            TokenRecord t;
            t.token.id   = e.value("id",   std::string{});
            t.token.name = e.value("name", std::string{});
            t.hash       = e.value("hash", std::string{});
            // As with a user record: every one of these is load-bearing, and a
            // token with no hash is not a token with no secret — it is a record
            // nothing can ever match, which would read as a working credential
            // in every listing.
            if (t.token.id.empty() || t.token.name.empty() || t.hash.empty()) return false;
            t.token.created_by   = e.value("createdBy",  std::string{});
            t.token.created_at   = e.value("createdAt",  std::int64_t{0});
            t.token.last_used_at = e.value("lastUsedAt", std::int64_t{0});
            t.saved_use          = t.token.last_used_at;
            parsed_tokens.push_back(std::move(t));
        }
    }

    std::lock_guard lock{mutex_};
    users_        = std::move(parsed);
    tokens_       = std::move(parsed_tokens);
    token_secret_ = std::move(secret);
    auth_required_override_ = auth_override;
    return true;
}

} // namespace liveplay::core
