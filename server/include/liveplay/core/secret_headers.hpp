#pragma once
// ============================================================================
// secret_headers.hpp - which HTTP header values are secrets, and the text
// helpers that keep them away from clients (audit F-01, F-23).
// ----------------------------------------------------------------------------
// An HTTP connection, preset or event carries headers as "Name: value" lines.
// The values of credential-looking headers (Authorization, X-API-Key, a
// cookie, anything named like a token or password) are secrets:
//
//   * no client is ever sent one: every client sees MASK in its place, and an
//     API token sees MASK for every header value;
//   * a client that sends MASK back means "keep the stored value", so saving
//     what you were shown never wipes the secret;
//   * on disk each one is sealed with a key that never leaves the server
//     machine (secrets.hpp), bound to its connection, address and header
//     name. A show opened elsewhere asks for them again;
//   * a mask means "as stored" only for the same connection at the same
//     address: a credential never follows a connection somewhere new (R2-01).
//
// Header-only and free of libsodium so the rules can be unit-tested
// (server/tests/secret_headers_test.cpp).
// ============================================================================
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace liveplay::core::secrets {

// What a client sees in place of a secret value.
inline constexpr std::string_view kMask = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";  // ••••••
// The prefixes of a value sealed for disk (secrets.hpp). v1 (2.6 RC2 only) is
// still opened on load; v2 is what is written, bound to where it may be sent.
inline constexpr std::string_view kSealedPrefix   = "lpenc:v1:";
inline constexpr std::string_view kSealedPrefixV2 = "lpenc:v2:";
// What every sealed value starts with, whatever its version.
inline constexpr std::string_view kSealedFamily   = "lpenc:v";

inline std::string lower(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return std::string{s.substr(b, e - b + 1)};
}

// True for a header whose value is a credential.
inline bool is_secret_header(std::string_view name) {
    const std::string n = lower(trim(name));
    if (n.empty()) return false;
    for (const char* exact : {"authorization", "proxy-authorization", "cookie", "set-cookie"})
        if (n == exact) return true;
    for (const char* part : {"token", "key", "secret", "pass", "auth", "session",
                             "signature", "credential", "apikey"})
        if (n.find(part) != std::string::npos) return true;
    return false;
}

inline bool is_sealed(std::string_view v) {
    return v.substr(0, kSealedFamily.size()) == kSealedFamily;
}

// An HTTP base URL as compared for "still the same destination" and bound into
// a seal: trimmed, "http://" when it names no scheme (as the HTTP plugin sends
// it), scheme and host in lower case, the default port dropped, no trailing
// slash. "Host:80", "http://host" and "HTTP://Host/" are one destination.
inline std::string normalise_base_url(std::string_view in) {
    std::string url = trim(in);
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.empty()) return url;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        url = "http://" + url;
        scheme_end = 4;
    }
    const auto host_start = scheme_end + 3;
    auto host_end = url.find('/', host_start);
    if (host_end == std::string::npos) host_end = url.size();
    std::transform(url.begin(), url.begin() + static_cast<std::ptrdiff_t>(host_end), url.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const std::string scheme = url.substr(0, scheme_end);
    const std::string default_port = scheme == "https" ? ":443" : scheme == "http" ? ":80" : "";
    if (!default_port.empty() && host_end >= host_start + default_port.size() &&
        url.compare(host_end - default_port.size(), default_port.size(), default_port) == 0)
        url.erase(host_end - default_port.size(), default_port.size());
    return url;
}

struct HeaderLine {
    std::string name;    // trimmed; empty for a line with no colon
    std::string value;   // trimmed
    std::string raw;     // the line as written, for lines with no colon
    bool        has_colon = false;
};

inline std::vector<HeaderLine> split_headers(const std::string& text) {
    std::vector<HeaderLine> out;
    std::istringstream in{text};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        HeaderLine h;
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            h.raw = line;
        } else {
            h.has_colon = true;
            h.name  = trim(std::string_view{line}.substr(0, colon));
            h.value = trim(std::string_view{line}.substr(colon + 1));
        }
        out.push_back(std::move(h));
    }
    return out;
}

inline std::string join_headers(const std::vector<HeaderLine>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        const auto& h = lines[i];
        if (!h.has_colon) out += h.raw;
        else out += h.name + ": " + h.value;
    }
    return out;
}

// What a client may see: secret values masked, or every value when
// `all_values` (an API token's view). A line without a colon could be a
// pasted credential, so it is masked whole in either case.
inline std::string mask_headers(const std::string& text, bool all_values) {
    if (text.empty()) return text;
    auto lines = split_headers(text);
    for (auto& h : lines) {
        if (!h.has_colon) {
            if (!trim(h.raw).empty()) h.raw = std::string{kMask};
            continue;
        }
        if (!h.value.empty() && (all_values || is_secret_header(h.name) || is_sealed(h.value)))
            h.value = std::string{kMask};
    }
    return join_headers(lines);
}

