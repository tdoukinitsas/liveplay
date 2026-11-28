#pragma once
// =============================================================================
// liveplay/events/osc.hpp — OSC 1.0 messages, for plugins that speak OSC
// -----------------------------------------------------------------------------
// Encoding and decoding only; the transport (UDP, usually) is the plugin's.
// Used by the `osc` and `xair` plugins. See docs/EVENT_PLUGINS.md, "Helpers".
// =============================================================================
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace liveplay::events::osc {

struct Arg {
    char         type = 'N';   // i (int32) f (float32) s (string) b (blob) T F N
    std::int32_t i = 0;
    float        f = 0.0f;
    std::string  s;            // string, or blob bytes

    static Arg integer(std::int32_t v) { Arg a; a.type = 'i'; a.i = v; return a; }
    static Arg real(float v)           { Arg a; a.type = 'f'; a.f = v; return a; }
    static Arg string(std::string v)   { Arg a; a.type = 's'; a.s = std::move(v); return a; }
    static Arg boolean(bool v)         { Arg a; a.type = v ? 'T' : 'F'; return a; }

    // The value as a number / string, whatever its OSC type.
    double      number() const;
    std::string text() const;
};

struct Message {
    std::string      address;
    std::vector<Arg> args;
};

// One message as an OSC packet. An empty `args` still carries the "," type
// tag, which is how a "get" (the address alone) is written for most devices.
std::string encode(const Message& m);

// A packet back into a message. Bundles and malformed packets give nullopt.
std::optional<Message> decode(const void* data, std::size_t size);

} // namespace liveplay::events::osc
