// server_config.cpp — see server_config.hpp.
#include "liveplay/core/server_config.hpp"

#include "liveplay/audio/types.hpp"
#include "liveplay/logger.hpp"
#include "liveplay/util/unicode_path.hpp"

#include <fstream>

#if !defined(_WIN32)
#  include <sys/stat.h>
#endif

namespace liveplay::core {

namespace fs = std::filesystem;

namespace {

// "= 9999 is outside the supported range [1, 120]".
//
// The value and the range both belong in the reason, and S3's log lines are
// where that convention came from: an operator reading a console at load-in
// gets told what they typed AND what would have been accepted, which is the
// difference between a message they can act on and one they have to look up.
// The settings page shows the same string against the field.
std::string out_of_range(double v, double lo, double hi, bool whole) {
    const auto fmt = [whole](double d) {
        if (whole) return std::to_string(static_cast<long long>(d));
        std::string s = std::to_string(d);
        // Trim the trailing zeros std::to_string insists on, so a ceiling
        // reads as "-24.0" rather than "-24.000000".
        while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
        return s;
    };
    return "= " + fmt(v) + " is outside the supported range [" +
           fmt(lo) + ", " + fmt(hi) + "]";
}

} // namespace

std::string_view ServerConfig::to_string(Source s) {
    switch (s) {
        case Source::Default: return "default";
        case Source::File:    return "file";
        case Source::Env:     return "env";
        case Source::Cli:     return "cli";
    }
    return "default";
}

std::string_view ServerConfig::describe(Result r) {
    switch (r) {
        case Result::Ok:          return "ok";
        case Result::Locked:      return "server configuration is locked on this machine";
        case Result::NotAnObject: return "expected a JSON object";
        case Result::IoError:     return "the configuration file could not be written";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The schema — the one list
// ---------------------------------------------------------------------------
// Ranges here must match what main.cpp's flag parser enforces, because a value
// that the file accepts and a flag rejects (or the reverse) is a value whose
// legality depends on which door it came in by.
const std::vector<ServerConfig::Field>& ServerConfig::schema() {
    static const std::vector<Field> kSchema = {
        {"port", "--port", Kind::Int, 1, 65535, Applies::Restart,
         "TCP port the server listens on."},
        {"bind", "--bind", Kind::Text, 0, 0, Applies::Restart,
         "Network address to listen on. 0.0.0.0 accepts connections from the "
         "network; 127.0.0.1 allows this computer only."},
        {"meterHz", "--meter-hz", Kind::Int, 1, 120, Applies::Restart,
         "How often meters update, in Hz. Each screen can choose a lower rate."},
        {"maxUploadMb", "--max-upload-mb", Kind::Int, 1, 8192, Applies::Restart,
         "Largest file that can be uploaded, in MiB."},

        {"mixSampleRate", "--mix-sample-rate", Kind::Int, 8000, 192000, Applies::Restart,
         "Sample rate the mixer runs at."},
        {"renderBlock", "--render-block", Kind::Int, 32, 8192, Applies::Restart,
         "Audio processing block size, in samples. Smaller blocks lower "
         "latency but use more CPU."},
        {"ringBlocks", "--ring-blocks", Kind::Int, 2, 512, Applies::Restart,
         "Output buffer, in processing blocks. Raise it only if the audio "
         "stutters: each block adds delay."},
        {"masterChannels", "--master-channels", Kind::Int,
         static_cast<double>(audio::kMinMasterChannels), 1024, Applies::Restart,
         "Number of internal mix channels. The last two carry the Preview bus."},
        {"maxBuses", "--max-buses", Kind::Int, 2, 512, Applies::Restart,
         "Maximum number of buses in the mixer."},
        {"masterCeilingDb", "--master-ceiling-db", Kind::Real, -24.0, 0.0, Applies::Restart,
         "Brickwall limiter ceiling used until a project is open. Each "
         "project's Output Target sets its own."},

        // The two that are policy rather than preference. The lock exists for
        // these, and the settings page marks them.
        {"fsRoots", "--fs-root (repeatable; an array here)", Kind::PathList, 0, 0,
         Applies::Restart,
         "Limit file browsing and file access to these folders. Leave empty "
         "for no limit.", true},
        {"corsOrigin", "--cors-origin", Kind::Text, 0, 0, Applies::Restart,
         "Web origin that browsers may connect from (CORS and WebSocket). "
         "\"*\" allows any origin.", true},

        {"verbose", "--verbose", Kind::Bool, 0, 0, Applies::Restart,
         "Write detailed debug messages to the log."},
    };
    return kSchema;
}

const ServerConfig::Field* ServerConfig::find(std::string_view key) {
    for (const auto& f : schema()) if (f.key == key) return &f;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
std::optional<json> ServerConfig::validate(std::string_view key, const json& v,
                                           std::string& why) {
    const Field* f = find(key);
    if (!f) {
        why = "not a known setting";
        return std::nullopt;
    }

    switch (f->kind) {
    case Kind::Int: {
        if (!v.is_number() || v.is_number_float()) {
            // Deliberately strict rather than truncating. A port of 4480.7 is
            // a mistake somewhere upstream, and silently rounding it hides
            // whatever produced it.
            why = "must be a whole number";
            return std::nullopt;
        }
        const double d = v.get<double>();
        if (d < f->min || d > f->max) {
            why = out_of_range(d, f->min, f->max, true);
            return std::nullopt;
        }
        return std::optional<json>{json(static_cast<std::int64_t>(d))};
    }
    case Kind::Real: {
        if (!v.is_number()) { why = "must be a number"; return std::nullopt; }
        const double d = v.get<double>();
        if (d < f->min || d > f->max) {
            why = out_of_range(d, f->min, f->max, false);
            return std::nullopt;
        }
        return std::optional<json>{json(d)};
    }
    case Kind::Text: {
        if (!v.is_string()) { why = "must be a string"; return std::nullopt; }
        const auto s = v.get<std::string>();
        if (s.empty()) {
            // An empty string is not "unset" — clearing a key is done by
            // sending null, and conflating the two would make "" for
            // corsOrigin mean "allow nothing", which is not a posture anyone
            // can have asked for by accident.
            why = "must not be empty — send null to unset it";
            return std::nullopt;
        }
        if (s.size() > 4096) { why = "unreasonably long"; return std::nullopt; }
        return std::optional<json>{json(s)};
    }
    case Kind::Bool:
        if (!v.is_boolean()) { why = "must be true or false"; return std::nullopt; }
        return std::optional<json>{v};

    case Kind::PathList: {
        if (!v.is_array()) { why = "must be an array of paths"; return std::nullopt; }
        json out = json::array();
        for (const auto& e : v) {
            if (e.is_string() && !e.get<std::string>().empty()) out.push_back(e);
        }
        if (out.empty() && !v.empty()) {
            // Every entry was junk. Storing [] would read as "unrestricted",
            // which is the opposite of what someone editing fsRoots meant.
            why = "named no usable path";
            return std::nullopt;
        }
        return std::optional<json>{std::move(out)};
    }
    }
    why = "unhandled kind";
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Disk
// ---------------------------------------------------------------------------
void ServerConfig::set_path(fs::path p) {
    std::lock_guard lock{mutex_};
    path_ = std::move(p);
}

fs::path ServerConfig::path() const {
    std::lock_guard lock{mutex_};
    return path_;
}

void ServerConfig::set_locked(bool locked) {
    std::lock_guard lock{mutex_};
    locked_ = locked;
}

bool ServerConfig::locked() const {
    std::lock_guard lock{mutex_};
    return locked_;
}

json ServerConfig::read() const {
    std::lock_guard lock{mutex_};
    if (path_.empty()) return json::object();
    std::error_code ec;
    if (!fs::exists(path_, ec)) return json::object();
    try {
        std::ifstream in{path_, std::ios::binary};
        if (!in) return json::object();
        json j = json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return json::object();
        return j;
    } catch (...) {
        return json::object();
    }
}

ServerConfig::Result ServerConfig::patch(const json& p, json* out_file,
                                         std::vector<std::string>* dropped) {
    if (!p.is_object()) return Result::NotAnObject;

    {
        std::lock_guard lock{mutex_};
        if (locked_) return Result::Locked;
        if (path_.empty()) return Result::IoError;
    }

    json file = read();

    for (const auto& [k, v] : p.items()) {
        // The lock is not a setting. Letting it through here would make it a
        // lock an administrator can pick over the network, which is not one.
        if (k == "lockServerConfig") {
            if (dropped) dropped->push_back(k + " (set at launch, not here)");
            Logger::warn("server config patch: refused '{}' — a lock that can be "
                         "turned off remotely is not a lock", k);
            continue;
        }
        if (k == "schema_version") continue;

        if (v.is_null()) {
            // Unset: back to whatever the built-in default is, which is the
            // sparse file's whole point.
            if (find(k)) file.erase(k);
            else if (dropped) dropped->push_back(k + " (not a known setting)");
            continue;
        }

        std::string why;
        const auto accepted = validate(k, v, why);
        if (!accepted) {
            if (dropped) dropped->push_back(k + " (" + why + ")");
            Logger::warn("server config patch: dropped '{}' — {}", k, why);
            continue;
        }
        file[k] = *accepted;
    }

    file["schema_version"] = kConfigSchemaVersion;

    std::lock_guard lock{mutex_};
    if (!write_locked(file)) return Result::IoError;
    if (out_file) *out_file = file;
    return Result::Ok;
}

bool ServerConfig::write_locked(const json& doc) const {
    try {
        std::error_code ec;
        if (path_.has_parent_path()) fs::create_directories(path_.parent_path(), ec);

        const fs::path tmp = path_.string() + ".tmp";
        {
            std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
            if (!out) return false;
            out << doc.dump(2) << '\n';
            out.flush();
            if (!out) return false;
        }
#if !defined(_WIN32)
        // Readable by anyone, writable only by the owner. Unlike users.json
        // this holds no secrets — but it decides where the filesystem API may
        // reach, so a process that can rewrite it can widen that reach.
        ::chmod(tmp.string().c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
#endif
        fs::rename(tmp, path_, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
        Logger::info("Server configuration written to '{}'",
                     util::path_to_utf8(path_));
        return true;
    } catch (const std::exception& e) {
        Logger::warn("ServerConfig: failed to write '{}': {}",
                     util::path_to_utf8(path_), e.what());
        return false;
    }
}

} // namespace liveplay::core
