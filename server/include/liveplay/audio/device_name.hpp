// ============================================================================
// liveplay/audio/device_name.hpp
// ----------------------------------------------------------------------------
// How two playback-device names are compared.
//
// Windows renumbers an endpoint when the same model is re-enumerated — plug a
// USB interface into another port, or add a second of the same, and
// "OUT 3-4 (BEHRINGER UMC 404HD 192k)" comes back as
// "OUT 3-4 (2- BEHRINGER UMC 404HD 192k)". A show saved before that named the
// old string, and exact comparison declared the interface missing: the bus
// went silent and the remap dialog offered to map a device that was plugged
// in. The "N- " prefix carries no identity, so it is ignored, along with case
// and runs of whitespace.
//
// Callers compare exactly first and fall back to this only when the
// normalised form picks out ONE device — two identical interfaces are two
// devices, and guessing between them would be worse than asking.
// ============================================================================
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace liveplay::audio {

inline std::string normalise_device_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    bool pending_space = false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        // A Windows renumbering prefix: digits then "- ", at the start of the
        // name or straight after an opening parenthesis.
        const bool at_boundary = out.empty() || out.back() == '(';
        if (at_boundary && std::isdigit(c)) {
            std::size_t j = i;
            while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) ++j;
            if (j + 1 < name.size() && name[j] == '-' && name[j + 1] == ' ') {
                i = j + 1;              // skip "N- " (the loop's ++i eats the space)
                pending_space = false;
                continue;
            }
        }
        if (std::isspace(c)) { pending_space = !out.empty() && out.back() != '('; continue; }
        if (pending_space && c != ')') out.push_back(' ');
        pending_space = false;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

} // namespace liveplay::audio
