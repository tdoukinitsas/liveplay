// ============================================================================
// engine.cpp — see engine.hpp.
// ============================================================================
#include "liveplay/audio/engine.hpp"
#include "liveplay/logger.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <string>

#if defined(_WIN32)
#  include <windows.h>   // SetThreadPriority
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#  include <xmmintrin.h>   // _MM_SET_FLUSH_ZERO_MODE
#  include <pmmintrin.h>   // _MM_SET_DENORMALS_ZERO_MODE
#  define LIVEPLAY_HAVE_SSE_DENORMAL 1
#endif

namespace liveplay::audio {

namespace {

inline float db_to_lin(float db) noexcept {
    if (db <= -120.0f) return 0.0f;
    return std::pow(10.0f, db * 0.05f);
}

std::string gen_uuid_like() {
    static std::atomic<std::uint64_t> counter{0};
    thread_local std::mt19937_64 rng{
        std::random_device{}() ^ static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
    const auto a = rng();
    const auto b = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx-%016llx",
                  static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return std::string{buf};
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------
AudioEngine::AudioEngine(EngineConfig cfg) : cfg_(cfg) {
    // Zero means "unset" — fall back to the default without comment. A value
    // that was set but is unusable is a misconfiguration, so it gets clamped
    // loudly rather than silently producing a bus too narrow to route.
    if (cfg_.master_channels == 0) cfg_.master_channels = kDefaultMasterChannels;
    if (cfg_.render_block    == 0) cfg_.render_block    = kDefaultRenderBlock;
    if (cfg_.mix_sample_rate == 0) cfg_.mix_sample_rate = kDefaultMixSampleRate;

    if (cfg_.master_channels < kMinMasterChannels) {
        Logger::warn("AudioEngine: master_channels={} is below the minimum of {} "
                     "(masters 0/1 are the default Main output and the top {} are "
                     "reserved for Preview); clamping to {}",
                     cfg_.master_channels, kMinMasterChannels,
                     kReservedPreviewChannels, kMinMasterChannels);
        cfg_.master_channels = kMinMasterChannels;
    }

    master_state_.resize(cfg_.master_channels);
    for (auto& ms : master_state_) {
        ms.limiter = std::make_unique<Limiter>();
        ms.meter   = std::make_unique<Meter>();
        ms.limiter->configure(cfg_.mix_sample_rate, cfg_.master_ceiling_db);
        ms.meter->configure(cfg_.mix_sample_rate);
    }
    pending_.master_destinations.resize(cfg_.master_channels);

    // Publish an empty topology so the render thread has something to read.
    auto initial = std::make_shared<Topology>();
    initial->masters.resize(cfg_.master_channels);
    for (MasterChannelIndex i = 0; i < cfg_.master_channels; ++i) initial->masters[i].index = i;
    topology_.store(std::move(initial));
}

AudioEngine::~AudioEngine() {
    stop();
    std::lock_guard lock{mutex_};
    for (auto& dev : devices_) {
        if (dev->ma_dev) {
            ma_device_uninit(dev->ma_dev.get());
        }
        if (dev->ring) {
            ma_pcm_rb_uninit(dev->ring.get());
        }
    }
    devices_.clear();
}

bool AudioEngine::start() {
    if (running_.exchange(true)) return true;
    Logger::info("AudioEngine: starting (mix {} Hz, block {} frames, {} master ch, ceiling {:.1f} dB)",
                 cfg_.mix_sample_rate, cfg_.render_block, cfg_.master_channels,
                 cfg_.master_ceiling_db);

    // Pre-allocate scratch buffers sized for the worst-case topology. Sizing
    // for the cap here — rather than growing to fit the live mixer count in
    // render_one_block() — is what keeps the render thread allocation-free:
    // create_mixer_channel() refuses to exceed max_mixer_channels, so these can
    // never come up short mid-block.
    mixer_accumulators_.assign(static_cast<std::size_t>(cfg_.max_mixer_channels) * kMixerLanes,
                               std::vector<Sample>(cfg_.render_block, 0.0f));
    master_accumulators_.assign(cfg_.master_channels,
                                std::vector<Sample>(cfg_.render_block, 0.0f));

    // Initialise per-output-channel gains to unity (0 dB).
    output_channel_gains_.assign(cfg_.master_channels, 1.0f);

    render_thread_ = std::thread([this] { render_loop(); });

#if defined(_WIN32)
    // Lift the render thread above generic worker threads so consumption_counter_
    // notifications wake us promptly even when the system is under load. The
    // device callback already runs at MMCSS / RT priority — staying near it
    // limits scheduling jitter that would otherwise underrun the ring.
    SetThreadPriority(render_thread_.native_handle(), THREAD_PRIORITY_HIGHEST);
#endif

    // Bring up the default routing so a freshly-loaded cue can be heard
    // without any explicit routing API calls from the client.
    ensure_default_routing();
    return true;
}

void AudioEngine::stop() {
    if (!running_.exchange(false)) return;
    // Kick the render thread out of any consumption_counter_.wait().
    consumption_counter_.fetch_add(1, std::memory_order_release);
    consumption_counter_.notify_all();
    if (render_thread_.joinable()) render_thread_.join();
    Logger::info("AudioEngine: stopped.");
}

// ---------------------------------------------------------------------------
// Topology snapshot management
// ---------------------------------------------------------------------------
std::shared_ptr<const Topology> AudioEngine::snapshot_topology() const noexcept {
    return topology_.load();
}

void AudioEngine::publish_topology(std::shared_ptr<const Topology> snap) {
    topology_.store(std::move(snap));
}

void AudioEngine::rebuild_topology_locked() {
    topology_rebuilds_.fetch_add(1, std::memory_order_relaxed);
    auto snap = std::make_shared<Topology>();

    // ---- Items ----
    snap->items.reserve(items_.size());
    for (auto& [id_str, item] : items_) {
        ItemRouteEntry entry;
        entry.item = item;

        const ChannelCount n_src = item->source_channel_count();
        entry.per_source_channel.resize(n_src);

        // Find sends for this item, expanding kAllMixerLanes into one send
        // per concrete lane so the render loop never branches on it.
        auto it = pending_.item_sources.find(id_str);
        if (it != pending_.item_sources.end()) {
            const auto& isr = it->second.by_source_channel;
            for (ChannelIndex c = 0; c < n_src; ++c) {
                if (c >= isr.size()) continue;
                for (const auto& send : isr[c]) {
                    auto mit = mixers_.find(send.mixer.value);
                    if (mit == mixers_.end()) continue;
                    if (send.lane == kAllMixerLanes) {
                        for (ChannelIndex l = 0; l < kMixerLanes; ++l) {
                            entry.per_source_channel[c].sends.push_back(
                                {mit->second, l, send.gain_lin});
                        }
                    } else if (send.lane < kMixerLanes) {
                        entry.per_source_channel[c].sends.push_back(
                            {mit->second, send.lane, send.gain_lin});
                    }
                }
            }
        }
        snap->items.emplace_back(std::move(entry));
    }

    // ---- Masters ----
    snap->masters.resize(cfg_.master_channels);
    for (MasterChannelIndex m = 0; m < cfg_.master_channels; ++m) {
        snap->masters[m].index       = m;
        snap->masters[m].destination = pending_.master_destinations[m];
    }
    for (auto& [mixer_id_str, master_sends] : pending_.mixer_to_master) {
        auto mit = mixers_.find(mixer_id_str);
        if (mit == mixers_.end()) continue;
        for (auto& send : master_sends) {
            if (send.master >= cfg_.master_channels) continue;
            if (send.lane == kAllMixerLanes) {
                for (ChannelIndex l = 0; l < kMixerLanes; ++l) {
                    snap->masters[send.master].sends.push_back(
                        {mit->second, l, send.gain_lin});
                }
            } else if (send.lane < kMixerLanes) {
                snap->masters[send.master].sends.push_back(
                    {mit->second, send.lane, send.gain_lin});
            }
        }
    }

    // ---- Monitor + PFL taps ----
    // PFL is state on the strip, not a route the caller has to maintain, so
    // the tap list is derived here rather than stored. That keeps one source
    // of truth: raise the flag, and the next snapshot carries the edge.
    if (!monitor_mixer_.empty()) {
        auto mon = mixers_.find(monitor_mixer_.value);
        if (mon != mixers_.end()) {
            snap->monitor = mon->second;
            for (const auto& [id_str, m] : mixers_) {
                if (!m->is_pfl() || id_str == monitor_mixer_.value) continue;
                append_monitor_taps(snap->monitor_taps, m);
            }
        }
    }

    publish_topology(std::move(snap));
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------
std::vector<DeviceInfo> AudioEngine::enumerate_devices() const {
    std::vector<DeviceInfo> out;

    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return out;

    ma_device_info* playback_infos = nullptr;
    ma_uint32       playback_count = 0;
    if (ma_context_get_devices(&ctx, &playback_infos, &playback_count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < playback_count; ++i) {
            DeviceInfo info;
            info.id            = DeviceId{playback_infos[i].name};   // by-name id is portable
            info.display_name  = playback_infos[i].name;
            info.channel_count = 0;
            info.sample_rate   = 0;
            info.is_default    = (playback_infos[i].isDefault != 0);

            // Pull fuller info for the channel / rate fields.
            ma_device_info full;
            if (ma_context_get_device_info(&ctx, ma_device_type_playback,
                                           &playback_infos[i].id, &full) == MA_SUCCESS) {
                info.channel_count = full.nativeDataFormatCount > 0
                                         ? full.nativeDataFormats[0].channels : 2;
                info.sample_rate   = full.nativeDataFormatCount > 0
                                         ? full.nativeDataFormats[0].sampleRate : 48000;
            }
            out.emplace_back(std::move(info));
        }
    }
    ma_context_uninit(&ctx);
    return out;
}

// miniaudio data callback shared by all opened devices. Runs on the device's
// real-time audio thread. Drains the per-device ring buffer into miniaudio's
// output and — after consuming any samples — notifies the engine's render
// thread to refill the ring. This is the consumer side of our device-callback-
// driven synchronisation: the device clock dictates production cadence.
void AudioEngine::ma_data_callback(ma_device* dev,
                                   void* out,
                                   const void* /*in*/,
                                   std::uint32_t frames) {
    auto* device = reinterpret_cast<Device*>(dev->pUserData);
    if (!device) {
        // No device context — emit silence based on what miniaudio asked for.
        // We don't know channel count without `device`, so use the ma_device's
        // configured channel count to fill the right number of bytes.
        std::memset(out, 0, frames * dev->playback.channels * sizeof(Sample));
        return;
    }
    if (!device->ring) {
        std::memset(out, 0, frames * device->channels * sizeof(Sample));
        return;
    }

    Sample* dst = static_cast<Sample*>(out);
    std::uint32_t remaining = frames;
    bool consumed_any = false;
    while (remaining > 0) {
        void*         buf       = nullptr;
        ma_uint32     available = remaining;
        if (ma_pcm_rb_acquire_read(device->ring.get(), &available, &buf) != MA_SUCCESS) break;
        if (available == 0) {
            // Ring underrun — render thread isn't keeping up with hardware consumption.
            // Fill with silence so operators hear silence instead of garbage.
            if (device->engine) {
                device->engine->underruns_.fetch_add(1, std::memory_order_relaxed);
            }
            if (remaining == frames) {
                // Entire block is missing — likely indicates a scheduling hiccup
                Logger::warn("Ring underrun on device '{}': {} frames starved",
                             device->display_name, remaining);
            }
            std::memset(dst, 0, remaining * device->channels * sizeof(Sample));
            break;
        }
        std::memcpy(dst, buf, available * device->channels * sizeof(Sample));
        ma_pcm_rb_commit_read(device->ring.get(), available);
        dst       += available * device->channels;
        remaining -= available;
        consumed_any = true;
    }

    // Signal the render thread that ring space has been freed. fetch_add is
    // wait-free; notify_one() on a counter only takes a futex-style fast path
    // when a waiter exists. Safe to call from the RT callback.
    if (consumed_any && device->engine) {
        device->engine->consumption_counter_.fetch_add(1, std::memory_order_release);
        device->engine->consumption_counter_.notify_one();
    }
}

DeviceId AudioEngine::open_default_device(ChannelCount output_channels) {
    return open_device_by_name("", output_channels);
}

DeviceId AudioEngine::open_device_by_name(const std::string& name_substring,
                                          ChannelCount output_channels) {
    // Hand back a device already open on the same hardware rather than opening
    // a second handle to it.
    //
    // Nothing here ever closed one, so every caller minted a fresh ma_device:
    // wiring a stereo bus opened the same card twice (once per channel), and
    // re-materialising the buses — which a project save does — opened two more
    // every time. The handles accumulated for the life of the process, each
    // with its own callback pulling from one render thread, which is what the
    // "Ring underrun" storm was.
    //
    // Sharing is safe because routing never closes a device; only the explicit
    // DELETE /api/devices endpoint does.
    {
        const std::string want = name_substring.empty() ? "Default Output" : name_substring;
        std::lock_guard lock{mutex_};
        for (const auto& d : devices_) {
            if (d->display_name == want && d->channels == output_channels) {
                return d->id;
            }
        }
    }

    auto dev = std::make_unique<Device>();
    dev->id           = DeviceId{gen_uuid_like()};
    dev->channels     = output_channels;
    dev->sample_rate  = cfg_.mix_sample_rate;
    dev->display_name = name_substring.empty() ? "Default Output" : name_substring;
    dev->ma_dev       = std::make_unique<ma_device>();
    dev->ring         = std::make_unique<ma_pcm_rb>();
    dev->engine       = this;                       // for callback → consumption_counter_

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = output_channels;
    cfg.sampleRate         = cfg_.mix_sample_rate;
    cfg.dataCallback       = &AudioEngine::ma_data_callback;
    cfg.pUserData          = dev.get();
    cfg.periodSizeInFrames = static_cast<ma_uint32>(cfg_.render_block);

    // For name-substring matching we walk enumerated devices and pick the
    // first whose name contains the substring (case-insensitive). Empty
    // substring → leave pDeviceID null = default device.
    ma_context ctx;
    ma_device_id matched_id;
    bool         have_match = false;
    if (!name_substring.empty() && ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
        ma_device_info* infos = nullptr;
        ma_uint32       count = 0;
        if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
            std::string needle = name_substring;
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            for (ma_uint32 i = 0; i < count; ++i) {
                std::string haystack = infos[i].name;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                if (haystack.find(needle) != std::string::npos) {
                    matched_id     = infos[i].id;
                    cfg.playback.pDeviceID = &matched_id;
                    dev->display_name = infos[i].name;
                    have_match = true;
                    break;
                }
            }
        }
        ma_context_uninit(&ctx);
        if (!have_match) {
            Logger::warn("Device matching '{}' not found, falling back to default.",
                         name_substring);
        }
    }

    if (ma_device_init(nullptr, &cfg, dev->ma_dev.get()) != MA_SUCCESS) {
        Logger::error("ma_device_init failed for '{}'", dev->display_name);
        return {};
    }

    // The ring is allocated AFTER the device, because its size depends on what
    // the device settled on rather than on what it was asked for. The request
    // above is a hint: WASAPI shared mode has its own minimum and quietly runs
    // a larger period — 480 frames x 3 on the machine this was written on,
    // against a 256-frame request.
    //
    // Its depth IS the output latency, because the render thread fills it
    // whenever there is room and so it sits full in steady state. It is also
    // the entire margin against a slow decode, since decoding runs on the
    // render thread: the two are the same number and cannot be tuned apart.
    //
    // Floored at three device periods whatever the configuration says. A ring
    // shallower than the callback's own appetite starves it on the first
    // request regardless of how fast the render thread is, which would turn a
    // low --ring-blocks into continuous dropouts rather than low latency.
    const ma_uint32 period = dev->ma_dev->playback.internalPeriodSizeInFrames;
    const ma_uint32 wanted =
        static_cast<ma_uint32>(cfg_.render_block * (cfg_.ring_blocks + 1));
    const ma_uint32 floor_frames = period > 0 ? period * 3 : 0;
    const ma_uint32 ring_frames  = std::max(wanted, floor_frames);
    if (ring_frames > wanted) {
        Logger::info("Device '{}' runs {} frame periods; ring raised to {} frames "
                     "({:.0f} ms) to stay ahead of it",
                     dev->display_name, period, ring_frames,
                     (static_cast<double>(ring_frames) / cfg_.mix_sample_rate) * 1000.0);
    }
    if (ma_pcm_rb_init(ma_format_f32, output_channels, ring_frames,
                       nullptr, nullptr, dev->ring.get()) != MA_SUCCESS) {
        Logger::error("Failed to allocate ring buffer for device '{}'", dev->display_name);
        ma_device_uninit(dev->ma_dev.get());
        return {};
    }

    if (ma_device_start(dev->ma_dev.get()) != MA_SUCCESS) {
        Logger::error("ma_device_start failed for '{}'", dev->display_name);
        ma_device_uninit(dev->ma_dev.get());
        ma_pcm_rb_uninit(dev->ring.get());
        return {};
    }

    dev->scratch.assign(cfg_.render_block * output_channels, 0.0f);
    dev->started.store(true);

    DeviceId id = dev->id;
    {
        std::lock_guard lock{mutex_};
        devices_.emplace_back(std::move(dev));
    }
    // Wake render thread: when we boot with no devices it idles on a coarse
    // timer; opening the first device should kick it into the live path
    // immediately so the first audio block lands before the device starves.
    consumption_counter_.fetch_add(1, std::memory_order_release);
    consumption_counter_.notify_all();

    Logger::success("Opened audio device '{}' ({} ch @ {} Hz) → DeviceId {}",
                    devices_.back()->display_name, output_channels,
                    cfg_.mix_sample_rate, id.value);
    return id;
}

void AudioEngine::close_device(const DeviceId& id) {
    {
        std::lock_guard lock{mutex_};
        auto it = std::find_if(devices_.begin(), devices_.end(),
                               [&](const std::unique_ptr<Device>& d){ return d->id == id; });
        if (it == devices_.end()) return;
        if ((*it)->ma_dev) ma_device_uninit((*it)->ma_dev.get());
        if ((*it)->ring)   ma_pcm_rb_uninit((*it)->ring.get());
        Logger::info("Closed audio device '{}'", (*it)->display_name);
        devices_.erase(it);

        // Drop any master assignments that pointed at this device.
        for (auto& dest : pending_.master_destinations) {
            if (dest && dest->device == id) dest.reset();
        }
        rebuild_topology_locked();
    }
    // Wake the render thread so it re-evaluates state (in particular, if this
    // was the last device it should drop into the idle-timer path).
    consumption_counter_.fetch_add(1, std::memory_order_release);
    consumption_counter_.notify_all();
}

AudioEngine::Device* AudioEngine::find_device_locked(const DeviceId& id) const {
    for (const auto& d : devices_) if (d->id == id) return d.get();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------
CueId AudioEngine::load_cue(const std::filesystem::path& file_path,
                            std::optional<CueId> requested_id) {
    PlaybackItemDesc desc;
    desc.id              = requested_id.value_or(CueId{gen_uuid_like()});
    desc.file_path       = file_path;
    desc.mix_sample_rate = cfg_.mix_sample_rate;
    desc.render_block    = cfg_.render_block;

    auto item = std::make_shared<PlaybackItem>(std::move(desc));
    if (!item->load()) return {};

    {
        std::lock_guard lock{mutex_};
        item->set_meter_ballistics(meter_ballistics_);
        item->set_true_peak_metering(meter_true_peak_);
        item->set_loudness_metering(meter_loudness_);
        items_[item->id().value] = item;
        pending_.item_sources[item->id().value]
            .by_source_channel
            .resize(item->source_channel_count());
        rebuild_topology_locked();
    }
    // Bring the new cue into the default routing (no-op if already wired).
    ensure_default_routing();
    return item->id();
}

CueId AudioEngine::load_cue_no_route(const std::filesystem::path& file_path,
                                     std::optional<CueId> requested_id) {
    PlaybackItemDesc desc;
    desc.id              = requested_id.value_or(CueId{gen_uuid_like()});
    desc.file_path       = file_path;
    desc.mix_sample_rate = cfg_.mix_sample_rate;
    desc.render_block    = cfg_.render_block;

    auto item = std::make_shared<PlaybackItem>(std::move(desc));
    if (!item->load()) return {};

    // Register the cue, but do NOT call rebuild_topology_locked() or
    // ensure_default_routing() — both walk every loaded cue and turn a bulk
    // load into O(N²) work. The caller is responsible for invoking
    // ensure_default_routing() once after the batch finishes.
    std::lock_guard lock{mutex_};
    item->set_meter_ballistics(meter_ballistics_);
    item->set_true_peak_metering(meter_true_peak_);
    item->set_loudness_metering(meter_loudness_);
    items_[item->id().value] = item;
    pending_.item_sources[item->id().value]
        .by_source_channel
        .resize(item->source_channel_count());
    return item->id();
}

CueId AudioEngine::new_cue_id() const {
    return CueId{gen_uuid_like()};
}

void AudioEngine::unload_cue(const CueId& id) {
    std::lock_guard lock{mutex_};
    auto it = items_.find(id.value);
    if (it == items_.end()) return;
    it->second->unload();
    items_.erase(it);
    pending_.item_sources.erase(id.value);
    rebuild_topology_locked();
}

PlaybackItem* AudioEngine::find_cue(const CueId& id) const {
    std::lock_guard lock{mutex_};
    auto it = items_.find(id.value);
    return it != items_.end() ? it->second.get() : nullptr;
}

void AudioEngine::play(const CueId& id) {
    if (auto* item = find_cue(id)) {
        Logger::info("play() cue='{}'", id.value);
        item->play();
    } else {
        Logger::warn("play() ignored — no cue with id '{}'", id.value);
    }
}

void AudioEngine::stop(const CueId& id) {
    if (auto* item = find_cue(id)) {
        Logger::info("stop() cue='{}'", id.value);
        item->stop();
    }
}

void AudioEngine::stop_all(std::chrono::milliseconds fade, bool force_fade) {
    std::lock_guard lock{mutex_};
    for (auto& [_, item] : items_) {
        if (force_fade) {
            // Global fade wins: apply `fade` to EVERY item, ignoring its own
            // fade-out. fade == 0 → hard stop for all. Restore the item's
            // configured fade-out afterwards so a later single stop still uses
            // the per-cue value.
            const auto item_fade = item->desc().fade_out_duration;
            if (fade.count() > 0) item->set_fade_out(fade);
            else                  item->set_fade_out(std::chrono::milliseconds{0});
            item->stop();
            item->set_fade_out(item_fade);
            continue;
        }
        // Default rule: each item's own fade_out_duration wins when non-zero;
        // only when an item is configured for a hard stop (fade-out == 0) do we
        // fall back to the caller-provided `fade`.
        const auto item_fade = item->desc().fade_out_duration;
        if (item_fade.count() > 0) {
            item->stop();
        } else if (fade.count() > 0) {
            item->set_fade_out(fade);
            item->stop();
            item->set_fade_out(std::chrono::milliseconds{0});
        } else {
            item->stop();
        }
    }
}

// ---------------------------------------------------------------------------
// Sensible-default routing — bootstrap the engine into a usable state
// without requiring the caller to call open_default_device + create mixer
// + route + assign master explicitly. Idempotent.
// ---------------------------------------------------------------------------
void AudioEngine::ensure_default_routing() {
    // Step 1: open a device if none open. open_device_by_name takes its own
    // lock; we must therefore call it OUTSIDE the engine mutex.
    DeviceId chosen_device{};
    {
        std::lock_guard lock{mutex_};
        if (!devices_.empty()) chosen_device = devices_.front()->id;
    }
    if (chosen_device.empty()) {
        chosen_device = open_default_device(2);
        if (chosen_device.empty()) {
            Logger::warn("ensure_default_routing: could not open default device — playback will be silent.");
            return;
        }
    }

    // Step 2: ensure a "Main" mixer exists. create_mixer_channel locks too.
    MixerChannelId main_mixer{};
    {
        std::lock_guard lock{mutex_};
        // Never the monitor, and prefer a strip actually called Main.
        //
        // This used to take mixers_.begin(), which is an arbitrary strip out
        // of an unordered_map — and once Monitor existed, it sometimes picked
        // that one and wired it to masters 0/1. Everything PFL'd then arrived
        // in the house, at roughly one launch in N, which is the precise
        // accident PFL was chosen over solo to make impossible. Caught by
        // metering the master with PFL up.
        for (const auto& [_, m] : mixers_) {
            if (!monitor_mixer_.empty() && m->id() == monitor_mixer_) continue;
            if (main_mixer.empty()) main_mixer = m->id();
            if (m->display_name() == "Main") { main_mixer = m->id(); break; }
        }
    }
    if (main_mixer.empty()) {
        main_mixer = create_mixer_channel("Main");
        if (main_mixer.empty()) {
            Logger::error("ensure_default_routing: cannot create the Main mixer — "
                          "at the {} strip limit; no default routing is possible",
                          cfg_.max_mixer_channels);
            return;
        }
        Logger::info("ensure_default_routing: created Main mixer '{}'", main_mixer.value);
    }

    // Step 3: wire master 0/1 → device 0/1 if not already.
    //
    // Everything below is idempotent, and `changed` records whether any of it
    // actually did something. That matters because this runs on every project
    // save — the client round-trips the whole document — and it used to end in
    // an unconditional topology rebuild. A rebuild walks every item, route and
    // master allocating as it goes, all while holding the mutex the RENDER
    // thread takes twice a block. Saving three times in a second, which the
    // client does, meant three of those stalls in a second.
    bool changed = false;
    {
        std::lock_guard lock{mutex_};
        if (pending_.master_destinations.size() < 2) {
            pending_.master_destinations.resize(2);
            changed = true;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (!pending_.master_destinations[i].has_value()) {
                MasterDestination dest;
                dest.device     = chosen_device;
                dest.hw_channel = static_cast<ChannelIndex>(i);
                pending_.master_destinations[i] = dest;
                changed = true;
            }
        }
        // Step 4: route Main mixer lanes → masters (lane 0 → master 0 = L,
        // lane 1 → master 1 = R) so the strip's stereo image survives.
        auto& m2m = pending_.mixer_to_master[main_mixer.value];
        bool has_m0 = false, has_m1 = false;
        for (auto& s : m2m) {
            if (s.master == 0) has_m0 = true;
            if (s.master == 1) has_m1 = true;
        }
        if (!has_m0) { m2m.push_back({0, 0, 1.0f}); changed = true; }
        if (!has_m1) { m2m.push_back({1, 1, 1.0f}); changed = true; }

        // Step 5: auto-route every loaded cue's source channels → Main, but
        // ONLY for cues that have no routes yet. Cues that were explicitly
        // routed elsewhere (preview bus, per-device override, etc.) must not
        // be silently dragged back onto Main — that was the source of two
        // separate bugs:
        //   1. The preview cue (loaded with load_cue_no_route, routed only to
        //      the Preview mixer) bled onto Main on any subsequent play_item,
        //      because each play_item re-runs ensure_default_routing().
        //   2. Cues with `deviceOverride` were being double-routed (override
        //      mixer AND Main), so audio appeared in both outputs.
        // The LTC synthetic channel (always the last source channel on
        // LTC-enabled cues) is deliberately excluded — it has its own
        // dedicated device routing managed by apply_ltc_device_routing().
        for (auto& [cue_id, item] : items_) {
            auto& srcs = pending_.item_sources[cue_id].by_source_channel;
            const auto src_count = item->source_channel_count();
            if (srcs.size() < src_count) { srcs.resize(src_count); changed = true; }
            const auto audio_count = item->desc().ltc_enabled
                                     ? src_count - 1 : src_count;
            // Determine whether ANY audio source channel of this cue already
            // has a route to any mixer. If so, leave the cue alone.
            bool has_any_existing_route = false;
            for (ChannelIndex ch = 0; ch < audio_count; ++ch) {
                if (!srcs[ch].empty()) { has_any_existing_route = true; break; }
            }
            if (has_any_existing_route) continue;
            for (ChannelIndex ch = 0; ch < audio_count; ++ch) {
                // Mono cues fan out to every lane (centre image); multi-channel
                // cues map even channels → lane 0 (L), odd → lane 1 (R).
                const ChannelIndex lane = (audio_count == 1)
                    ? kAllMixerLanes
                    : static_cast<ChannelIndex>(ch % kMixerLanes);
                srcs[ch].push_back({main_mixer, lane, 1.0f});
                changed = true;
            }
        }

        // Only when something actually moved. See `changed` above.
        if (changed) rebuild_topology_locked();
    }
    // Debug, not info: this ran on every save and every play_item, so at info
    // it was three lines a second in the log during ordinary editing — noise
    // that buries the one line that matters mid-show.
    Logger::debug("ensure_default_routing: {} (device='{}', main_mixer='{}')",
                  changed ? "rewired" : "already wired",
                  chosen_device.value, main_mixer.value);
}

// ---------------------------------------------------------------------------
// Mixer channels
// ---------------------------------------------------------------------------
MixerChannelId AudioEngine::create_mixer_channel(std::string display_name) {
    auto id = MixerChannelId{gen_uuid_like()};
    auto ch = std::make_shared<MixerChannel>(id, std::move(display_name));
    ch->configure(cfg_.mix_sample_rate, cfg_.render_block);
    std::lock_guard lock{mutex_};
    // The render thread's lane accumulators are sized for max_mixer_channels at
    // start() and never grown, so this cap is what keeps that promise. Refuse
    // rather than allocate mid-block.
    if (mixers_.size() >= cfg_.max_mixer_channels) {
        Logger::error("create_mixer_channel: at the {} strip limit, refusing '{}'",
                      cfg_.max_mixer_channels, ch->display_name());
        return MixerChannelId{};
    }
    ch->configure_meters(meter_ballistics_);        // inherit project settings
    ch->set_true_peak_enabled(meter_true_peak_);
    ch->set_loudness_enabled(meter_loudness_);
    mixers_[id.value] = ch;
    // Tells the render thread its cached strip list and index are stale.
    mixers_generation_.fetch_add(1, std::memory_order_relaxed);
    rebuild_topology_locked();
    return id;
}

std::vector<AudioEngine::MixerChannelInfo> AudioEngine::list_mixer_channels() const {
    std::vector<MixerChannelInfo> out;
    std::lock_guard lock{mutex_};
    out.reserve(mixers_.size());
    for (const auto& [_, m] : mixers_) {
        out.push_back(MixerChannelInfo{
            m->id(),
            m->display_name(),
            linear_to_db_precise(m->peek_gain_linear()),
            m->is_muted(),
            m->is_pfl(),
        });
    }
    return out;
}

// ---------------------------------------------------------------------------
// PFL / Monitor
// ---------------------------------------------------------------------------
void AudioEngine::set_monitor_mixer(const MixerChannelId& id) {
    std::lock_guard lock{mutex_};
    if (monitor_mixer_ == id) return;
    monitor_mixer_ = id;
    rebuild_topology_locked();
}

MixerChannelId AudioEngine::monitor_mixer() const {
    std::lock_guard lock{mutex_};
    return monitor_mixer_;
}

void AudioEngine::set_mixer_pfl(const MixerChannelId& id, bool on) {
    std::lock_guard lock{mutex_};
    if (on && id == monitor_mixer_) {
        Logger::warn("set_mixer_pfl: refusing PFL on the monitor strip itself");
        return;
    }
    auto it = mixers_.find(id.value);
    if (it == mixers_.end()) return;
    if (it->second->is_pfl() == on) return;
    it->second->set_pfl(on);
    // The tap list lives in the topology, so a flag change needs a new
    // snapshot. Same cost as any other routing change, at user rate.
    rebuild_topology_locked();
}

std::size_t AudioEngine::clear_all_pfl() {
    std::lock_guard lock{mutex_};
    std::size_t cleared = 0;
    for (auto& [_, m] : mixers_) {
        if (!m->is_pfl()) continue;
        m->set_pfl(false);
        ++cleared;
    }
    if (cleared > 0) rebuild_topology_locked();
    return cleared;
}

std::size_t AudioEngine::pfl_count() const {
    std::lock_guard lock{mutex_};
    std::size_t n = 0;
    for (const auto& [_, m] : mixers_) if (m->is_pfl()) ++n;
    return n;
}

void AudioEngine::set_mixer_width(const MixerChannelId& id, ChannelCount width) {
    std::lock_guard lock{mutex_};
    auto it = mixers_.find(id.value);
    if (it == mixers_.end()) return;
    if (it->second->width() == width) return;
    it->second->set_width(width);
    // Width decides how a PFL'd strip is placed in the monitor, so a strip
    // that is currently tapped needs its taps rebuilt.
    if (it->second->is_pfl()) rebuild_topology_locked();
}

void AudioEngine::set_mixer_pan(const MixerChannelId& id, float pan) {
    std::lock_guard lock{mutex_};
    auto it = mixers_.find(id.value);
    if (it == mixers_.end()) return;
    if (it->second->pan() == pan) return;
    it->second->set_pan(pan);
    // The monitor tap carries the pan, so a tapped strip needs new taps. A
    // strip nobody is listening to costs nothing here — which matters, since
    // this is called for every event of a pan drag.
    if (it->second->is_pfl()) rebuild_topology_locked();
}

void AudioEngine::set_mixer_dsp(const MixerChannelId& id, const StripDspParams& params) {
    // The strip publishes into its own slot, so this needs the registry lock
    // only long enough to find the strip — not a topology rebuild.
    std::lock_guard lock{mutex_};
    auto it = mixers_.find(id.value);
    if (it == mixers_.end()) return;
    it->second->dsp().set_params(params);
}

void AudioEngine::remove_mixer_channel(const MixerChannelId& id) {
    std::lock_guard lock{mutex_};
    mixers_.erase(id.value);
    mixers_generation_.fetch_add(1, std::memory_order_relaxed);
    pending_.mixer_to_master.erase(id.value);
    // A dangling monitor designation would survive a project reload and point
    // at a strip that no longer exists, quietly disabling PFL.
    if (monitor_mixer_ == id) monitor_mixer_ = MixerChannelId{};
    for (auto& [_, item_routes] : pending_.item_sources) {
        for (auto& sends : item_routes.by_source_channel) {
            sends.erase(std::remove_if(sends.begin(), sends.end(),
                                       [&](auto& s){ return s.mixer == id; }),
                        sends.end());
        }
    }
    rebuild_topology_locked();
}

MixerChannel* AudioEngine::find_mixer_channel(const MixerChannelId& id) const {
    std::lock_guard lock{mutex_};
    auto it = mixers_.find(id.value);
    return it != mixers_.end() ? it->second.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------
void AudioEngine::route_item_source_to_mixer(const CueId& cue,
                                             ChannelIndex source_channel,
                                             const MixerChannelId& mixer,
                                             float gain_db,
                                             ChannelIndex lane) {
    std::lock_guard lock{mutex_};
    auto it = items_.find(cue.value);
    if (it == items_.end()) return;
    if (mixers_.find(mixer.value) == mixers_.end()) return;

    auto& routes = pending_.item_sources[cue.value].by_source_channel;
    if (source_channel >= routes.size()) routes.resize(source_channel + 1);

    // One send per (source_channel, mixer) pair — re-routing replaces the
    // existing send's gain and lane rather than stacking a second feed.
    auto& sends = routes[source_channel];
    auto sit = std::find_if(sends.begin(), sends.end(),
                            [&](auto& s){ return s.mixer == mixer; });
    const float gl = db_to_lin(gain_db);
    if (sit != sends.end()) { sit->gain_lin = gl; sit->lane = lane; }
    else sends.push_back({mixer, lane, gl});

    rebuild_topology_locked();
}

void AudioEngine::unroute_item_source_from_mixer(const CueId& cue,
                                                  ChannelIndex source_channel,
                                                  const MixerChannelId& mixer) {
    std::lock_guard lock{mutex_};
    auto it = pending_.item_sources.find(cue.value);
    if (it == pending_.item_sources.end()) return;
    auto& routes = it->second.by_source_channel;
    if (source_channel >= routes.size()) return;
    auto& sends = routes[source_channel];
    sends.erase(std::remove_if(sends.begin(), sends.end(),
                               [&](auto& s){ return s.mixer == mixer; }),
                sends.end());
    rebuild_topology_locked();
}

void AudioEngine::unroute_item_from_all_mixers(const CueId& cue) {
    std::lock_guard lock{mutex_};
    auto it = pending_.item_sources.find(cue.value);
    if (it == pending_.item_sources.end()) return;
    bool changed = false;
    for (auto& sends : it->second.by_source_channel) {
        if (!sends.empty()) { sends.clear(); changed = true; }
    }
    if (changed) rebuild_topology_locked();
}

void AudioEngine::route_mixer_to_master(const MixerChannelId& mixer,
                                        MasterChannelIndex master,
                                        float gain_db,
                                        ChannelIndex lane) {
    if (master >= cfg_.master_channels) return;
    std::lock_guard lock{mutex_};
    if (mixers_.find(mixer.value) == mixers_.end()) return;
    // One send per (mixer, master) pair — re-routing replaces the existing
    // send's gain and lane rather than stacking a second feed.
    auto& v = pending_.mixer_to_master[mixer.value];
    auto vit = std::find_if(v.begin(), v.end(),
                            [&](auto& s){ return s.master == master; });
    const float gl = db_to_lin(gain_db);
    if (vit != v.end()) { vit->gain_lin = gl; vit->lane = lane; }
    else v.push_back({master, lane, gl});
    rebuild_topology_locked();
}

void AudioEngine::unroute_mixer_from_master(const MixerChannelId& mixer,
                                            MasterChannelIndex master) {
    std::lock_guard lock{mutex_};
    auto it = pending_.mixer_to_master.find(mixer.value);
    if (it == pending_.mixer_to_master.end()) return;
    auto& v = it->second;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](auto& s){ return s.master == master; }), v.end());
    rebuild_topology_locked();
}

void AudioEngine::assign_master_to_device(MasterChannelIndex master,
                                          const DeviceId& device,
                                          ChannelIndex hw_channel) {
    if (master >= cfg_.master_channels) return;
    std::lock_guard lock{mutex_};
    if (!find_device_locked(device)) return;
    pending_.master_destinations[master] = MasterDestination{device, hw_channel};
    rebuild_topology_locked();
}

void AudioEngine::clear_master_assignment(MasterChannelIndex master) {
    if (master >= cfg_.master_channels) return;
    std::lock_guard lock{mutex_};
    pending_.master_destinations[master].reset();
    rebuild_topology_locked();
}

// ---------------------------------------------------------------------------
// Master
// ---------------------------------------------------------------------------
void AudioEngine::set_master_ceiling_db(float db) {
    std::lock_guard lock{mutex_};
    cfg_.master_ceiling_db = db;
    for (auto& ms : master_state_) {
        ms.limiter->configure(cfg_.mix_sample_rate, db);
    }
}

void AudioEngine::set_limiter_enabled(bool enabled) noexcept {
    limiter_enabled_.store(enabled, std::memory_order_release);
}

void AudioEngine::set_master_gain_db(float db) {
    const float clamped = std::clamp(db, -120.0f, 12.0f);
    const float lin = (clamped <= -120.0f) ? 0.0f
                                            : std::pow(10.0f, clamped / 20.0f);
    master_gain_linear_.store(lin, std::memory_order_release);
}

float AudioEngine::master_gain_db() const noexcept {
    const float lin = master_gain_linear_.load(std::memory_order_acquire);
    if (lin <= 0.0f) return -120.0f;
    return 20.0f * std::log10(lin);
}

void AudioEngine::set_output_channel_gain_db(MasterChannelIndex ch, float db) {
    // Pre-limiter output trim. Allows substantial boost (up to +40 dB) so the
    // operator can drive quiet material hard; the master limiter (when enabled)
    // still catches the resulting peaks.
    const float clamped = std::clamp(db, -120.0f, 40.0f);
    const float lin = (clamped <= -120.0f) ? 0.0f
                                           : std::pow(10.0f, clamped / 20.0f);
    std::lock_guard lock{mutex_};
    if (ch < output_channel_gains_.size()) {
        output_channel_gains_[ch] = lin;
    }
}

float AudioEngine::output_channel_gain_db(MasterChannelIndex ch) const noexcept {
    std::lock_guard lock{mutex_};
    if (ch >= output_channel_gains_.size()) return 0.0f;
    const float lin = output_channel_gains_[ch];
    if (lin <= 0.0f) return -120.0f;
    return 20.0f * std::log10(lin);
}

void AudioEngine::set_meter_ballistics(const MeterBallistics& b) {
    std::lock_guard lock{mutex_};
    meter_ballistics_ = b;
    for (auto& ms : master_state_) {
        if (ms.meter) ms.meter->configure(cfg_.mix_sample_rate, b);
    }
    for (auto& [_, m] : mixers_) m->configure_meters(b);
    for (auto& [_, item] : items_) item->set_meter_ballistics(b);
}

void AudioEngine::set_true_peak_metering(bool enabled) {
    std::lock_guard lock{mutex_};
    meter_true_peak_ = enabled;
    for (auto& ms : master_state_) {
        if (ms.meter) ms.meter->set_true_peak_enabled(enabled);
    }
    for (auto& [_, m] : mixers_) m->set_true_peak_enabled(enabled);
    for (auto& [_, item] : items_) item->set_true_peak_metering(enabled);
}

void AudioEngine::set_loudness_metering(bool enabled) {
    std::lock_guard lock{mutex_};
    meter_loudness_ = enabled;
    for (auto& ms : master_state_) {
        if (ms.meter) ms.meter->set_loudness_enabled(enabled);
    }
    for (auto& [_, m] : mixers_) m->set_loudness_enabled(enabled);
    for (auto& [_, item] : items_) item->set_loudness_metering(enabled);
}

MeterSnapshot AudioEngine::read_master_meter(MasterChannelIndex master) const {
    if (master >= master_state_.size()) return {};
    return master_state_[master].meter->snapshot();
}

EngineStats AudioEngine::stats(bool reset_peaks) {
    EngineStats s;
    const double sr = static_cast<double>(cfg_.mix_sample_rate);
    s.block_budget_us = (static_cast<double>(cfg_.render_block) / sr) * 1e6;

    {
        std::lock_guard lock{mutex_};
        s.devices = devices_.size();
        // The primary device is the one production is gated on, so it is the
        // one whose queue depth is the engine's output latency.
        if (!devices_.empty() && devices_.front()->ring) {
            auto& primary = devices_.front();
            s.queued_frames = ma_pcm_rb_available_read(primary->ring.get());
            s.ring_capacity_frames =
                ma_pcm_rb_available_read(primary->ring.get()) +
                ma_pcm_rb_available_write(primary->ring.get());
            if (primary->ma_dev) {
                // What the device SETTLED on, not what it was asked for.
                // WASAPI shared mode in particular has its own minimum and
                // will quietly ignore a smaller request.
                s.device_period_frames = primary->ma_dev->playback.internalPeriodSizeInFrames;
                s.device_periods       = primary->ma_dev->playback.internalPeriods;
            }
        }
    }
    s.queued_ms = (s.queued_frames / sr) * 1000.0;
    s.device_ms =
        ((static_cast<double>(s.device_period_frames) * s.device_periods) / sr) * 1000.0;

    const auto blocks = blocks_rendered_.load(std::memory_order_relaxed);
    s.blocks_rendered     = blocks;
    s.underruns           = underruns_.load(std::memory_order_relaxed);
    s.topology_rebuilds   = topology_rebuilds_.load(std::memory_order_relaxed);
    s.mutex_wait_us_max   = static_cast<double>(
        mutex_wait_us_max_.load(std::memory_order_relaxed));
    s.discontinuities     = discontinuities_.load(std::memory_order_relaxed);
    s.worst_seam          =
        worst_seam_milli_.load(std::memory_order_relaxed) / 1000.0;
    s.render_block_us_max = static_cast<double>(render_us_max_.load(std::memory_order_relaxed));
    s.render_block_us_avg =
        blocks ? static_cast<double>(render_us_total_.load(std::memory_order_relaxed)) /
                 static_cast<double>(blocks)
               : 0.0;

    if (reset_peaks) {
        render_us_max_.store(0, std::memory_order_relaxed);
        render_us_total_.store(0, std::memory_order_relaxed);
        blocks_rendered_.store(0, std::memory_order_relaxed);
        underruns_.store(0, std::memory_order_relaxed);
        topology_rebuilds_.store(0, std::memory_order_relaxed);
        mutex_wait_us_max_.store(0, std::memory_order_relaxed);
        discontinuities_.store(0, std::memory_order_relaxed);
        worst_seam_milli_.store(0, std::memory_order_relaxed);
    }
    return s;
}

MeterSnapshot AudioEngine::read_master_meter_consume(MasterChannelIndex master) {
    if (master >= master_state_.size()) return {};
    return master_state_[master].meter->snapshot_consume_max();
}

float AudioEngine::read_master_gain_reduction_db(MasterChannelIndex master) const {
    if (master >= master_state_.size()) return 0.0f;
    // No gain reduction is happening while the limiter is bypassed.
    if (!limiter_enabled_.load(std::memory_order_acquire)) return 0.0f;
    return master_state_[master].limiter->gain_reduction_db();
}

// ---------------------------------------------------------------------------
// Render loop — device-callback-driven
// ---------------------------------------------------------------------------
// The render thread does NOT poll. Instead it blocks on consumption_counter_
// via std::atomic::wait() and is woken by the ma_data_callback() of every
// device after it consumes samples. This couples production cadence to the
// hardware clock of the slowest active device and eliminates the timing jitter
// that produced the residual crackles in the polling implementation.
//
// Invariants:
//   * After waking, the render thread checks whether *any* device's ring has
//     >= one full block of writable space. If so, it renders one block and
//     writes to every device. If not, it loops back to wait.
//   * With multiple devices at different sample rates / clock drifts, the
//     loop renders whenever the slowest consumer frees up enough space, so
//     no device ever overflows and the faster ones simply stay closer to the
//     top of their ring.
//   * When no devices are open we fall back to a coarse timer so playhead
//     advancement remains aligned with wall-clock (matters for the future
//     "preview without an output device assigned" case).
void AudioEngine::render_loop() {
    Logger::debug("Render thread started.");
#if defined(LIVEPLAY_HAVE_SSE_DENORMAL)
    // Flush-to-zero + denormals-are-zero for this thread. The limiter and meter
    // states decay exponentially toward zero during silence and can enter
    // denormal range, which incurs large per-sample CPU penalties on x86;
    // FTZ/DAZ makes those flush to zero with no audible consequence.
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    const auto block_duration =
        std::chrono::nanoseconds{static_cast<long long>(cfg_.render_block) * 1'000'000'000LL /
                                 static_cast<long long>(cfg_.mix_sample_rate)};

    while (running_.load(std::memory_order_acquire)) {
        try {
            auto snap = snapshot_topology();
            if (!snap) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Production is gated by the SLOWEST consumer: we only render a block
            // when every device has room for it. Gating on "any device has space"
            // would let a fast device (higher SR, larger ring, or clock drift
            // ahead) pull production above real-time, overflowing the slower
            // devices' rings and making audio sound sped up.
            bool can_render   = true;
            bool has_devices  = false;
            bool has_ring     = false;
            {
                auto lock = lock_timed();
                has_devices = !devices_.empty();
                if (has_devices) {
                    // Gate production on the primary device only, to prevent
                    // clock drift on secondary devices from starving the primary.
                    auto& primary = devices_.front();
                    if (primary->ring) {
                        has_ring = true;
                        if (ma_pcm_rb_available_write(primary->ring.get()) < cfg_.render_block) {
                            can_render = false;
                        }
                    }
                }
                if (!has_ring) can_render = false;
            }

            if (!has_devices) {
                // Idle (no output). Coarse timer; consumption_counter_ will
                // never fire without a device callback to bump it.
                std::this_thread::sleep_for(block_duration);
                continue;
            }

            if (!can_render) {
                // Block until a device callback notifies us. Use an atomic
                // counter to avoid lost wakeups: we capture the current value
                // before checking-then-waiting on it.
                const std::uint32_t before = consumption_counter_.load(std::memory_order_acquire);
                // Re-check under the lock just before waiting (defence against a
                // device closing between the earlier check and now).
                consumption_counter_.wait(before, std::memory_order_acquire);
                continue;
            }

            // Timed, because "is the engine keeping up" is the question behind
            // every latency decision and it is not answerable from the config.
            // steady_clock::now() twice per block is a handful of nanoseconds
            // against a budget of thousands.
            const auto t0 = std::chrono::steady_clock::now();
            render_one_block(*snap);
            const auto us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            render_us_total_.fetch_add(us, std::memory_order_relaxed);
            blocks_rendered_.fetch_add(1, std::memory_order_relaxed);
            auto prev = render_us_max_.load(std::memory_order_relaxed);
            while (us > prev &&
                   !render_us_max_.compare_exchange_weak(prev, us,
                                                         std::memory_order_relaxed)) {}

            // Say so when a block takes a serious bite out of its budget.
            //
            // An underrun is logged already, but by then the audio has gone —
            // and a stall can be loud enough to hear as the queue lurches
            // without ever fully draining. This catches the near miss, which is
            // what makes a reported pop diagnosable on the machine it happens
            // on rather than only on one that reproduces it.
            //
            // Rate-limited to one line a second: the failure being chased fires
            // in bursts, and a burst that fills the log is its own problem.
            const auto budget_us = static_cast<std::uint64_t>(
                (static_cast<double>(cfg_.render_block) /
                 static_cast<double>(cfg_.mix_sample_rate)) * 1e6);
            if (us > budget_us / 2) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_slow_block_log_ > std::chrono::seconds{1}) {
                    last_slow_block_log_ = now;
                    std::uint32_t queued = 0;
                    {
                        std::lock_guard lock{mutex_};
                        if (!devices_.empty() && devices_.front()->ring)
                            queued = ma_pcm_rb_available_read(devices_.front()->ring.get());
                    }
                    Logger::warn("render: block took {} us of a {} us budget "
                                 "({} frames still queued). Something is stalling "
                                 "the audio thread.", us, budget_us, queued);
                }
            }
        } catch (const std::bad_alloc&) {
            // Memory pressure: skip this block and give the system a moment.
            // Audio will glitch but the server survives.
            Logger::error("Render thread: out of memory — skipping block.");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } catch (const std::exception& e) {
            Logger::error("Render thread exception (audio may glitch): {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } catch (...) {
            Logger::error("Render thread caught unknown exception (audio may glitch).");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Logger::debug("Render thread exiting.");
}

void AudioEngine::render_one_block(const Topology& topo) {
    const std::size_t block = static_cast<std::size_t>(cfg_.render_block);

    // ---- The strips, and the per-channel gains ----
    //
    // The strip list and its index are CACHED and rebuilt only when a strip is
    // created or removed. They used to be rebuilt here every block, which
    // allocated a vector of shared_ptr and an unordered_map — with a node and
    // a string hash per strip — on the audio thread, 187 times a second. The
    // gains are copied into a member for the same reason.
    //
    // This is the difference between "the audio thread does arithmetic" and
    // "the audio thread asks the allocator for memory". The second only shows
    // up when something else is hammering the heap, which is exactly what a
    // project save does, which is when the popping was reported.
    {
        auto lock = lock_timed();
        const auto gen = mixers_generation_.load(std::memory_order_relaxed);
        if (!render_mixers_valid_ || gen != render_mixers_seen_) {
            render_mixers_.clear();
            render_mixer_index_.clear();
            render_mixers_.reserve(mixers_.size());
            for (auto& [_, m] : mixers_) render_mixers_.emplace_back(m);
            for (std::size_t i = 0; i < render_mixers_.size(); ++i) {
                render_mixer_index_.emplace(render_mixers_[i]->id().value, i);
            }
            render_mixers_seen_  = gen;
            render_mixers_valid_ = true;
        }
        // assign() over a member keeps the capacity, so this stops allocating
        // after the first block.
        render_gains_.assign(output_channel_gains_.begin(), output_channel_gains_.end());
    }
    auto& active_mixers          = render_mixers_;
    auto& channel_gains_snapshot = render_gains_;
    // One accumulator per mixer *lane* (stereo strips: L and R stay separate
    // all the way to the masters).
    const std::size_t lane_buf_count = active_mixers.size() * kMixerLanes;
    if (lane_buf_count > mixer_accumulators_.size()) {
        // Should be unreachable: create_mixer_channel() enforces the cap. Drop
        // the overflow rather than allocating on the render thread.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            Logger::error("render: {} mixer lanes exceeds the preallocated {} — "
                          "extra strips will be silent",
                          lane_buf_count, mixer_accumulators_.size());
        }
        // Not resized in place: active_mixers is the cached list now, and
        // truncating it would quietly drop strips for every later block too.
        // The loops below are bounded instead.
    }
    const std::size_t usable_mixers =
        std::min(active_mixers.size(), mixer_accumulators_.size() / kMixerLanes);
    for (auto& mb : mixer_accumulators_) {
        if (mb.size() < block) mb.assign(block, 0.0f);
        else std::fill(mb.begin(), mb.begin() + block, 0.0f);
    }
    if (master_accumulators_.size() < cfg_.master_channels) {
        master_accumulators_.resize(cfg_.master_channels,
                                    std::vector<Sample>(block, 0.0f));
    }
    for (auto& mb : master_accumulators_) {
        if (mb.size() < block) mb.assign(block, 0.0f);
        else std::fill(mb.begin(), mb.begin() + block, 0.0f);
    }
    const auto& mixer_index = render_mixer_index_;

    // ---- Per-item render + Tier-1 → Tier-2 mix ----
    if (item_channel_buffers_.size() < topo.items.size()) {
        item_channel_buffers_.resize(topo.items.size());
    }
    for (std::size_t i = 0; i < topo.items.size(); ++i) {
        auto& entry = topo.items[i];
        const ChannelCount n_src = entry.item->source_channel_count();
        auto& chbufs = item_channel_buffers_[i];
        if (chbufs.size() < n_src) chbufs.resize(n_src);
        // A member, cleared rather than constructed: this ran once per item
        // per block, so a project with a hundred cues loaded was asking the
        // allocator for memory a hundred times every 5.3 ms.
        render_ptrs_.clear();
        for (ChannelCount c = 0; c < n_src; ++c) {
            if (chbufs[c].size() < block) chbufs[c].assign(block, 0.0f);
            render_ptrs_.push_back(chbufs[c].data());
        }

        entry.item->render_block(render_ptrs_.data(), n_src, block);

        // Route each source channel to its destination mixer lanes.
        for (ChannelCount c = 0; c < n_src && c < entry.per_source_channel.size(); ++c) {
            for (const auto& send : entry.per_source_channel[c].sends) {
                auto mit = mixer_index.find(send.mixer->id().value);
                if (mit == mixer_index.end()) continue;
                Sample* acc = mixer_accumulators_[mit->second * kMixerLanes + send.lane].data();
                const Sample* src = chbufs[c].data();
                for (std::size_t s = 0; s < block; ++s) acc[s] += src[s] * send.gain;
            }
        }
    }

    // ---- Tier-2 strip processing: the channel chain ----
    // HPF, LPF, EQ and the gate, in place on each bus's accumulators, before
    // the tap and before the fader. This is the console order — you are
    // processing the channel, not the send — and it is what makes PFL
    // post-processing.
    //
    // It runs regardless of mute, because PFL is pre-mute: a muted channel
    // still has to be checkable in the phones. Strips whose controls are all
    // flat report inactive and cost nothing, which is most of them.
    //
    // Every lane goes in together rather than one at a time: the gate's
    // detector is linked across them, so it has to see the whole strip.
    const auto run_strip_dsp = [&](std::size_t i) {
        auto& dsp = active_mixers[i]->dsp();
        if (!dsp.needs_processing()) return;
        Sample* lanes[kMixerLanes];
        for (ChannelIndex lane = 0; lane < kMixerLanes; ++lane) {
            lanes[lane] = mixer_accumulators_[i * kMixerLanes + lane].data();
        }
        // A mono strip's lane 1 carries nothing, so keying the gate on it
        // would hold the detector at silence and shut the strip down.
        const ChannelCount used = std::min<ChannelCount>(
            std::max<ChannelCount>(1, active_mixers[i]->width()), kMixerLanes);
        dsp.process(lanes, used, block);
    };

    // Monitor is deliberately left out of this pass and run after the taps
    // below. Its accumulator is empty until they land, so processing it here
    // would be processing silence: the mono-sum audition folded nothing, and
    // any EQ or dynamics on Monitor applied to cue pre-listen but not to
    // anything PFL'd — the same strip treating its two sources differently.
    const std::size_t monitor_index = [&]() -> std::size_t {
        if (!topo.monitor) return usable_mixers;
        const auto it = mixer_index.find(topo.monitor->id().value);
        return it == mixer_index.end() ? usable_mixers : it->second;
    }();

    for (std::size_t i = 0; i < usable_mixers; ++i) {
        if (i == monitor_index) continue;
        run_strip_dsp(i);
    }

    // ---- PFL taps into the Monitor strip ----
    // Placed here, between the tone chain and the fader, which is what makes
    // the tap post-processing and pre-fader. Monitor is a strip like any
    // other, so the pass below then applies its own fader and mute to the
    // result — that fader is the headphone level.
    //
    // Nothing is copied. The tap adds straight into the monitor's
    // accumulator, which is the buffer copy §2.4 costed, minus the copy.
    if (topo.monitor && !topo.monitor_taps.empty() &&
        monitor_index < usable_mixers) {
        mix_monitor_taps(monitor_index, topo.monitor_taps, mixer_index,
                         mixer_accumulators_, block);
    }

    // ---- Monitor's own chain, now that everything it carries has arrived ----
    // Both sources are in: cue pre-listen from the item pass, PFL from the taps
    // just above. This is where the mono-sum audition folds the phones.
    if (monitor_index < usable_mixers) run_strip_dsp(monitor_index);

    // ---- Tier-2 strip processing (gain/mute/fade) + meter ----
    // No solo scan. Solo was never reachable from the UI and the mixer design
    // replaced it with PFL outright (§2.4), which costs the render thread a
    // flat list of taps instead of a per-block scan of every strip plus a
    // per-strip audibility test that depended on all the others.
    for (std::size_t i = 0; i < usable_mixers; ++i) {
        auto& m = active_mixers[i];
        // Advance the strip's fade envelope by exactly one render block, then
        // read the resulting gain. peek_gain_linear() is side-effect-free, so
        // the read may be repeated (metering, gain application) without the
        // fade running at a multiple of its configured speed.
        m->advance_block();
        const float gain_lin  = m->peek_gain_linear();
        const float effective = m->is_muted() ? 0.0f : gain_lin;
        for (ChannelIndex lane = 0; lane < kMixerLanes; ++lane) {
            Sample* buf = mixer_accumulators_[i * kMixerLanes + lane].data();
            for (std::size_t s = 0; s < block; ++s) buf[s] *= effective;
            m->update_meter(lane, buf, block);
        }
        // Correlation between the lanes, for the width control's readout. Taken
        // here, after the chain, so it describes the image that actually leaves
        // the strip rather than the one that arrived — which is the whole point
        // when the thing being checked is what widening did to it. It is a
        // ratio, so the fader above cannot move it.
        //
        // Mono strips are skipped rather than fed a silent lane 1, which would
        // read as a correlation of nothing and warn about a strip that has no
        // image to be wrong about.
        if (m->width() >= kMixerLanes) {
            m->update_correlation(mixer_accumulators_[i * kMixerLanes].data(),
                                  mixer_accumulators_[i * kMixerLanes + 1].data(),
                                  block);
        }
    }

    // ---- Tier-2 → Tier-3 mix into master accumulators ----
    for (MasterChannelIndex mc = 0; mc < cfg_.master_channels; ++mc) {
        Sample* acc = master_accumulators_[mc].data();
        for (const auto& send : topo.masters[mc].sends) {
            auto mit = mixer_index.find(send.mixer->id().value);
            if (mit == mixer_index.end()) continue;
            const Sample* src = mixer_accumulators_[mit->second * kMixerLanes + send.lane].data();
            for (std::size_t s = 0; s < block; ++s) acc[s] += src[s] * send.gain;
        }
    }

    // ---- Tier-3: master gain → per-channel output gain → limiter + meter ----
    const float mg = master_gain_linear_.load(std::memory_order_acquire);
    for (MasterChannelIndex mc = 0; mc < cfg_.master_channels; ++mc) {
        Sample* buf = master_accumulators_[mc].data();
        // Global master gain
        if (mg != 1.0f) {
            for (std::size_t s = 0; s < block; ++s) buf[s] *= mg;
        }
        // Per-output-channel gain (independent fader per device output pair)
        if (mc < channel_gains_snapshot.size()) {
            const float og = channel_gains_snapshot[mc];
            if (og != 1.0f) {
                for (std::size_t s = 0; s < block; ++s) buf[s] *= og;
            }
        }
        // Brick-wall limiter (bypassed when the operator has disabled it, so
        // peaks above the ceiling pass through unmodified).
        if (limiter_enabled_.load(std::memory_order_acquire)) {
            master_state_[mc].limiter->process(buf, block);
        }
        master_state_[mc].meter->push_block(buf, block);

        // ---- Seam detector ----
        //
        // Catches the pop itself, rather than a condition that might cause one.
        //
        // The trick is WHERE it looks. A parameter that changes between blocks —
        // a gain snapped instead of slewed, a decoder seeking, a route
        // reconnecting — puts a step exactly on the block boundary. Programme
        // material's own transients land anywhere, so comparing the boundary
        // jump against the largest jump INSIDE the same block separates the two
        // without needing to know anything about the material: a drum hit makes
        // both large, a seam makes only the first large.
        //
        // This exists because three rounds of fixing plausible causes did not
        // stop a reported pop, and every timing measurement came back clean. It
        // turns "it pops when I save" into a timestamped line naming the size
        // of the step.
        if (mc < 2 && block > 1) {
            float worst_internal = 0.0f;
            for (std::size_t s = 1; s < block; ++s) {
                worst_internal = std::max(worst_internal, std::fabs(buf[s] - buf[s - 1]));
            }
            const float seam = std::fabs(buf[0] - master_last_sample_[mc]);
            // Both tests matter. The ratio is what identifies a seam; the floor
            // stops near-silence, where every ratio is enormous and nothing is
            // audible, from filling the log.
            if (seam > 0.05f && seam > worst_internal * 4.0f) {
                discontinuities_.fetch_add(1, std::memory_order_relaxed);
                const auto milli = static_cast<std::uint32_t>(seam * 1000.0f);
                auto prev = worst_seam_milli_.load(std::memory_order_relaxed);
                while (milli > prev &&
                       !worst_seam_milli_.compare_exchange_weak(prev, milli,
                                                                std::memory_order_relaxed)) {}
                const auto now = std::chrono::steady_clock::now();
                if (now - last_seam_log_ > std::chrono::milliseconds{250}) {
                    last_seam_log_ = now;
                    Logger::warn("render: DISCONTINUITY on master {} — {:.4f} step at a "
                                 "block boundary against {:.4f} inside the block. "
                                 "Something changed a value under playing audio.",
                                 mc, seam, worst_internal);
                }
            }
            master_last_sample_[mc] = buf[block - 1];
        }
    }

    // ---- Dispatch to devices ----
    // For each device, build an interleaved block of its hardware channels by
    // picking the right master accumulator for each.
    // A member, cleared and refilled: this built a fresh vector every block.
    render_devices_.clear();
    {
        auto lock = lock_timed();
        for (auto& d : devices_) render_devices_.push_back(d.get());
    }
    for (auto* dev : render_devices_) {
        if (!dev->ring) continue;
        if (dev->scratch.size() < block * dev->channels) {
            dev->scratch.assign(block * dev->channels, 0.0f);
        } else {
            std::fill_n(dev->scratch.data(), block * dev->channels, 0.0f);
        }

        // Walk master assignments and copy contributions into the right hw ch.
        for (MasterChannelIndex mc = 0; mc < cfg_.master_channels; ++mc) {
            const auto& dest = topo.masters[mc].destination;
            if (!dest || dest->device != dev->id) continue;
            if (dest->hw_channel >= dev->channels) continue;
            const Sample* src = master_accumulators_[mc].data();
            Sample*       dst = dev->scratch.data();
            for (std::size_t s = 0; s < block; ++s) {
                dst[s * dev->channels + dest->hw_channel] += src[s];
            }
        }

        // Push into the device's ring buffer.
        ma_uint32 remaining = static_cast<ma_uint32>(block);
        const Sample* src   = dev->scratch.data();
        while (remaining > 0) {
            ma_uint32 frames_to_write = remaining;
            void*     buf = nullptr;
            if (ma_pcm_rb_acquire_write(dev->ring.get(), &frames_to_write, &buf) != MA_SUCCESS) break;
            if (frames_to_write == 0) {
                // Ring is full. Since we only gate production on the primary device, 
                // secondary devices with slower clocks will occasionally drop a frame here.
                break;
            }
            std::memcpy(buf, src,
                        frames_to_write * dev->channels * sizeof(Sample));
            ma_pcm_rb_commit_write(dev->ring.get(), frames_to_write);
            src       += frames_to_write * dev->channels;
            remaining -= frames_to_write;
        }
    }
}

} // namespace liveplay::audio