// A client's headers with every MASK put back to the value stored for the same
// header name (in order, for a name that appears twice). A MASK with nothing
// to restore from stays as it is; mask_headers shows it as unset and the
// connection asks for it (it is never sent: see has_unusable_secret).
inline std::string restore_headers(const std::string& incoming, const std::string& stored) {
    if (incoming.find(kMask) == std::string::npos) return incoming;
    auto lines = split_headers(incoming);
    auto old = split_headers(stored);
    std::vector<bool> used(old.size(), false);
    std::vector<bool> raw_used(old.size(), false);
    for (auto& h : lines) {
        if (!h.has_colon) {
            if (trim(h.raw) != kMask) continue;
            for (std::size_t i = 0; i < old.size(); ++i) {
                // A blank line was never masked, so it can't be what came back.
                if (raw_used[i] || old[i].has_colon || trim(old[i].raw).empty()) continue;
                raw_used[i] = true;
                h.raw = old[i].raw;
                break;
            }
            continue;
        }
        if (h.value != kMask) continue;
        const std::string name = lower(h.name);
        for (std::size_t i = 0; i < old.size(); ++i) {
            if (used[i] || !old[i].has_colon || lower(old[i].name) != name) continue;
            used[i] = true;
            h.value = old[i].value;
            break;
        }
    }
    return join_headers(lines);
}

// Like restore_headers, but from several candidate texts (other events and
// presets on the same connection), and only where they agree: a masked line
// gets a value only when every candidate that has that header has the same
// one. Two events on one connection authenticating as different principals
// leave the mask (and a sign-in prompt) rather than lend one the other's
// credential (R2-19).
inline std::string restore_headers_unique(const std::string& incoming,
                                          const std::vector<std::string>& candidates) {
    if (incoming.find(kMask) == std::string::npos) return incoming;
    auto lines = split_headers(incoming);
    for (auto& h : lines) {
        const bool masked = h.has_colon ? h.value == kMask : trim(h.raw) == kMask;
        if (!masked) continue;
        std::vector<std::string> values;
        for (const auto& c : candidates) {
            for (const auto& o : split_headers(c)) {
                if (h.has_colon != o.has_colon) continue;
                std::string v;
                if (h.has_colon) {
                    if (lower(o.name) != lower(h.name) || o.value.empty() || o.value == kMask) continue;
                    v = o.value;
                } else {
                    v = trim(o.raw);
                    if (v.empty() || v == kMask) continue;
                }
                if (std::find(values.begin(), values.end(), v) == values.end()) values.push_back(v);
            }
        }
        if (values.size() != 1) continue;
        if (h.has_colon) h.value = values.front();
        else h.raw = values.front();
    }
    return join_headers(lines);
}

// Every secret value through `fn` (seal on the way to disk, open on the way
// in). `fn(value)` or `fn(value, header_name)` returns the new value; the name
// is lower case, "" for a line with no colon. A line with no colon is never
// sent as a header, but it is masked like a secret (it may be a pasted token),
// so it is sealed like one too.
template <typename Fn>
std::string map_secret_values(const std::string& text, Fn&& fn) {
    if (text.empty()) return text;
    const auto call = [&](const std::string& v, const std::string& name) -> std::string {
        if constexpr (std::is_invocable_v<Fn&, const std::string&, const std::string&>) {
            return fn(v, name);
        } else {
            (void)name;
            return fn(v);
        }
    };
    auto lines = split_headers(text);
    bool changed = false;
    for (auto& h : lines) {
        if (!h.has_colon) {
            const std::string raw = trim(h.raw);
            if (raw.empty() || raw == kMask) continue;
            std::string next = call(raw, std::string{});
            if (next != raw) { h.raw = std::move(next); changed = true; }
            continue;
        }
        if (h.value.empty() || h.value == kMask) continue;
        if (!is_secret_header(h.name) && !is_sealed(h.value)) continue;
        std::string next = call(h.value, lower(h.name));
        if (next != h.value) { h.value = std::move(next); changed = true; }
    }
    return changed ? join_headers(lines) : text;
}

// The "where it may be sent" a sealed value is bound to (secrets::seal's
// associated data): the connection, its normalised base URL, the header name.
inline std::string seal_binding(std::string_view connection_id, std::string_view base_url,
                                std::string_view header_name) {
    std::string ad{connection_id};
    ad += '\x1f';
    ad += normalise_base_url(base_url);
    ad += '\x1f';
    ad += lower(header_name);
    return ad;
}

// True when a header can't be sent as it is: a value still sealed (the key
// that sealed it is on another machine) or a MASK nothing could restore.
// Lines with no colon are never sent, so they don't count.
inline bool has_unusable_secret(const std::string& text) {
    for (const auto& h : split_headers(text)) {
        if (h.has_colon && (is_sealed(h.value) || h.value == kMask)) return true;
    }
    return false;
}

}  // namespace liveplay::core::secrets
