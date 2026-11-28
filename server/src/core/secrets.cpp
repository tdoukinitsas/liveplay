// secrets.cpp — see secrets.hpp.
#include "liveplay/core/secrets.hpp"
#include "liveplay/core/secret_headers.hpp"

#include "liveplay/logger.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <sodium.h>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <aclapi.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "advapi32.lib")
#  endif
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace liveplay::core::secrets {
namespace {

namespace fs = std::filesystem;

using Key = std::array<unsigned char, crypto_secretbox_KEYBYTES>;
static_assert(crypto_kdf_KEYBYTES == crypto_secretbox_KEYBYTES, "one key file serves both");
static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES == crypto_kdf_KEYBYTES, "subkey size");

std::mutex         g_mutex;
fs::path           g_path;
std::optional<Key> g_key;
bool               g_warned_unreadable = false;

std::optional<Key> read_key(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    Key k{};
    if (in && in.read(reinterpret_cast<char*>(k.data()), static_cast<std::streamsize>(k.size())) &&
        in.gcount() == static_cast<std::streamsize>(k.size()))
        return k;
    return std::nullopt;
}

// The key file readable and writable by the server's own account only
// (R2-20). On Windows the file would otherwise inherit its folder's ACL, and
// the data folder can be the executable's own (a portable install that every
// local account can read).
void restrict_to_owner(const fs::path& path) {
#if defined(_WIN32)
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<unsigned char> buf(len);
    const bool got = len && GetTokenInformation(token, TokenUser, buf.data(), len, &len);
    CloseHandle(token);
    if (!got) return;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buf.data());
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(user->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS) return;
    std::wstring wide = path.wstring();
    const DWORD rc = SetNamedSecurityInfoW(
        wide.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    if (rc != ERROR_SUCCESS)
        Logger::warn("Secrets: could not restrict '{}' to this account (error {})",
                     util::path_to_utf8(path), rc);
#else
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

#if !defined(_WIN32)
// An existing key that other accounts can read is tightened, and said so.
void tighten_existing(const fs::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return;
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) == 0) return;
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0)
        Logger::warn("Secrets: '{}' was readable by other accounts; it is now this account's only",
                     util::path_to_utf8(path));
}
#endif

// Writes a new key to `tmp`, owner-only from the moment it exists.
bool write_new_key(const fs::path& tmp, const Key& k) {
    std::error_code ec;
    fs::remove(tmp, ec);   // a leftover from an attempt that died half-way
#if defined(_WIN32)
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out || !out.write(reinterpret_cast<const char*>(k.data()),
                               static_cast<std::streamsize>(k.size())))
            return false;
    }
    restrict_to_owner(tmp);
    return true;
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    const auto n = ::write(fd, k.data(), k.size());
    const bool ok = n == static_cast<ssize_t>(k.size()) && ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

// Under g_mutex. Reads the key, or makes one the first time it is needed.
//
// An existing key file is never replaced: every show sealed on this machine
// depends on it. One that exists but can't be read right now (a virus
// scanner's lock, a short read) is tried again on the next call, and until
// then nothing is sealed or opened with a different key. Only `may_wait`
// callers (ensure_key, outside every other lock) wait between attempts.
const Key* key_locked(bool create, bool may_wait) {
    if (g_key) return &*g_key;
    if (g_path.empty() || sodium_init() < 0) return nullptr;
    std::error_code ec;
    if (fs::exists(g_path, ec)) {
        const int attempts = may_wait ? 3 : 1;
        for (int attempt = 0; attempt < attempts && !g_key; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds{30});
            g_key = read_key(g_path);
        }
        if (g_key) {
#if !defined(_WIN32)
            tighten_existing(g_path);
#endif
            return &*g_key;
        }
        if (!g_warned_unreadable) {
            g_warned_unreadable = true;
            Logger::error("Secrets: the key file '{}' exists but can't be read; credentials can't "
                          "be sealed or opened until it can, and are left out of saved shows",
                          util::path_to_utf8(g_path));
        }
        return nullptr;
    }
    if (!create) return nullptr;
    Key k{};
    crypto_secretbox_keygen(k.data());
    if (g_path.has_parent_path()) fs::create_directories(g_path.parent_path(), ec);
    // Written beside it, then moved into place, so the key file is never
    // half-written; the move fails rather than replace one that appeared.
    fs::path tmp = g_path;
    tmp += ".new";
    if (!write_new_key(tmp, k)) {
        Logger::error("Secrets: could not write the key file '{}'; credentials are left out of "
                      "saved shows until it can be", util::path_to_utf8(g_path));
        fs::remove(tmp, ec);
        return nullptr;
    }
    if (fs::exists(g_path, ec)) {
        fs::remove(tmp, ec);
        g_key = read_key(g_path);
        return g_key ? &*g_key : nullptr;
    }
    fs::rename(tmp, g_path, ec);
    if (ec) {
        Logger::error("Secrets: could not put the key file in place: {}", ec.message());
        fs::remove(tmp, ec);
        return nullptr;
    }
    Logger::info("Secrets: made a new key for sealing credentials in show files");
    g_key = k;
    return &*g_key;
}

