// ============================================================================
// secrets_test.cpp - standalone assertions for sealing credentials on disk.
// ----------------------------------------------------------------------------
// No test framework: each check prints PASS/FAIL and the binary exits non-zero
// if anything failed. Pinned here (R2-02, R2-03, R2-20):
//   * a sealed value opens only for the binding it was sealed with (its
//     connection, that connection's address, its header name), so a value
//     copied anywhere else is useless;
//   * a value sealed by 2.6 RC2 (lpenc:v1, unbound) still opens;
//   * with no usable key nothing is sealed, so nothing can be written in
//     plain text by mistake;
//   * on POSIX the key file is readable by this account only.
// ============================================================================
#include "liveplay/core/secrets.hpp"
#include "liveplay/core/secret_headers.hpp"

#include <sodium.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace liveplay::core::secrets;

static int failures = 0;
static void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "   ", detail.c_str());
    if (!ok) ++failures;
}

// A v1 value as 2.6 RC2 wrote it: secretbox under the key file, no binding.
static std::string seal_v1(const fs::path& key_file, const std::string& plain) {
    unsigned char key[crypto_secretbox_KEYBYTES];
    std::ifstream in{key_file, std::ios::binary};
    in.read(reinterpret_cast<char*>(key), sizeof key);
    std::vector<unsigned char> box(crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + plain.size());
    randombytes_buf(box.data(), crypto_secretbox_NONCEBYTES);
    crypto_secretbox_easy(box.data() + crypto_secretbox_NONCEBYTES,
                          reinterpret_cast<const unsigned char*>(plain.data()), plain.size(),
                          box.data(), key);
    std::string b64(sodium_base64_ENCODED_LEN(box.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(b64.data(), b64.size(), box.data(), box.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    b64.resize(std::strlen(b64.c_str()));
    return std::string{kSealedPrefix} + b64;
}

int main() {
    if (sodium_init() < 0) { std::printf("FAIL  libsodium init\n"); return 1; }
    std::mt19937_64 rng{std::random_device{}()};
    const fs::path dir = fs::temp_directory_path() / ("liveplay-secrets-test-" + std::to_string(rng()));
    fs::create_directories(dir);
    const fs::path key_file = dir / "secret.key";

    // ---- A working key --------------------------------------------------------
    set_key_path(key_file);
    check("no key yet: ensure_key without create has none", !ensure_key(/*create=*/false));
    check("...and makes no file", !fs::exists(key_file));
    const std::string here  = seal_binding("c1", "http://desk:8000", "Authorization");
    const std::string there = seal_binding("c1", "http://evil:8000", "Authorization");
    const auto sealed = seal("Bearer s3cret", here);
    check("a value is sealed (making the key)", sealed.has_value() && fs::exists(key_file));
    check("...as v2", sealed && sealed->rfind(std::string{kSealedPrefixV2}, 0) == 0, sealed.value_or(""));
    check("...without the value in it", sealed && sealed->find("s3cret") == std::string::npos);
    check("it opens for the same binding", sealed && open(*sealed, here) == std::optional<std::string>{"Bearer s3cret"});
    check("it doesn't open for another address", sealed && !open(*sealed, there));
    check("...another connection",
          sealed && !open(*sealed, seal_binding("c2", "http://desk:8000", "Authorization")));
    check("...or another header",
          sealed && !open(*sealed, seal_binding("c1", "http://desk:8000", "X-Api-Key")));
    check("the same address however it is written still opens",
          sealed && open(*sealed, seal_binding("c1", "DESK:8000/", "authorization")).has_value());
    {
        std::string tampered = *sealed;
        tampered.back() = tampered.back() == 'A' ? 'B' : 'A';
        check("an altered seal doesn't open", !open(tampered, here));
    }
    const std::string old = seal_v1(key_file, "Bearer from-rc2");
    check("a v1 value (2.6 RC2) still opens, whatever the binding",
          open(old, there) == std::optional<std::string>{"Bearer from-rc2"});
    check("two seals of one value differ (a fresh nonce each)", seal("x", here) != seal("x", here));
#if !defined(_WIN32)
    {
        struct stat st{};
        ::stat(key_file.c_str(), &st);
        check("the key file is this account's only", (st.st_mode & 077) == 0,
              std::to_string(st.st_mode & 0777));
        ::chmod(key_file.c_str(), 0644);
        set_key_path(key_file);
        check("a key others can read still works", ensure_key(false));
        ::stat(key_file.c_str(), &st);
        check("...and is tightened", (st.st_mode & 077) == 0, std::to_string(st.st_mode & 0777));
    }
#endif

    // ---- Another machine's key --------------------------------------------------
    {
        const fs::path other = dir / "other.key";
        set_key_path(other);
        check("another machine makes its own key", seal("y", here).has_value() && fs::exists(other));
        check("another key opens nothing sealed with this one", sealed && !open(*sealed, here));
        check("...not even a v1 value", !open(old, here));
    }

    // ---- No usable key (R2-03) ------------------------------------------------
    {
        const fs::path broken = dir / "broken.key";
        fs::create_directories(broken);   // a directory where the key should be
        set_key_path(broken);
        check("a key file that can't be read is no key", !ensure_key(true));
        check("...so nothing is sealed (the caller writes no plain text)", !seal("Bearer x", here));
        check("...and it is never replaced", fs::is_directory(broken));
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
