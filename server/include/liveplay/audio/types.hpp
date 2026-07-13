// ============================================================================
// liveplay/audio/types.hpp
// ----------------------------------------------------------------------------
// Shared primitive types for the audio engine. Strong typedefs keep the public
// API readable (you can't accidentally pass a CueId where a DeviceId is wanted)
// and make routing-graph code self-documenting.
// ============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>

namespace liveplay::audio {

// ---------------------------------------------------------------------------
// Sample-domain primitives
// ---------------------------------------------------------------------------
using Sample        = float;          // canonical mix-bus sample type
using SampleRate    = std::uint32_t;  // Hz
using FrameCount    = std::uint64_t;  // number of multi-channel frames
using ChannelCount  = std::uint32_t;
using ChannelIndex  = std::uint32_t;

inline constexpr SampleRate   kDefaultMixSampleRate  = 48'000;
inline constexpr FrameCount   kDefaultRenderBlock    = 256;   // ~5.3 ms @ 48k
inline constexpr ChannelCount kDefaultMasterChannels = 64;    // sparse; usually only a handful are wired

// Mixer strips carry this many parallel audio lanes (stereo: L=0, R=1).
// Item→mixer and mixer→master sends address a specific lane; kAllMixerLanes
// fans the send across every lane — used for mono sources (centre image) and
// as the backwards-compatible default for API callers that predate lanes.
inline constexpr ChannelCount kMixerLanes    = 2;
inline constexpr ChannelIndex kAllMixerLanes = static_cast<ChannelIndex>(-1);

// ---------------------------------------------------------------------------
// Strong-typed identifiers. Implemented as a CRTP-free template so each ID
// type is distinct in the type system but uses the same machinery.
// ---------------------------------------------------------------------------
namespace detail {
template <typename Tag>
struct StringId {
    std::string value;

    StringId() = default;
    explicit StringId(std::string v) : value(std::move(v)) {}

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }

    friend bool operator==(const StringId& a, const StringId& b) noexcept { return a.value == b.value; }
    friend bool operator!=(const StringId& a, const StringId& b) noexcept { return !(a == b); }
    friend bool operator<(const StringId& a, const StringId& b) noexcept  { return a.value <  b.value; }

    friend std::ostream& operator<<(std::ostream& os, const StringId& id) { return os << id.value; }
};
} // namespace detail

struct CueIdTag;
struct MixerChannelIdTag;
struct DeviceIdTag;

using CueId          = detail::StringId<CueIdTag>;
using MixerChannelId = detail::StringId<MixerChannelIdTag>;
using DeviceId       = detail::StringId<DeviceIdTag>;

// Master-bus channels are addressed by integer index because there's a fixed
// number of them and they map 1:1 to hardware destinations.
using MasterChannelIndex = std::uint32_t;
inline constexpr MasterChannelIndex kInvalidMasterChannel = static_cast<MasterChannelIndex>(-1);

// ---------------------------------------------------------------------------
// Linear / decibel gain helpers
// ---------------------------------------------------------------------------
constexpr float db_to_linear(float db) noexcept {
    // 10^(db / 20). Avoid pulling in <cmath> for the constexpr path.
    // -120 dB ≈ silence; clamp to keep callers honest.
    if (db <= -120.0f) return 0.0f;
    // 20 / ln(10) ≈ 8.685889638; gain = exp(db / 8.685889638)
    // constexpr-friendly enough via Taylor would be overkill — use a runtime
    // helper at the call site instead. Keep this entry inline-fast:
    float x = db * 0.11512925465f;        // db * ln(10)/20
    float result = 1.0f + x + x*x/2 + x*x*x/6 + x*x*x*x/24; // 4-term Taylor; "good enough" near 0
    return result;
}

// Higher-accuracy variant for one-off control-thread conversions.
float db_to_linear_precise(float db) noexcept;
float linear_to_db_precise(float lin) noexcept;

// ---------------------------------------------------------------------------
// A routing "send" — used in many places below.
// ---------------------------------------------------------------------------
struct ItemSourceSend {
    ChannelIndex   source_channel;   // index inside the item's source layout
    MixerChannelId destination;
    float          gain_linear;
};

struct MixerToMasterSend {
    MasterChannelIndex destination;
    float              gain_linear;
};

struct MasterDestination {
    DeviceId     device;
    ChannelIndex hw_channel;       // index into that device's hardware output channels
};

} // namespace liveplay::audio

// ---------------------------------------------------------------------------
// std::hash specialisations so IDs work in unordered_map / unordered_set.
// ---------------------------------------------------------------------------
namespace std {
template <typename Tag>
struct hash<liveplay::audio::detail::StringId<Tag>> {
    size_t operator()(const liveplay::audio::detail::StringId<Tag>& id) const noexcept {
        return std::hash<std::string>{}(id.value);
    }
};
} // namespace std
