// ============================================================================
// secret_headers_test.cpp - standalone assertions for credential headers.
// ----------------------------------------------------------------------------
// No test framework: each check prints PASS/FAIL and the binary exits non-zero
// if anything failed. What is pinned here is the promise to the operator: a
// client is never shown a credential, and saving what it was shown never
// loses one (audit F-01, F-23).
// ============================================================================
#include "liveplay/core/secret_headers.hpp"
#include "liveplay/core/show_tabs.hpp"

#include <cstdio>
#include <string>

using namespace liveplay::core::secrets;

static int failures = 0;
static void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "   ", detail.c_str());
    if (!ok) ++failures;
}

int main() {
    const std::string M{kMask};

    // ---- Which headers are secrets ---------------------------------------
    check("Authorization is a secret", is_secret_header("Authorization"));
    check("...in any case, with spaces", is_secret_header("  authorization "));
    check("X-API-Key is a secret", is_secret_header("X-API-Key"));
    check("X-Auth-Token is a secret", is_secret_header("X-Auth-Token"));
    check("Cookie is a secret", is_secret_header("Cookie"));
    check("a password header is a secret", is_secret_header("X-Password"));
    check("Content-Type is not", !is_secret_header("Content-Type"));
    check("Accept is not", !is_secret_header("Accept"));

    // ---- What a client sees ------------------------------------------------
    const std::string headers = "Authorization: Bearer abc123\nContent-Type: application/json";
    const std::string shown = mask_headers(headers, false);
    check("the credential is masked", shown.find("abc123") == std::string::npos, shown);
    check("...the rest is not", shown.find("application/json") != std::string::npos, shown);
    check("...and the name stays", shown.find("Authorization: " + M) != std::string::npos, shown);
    const std::string token_view = mask_headers(headers, true);
    check("an API token sees no value at all",
          token_view == "Authorization: " + M + "\nContent-Type: " + M, token_view);
    check("a line with no colon is masked whole",
          mask_headers("Bearer abc123", false) == M);
    check("a sealed value is masked even under a plain name",
          mask_headers("X-Thing: lpenc:v1:AAAA", false) == "X-Thing: " + M);
    check("no headers, nothing to do", mask_headers("", false).empty());

    // ---- What a client sends back ------------------------------------------
    check("a mask comes back as the stored value",
          restore_headers(shown, headers) == "Authorization: Bearer abc123\nContent-Type: application/json");
    check("a new value replaces it",
          restore_headers("Authorization: Bearer new", headers) == "Authorization: Bearer new");
    check("a header added beside a masked one keeps both",
          restore_headers("Authorization: " + M + "\nX-New: 1", headers) ==
              "Authorization: Bearer abc123\nX-New: 1");
    check("the same name twice restores in order",
          restore_headers("Cookie: " + M + "\nCookie: " + M, "Cookie: a=1\nCookie: b=2") ==
              "Cookie: a=1\nCookie: b=2");
    check("a header renamed has nothing to restore from",
          restore_headers("X-Key: " + M, headers) == "X-Key: " + M);
    check("...and so can't be sent", has_unusable_secret("X-Key: " + M));
    check("a sealed value can't be sent either", has_unusable_secret("Authorization: lpenc:v1:xyz"));
    check("plain headers can be", !has_unusable_secret(headers));

    // ---- Sealing walks only the secrets -------------------------------------
    int sealed = 0;
    const std::string mapped = map_secret_values(headers, [&](const std::string& v) {
        ++sealed;
        return "lpenc:v1:" + v;
    });
    check("only the credential is sealed", sealed == 1, mapped);
    check("...in place", mapped == "Authorization: lpenc:v1:Bearer abc123\nContent-Type: application/json",
          mapped);
    {
        const std::string stored = "K: v\n\nBearer abc";
        const std::string back = mask_headers(stored, false);
        check("a blank line never takes a masked line's value",
              restore_headers(back, stored) == "K: v\n\nBearer abc", restore_headers(back, stored));
        int n = 0;
        const std::string sealed_raw = map_secret_values(stored, [&](const std::string& v) {
            ++n;
            return "lpenc:v1:" + v;
        });
        check("a line with no colon is sealed like a secret",
              sealed_raw.find("lpenc:v1:Bearer abc") != std::string::npos && n == 1, sealed_raw);
        check("...and never makes a connection ask for a sign-in",
              !has_unusable_secret("Bearer " + M + "\n" + M));
    }
    check("a mask is never sealed",
          map_secret_values("Authorization: " + M, [](const std::string&) { return std::string{"x"}; }) ==
              "Authorization: " + M);

    // ---- One destination, however it is written (R2-01) ----------------------
    check("host alone is http://host", normalise_base_url("10.0.0.5:8000") == "http://10.0.0.5:8000",
          normalise_base_url("10.0.0.5:8000"));
    check("scheme and host case and a trailing slash don't matter",
          normalise_base_url("HTTP://Desk.Local/") == "http://desk.local", normalise_base_url("HTTP://Desk.Local/"));
    check("the default port doesn't matter",
          normalise_base_url("http://desk:80/api") == normalise_base_url("desk/api"),
          normalise_base_url("http://desk:80/api"));
    check("the path's case does", normalise_base_url("http://h/API") != normalise_base_url("http://h/api"));
    check("another port is another place", normalise_base_url("h:8080") != normalise_base_url("h"));
    check("a seal is bound to connection, place and header",
          seal_binding("c1", "HTTP://H:80/", "Authorization") == std::string{"c1\x1fhttp://h\x1f" "authorization"});
    check("any version of a seal is a seal", is_sealed("lpenc:v2:AAAA") && is_sealed("lpenc:v1:AAAA") &&
                                              !is_sealed("lpencv1"));

    // ---- Only where the candidates agree (R2-19) -----------------------------
    check("one candidate value is restored",
          restore_headers_unique("Authorization: " + M, {"Authorization: Bearer a", "X-Other: 1"}) ==
              "Authorization: Bearer a");
    check("two that agree are restored",
          restore_headers_unique("Authorization: " + M, {"Authorization: Bearer a", "authorization: Bearer a"}) ==
              "Authorization: Bearer a");
    check("two that differ leave the mask",
          restore_headers_unique("Authorization: " + M, {"Authorization: Bearer a", "Authorization: Bearer b"}) ==
              "Authorization: " + M);
    check("none leaves the mask", restore_headers_unique("X-Key: " + M, {"Authorization: Bearer a"}) ==
                                      "X-Key: " + M);
    {
        std::string names;
        map_secret_values("Authorization: a\nX-Api-Key: b\nAccept: c\nraw-token",
                          [&](const std::string& v, const std::string& name) {
                              names += name + "|";
                              return v;
                          });
        check("the seal callback is told each secret's header name", names == "authorization|x-api-key||",
              names);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
