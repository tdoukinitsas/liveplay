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

    render_thread_ = std::thread([this] { render_loop(); });
    return true;
}

void AudioEngine::stop() {
    if (!running_.exchange(false)) return;
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

// miniaudio data callback shared by all opened devices.
void AudioEngine::ma_data_callback(ma_device* dev,
                                   void* out,
                                   const void* /*in*/,
                                   std::uint32_t frames) {
    auto* device = reinterpret_cast<Device*>(dev->pUserData);
    if (!device || !device->ring) {
        std::memset(out, 0, frames * device->channels * sizeof(Sample));
        return;
    }

    Sample* dst = static_cast<Sample*>(out);
    std::uint32_t remaining = frames;
    while (remaining > 0) {
        void*         buf       = nullptr;
        ma_uint32     available = remaining;
        if (ma_pcm_rb_acquire_read(device->ring.get(), &available, &buf) != MA_SUCCESS) break;
        if (available == 0) {
            // Ring underrun — fill with silence and bail. Operators will hear
            // a brief gap rather than garbage if the render thread stalls.
            std::memset(dst, 0, remaining * device->channels * sizeof(Sample));
            break;
        }
        std::memcpy(dst, buf, available * device->channels * sizeof(Sample));
        ma_pcm_rb_commit_read(device->ring.get(), available);
        dst       += available * device->channels;
        remaining -= available;
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

    // Allocate a ring big enough for ~5 render blocks of headroom.
    const ma_uint32 ring_frames = static_cast<ma_uint32>(cfg_.render_block * 5);
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
    Logger::success("Opened audio device '{}' ({} ch @ {} Hz) → DeviceId {}",
                    devices_.back()->display_name, output_channels,
                    cfg_.mix_sample_rate, id.value);
    return id;
}

void AudioEngine::close_device(const DeviceId& id) {
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

    std::lock_guard lock{mutex_};
    items_[item->id().value] = item;
    pending_.item_sources[item->id().value]
        .by_source_channel
        .resize(item->source_channel_count());
    rebuild_topology_locked();
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
    if (auto* item = find_cue(id)) item->play();
}

void AudioEngine::stop(const CueId& id) {
    if (auto* item = find_cue(id)) item->stop();
}

void AudioEngine::stop_all(std::chrono::milliseconds fade) {
    std::lock_guard lock{mutex_};
    for (auto& [_, item] : items_) {
        if (fade.count() > 0) {
            // Temporarily set the item's fade-out so stop() honours it.
            const auto prev = item->desc().fade_out_duration;
            item->set_fade_out(fade);
            item->stop();
            item->set_fade_out(prev);
        } else {
            item->stop();
        }
    }
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

MeterSnapshot AudioEngine::read_master_meter(MasterChannelIndex master) const {
    if (master >= master_state_.size()) return {};
    return master_state_[master].meter->snapshot();
}

float AudioEngine::read_master_gain_reduction_db(MasterChannelIndex master) const {
    if (master >= master_state_.size()) return 0.0f;
    return master_state_[master].limiter->gain_reduction_db();
}

// ---------------------------------------------------------------------------
// Render loop
// ---------------------------------------------------------------------------
void AudioEngine::render_loop() {
    Logger::debug("Render thread started.");
    using clock = std::chrono::steady_clock;
    const auto block_duration =
        std::chrono::nanoseconds{static_cast<long long>(cfg_.render_block) * 1'000'000'000LL /
                                 static_cast<long long>(cfg_.mix_sample_rate)};

    while (running_.load(std::memory_order_acquire)) {
        auto snap = snapshot_topology();
        if (!snap) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        render_one_block(*snap);

        // Lightly throttle. The ring buffers absorb timing jitter; we just
        // don't want to spin at 100% CPU when devices haven't drained yet.
        std::this_thread::sleep_for(block_duration / 2);
    }
    Logger::debug("Render thread exiting.");
}

void AudioEngine::render_one_block(const Topology& topo) {
    const std::size_t block = static_cast<std::size_t>(cfg_.render_block);

    // ---- (Re)size scratch buffers if the mixer count changed ----
    std::vector<std::shared_ptr<MixerChannel>> active_mixers;
    {
        std::lock_guard lock{mutex_};
        active_mixers.reserve(mixers_.size());
        for (auto& [_, m] : mixers_) active_mixers.emplace_back(m);
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

    // ---- Tier-3: per master-channel limiter + meter ----
    for (MasterChannelIndex mc = 0; mc < cfg_.master_channels; ++mc) {
        Sample* buf = master_accumulators_[mc].data();
        master_state_[mc].limiter->process(buf, block);
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

        // Push into the device's ring buffer. If it's full we drop this block
        // — the device will fill silence until the render thread catches up.
        ma_uint32 frames_to_write = static_cast<ma_uint32>(block);
        void*     buf = nullptr;
        if (ma_pcm_rb_acquire_write(dev->ring.get(), &frames_to_write, &buf) == MA_SUCCESS &&
            frames_to_write > 0) {
            std::memcpy(buf, dev->scratch.data(),
                        frames_to_write * dev->channels * sizeof(Sample));
            ma_pcm_rb_commit_write(dev->ring.get(), frames_to_write);
        }
    }
}

} // namespace liveplay::audio
