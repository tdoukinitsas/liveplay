// ============================================================================
// liveplay/audio/types.hpp
// ----------------------------------------------------------------------------
// Shared primitive types for the audio engine. Strong typedefs keep the public
// API readable (you can't accidentally pass a CueId where a DeviceId is wanted)
// and make routing-graph code self-documenting.
// ============================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
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
inline constexpr ChannelCount kDefaultMasterChannels = 32;    // sparse; usually only a handful are wired
inline constexpr float        kDefaultMasterCeilingDb = -0.3f;

// The top two master channels are reserved for the Preview bus, and the default
// stereo Main output always occupies masters 0/1, so the bus can never be
// narrower than four channels.
inline constexpr ChannelCount kReservedPreviewChannels = 2;
inline constexpr ChannelCount kMinMasterChannels       = 4;

// Hard ceiling on simultaneous mixer strips ("buses"). The render thread's
// per-lane accumulators are sized for this once at start() so it never has to
// allocate mid-block; creating a strip beyond the cap is refused instead.
inline constexpr std::uint32_t kDefaultMaxMixerChannels = 64;

// Power-preserving pan / downmix law. Governs both folding a stereo source to
// mono and placing a mono source at the centre of a stereo destination, so the
// two stay consistent. Referenced by symbol everywhere rather than written as
// a literal, because this is intended to become a server config value.
inline constexpr float kDefaultDownmixDb = -3.0f;

// Anything below this is inaudible; used as the floor for a hard-panned
// send so 20*log10(0) never reaches the engine as -inf.
inline constexpr float kSilentGainDb = -120.0f;

// Constant-power pan of a mono source across two lanes of a stereo
// destination. `pan` is -1 (hard left) .. 0 (centre) .. +1 (hard right).
//
// The two returned gains satisfy l^2 + r^2 = 1, so the perceived level holds
// steady as the source sweeps across the image. At centre both come out at
// -3.01 dB, which is kDefaultDownmixDb to within a hundredth of a dB: a
// centred mono source and a stereo fold-down land at the same level, which is
// the consistency §2.5.2/§2.5.3 asked for. Hard over, the live side is exactly
// unity — panning never adds gain.
struct PanGainsDb { float left; float right; };

inline PanGainsDb pan_gains_db(float pan) noexcept {
    const float p     = std::clamp(pan, -1.0f, 1.0f);
    const float theta = (p + 1.0f) * 0.25f * 3.14159265358979323846f;  // 0 .. pi/2
    const float l     = std::cos(theta);
    const float r     = std::sin(theta);
    const auto to_db  = [](float g) {
        return g <= 0.0f ? kSilentGainDb
                         : std::max(kSilentGainDb, 20.0f * std::log10(g));
    };
    return {to_db(l), to_db(r)};
}

// BALANCE of a stereo source across its own two lanes. Same -1..+1 control,
// deliberately NOT the same law as pan_gains_db above.
//
// Pan places one mono signal somewhere in an image it does not otherwise
// occupy, so it has to hold constant power as it sweeps — which means both
// gains sit at -3 dB in the middle and the live side climbs to unity at the
// end of the travel. Applying that to a stereo bus would be wrong twice over:
// a centred stereo bus would lose 3 dB for doing nothing, and moving the
// control would ADD 3 dB of gain to the side you moved toward.
//
// Balance only ever takes away. Centre is unity on both lanes, and moving the
// control attenuates the lane you are moving away from until it is silent. That
// is what a console balance pot does, and it means the loud side of an
// already-lopsided mix cannot be pushed into the limiter by trying to correct
// the quiet one.
inline PanGainsDb balance_gains_db(float balance) noexcept {
    const float b = std::clamp(balance, -1.0f, 1.0f);
    const float l = b > 0.0f ? 1.0f - b : 1.0f;
    const float r = b < 0.0f ? 1.0f + b : 1.0f;
    const auto to_db = [](float g) {
        return g <= 0.0f ? kSilentGainDb
                         : std::max(kSilentGainDb, 20.0f * std::log10(g));
    };
    return {to_db(l), to_db(r)};
}

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

// First master channel of the Preview reserve, which always sits at the very top
// of the bus. Preview routing and the device-override allocator both have to
// agree on where the reserve begins, so it is derived here in exactly one place
// instead of being spelled as literals in each.
constexpr MasterChannelIndex preview_master_base(MasterChannelIndex bus_width) noexcept {
    return bus_width >= kReservedPreviewChannels
               ? static_cast<MasterChannelIndex>(bus_width - kReservedPreviewChannels)
               : 0;
}

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
