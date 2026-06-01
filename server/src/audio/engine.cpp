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
    if (cfg_.master_channels == 0) cfg_.master_channels = kDefaultMasterChannels;
    if (cfg_.render_block    == 0) cfg_.render_block    = kDefaultRenderBlock;
    if (cfg_.mix_sample_rate == 0) cfg_.mix_sample_rate = kDefaultMixSampleRate;

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

    // Pre-allocate scratch buffers sized for the worst-case topology.
    mixer_accumulators_.clear();
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
    auto snap = std::make_shared<Topology>();

    // ---- Items ----
    snap->items.reserve(items_.size());
    for (auto& [id_str, item] : items_) {
        ItemRouteEntry entry;
        entry.item = item;

        const ChannelCount n_src = item->source_channel_count();
        entry.per_source_channel.resize(n_src);

        // Find sends for this item.
        auto it = pending_.item_sources.find(id_str);
        if (it != pending_.item_sources.end()) {
            const auto& isr = it->second.by_source_channel;
            for (ChannelIndex c = 0; c < n_src; ++c) {
                if (c >= isr.size()) continue;
                for (const auto& [mixer_id, gain_lin] : isr[c]) {
                    auto mit = mixers_.find(mixer_id.value);
                    if (mit == mixers_.end()) continue;
                    entry.per_source_channel[c].sends.emplace_back(mit->second, gain_lin);
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
        for (auto& [master_idx, gain_lin] : master_sends) {
            if (master_idx >= cfg_.master_channels) continue;
            snap->masters[master_idx].sends.emplace_back(mit->second, gain_lin);
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
    auto dev = std::make_unique<Device>();
    dev->id           = DeviceId{gen_uuid_like()};
    dev->channels     = output_channels;
    dev->sample_rate  = cfg_.mix_sample_rate;
    dev->display_name = name_substring.empty() ? "Default Output" : name_substring;
    dev->ma_dev       = std::make_unique<ma_device>();
    dev->ring         = std::make_unique<ma_pcm_rb>();
    dev->engine       = this;                       // for callback → consumption_counter_

    // Allocate a ring big enough for ~80 render blocks of headroom. With 256
    // frames @ 48 kHz that's ~427 ms — provides extra margin against decode-path
    // spikes (disk I/O, MP3/FLAC inner-loop) so transient stalls never reach
    // the device callback. Transport latency is unaffected because the engine
    // commits gain/transport state independently of the buffer position.
    const ma_uint32 ring_frames = static_cast<ma_uint32>(cfg_.render_block * 80);
    if (ma_pcm_rb_init(ma_format_f32, output_channels, ring_frames,
                       nullptr, nullptr, dev->ring.get()) != MA_SUCCESS) {
        Logger::error("Failed to allocate ring buffer for device '{}'", dev->display_name);
        return {};
    }

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
        ma_pcm_rb_uninit(dev->ring.get());
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
    items_[item->id().value] = item;
    pending_.item_sources[item->id().value]
        .by_source_channel
        .resize(item->source_channel_count());
    return item->id();
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

void AudioEngine::stop_all(std::chrono::milliseconds fade) {
    std::lock_guard lock{mutex_};
    for (auto& [_, item] : items_) {
        // Rule: each item's own fade_out_duration wins when non-zero; only when
        // an item is configured for a hard stop (fade-out == 0) do we fall back
        // to the caller-provided `fade`. This lets the global stop button honour
        // the per-cue fade-out the user configured.
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
        if (!mixers_.empty()) main_mixer = mixers_.begin()->second->id();
    }
    if (main_mixer.empty()) {
        main_mixer = create_mixer_channel("Main");
        Logger::info("ensure_default_routing: created Main mixer '{}'", main_mixer.value);
    }

    // Step 3: wire master 0/1 → device 0/1 if not already.
    {
        std::lock_guard lock{mutex_};
        if (pending_.master_destinations.size() < 2) {
            pending_.master_destinations.resize(2);
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (!pending_.master_destinations[i].has_value()) {
                MasterDestination dest;
                dest.device     = chosen_device;
                dest.hw_channel = static_cast<ChannelIndex>(i);
                pending_.master_destinations[i] = dest;
            }
        }
        // Step 4: route Main mixer → master 0 + 1 (mono → both, stereo → L/R).
        auto& m2m = pending_.mixer_to_master[main_mixer.value];
        bool has_m0 = false, has_m1 = false;
        for (auto& p : m2m) {
            if (p.first == 0) has_m0 = true;
            if (p.first == 1) has_m1 = true;
        }
        if (!has_m0) m2m.emplace_back(0, 1.0f);
        if (!has_m1) m2m.emplace_back(1, 1.0f);

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
            if (srcs.size() < src_count) srcs.resize(src_count);
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
                srcs[ch].emplace_back(main_mixer, 1.0f);
            }
        }

        rebuild_topology_locked();
    }
    Logger::info("ensure_default_routing: ready (device='{}', main_mixer='{}')",
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
    mixers_[id.value] = ch;
    rebuild_topology_locked();
    return id;
}

void AudioEngine::remove_mixer_channel(const MixerChannelId& id) {
    std::lock_guard lock{mutex_};
    mixers_.erase(id.value);
    pending_.mixer_to_master.erase(id.value);
    for (auto& [_, item_routes] : pending_.item_sources) {
        for (auto& sends : item_routes.by_source_channel) {
            sends.erase(std::remove_if(sends.begin(), sends.end(),
                                       [&](auto& p){ return p.first == id; }),
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
                                             float gain_db) {
    std::lock_guard lock{mutex_};
    auto it = items_.find(cue.value);
    if (it == items_.end()) return;
    if (mixers_.find(mixer.value) == mixers_.end()) return;

    auto& routes = pending_.item_sources[cue.value].by_source_channel;
    if (source_channel >= routes.size()) routes.resize(source_channel + 1);

    auto& sends = routes[source_channel];
    auto sit = std::find_if(sends.begin(), sends.end(),
                            [&](auto& p){ return p.first == mixer; });
    const float gl = db_to_lin(gain_db);
    if (sit != sends.end()) sit->second = gl;
    else sends.emplace_back(mixer, gl);

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
                               [&](auto& p){ return p.first == mixer; }),
                sends.end());
    rebuild_topology_locked();
}

void AudioEngine::route_mixer_to_master(const MixerChannelId& mixer,
                                        MasterChannelIndex master,
                                        float gain_db) {
    if (master >= cfg_.master_channels) return;
    std::lock_guard lock{mutex_};
    if (mixers_.find(mixer.value) == mixers_.end()) return;
    auto& v = pending_.mixer_to_master[mixer.value];
    auto vit = std::find_if(v.begin(), v.end(),
                            [&](auto& p){ return p.first == master; });
    const float gl = db_to_lin(gain_db);
    if (vit != v.end()) vit->second = gl;
    else v.emplace_back(master, gl);
    rebuild_topology_locked();
}

void AudioEngine::unroute_mixer_from_master(const MixerChannelId& mixer,
                                            MasterChannelIndex master) {
    std::lock_guard lock{mutex_};
    auto it = pending_.mixer_to_master.find(mixer.value);
    if (it == pending_.mixer_to_master.end()) return;
    auto& v = it->second;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](auto& p){ return p.first == master; }), v.end());
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

MeterSnapshot AudioEngine::read_master_meter(MasterChannelIndex master) const {
    if (master >= master_state_.size()) return {};
    return master_state_[master].meter->snapshot();
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
                std::lock_guard lock{mutex_};
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

            render_one_block(*snap);
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

    // ---- (Re)size scratch buffers if the mixer count changed ----
    std::vector<std::shared_ptr<MixerChannel>> active_mixers;
    std::vector<float> channel_gains_snapshot;
    {
        std::lock_guard lock{mutex_};
        active_mixers.reserve(mixers_.size());
        for (auto& [_, m] : mixers_) active_mixers.emplace_back(m);
        channel_gains_snapshot = output_channel_gains_;
    }
    if (mixer_accumulators_.size() < active_mixers.size()) {
        mixer_accumulators_.resize(active_mixers.size(),
                                   std::vector<Sample>(block, 0.0f));
    }
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
    // Map mixer-id → accumulator index for the duration of this block.
    std::unordered_map<std::string, std::size_t> mixer_index;
    for (std::size_t i = 0; i < active_mixers.size(); ++i) {
        mixer_index.emplace(active_mixers[i]->id().value, i);
    }

    // ---- Per-item render + Tier-1 → Tier-2 mix ----
    if (item_channel_buffers_.size() < topo.items.size()) {
        item_channel_buffers_.resize(topo.items.size());
    }
    for (std::size_t i = 0; i < topo.items.size(); ++i) {
        auto& entry = topo.items[i];
        const ChannelCount n_src = entry.item->source_channel_count();
        auto& chbufs = item_channel_buffers_[i];
        if (chbufs.size() < n_src) chbufs.resize(n_src);
        std::vector<Sample*> ptrs;
        ptrs.reserve(n_src);
        for (ChannelCount c = 0; c < n_src; ++c) {
            if (chbufs[c].size() < block) chbufs[c].assign(block, 0.0f);
            ptrs.push_back(chbufs[c].data());
        }

        entry.item->render_block(ptrs.data(), n_src, block);

        // Route each source channel to its destination mixers.
        for (ChannelCount c = 0; c < n_src && c < entry.per_source_channel.size(); ++c) {
            for (const auto& [mixer_ptr, gain_lin] : entry.per_source_channel[c].sends) {
                auto mit = mixer_index.find(mixer_ptr->id().value);
                if (mit == mixer_index.end()) continue;
                Sample* acc = mixer_accumulators_[mit->second].data();
                const Sample* src = chbufs[c].data();
                for (std::size_t s = 0; s < block; ++s) acc[s] += src[s] * gain_lin;
            }
        }
    }

    // ---- Tier-2 strip processing (gain/mute/solo/fade) + meter ----
    bool any_soloed = false;
    for (auto& m : active_mixers) if (m->is_soloed()) { any_soloed = true; break; }

    for (std::size_t i = 0; i < active_mixers.size(); ++i) {
        auto& m = active_mixers[i];
        const float gain_lin = m->current_gain_linear();
        const bool  audible  = !m->is_muted() && (!any_soloed || m->is_soloed());
        const float effective = audible ? gain_lin : 0.0f;
        Sample* buf = mixer_accumulators_[i].data();
        for (std::size_t s = 0; s < block; ++s) buf[s] *= effective;
        m->update_meter(buf, block);
    }

    // ---- Tier-2 → Tier-3 mix into master accumulators ----
    for (MasterChannelIndex mc = 0; mc < cfg_.master_channels; ++mc) {
        Sample* acc = master_accumulators_[mc].data();
        for (const auto& [mixer_ptr, gain_lin] : topo.masters[mc].sends) {
            auto mit = mixer_index.find(mixer_ptr->id().value);
            if (mit == mixer_index.end()) continue;
            const Sample* src = mixer_accumulators_[mit->second].data();
            for (std::size_t s = 0; s < block; ++s) acc[s] += src[s] * gain_lin;
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
    }

    // ---- Dispatch to devices ----
    // For each device, build an interleaved block of its hardware channels by
    // picking the right master accumulator for each.
    std::vector<Device*> devices_copy;
    {
        std::lock_guard lock{mutex_};
        devices_copy.reserve(devices_.size());
        for (auto& d : devices_) devices_copy.push_back(d.get());
    }
    for (auto* dev : devices_copy) {
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
