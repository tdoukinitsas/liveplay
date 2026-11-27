// secrets.cpp — see secrets.hpp.
#include "liveplay/core/secrets.hpp"
#include "liveplay/core/secret_headers.hpp"

#include "liveplay/logger.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <sodium.h>

#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#if !defined(_WIN32)
#  include <sys/stat.h>
#endif

namespace liveplay::core::secrets {
namespace {

namespace fs = std::filesystem;

std::mutex             g_mutex;
fs::path               g_path;
bool                   g_loaded = false;
std::optional<std::array<unsigned char, crypto_secretbox_KEYBYTES>> g_key;

// Under g_mutex. Reads the key, or makes one the first time it is needed.
const std::array<unsigned char, crypto_secretbox_KEYBYTES>* key_locked(bool create) {
    if (g_key) return &*g_key;
    if (g_path.empty() || sodium_init() < 0) return nullptr;
    if (!g_loaded) {
        g_loaded = true;
        std::ifstream in{g_path, std::ios::binary};
        std::array<unsigned char, crypto_secretbox_KEYBYTES> k{};
        if (in && in.read(reinterpret_cast<char*>(k.data()), static_cast<std::streamsize>(k.size())) &&
            in.gcount() == static_cast<std::streamsize>(k.size())) {
            g_key = k;
            return &*g_key;
        }
    }
    if (!create) return nullptr;
    std::array<unsigned char, crypto_secretbox_KEYBYTES> k{};
    crypto_secretbox_keygen(k.data());
    std::error_code ec;
    if (g_path.has_parent_path()) fs::create_directories(g_path.parent_path(), ec);
    {
        std::ofstream out{g_path, std::ios::binary | std::ios::trunc};
        if (!out || !out.write(reinterpret_cast<const char*>(k.data()),
                               static_cast<std::streamsize>(k.size()))) {
            Logger::error("Secrets: could not write the key file '{}'; credentials will be "
                          "saved unsealed", util::path_to_utf8(g_path));
            return nullptr;
        }
    }
#if !defined(_WIN32)
    ::chmod(g_path.c_str(), S_IRUSR | S_IWUSR);   // the server's own user only
#endif
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
    g_path   = std::move(path);
    g_loaded = false;
    g_key.reset();
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
