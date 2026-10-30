// =============================================================================
// events/osc.cpp — OSC 1.0 message encoding and decoding (see osc.hpp)
// =============================================================================
#include "liveplay/events/osc.hpp"

#include <cstring>

namespace liveplay::events::osc {
namespace {

// Strings and blobs are padded with NULs to a multiple of 4 (a string always
// gets at least one terminator).
void put_padded(std::string& out, const std::string& s) {
    out += s;
    out.push_back('\0');
    while (out.size() % 4) out.push_back('\0');
}

void put_be32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

std::uint32_t get_be32(const unsigned char* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

} // namespace

double Arg::number() const {
    switch (type) {
        case 'i': return i;
        case 'f': return f;
        case 'T': return 1.0;
        case 's': try { return std::stod(s); } catch (...) { return 0.0; }
        default:  return 0.0;
    }
}

std::string Arg::text() const {
    switch (type) {
        case 's': case 'b': return s;
        case 'i': return std::to_string(i);
        case 'f': return std::to_string(f);
        case 'T': return "true";
        case 'F': return "false";
        default:  return {};
    }
}

std::string encode(const Message& m) {
    std::string tags = ",";
    std::string payload;
    for (const auto& a : m.args) {
        tags += a.type;
        switch (a.type) {
            case 'i': put_be32(payload, static_cast<std::uint32_t>(a.i)); break;
            case 'f': {
                std::uint32_t bits;
                std::memcpy(&bits, &a.f, sizeof bits);
                put_be32(payload, bits);
                break;
            }
            case 's': put_padded(payload, a.s); break;
            case 'b':
                put_be32(payload, static_cast<std::uint32_t>(a.s.size()));
                payload += a.s;
                while (payload.size() % 4) payload.push_back('\0');
                break;
            default: break;   // T F N: tag only
        }
    }
    std::string out;
    put_padded(out, m.address);
    put_padded(out, tags);
    return out + payload;
}

std::optional<Message> decode(const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::size_t off = 0;
    const auto read_string = [&](std::string& out) -> bool {
        const auto* start = p + off;
        const auto* end = static_cast<const unsigned char*>(std::memchr(start, 0, size - off));
        if (!end) return false;
        out.assign(reinterpret_cast<const char*>(start), static_cast<std::size_t>(end - start));
        off += ((static_cast<std::size_t>(end - start) + 4) & ~std::size_t{3});
        return off <= size;
    };

    Message m;
    if (size == 0 || p[0] != '/' || !read_string(m.address)) return std::nullopt;
    if (off >= size) return m;   // address only
    std::string tags;
    if (!read_string(tags) || tags.empty() || tags[0] != ',') return std::nullopt;

    for (std::size_t t = 1; t < tags.size(); ++t) {
        Arg a;
        a.type = tags[t];
        switch (a.type) {
            case 'i':
                if (off + 4 > size) return std::nullopt;
                a.i = static_cast<std::int32_t>(get_be32(p + off));
                off += 4;
                break;
            case 'f': {
                if (off + 4 > size) return std::nullopt;
                const std::uint32_t bits = get_be32(p + off);
                std::memcpy(&a.f, &bits, sizeof a.f);
                off += 4;
                break;
            }
            case 's':
                if (!read_string(a.s)) return std::nullopt;
                break;
            case 'b': {
                if (off + 4 > size) return std::nullopt;
                const std::size_t n = get_be32(p + off);
                off += 4;
                if (off + n > size) return std::nullopt;
                a.s.assign(reinterpret_cast<const char*>(p + off), n);
                off += (n + 3) & ~std::size_t{3};
                break;
            }
            case 'T': case 'F': case 'N': break;
            default: return std::nullopt;   // a type we can't skip safely
        }
        m.args.push_back(std::move(a));
    }
    return m;
}

} // namespace liveplay::events::osc
