// ============================================================================
// liveplay/audio/analyser_tap.hpp
// ----------------------------------------------------------------------------
// A spectrum-analyser tap on one strip: the last few thousand samples of its
// signal before the EQ and after it, for the channel view's real-time
// analyser.
//
// The render thread WRITES; the broadcast thread reads the latest window and
// runs the FFT. It is an overwrite ring, not a FIFO: an analyser only ever
// wants the newest N samples, so there is no backpressure and nothing waits.
// A reader copies the window it wants and then re-checks the write counter;
// if the writer lapped the copied region meanwhile, the copy is discarded and
// the next tick simply tries again.
//
// A fixed pool of these lives in the engine, allocated once at start. Arming
// one is a single atomic store of the strip it should listen to, so nothing
// about the audio path allocates, locks or changes shape when a client opens
// the analyser. The target is compared, never dereferenced, so a strip
// removed while armed costs nothing worse than a tap that stops receiving.
// ============================================================================
#pragma once

#include "liveplay/audio/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace liveplay::audio {

struct AnalyserTap {
    // Power of two, comfortably above the largest FFT window (4096) plus a
    // few render blocks of slack for the reader's copy.
    static constexpr std::size_t kRing = 16384;
    static constexpr std::size_t kMask = kRing - 1;

    // The strip being listened to (a MixerChannel*), or null when unarmed.
    std::atomic<const void*> target{nullptr};

    // Mono (lanes averaged) — an analyser shows the programme, not the image.
    std::array<float, kRing> pre{};     // strip input, before HPF/LPF/EQ
    std::array<float, kRing> post{};    // after the EQ, before the dynamics
    // Frames written since arming. Written only by the render thread.
    std::atomic<std::uint64_t> written{0};

    // ---- Render thread ----------------------------------------------------
    static float mono_at(Sample* const* lanes, std::size_t lane_count, std::size_t s) noexcept {
        if (lane_count >= 2) return 0.5f * (lanes[0][s] + lanes[1][s]);
        return lane_count ? lanes[0][s] : 0.0f;
    }
    void write_pre(Sample* const* lanes, std::size_t lane_count, std::size_t frames) noexcept {
        const std::uint64_t w = written.load(std::memory_order_relaxed);
        for (std::size_t s = 0; s < frames; ++s) pre[(w + s) & kMask] = mono_at(lanes, lane_count, s);
    }
    void write_post(Sample* const* lanes, std::size_t lane_count, std::size_t frames) noexcept {
        const std::uint64_t w = written.load(std::memory_order_relaxed);
        for (std::size_t s = 0; s < frames; ++s) post[(w + s) & kMask] = mono_at(lanes, lane_count, s);
    }
    // Publish the block written by write_pre/write_post.
    void commit(std::size_t frames) noexcept {
        written.fetch_add(frames, std::memory_order_release);
    }

    // ---- Reader (broadcast thread) ------------------------------------------
    // Copy the newest `n` samples of each ring. False when there is not yet
    // that much, or the writer overran the copy.
    bool read_latest(float* pre_out, float* post_out, std::size_t n) const noexcept {
        if (n > kRing / 2) return false;
        const std::uint64_t end = written.load(std::memory_order_acquire);
        if (end < n) return false;
        const std::uint64_t start = end - n;
        for (std::size_t i = 0; i < n; ++i) {
            pre_out[i]  = pre[(start + i) & kMask];
            post_out[i] = post[(start + i) & kMask];
        }
        // The writer may have advanced while we copied; the copy is good as
        // long as it did not wrap onto the region we read.
        const std::uint64_t after = written.load(std::memory_order_acquire);
        return after - start <= kRing;
    }
};

inline constexpr std::size_t kMaxAnalyserTaps = 4;

} // namespace liveplay::audio
