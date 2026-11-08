// ============================================================================
// liveplay/audio/gain_envelope.hpp
// ----------------------------------------------------------------------------
// A cue's volume automation: keyframes of (source-file frame, dB), applied on
// top of the item's own volume. Header-only so it is unit-testable without an
// audio device (tests/gain_envelope_test.cpp).
//
// The rules, which the client mirrors in app/utils/volumeAutomation.ts so the
// waveform it draws is the level the engine plays:
//
//   * Positions are SOURCE-FILE time, not time since the in-point: a dip placed
//     on a word stays on that word when the trim moves.
//   * Between two keyframes the level moves linearly in dB, which sounds even
//     to the ear (a 0 → -20 dB ramp is a steady fade, not one that drops away
//     at the end).
//   * Before the first keyframe the level holds at the first one's value, and
//     after the last at the last one's. No keyframes = 0 dB everywhere.
//   * A keyframe at or below kEnvelopeFloorDb is silence (-inf). Ramps to and
//     from it interpolate against the floor value, so the approach is smooth
//     and only the keyframe itself is exactly silent.
//   * The range tops out at kEnvelopeMaxDb (the item volume slider's range).
// ============================================================================
#pragma once

#include "liveplay/audio/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace liveplay::audio {

inline constexpr float kEnvelopeFloorDb = -60.0f;   // and below: silence
inline constexpr float kEnvelopeMaxDb   =  10.0f;

struct GainEnvelope {
    struct Point {
        std::uint64_t frame = 0;   // source-file frame at the mix rate
        float         db    = 0.0f;
    };
    // Sorted by frame. normalise() makes it so.
    std::vector<Point> points;

    bool empty() const noexcept { return points.empty(); }

    // Sort, clamp each level into [floor, max], and drop exact duplicate
    // frames (the later one wins, as it would in the editor).
    void normalise() {
        std::stable_sort(points.begin(), points.end(),
                         [](const Point& a, const Point& b) { return a.frame < b.frame; });
        std::vector<Point> out;
        out.reserve(points.size());
        for (const auto& p : points) {
            Point q = p;
            if (!std::isfinite(q.db)) q.db = (q.db > 0) ? kEnvelopeMaxDb : kEnvelopeFloorDb;
            q.db = std::clamp(q.db, kEnvelopeFloorDb, kEnvelopeMaxDb);
            if (!out.empty() && out.back().frame == q.frame) out.back() = q;
            else out.push_back(q);
        }
        points = std::move(out);
    }

    // True when every keyframe is 0 dB: applying it would change nothing.
    bool is_unity() const noexcept {
        for (const auto& p : points) if (p.db != 0.0f) return false;
        return true;
    }

    // Level at a source frame, in dB (kEnvelopeFloorDb or below = silence).
    float db_at(std::uint64_t frame) const noexcept {
        if (points.empty()) return 0.0f;
        if (frame <= points.front().frame) return points.front().db;
        if (frame >= points.back().frame)  return points.back().db;
        // First point strictly after `frame`; the one before it starts the segment.
        const auto it = std::upper_bound(points.begin(), points.end(), frame,
            [](std::uint64_t f, const Point& p) { return f < p.frame; });
        const Point& b = *it;
        const Point& a = *(it - 1);
        const double t = static_cast<double>(frame - a.frame) /
                         static_cast<double>(b.frame - a.frame);
        return static_cast<float>(a.db + (b.db - a.db) * t);
    }

    // Linear gain at a source frame.
    float gain_at(std::uint64_t frame) const noexcept {
        return db_to_gain(db_at(frame));
    }

    static float db_to_gain(float db) noexcept {
        if (db <= kEnvelopeFloorDb) return 0.0f;
        return std::pow(10.0f, db / 20.0f);
    }

    // Multiply `frames` interleaved frames of `channels` channels, the first of
    // which sits at `start_frame` in the source file.
    //
    // Exact, and cheap: within a segment the level is linear in dB, so the gain
    // is geometric — one multiply per frame. The run is split at every keyframe
    // and re-seeded from db_at() at least every kReseed frames so float error
    // never accumulates; pow() runs once per run, not per sample.
    void apply(Sample* interleaved, std::size_t frames, std::size_t channels,
               std::uint64_t start_frame) const noexcept {
        if (points.empty() || frames == 0 || channels == 0) return;
        constexpr std::size_t kReseed = 256;
        std::size_t i = 0;
        while (i < frames) {
            const std::uint64_t f = start_frame + i;
            // The segment f is in: its slope, and where the next keyframe is.
            double slope = 0.0;                      // dB per frame
            std::uint64_t next_kf = ~std::uint64_t{0};
            if (f < points.front().frame) {
                next_kf = points.front().frame;
            } else if (f < points.back().frame) {
                const auto it = std::upper_bound(points.begin(), points.end(), f,
                    [](std::uint64_t x, const Point& p) { return x < p.frame; });
                const Point& b = *it;
                const Point& a = *(it - 1);
                slope   = static_cast<double>(b.db - a.db) /
                          static_cast<double>(b.frame - a.frame);
                next_kf = b.frame;
            }
            std::size_t n = std::min<std::size_t>(frames - i, kReseed);
            if (next_kf != ~std::uint64_t{0})
                n = static_cast<std::size_t>(std::min<std::uint64_t>(n, next_kf - f));
            if (n == 0) n = 1;

            const float db0 = db_at(f);
            Sample* fr = interleaved + i * channels;
            if (db0 <= kEnvelopeFloorDb) {
                // Silence. A flat or falling stretch stays silent to the end of
                // the run; a rising one is only silent at this frame.
                const std::size_t m = (slope > 0.0) ? 1 : n;
                for (std::size_t k = 0; k < m * channels; ++k) fr[k] = 0.0f;
                i += m;
                continue;
            }
            double g = std::pow(10.0, static_cast<double>(db0) / 20.0);
            const double r = std::pow(10.0, slope / 20.0);
            for (std::size_t k = 0; k < n; ++k) {
                const float gf = static_cast<float>(g);
                for (std::size_t c = 0; c < channels; ++c) fr[c] *= gf;
                fr += channels;
                g *= r;
            }
            i += n;
        }
    }
};

} // namespace liveplay::audio