// The AEAD key for v2 seals, derived from the key file so one file serves
// both versions without using one key for two algorithms.
std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> v2_key(const Key& master) {
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> sub{};
    crypto_kdf_derive_from_key(sub.data(), sub.size(), 2, "lpsecret", master.data());
    return sub;
}

std::string b64(const unsigned char* data, std::size_t len) {
    std::string out(sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(out.data(), out.size(), data, len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::optional<std::vector<unsigned char>> unb64(std::string_view text) {
    std::vector<unsigned char> box(text.size());
    std::size_t len = 0;
    if (sodium_base642bin(box.data(), box.size(), text.data(), text.size(), nullptr, &len,
                          nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0)
        return std::nullopt;
    box.resize(len);
    return box;
}

}  // namespace

void set_key_path(fs::path path) {
    std::lock_guard lock{g_mutex};
    g_path = std::move(path);
    g_key.reset();
    g_warned_unreadable = false;
}

bool ensure_key(bool create) {
    std::lock_guard lock{g_mutex};
    return key_locked(create, /*may_wait=*/true) != nullptr;
}

std::optional<std::string> seal(std::string_view plain, std::string_view binding) {
    std::lock_guard lock{g_mutex};
    const auto* key = key_locked(/*create=*/true, /*may_wait=*/false);
    if (!key) return std::nullopt;
    const auto sub = v2_key(*key);
    constexpr std::size_t kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    std::vector<unsigned char> box(kNonce + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    randombytes_buf(box.data(), kNonce);
    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        box.data() + kNonce, &clen,
        reinterpret_cast<const unsigned char*>(plain.data()), plain.size(),
        reinterpret_cast<const unsigned char*>(binding.data()), binding.size(),
        nullptr, box.data(), sub.data());
    box.resize(kNonce + static_cast<std::size_t>(clen));
    return std::string{kSealedPrefixV2} + b64(box.data(), box.size());
}

std::optional<std::string> open(std::string_view sealed, std::string_view binding) {
    const bool v1 = sealed.substr(0, kSealedPrefix.size()) == kSealedPrefix;
    const bool v2 = sealed.substr(0, kSealedPrefixV2.size()) == kSealedPrefixV2;
    if (!v1 && !v2) return std::nullopt;
    std::lock_guard lock{g_mutex};
    const auto* key = key_locked(/*create=*/false, /*may_wait=*/false);
    if (!key) return std::nullopt;
    const auto box = unb64(sealed.substr(v1 ? kSealedPrefix.size() : kSealedPrefixV2.size()));
    if (!box) return std::nullopt;
    if (v1) {
        if (box->size() < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) return std::nullopt;
        std::string plain(box->size() - crypto_secretbox_NONCEBYTES - crypto_secretbox_MACBYTES, '\0');
        if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(plain.data()),
                                       box->data() + crypto_secretbox_NONCEBYTES,
                                       box->size() - crypto_secretbox_NONCEBYTES, box->data(),
                                       key->data()) != 0)
            return std::nullopt;
        return plain;
    }
    constexpr std::size_t kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (box->size() < kNonce + crypto_aead_xchacha20poly1305_ietf_ABYTES) return std::nullopt;
    const auto sub = v2_key(*key);
    std::string plain(box->size() - kNonce - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plain.data()), &mlen, nullptr,
            box->data() + kNonce, box->size() - kNonce,
            reinterpret_cast<const unsigned char*>(binding.data()), binding.size(),
            box->data(), sub.data()) != 0)
        return std::nullopt;
    plain.resize(static_cast<std::size_t>(mlen));
    return plain;
}

}  // namespace liveplay::core::secrets
