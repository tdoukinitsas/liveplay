// ============================================================================
// liveplay/audio/monitor_tap.hpp
// ----------------------------------------------------------------------------
// PFL — pre-fade listen — expressed as sends into the Monitor strip.
//
// A PFL'd bus adds one edge into Monitor and changes nothing else: the house
// mix is untouched, several buses can be tapped at once, and the tap is taken
// before the strip's fader and before its mute, because hearing a channel with
// its fader down is the entire diagnostic use. See BUS_ARCHITECTURE.md §2.4.
//
// The tap list is built on the control thread, where a strip's width is known,
// so the render thread only ever adds one buffer into another at a gain.
//
// This lives in its own header, apart from the engine, so the placement rule
// can be exercised without opening an audio device — every other line of the
// render loop needs a device to run at all, and this is the line with a
// decision in it.
// ============================================================================
#pragma once

#include "liveplay/audio/mixer_channel.hpp"
#include "liveplay/audio/types.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace liveplay::audio {

// One PFL'd strip's contribution to the Monitor strip.
//
// Deliberately NOT the general bus→bus edge. Monitor is a single, known
// destination that nothing may feed onward, so it needs no processing order
// and no cycle check; those arrive with bus→bus routing.
struct MonitorTap {
    std::shared_ptr<MixerChannel> source;
    ChannelIndex                  src_lane = 0;
    ChannelIndex                  dst_lane = 0;
    float                         gain     = 1.0f;
};

// Fold every tap into the monitor strip's lane buffers, in place.
//
// `lane_buffers` is the render thread's accumulator array, addressed
// [strip_index * kMixerLanes + lane]; `mixer_index` maps a strip id to its
// index for this block. Taps whose source is no longer live are skipped, as is
// any tap that resolves to the monitor itself — that would be a strip feeding
// its own accumulator, which accumulates rather than routes.
inline void mix_monitor_taps(
        std::size_t monitor_index,
        const std::vector<MonitorTap>& taps,
        const std::unordered_map<std::string, std::size_t>& mixer_index,
        std::vector<std::vector<Sample>>& lane_buffers,
        std::size_t block) noexcept {
    const std::size_t mon_base = monitor_index * kMixerLanes;
    if (mon_base + kMixerLanes > lane_buffers.size()) return;

    for (const auto& tap : taps) {
        if (!tap.source) continue;
        if (tap.src_lane >= kMixerLanes || tap.dst_lane >= kMixerLanes) continue;
        const auto sit = mixer_index.find(tap.source->id().value);
        if (sit == mixer_index.end() || sit->second == monitor_index) continue;
        const std::size_t src_idx = sit->second * kMixerLanes + tap.src_lane;
        if (src_idx >= lane_buffers.size()) continue;

        const Sample* src = lane_buffers[src_idx].data();
        Sample*       dst = lane_buffers[mon_base + tap.dst_lane].data();
        const std::size_t n = std::min({block, lane_buffers[src_idx].size(),
                                        lane_buffers[mon_base + tap.dst_lane].size()});
        for (std::size_t s = 0; s < n; ++s) dst[s] += src[s] * tap.gain;
    }
}

// The taps a strip contributes when PFL is up.
//
// Stereo goes lane-for-lane, mono places its single lane across both — and
// either way the tap carries the strip's own position gains.
//
// That is what makes PFL post-pan. Neither pan nor balance is a strip operation
// here: both live in the strip's sends to the master (§2.5.3), which sit
// downstream of this tap, so a tap that ignored them would put every mono bus
// dead centre in the phones and show a hard-balanced stereo bus as though it
// were still even. At pan centre the law gives -3.01 dB, the downmix constant
// to within a hundredth of a dB; at balance centre it gives unity, so a centred
// bus of either width reads exactly as it did before.
//
// Width, by contrast, needs nothing here: it is applied inside the strip's DSP
// chain, which is upstream of this tap, so the phones hear it for free.
inline void append_monitor_taps(std::vector<MonitorTap>& out,
                                std::shared_ptr<MixerChannel> strip) {
    if (!strip) return;
    if (strip->width() >= kMixerLanes) {
        const auto g = balance_gains_db(strip->pan());
        out.push_back({strip, 0, 0, db_to_linear_precise(g.left)});
        out.push_back({strip, 1, 1, db_to_linear_precise(g.right)});
    } else {
        const auto g = pan_gains_db(strip->pan());
        out.push_back({strip, 0, 0, db_to_linear_precise(g.left)});
        out.push_back({strip, 0, 1, db_to_linear_precise(g.right)});
    }
}

} // namespace liveplay::audio
