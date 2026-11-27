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

#if !defined(_WIN32)
#  include <sys/stat.h>
#endif

namespace liveplay::core::secrets {
namespace {

namespace fs = std::filesystem;

using Key = std::array<unsigned char, crypto_secretbox_KEYBYTES>;

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

// Under g_mutex. Reads the key, or makes one the first time it is needed.
//
// An existing key file is never replaced: every show sealed on this machine
// depends on it. One that exists but can't be read right now (a virus
// scanner's lock, a short read) is tried again on the next call, and until
// then nothing is sealed or opened with a different key.
const Key* key_locked(bool create) {
    if (g_key) return &*g_key;
    if (g_path.empty() || sodium_init() < 0) return nullptr;
    std::error_code ec;
    if (fs::exists(g_path, ec)) {
        for (int attempt = 0; attempt < 3 && !g_key; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds{30});
            g_key = read_key(g_path);
        }
        if (g_key) return &*g_key;
        if (!g_warned_unreadable) {
            g_warned_unreadable = true;
            Logger::error("Secrets: the key file '{}' exists but can't be read; credentials can't "
                          "be sealed or opened until it can", util::path_to_utf8(g_path));
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
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out || !out.write(reinterpret_cast<const char*>(k.data()),
                               static_cast<std::streamsize>(k.size()))) {
            Logger::error("Secrets: could not write the key file '{}'; credentials will be "
                          "saved unsealed", util::path_to_utf8(g_path));
            fs::remove(tmp, ec);
            return nullptr;
        }
    }
#if !defined(_WIN32)
    ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);   // the server's own user only
#endif
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

std::string b64(const unsigned char* data, std::size_t len) {
    std::string out(sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(out.data(), out.size(), data, len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::strlen(out.c_str()));
    return out;
}

}  // namespace

void set_key_path(fs::path path) {
    std::lock_guard lock{g_mutex};
    g_path = std::move(path);
    g_key.reset();
    g_warned_unreadable = false;
}

std::optional<std::string> seal(std::string_view plain) {
    std::lock_guard lock{g_mutex};
    const auto* key = key_locked(/*create=*/true);
    if (!key) return std::nullopt;
    std::vector<unsigned char> box(crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + plain.size());
    unsigned char* nonce = box.data();
    randombytes_buf(nonce, crypto_secretbox_NONCEBYTES);
    crypto_secretbox_easy(box.data() + crypto_secretbox_NONCEBYTES,
                          reinterpret_cast<const unsigned char*>(plain.data()), plain.size(),
                          nonce, key->data());
    return std::string{kSealedPrefix} + b64(box.data(), box.size());
}

std::optional<std::string> open(std::string_view sealed) {
    if (!is_sealed(sealed)) return std::nullopt;
    std::lock_guard lock{g_mutex};
    const auto* key = key_locked(/*create=*/false);
    if (!key) return std::nullopt;
    const std::string_view text = sealed.substr(kSealedPrefix.size());
    std::vector<unsigned char> box(text.size());
    std::size_t len = 0;
    if (sodium_base642bin(box.data(), box.size(), text.data(), text.size(), nullptr, &len,
                          nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0)
        return std::nullopt;
    if (len < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) return std::nullopt;
    std::string plain(len - crypto_secretbox_NONCEBYTES - crypto_secretbox_MACBYTES, '\0');
    if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(plain.data()),
                                   box.data() + crypto_secretbox_NONCEBYTES,
                                   len - crypto_secretbox_NONCEBYTES, box.data(), key->data()) != 0)
        return std::nullopt;
    return plain;
}

}  // namespace liveplay::core::secrets
