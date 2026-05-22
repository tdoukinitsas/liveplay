// ============================================================================
// liveplay/core/project_state.hpp
// ----------------------------------------------------------------------------
// The master state holder. Owns the high-level project document — cue list,
// cue metadata, mixer channel layout, routing matrix, master-bus device
// assignments — and provides JSON serialisation/deserialisation. The
// AudioEngine remains the authoritative source for *audio* state (what's
// currently playing, real-time meter values); ProjectState owns the
// *project document* (what cues exist, how they're wired up).
//
// On load(), the ProjectState walks the document and instructs the
// AudioEngine to mirror it (load cues, create mixer channels, wire routes).
//
// Legacy compatibility:
//   * `.liveplay` JSON projects from the 1.x client are accepted by load().
//     The translator maps the old "stereo file → mono speaker pair" routing
//     to the new matrix:  L→(default-device, hwCh 0), R→(default-device, hwCh 1).
//   * If the document already has a "v2" routing block (new schema), it's
//     used as-is.
// ============================================================================
#pragma once

#include "liveplay/audio/engine.hpp"
#include "liveplay/audio/types.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace liveplay::core {

using json = nlohmann::json;

struct CueMeta {
    audio::CueId             id;
    std::string              display_name;
    std::filesystem::path    file_path;
    std::string              artist;          // populated by TagLib (M4)
    std::string              title;           // populated by TagLib (M4)
    double                   duration_seconds = 0.0;
    float                    gain_db          = 0.0f;
    std::chrono::milliseconds fade_in_ms  {0};
    std::chrono::milliseconds fade_out_ms {0};
    bool                     ltc_enabled = false;
    int                      ltc_frame_rate_index = 4;     // 30 fps
    std::chrono::nanoseconds ltc_offset_ns{0};
};

struct MixerChannelMeta {
    audio::MixerChannelId id;
    std::string           display_name;
    float                 gain_db = 0.0f;
    bool                  muted   = false;
    bool                  soloed  = false;
};

struct RouteSendV2 {
    audio::ChannelIndex source_channel;
    audio::MixerChannelId destination_mixer;
    float gain_db;
};

struct MixerToMasterV2 {
    audio::MixerChannelId mixer;
    audio::MasterChannelIndex master_channel;
    float gain_db;
};

struct MasterAssignment {
    audio::MasterChannelIndex master_channel;
    audio::DeviceId device;
    audio::ChannelIndex hw_channel;
};

class ProjectState {
public:
    explicit ProjectState(audio::AudioEngine& engine);

    // Load a project file (.liveplay JSON). Returns true on success. On
    // failure, the previous state is preserved.
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    // Replace state from an in-memory JSON document. Same semantics as load.
    bool load_from_json(const json& doc);
    json to_json() const;

    // Clear everything and rewind the AudioEngine to an empty graph.
    void reset();

    // ---- High-level mutations --------------------------------------------
    // Wraps engine + state updates atomically so REST handlers can call a
    // single method.
    audio::CueId add_cue_from_file(const std::filesystem::path& file,
                                   std::string display_name = "");
    void remove_cue(const audio::CueId& id);
    void rename_cue(const audio::CueId& id, std::string new_name);
    void set_cue_gain_db(const audio::CueId& id, float db);
    void set_cue_fade_in (const audio::CueId& id, std::chrono::milliseconds d);
    void set_cue_fade_out(const audio::CueId& id, std::chrono::milliseconds d);
    void set_cue_ltc(const audio::CueId& id, bool enabled, int fps_index,
                     std::chrono::nanoseconds offset);

    // ---- Introspection ---------------------------------------------------
    std::vector<CueMeta> list_cues() const;
    std::optional<CueMeta> find_cue(const audio::CueId& id) const;

    std::vector<MixerChannelMeta> list_mixer_channels() const;

    std::filesystem::path media_root() const;
    void set_media_root(std::filesystem::path p);

    audio::AudioEngine& engine() noexcept { return engine_; }

private:
    audio::AudioEngine&        engine_;
    mutable std::mutex         mutex_;

    std::unordered_map<std::string, CueMeta>          cues_;
    std::unordered_map<std::string, MixerChannelMeta> mixers_;
    std::vector<RouteSendV2>       item_routes_;
    std::vector<MixerToMasterV2>   mixer_routes_;
    std::vector<MasterAssignment>  master_assignments_;

    std::filesystem::path media_root_ = std::filesystem::current_path() / "media";
    std::string           project_name_ = "Untitled";

    // Backward-compat translator: takes a legacy 1.x project document and
    // produces a v2-flavoured one with the equivalent routing matrix.
    json upgrade_legacy_document(const json& legacy) const;
    bool is_legacy_document(const json& doc) const;

    // Re-apply the in-memory state to the AudioEngine (post-load or reset).
    void apply_to_engine_locked();
};

} // namespace liveplay::core
