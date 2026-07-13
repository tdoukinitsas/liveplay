// ============================================================================
// playback_item.cpp — see playback_item.hpp.
// ============================================================================
#include "liveplay/audio/playback_item.hpp"
#include "liveplay/logger.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace liveplay::audio {

namespace {

inline float db_to_lin(float db) noexcept {
    if (db <= -120.0f) return 0.0f;
    return std::pow(10.0f, db * 0.05f);
}

// Deinterleave `src` (frame_count × ch frames) into per-channel buffers `dst`.
// Out-of-range source channels copy from the closest in-range channel; LTC
// virtual channel is handled separately by the caller.
inline void deinterleave_to(const Sample* src,
                            std::size_t   frame_count,
                            ChannelCount  file_ch,
                            Sample* const* dst,
                            ChannelCount  dst_ch) noexcept {
    for (ChannelCount c = 0; c < dst_ch; ++c) {
        Sample* d = dst[c];
        if (c < file_ch) {
            for (std::size_t i = 0; i < frame_count; ++i) {
                d[i] = src[i * file_ch + c];
            }
        } else {
            // The caller may pass extra dst channels (e.g. LTC slot). Silence.
            std::memset(d, 0, frame_count * sizeof(Sample));
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------

PlaybackItem::PlaybackItem(PlaybackItemDesc desc)
    : desc_(std::move(desc)) {
    gain_target_linear_.store(1.0f);
    gain_current_linear_.store(1.0f);
    ltc_enabled_atomic_.store(desc_.ltc_enabled);
    ltc_offset_ns_.store(desc_.ltc_offset.count());
    if (desc_.ltc_enabled) {
        ltc_ = std::make_unique<LTCGenerator>();
        ltc_->configure(desc_.mix_sample_rate, desc_.ltc_frame_rate, desc_.ltc_offset);
    }
}

PlaybackItem::~PlaybackItem() {
    unload();
}

bool PlaybackItem::load() {
    std::lock_guard lock{decoder_mutex_};

    decoder_ready_ = false;
    if (decoder_) {
        ma_decoder_uninit(decoder_.get());
        decoder_.reset();
    }

    decoder_ = std::make_unique<ma_decoder>();
    ma_decoder_config cfg = ma_decoder_config_init(
        ma_format_f32,
        /*channels (0 = native)*/ 0,
        desc_.mix_sample_rate);

    // UTF-8 path for logs and JSON; native open uses the platform's preferred
    // encoding (UTF-16 on Windows so non-ASCII filenames work).
    const std::string path_utf8 = util::path_to_utf8(desc_.file_path);

    // Diagnose path-encoding issues up front: if the path doesn't make it to
    // the filesystem layer intact, miniaudio's "Resource does not exist"
    // becomes ambiguous. fs::exists uses native wide-char syscalls on Windows.
    std::error_code ec;
    const bool path_exists = std::filesystem::exists(desc_.file_path, ec);
    if (!path_exists) {
        Logger::error("PlaybackItem[{}] file not found at '{}' (fs::exists=false, ec={})",
                      desc_.id.value, path_utf8, ec.message());
        decoder_.reset();
        return false;
    }

#if defined(_WIN32)
    const std::wstring path_w = desc_.file_path.wstring();
    const ma_result rv = ma_decoder_init_file_w(path_w.c_str(), &cfg, decoder_.get());
#else
    const std::string path_str = desc_.file_path.string();
    const ma_result rv = ma_decoder_init_file(path_str.c_str(), &cfg, decoder_.get());
#endif
    if (rv != MA_SUCCESS) {
        Logger::error("PlaybackItem[{}] decoder init failed for '{}': {}",
                      desc_.id.value, path_utf8, ma_result_description(rv));
        decoder_.reset();
        return false;
    }

    ma_uint32 ch = 0;
    ma_decoder_get_data_format(decoder_.get(), nullptr, &ch, nullptr, nullptr, 0);
    if (ch == 0) ch = desc_.fallback_channels;
    file_channels_ = static_cast<ChannelCount>(ch);

    const ChannelCount total = file_channels_ + (desc_.ltc_enabled ? 1u : 0u);
    resize_meters(total);

    decoder_ready_ = true;
    Logger::info("PlaybackItem[{}] loaded '{}' ({} ch, {} Hz mix-rate, LTC={})",
                 desc_.id.value, path_utf8, file_channels_,
                 desc_.mix_sample_rate, desc_.ltc_enabled ? "on" : "off");
    return true;
}

void PlaybackItem::unload() {
    stop_now();
    std::lock_guard lock{decoder_mutex_};
    if (decoder_) {
        ma_decoder_uninit(decoder_.get());
        decoder_.reset();
    }
    decoder_ready_ = false;
}

ChannelCount PlaybackItem::source_channel_count() const noexcept {
    return file_channels_ + (ltc_enabled_atomic_.load(std::memory_order_relaxed) ? 1u : 0u);
}

PlaybackItemStats PlaybackItem::stats() const noexcept {
    PlaybackItemStats s;
    s.transport       = transport_.load(std::memory_order_relaxed);
    s.playhead_frame  = playhead_frames_.load(std::memory_order_relaxed);
    s.playhead_seconds =
        static_cast<double>(s.playhead_frame) / static_cast<double>(desc_.mix_sample_rate);
    s.source_channels = source_channel_count();
    s.file_loaded     = decoder_ready_;
    return s;
}

MeterSnapshot PlaybackItem::source_meter(ChannelIndex ch) const noexcept {
    if (ch >= source_meters_.size() || !source_meters_[ch]) return {};
    return source_meters_[ch]->snapshot();
}

MeterSnapshot PlaybackItem::source_meter_consume(ChannelIndex ch) noexcept {
    if (ch >= source_meters_.size() || !source_meters_[ch]) return {};
    return source_meters_[ch]->snapshot_consume_max();
}

void PlaybackItem::play() {
    if (!decoder_ready_) {
        Logger::warn("PlaybackItem[{}] play() ignored — decoder not ready", desc_.id.value);
        return;
    }
    const TransportState st = transport_.load(std::memory_order_acquire);
    if (st == TransportState::Playing || st == TransportState::FadingIn) return;

    // Reset natural-end flags so take_natural_end() doesn't fire for a
    // stale previous play on this same item.
    stopped_naturally_.store(false, std::memory_order_release);
    fading_out_naturally_.store(false, std::memory_order_release);

    // Begin a fade-in if configured; otherwise jump straight to full gain.
    const auto fade = desc_.fade_in_duration;
    if (fade.count() > 0) {
        start_fade(/*from*/ 0.0f,
                   /*to*/   gain_target_linear_.load(),
                   fade,
                   TransportState::FadingIn,
                   TransportState::Playing);
    } else {
        gain_current_linear_.store(gain_target_linear_.load(), std::memory_order_release);
        transport_.store(TransportState::Playing, std::memory_order_release);
    }
}

void PlaybackItem::stop() {
    const TransportState st = transport_.load(std::memory_order_acquire);
    if (st == TransportState::Stopped) return;
    if (st == TransportState::FadingOut) {
        // Honour an explicit stop even when the item is already fading naturally
        // (e.g. reached its out-point). Cutting it immediately prevents audible
        // overlap with the next item that is about to start.
        stop_now();
        return;
    }

    // User-initiated stop: cancel natural-end tracking so the sequencer
    // doesn't auto-advance after this explicit stop.
    fading_out_naturally_.store(false, std::memory_order_release);

    const auto fade = desc_.fade_out_duration;
    if (fade.count() > 0) {
        start_fade(/*from*/ gain_current_linear_.load(),
                   /*to*/   0.0f,
                   fade,
                   TransportState::FadingOut,
                   TransportState::Stopped);
    } else {
        stop_now();
    }
}

void PlaybackItem::stop_now() {
    stopped_naturally_.store(false, std::memory_order_release);
    fading_out_naturally_.store(false, std::memory_order_release);
    transport_.store(TransportState::Stopped, std::memory_order_release);
    gain_current_linear_.store(gain_target_linear_.load(), std::memory_order_release);
    fade_duration_samples_.store(0, std::memory_order_relaxed);
    fade_elapsed_samples_.store(0, std::memory_order_relaxed);
    playhead_frames_.store(0, std::memory_order_relaxed);

    std::lock_guard lock{decoder_mutex_};
    if (decoder_) ma_decoder_seek_to_pcm_frame(decoder_.get(), 0);
    if (ltc_) ltc_->reset(std::chrono::nanoseconds{ltc_offset_ns_.load()});
}

void PlaybackItem::pause() {
    const TransportState st = transport_.load(std::memory_order_acquire);
    if (st == TransportState::Playing || st == TransportState::FadingIn) {
        transport_.store(TransportState::Paused, std::memory_order_release);
    }
}

void PlaybackItem::resume() {
    if (transport_.load(std::memory_order_acquire) == TransportState::Paused) {
        transport_.store(TransportState::Playing, std::memory_order_release);
    }
}

void PlaybackItem::seek_seconds(double seconds) {
    if (seconds < 0) seconds = 0;
    const auto frame = static_cast<ma_uint64>(seconds * desc_.mix_sample_rate);
    {
        std::lock_guard lock{decoder_mutex_};
        if (decoder_) ma_decoder_seek_to_pcm_frame(decoder_.get(), frame);
    }
    playhead_frames_.store(frame, std::memory_order_release);
    if (ltc_) {
        // LTC resyncs lazily inside render_block(), but a hint here keeps the
        // generator's internal frame counter from doing a full rebuild on the
        // first render after a big seek.
        ltc_->reset(std::chrono::nanoseconds{ltc_offset_ns_.load()});
    }
}

void PlaybackItem::set_gain_db(float db) noexcept {
    const float lin = db_to_lin(db);
    gain_target_linear_.store(lin, std::memory_order_release);
    // Don't disturb an in-flight fade; the fade end-point reflects this value
    // by virtue of being captured on play()/stop(). If we're stopped, snap
    // so the next play() picks up the new value cleanly. While Playing/Paused
    // we leave gain_current_linear_ alone — render_block() smoothly slews it
    // toward the new target (see the no-fade branch there), which removes
    // the audible step on duck-others and on UI fader moves.
    const TransportState st = transport_.load(std::memory_order_acquire);
    if (st == TransportState::Stopped) {
        gain_current_linear_.store(lin, std::memory_order_release);
    }
}

void PlaybackItem::set_ltc_enabled(bool enabled) {
    if (enabled == ltc_enabled_atomic_.load()) return;
    {
        std::lock_guard lock{decoder_mutex_};
        if (enabled) {
            if (!ltc_) ltc_ = std::make_unique<LTCGenerator>();
            ltc_->configure(desc_.mix_sample_rate, desc_.ltc_frame_rate,
                            std::chrono::nanoseconds{ltc_offset_ns_.load()});
        }
        desc_.ltc_enabled = enabled;
        ltc_enabled_atomic_.store(enabled, std::memory_order_release);
        resize_meters(file_channels_ + (enabled ? 1u : 0u));
    }
}

void PlaybackItem::set_ltc_frame_rate(LTCFrameRate fr) {
    std::lock_guard lock{decoder_mutex_};
    desc_.ltc_frame_rate = fr;
    if (ltc_) {
        ltc_->configure(desc_.mix_sample_rate, fr,
                        std::chrono::nanoseconds{ltc_offset_ns_.load()});
    }
}

void PlaybackItem::set_ltc_offset(std::chrono::nanoseconds offset) noexcept {
    ltc_offset_ns_.store(offset.count(), std::memory_order_release);
}

void PlaybackItem::set_out_point_seconds(double seconds) noexcept {
    if (seconds <= 0.0) {
        out_point_frames_.store(0, std::memory_order_release);
        return;
    }
    const auto f = static_cast<std::uint64_t>(seconds *
                                              static_cast<double>(desc_.mix_sample_rate));
    out_point_frames_.store(f, std::memory_order_release);
}

void PlaybackItem::set_loop(bool enabled, double in_seconds) noexcept {
    const auto rate = static_cast<double>(desc_.mix_sample_rate);
    const auto in_frames = (in_seconds <= 0.0)
        ? std::uint64_t{0}
        : static_cast<std::uint64_t>(in_seconds * rate);
    loop_in_frames_.store(in_frames, std::memory_order_release);
    loop_enabled_.store(enabled, std::memory_order_release);
}

bool PlaybackItem::prime(double seconds, double start_seconds) noexcept {
    std::lock_guard lock{decoder_mutex_};
    if (!decoder_ || !decoder_ready_) return false;

    const auto start_frame = (start_seconds <= 0.0)
        ? ma_uint64{0}
        : static_cast<ma_uint64>(start_seconds *
                                 static_cast<double>(desc_.mix_sample_rate));
    if (start_frame > 0) ma_decoder_seek_to_pcm_frame(decoder_.get(), start_frame);

    // Read and discard `seconds` of audio. We only care that the OS file cache
    // and miniaudio's internal demuxer state are warmed up — the data goes
    // nowhere. After the warm read, seek back to start_frame so the first
    // real read during playback returns the correct position.
    const std::size_t frames_per_chunk = 4096;
    const std::size_t frames_total     = static_cast<std::size_t>(
        seconds * static_cast<double>(desc_.mix_sample_rate));
    if (frames_total == 0) {
        if (start_frame > 0) ma_decoder_seek_to_pcm_frame(decoder_.get(), start_frame);
        return true;
    }

    std::vector<float> discard(frames_per_chunk *
                               std::max<std::size_t>(1, file_channels_), 0.0f);

    std::size_t frames_consumed = 0;
    while (frames_consumed < frames_total) {
        const std::size_t to_read = std::min(frames_per_chunk,
                                             frames_total - frames_consumed);
        ma_uint64 got = 0;
        const ma_result rv = ma_decoder_read_pcm_frames(
            decoder_.get(), discard.data(), static_cast<ma_uint64>(to_read), &got);
        if (rv != MA_SUCCESS && rv != MA_AT_END) break;
        if (got == 0) break;
        frames_consumed += static_cast<std::size_t>(got);
        if (rv == MA_AT_END) break;
    }
    // Rewind to start_frame so playback resumes at the expected position.
    ma_decoder_seek_to_pcm_frame(decoder_.get(), start_frame);
    // Resync the engine's playhead counter to match the decoder. Without
    // this, a second play of an item that previously reached EOF would
    // start with playhead_frames_ at the file end — render_block would
    // see new_playhead >= out_point on the very first block and fire a
    // bogus natural-end immediately. The decoder keeps yielding audio
    // (from in_point onward) during the fade, so the symptom is "audio
    // continues playing even though the UI thinks the cue stopped" and
    // the up-next item never triggers because the fade gets clipped.
    playhead_frames_.store(start_frame, std::memory_order_release);
    // Also clear any stale natural-end flags from a prior playthrough so
    // the sequencer doesn't immediately consume one before we even play.
    stopped_naturally_.store(false, std::memory_order_release);
    fading_out_naturally_.store(false, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------

void PlaybackItem::start_fade(float from_lin, float to_lin,
                              std::chrono::milliseconds dur,
                              TransportState during,
                              TransportState after_complete) noexcept {
    const long long duration_samples = std::max<long long>(
        0, static_cast<long long>(dur.count()) *
               static_cast<long long>(desc_.mix_sample_rate) / 1000);

    fade_start_linear_.store(from_lin, std::memory_order_relaxed);
    fade_end_linear_  .store(to_lin,   std::memory_order_relaxed);
    fade_duration_samples_.store(duration_samples, std::memory_order_relaxed);
    fade_elapsed_samples_.store(0, std::memory_order_relaxed);
    gain_current_linear_.store(from_lin, std::memory_order_release);
    transport_.store(during, std::memory_order_release);

    // We pre-store `after_complete` into a static-ish slot by encoding it in
    // a sign convention: positive duration + transport==FadingIn → Playing
    // afterwards; FadingOut → Stopped afterwards. Render block reads transport_
    // to decide. So we don't need to store after_complete separately.
    (void)after_complete;
}

void PlaybackItem::resize_meters(ChannelCount n) {
    source_meters_.resize(n);
    for (ChannelCount i = 0; i < n; ++i) {
        if (!source_meters_[i]) source_meters_[i] = std::make_unique<Meter>();
        source_meters_[i]->configure(desc_.mix_sample_rate, meter_ballistics_);
        source_meters_[i]->set_true_peak_enabled(meter_true_peak_);
    }
}

void PlaybackItem::set_meter_ballistics(const MeterBallistics& b) noexcept {
    meter_ballistics_ = b;
    for (auto& m : source_meters_) {
        if (m) m->configure(desc_.mix_sample_rate, b);
    }
}

void PlaybackItem::set_true_peak_metering(bool enabled) noexcept {
    meter_true_peak_ = enabled;
    for (auto& m : source_meters_) {
        if (m) m->set_true_peak_enabled(enabled);
    }
}

// ---------------------------------------------------------------------------

std::size_t PlaybackItem::render_block(Sample* const* out_channel_buffers,
                                       ChannelCount   out_channel_count,
                                       std::size_t    frame_count) noexcept {
    // Silence first so early-outs leave well-defined buffers behind.
    for (ChannelCount c = 0; c < out_channel_count; ++c) {
        std::memset(out_channel_buffers[c], 0, frame_count * sizeof(Sample));
    }

    const TransportState st = transport_.load(std::memory_order_acquire);
    if (st == TransportState::Stopped || st == TransportState::Paused) return 0;
    if (!decoder_ready_) return 0;

    // Try-lock the decoder. If the control thread is currently swapping the
    // decoder (load/unload/seek), we silently skip this block — we never
    // block the audio thread.
    std::unique_lock<std::mutex> lock{decoder_mutex_, std::try_to_lock};
    if (!lock.owns_lock() || !decoder_) return 0;

    // ---- Decode interleaved file audio ----
    static thread_local std::vector<Sample> interleaved;
    const std::size_t needed = frame_count * static_cast<std::size_t>(file_channels_);
    if (interleaved.size() < needed) interleaved.resize(needed);

    ma_uint64 frames_read = 0;
    const ma_result rv = ma_decoder_read_pcm_frames(
        decoder_.get(),
        interleaved.data(),
        static_cast<ma_uint64>(frame_count),
        &frames_read);
    if (rv != MA_SUCCESS && rv != MA_AT_END) frames_read = 0;

    deinterleave_to(interleaved.data(),
                    static_cast<std::size_t>(frames_read),
                    file_channels_,
                    out_channel_buffers,
                    std::min<ChannelCount>(out_channel_count, file_channels_));

    // ---- LTC virtual channel ----
    const bool ltc_on = ltc_enabled_atomic_.load(std::memory_order_acquire);
    if (ltc_on && ltc_ && out_channel_count > file_channels_) {
        const long long playhead_frames =
            static_cast<long long>(playhead_frames_.load(std::memory_order_relaxed));
        const auto playhead_ns = std::chrono::nanoseconds{
            playhead_frames * 1'000'000'000LL / static_cast<long long>(desc_.mix_sample_rate)};
        // Refresh offset in case the control thread changed it.
        ltc_->configure(desc_.mix_sample_rate, desc_.ltc_frame_rate,
                        std::chrono::nanoseconds{ltc_offset_ns_.load(std::memory_order_acquire)},
                        /*amplitude*/ 0.5f);
        ltc_->render_block(out_channel_buffers[file_channels_], frame_count, playhead_ns);
    }

    // ---- Per-item gain + fade envelope ----
    // Compute the gain sample-by-sample. We keep the math simple: linear ramp.
    float gain_now = gain_current_linear_.load(std::memory_order_acquire);
    float gain_end = gain_now;
    bool  fade_active = (st == TransportState::FadingIn) || (st == TransportState::FadingOut);
    long long fade_total   = fade_duration_samples_.load(std::memory_order_relaxed);
    long long fade_elapsed = fade_elapsed_samples_.load(std::memory_order_relaxed);
    const float fade_from  = fade_start_linear_.load(std::memory_order_relaxed);
    const float fade_to    = fade_end_linear_.load(std::memory_order_relaxed);

    if (fade_active && fade_total > 0) {
        const long long advance = static_cast<long long>(frame_count);
        const long long new_elapsed = std::min<long long>(fade_elapsed + advance, fade_total);
        const float t_start = static_cast<float>(fade_elapsed) / static_cast<float>(fade_total);
        const float t_end   = static_cast<float>(new_elapsed)  / static_cast<float>(fade_total);
        gain_now = fade_from + (fade_to - fade_from) * t_start;
        gain_end = fade_from + (fade_to - fade_from) * t_end;

        fade_elapsed_samples_.store(new_elapsed, std::memory_order_relaxed);
        if (new_elapsed >= fade_total) {
            gain_current_linear_.store(fade_to, std::memory_order_release);
            // Transition state machine.
            if (st == TransportState::FadingIn) {
                transport_.store(TransportState::Playing, std::memory_order_release);
            } else {  // FadingOut
                transport_.store(TransportState::Stopped, std::memory_order_release);
                // If this fade was triggered by natural EOF/out-point,
                // signal the sequencer so it can auto-advance.
                if (fading_out_naturally_.exchange(false, std::memory_order_acq_rel)) {
                    stopped_naturally_.store(true, std::memory_order_release);
                }
            }
        } else {
            gain_current_linear_.store(gain_end, std::memory_order_release);
        }
    } else {
        // No fade active — but if set_gain_db() changed the *target* while we
        // were playing (e.g. auto-duck "duck-others", or a UI fader move), the
        // current and target diverge. Smoothly slew current toward target over
        // a short, fixed window. Without this, ducking is an audible step
        // (-20 dB in one sample is a click on every transition) and per-item
        // volume slider moves on the UI also click.
        const float target = gain_target_linear_.load(std::memory_order_acquire);
        if (target != gain_now) {
            // ~50 ms slew at the configured mix rate. Long enough to remove
            // any click, short enough that "ducking now" still feels immediate.
            constexpr long long kRampMs = 50;
            const long long ramp_samples = std::max<long long>(1,
                static_cast<long long>(desc_.mix_sample_rate) * kRampMs / 1000);
            const long long advance = static_cast<long long>(frame_count);
            if (advance >= ramp_samples) {
                gain_end = target;
            } else {
                const float alpha =
                    static_cast<float>(advance) / static_cast<float>(ramp_samples);
                gain_end = gain_now + (target - gain_now) * alpha;
            }
            gain_current_linear_.store(gain_end, std::memory_order_release);
        }
    }

    // Apply linear gain ramp across the block. Per-sample cosine would be
    // smoother but per-block linear is inaudible at the fade durations we use
    // and saves CPU.
    if (frames_read > 0) {
        for (ChannelCount c = 0; c < std::min<ChannelCount>(out_channel_count, file_channels_); ++c) {
            Sample* buf = out_channel_buffers[c];
            const std::size_t n = static_cast<std::size_t>(frames_read);
            if (n == 0) continue;
            const float dg = (gain_end - gain_now) / static_cast<float>(std::max<std::size_t>(n, 1));
            float g = gain_now;
            for (std::size_t i = 0; i < n; ++i) {
                buf[i] *= g;
                g += dg;
            }
        }
    }
    // Apply gain to LTC channel too (so item fade affects LTC level uniformly).
    if (ltc_on && out_channel_count > file_channels_) {
        Sample* buf = out_channel_buffers[file_channels_];
        const std::size_t n = frame_count;
        const float dg = (gain_end - gain_now) / static_cast<float>(std::max<std::size_t>(n, 1));
        float g = gain_now;
        for (std::size_t i = 0; i < n; ++i) {
            buf[i] *= g;
            g += dg;
        }
    }

    // ---- Update per-source-channel meters ----
    const ChannelCount meter_n = std::min<ChannelCount>(out_channel_count,
                                                       source_channel_count());
    for (ChannelCount c = 0; c < meter_n; ++c) {
        if (c < source_meters_.size() && source_meters_[c]) {
            source_meters_[c]->push_block(out_channel_buffers[c], frame_count);
        }
    }

    // ---- Advance playhead, handle EOF + soft out-point ----
    const std::uint64_t new_playhead = playhead_frames_.fetch_add(
        frames_read, std::memory_order_relaxed) + frames_read;

    const std::uint64_t out_point = out_point_frames_.load(std::memory_order_acquire);
    const bool hit_out_point = (out_point > 0) && (new_playhead >= out_point);

    if (rv == MA_AT_END || frames_read < frame_count || hit_out_point) {
        // Seamless loop: seek the decoder back to the in-point and reset the
        // playhead counter. Transport stays Playing — so the broadcast thread
        // never sees a transient Stopped edge mid-loop, which would otherwise
        // cause the client UI to drop the cue from "currently playing" and grey
        // out its stop button for the duration of the gap.
        if (loop_enabled_.load(std::memory_order_acquire)) {
            const auto in_frame = loop_in_frames_.load(std::memory_order_acquire);
            ma_decoder_seek_to_pcm_frame(decoder_.get(), static_cast<ma_uint64>(in_frame));
            playhead_frames_.store(in_frame, std::memory_order_release);
            // Don't enter FadingOut; the cue is still playing.
        } else {
            // Natural end-of-file, soft out-point, or short read all route through
            // the same fade-out logic so transport semantics are consistent
            // regardless of how playback terminated.
            //
            // Arm this ONLY from an actively-playing state. Re-entering from
            // FadingOut would restart an in-flight fade; re-entering from Stopped
            // is the real hazard: when a crossfade (or stop-fade) completes the
            // fade envelope sets transport=Stopped, and if the playhead is already
            // at/past a soft out-point that sits *before* end-of-file,
            // hit_out_point stays true on every subsequent block — so we would
            // restart the fade-out immediately, producing an endless
            // FadingOut⇄Stopped oscillation. A crossfaded-out cue is no longer
            // tracked by the sequencer, so nothing calls engine stop() to reset
            // the playhead and break it; the cue never settles to Stopped and the
            // client keeps showing it as playing.
            const auto cur = transport_.load(std::memory_order_acquire);
            if (cur == TransportState::Playing || cur == TransportState::FadingIn) {
                fading_out_naturally_.store(true, std::memory_order_release);
                const auto fade = desc_.fade_out_duration;
                if (fade.count() > 0) {
                    start_fade(gain_current_linear_.load(), 0.0f, fade,
                               TransportState::FadingOut, TransportState::Stopped);
                } else {
                    fading_out_naturally_.store(false, std::memory_order_release);
                    stopped_naturally_.store(true, std::memory_order_release);
                    transport_.store(TransportState::Stopped, std::memory_order_release);
                }
            }
        }
    }

    return static_cast<std::size_t>(frames_read);
}

bool PlaybackItem::take_natural_end() noexcept {
    return stopped_naturally_.exchange(false, std::memory_order_acq_rel);
}

float PlaybackItem::gain_db() const noexcept {
    const float lin = gain_target_linear_.load(std::memory_order_acquire);
    if (lin <= 0.0f) return -120.0f;
    return 20.0f * std::log10(lin);
}

void PlaybackItem::stop_with_fade(std::chrono::milliseconds dur) {
    const TransportState st = transport_.load(std::memory_order_acquire);
    if (st == TransportState::Stopped || st == TransportState::FadingOut) return;
    // Not a natural end — don't set fading_out_naturally_ so the sequencer
    // won't auto-advance on this particular fade completion.
    if (dur.count() > 0) {
        start_fade(gain_current_linear_.load(), 0.0f, dur,
                   TransportState::FadingOut, TransportState::Stopped);
    } else {
        stop_now();
    }
}

} // namespace liveplay::audio
