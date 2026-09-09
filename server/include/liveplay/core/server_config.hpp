// ============================================================================
// liveplay/core/server_config.hpp
// ----------------------------------------------------------------------------
// The machine's own settings: what port to bind, how wide the master bus is,
// where the filesystem API may reach, which origins may call in. S3 gave these
// a file — liveplay.json beside the executable — fed by a precedence chain:
//
//     built-in default  <  liveplay.json  <  environment  <  command line
//
// P3 gives them a settings page, which means this file gains a WRITER. That is
// a reversal of something S3 said out loud ("the server never writes it"), so
// it is worth stating why it is not a contradiction.
//
// S3's reasoning was about SPARSENESS: an absent key means "not set", not "the
// default", which is what lets an installation keep taking improved defaults
// instead of freezing whatever they were the day the file appeared. A server
// that serialised its whole configuration would destroy that — every default
// would become a decision nobody made, permanently.
//
// So the writer here only ever touches keys somebody explicitly named. A patch
// of {"corsOrigin": "..."} writes exactly one key and leaves the file otherwise
// as it found it. Sparseness holds, improved defaults still arrive, and the
// difference between "an operator chose this" and "this is what the default was
// in September" is still legible in the file.
//
// ---------------------------------------------------------------------------
// One schema, four consumers
// ---------------------------------------------------------------------------
// The list in schema() is read by the file validator, by `--help`, by
// GET /api/server/config (which renders the form) and by PATCH (which validates
// it). Four consumers, one list — so a key cannot be readable and
// undocumented, or documented and rejected, or editable in the UI and quietly
// dropped on the way in. That was already half true when only the first two
// existed; adding an API is what makes it load-bearing.
//
// ---------------------------------------------------------------------------
// The lock, and why it exists
// ---------------------------------------------------------------------------
// Two of these values are security policy: fsRoots confines the filesystem API,
// corsOrigin decides which web origins may drive the server. Making them
// editable over the network moves them from "needs a shell on the machine" to
// "needs an administrator's token" — a real change in the threat model, since
// an admin who could widen fsRoots to the drive root could then read any file
// on the machine through the filesystem API.
//
// That is defensible: an administrator owns the Server tier by definition, and
// this is that tier. But the ownership model's rule R3 says server policy must
// be LOCKABLE, and this is what it was for. `--lock-server-config` (or
// LIVEPLAY_LOCK_SERVER_CONFIG, or the key in the file) makes every write here
// refuse with 403 and the settings page render read-only, saying why.
//
// The lock is deliberately NOT writable through patch(). A lock an
// administrator can turn off over the network is not a lock; a venue that sets
// it means it, and turning it off is a decision to be made at the machine. It
// is also, on purpose, off by default — the same posture S1, S2, S3 and U3 all
// took: the knob appears, the behaviour does not change until someone asks.
// ============================================================================
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace liveplay::core {

using json = nlohmann::json;

// Bumped when the file's shape changes in a way an older server would read
// wrongly. Owned here because the file is owned here.
inline constexpr int kConfigSchemaVersion = 1;

class ServerConfig {
public:
    // Which tier supplied the value actually in force. The settings page needs
    // this or it lies: the desktop app always launches the server with --port,
    // so a port field that wrote the file and said nothing would appear to work
    // and change nothing.
    enum class Source { Default, File, Env, Cli };
    static std::string_view to_string(Source s);

    // When a change takes effect. The engine cannot be re-initialised while it
    // is running, so most of these are Restart and the page has to say so
    // rather than let an operator believe a sample-rate change took hold
    // mid-show.
    enum class Applies { Restart, Live };

    enum class Kind { Int, Real, Text, Bool, PathList };

    struct Field {
        std::string_view key;
        std::string_view flag;      // the equivalent command-line flag
        Kind             kind;
        double           min     = 0;
        double           max     = 0;
        Applies          applies = Applies::Restart;
        std::string_view help;
        // True for the two values that are security policy rather than
        // preference. The page marks them; the lock exists for them.
        bool             policy  = false;
    };

    static const std::vector<Field>& schema();
    static const Field*              find(std::string_view key);

    void                  set_path(std::filesystem::path p);
    std::filesystem::path path() const;

    // Off by default. Set from --lock-server-config, the environment, or the
    // file — never from patch(), which is the whole point.
    void set_locked(bool locked);
    bool locked() const;

    // The file as it stands, or an empty object when there is none. A file that
    // cannot be parsed reads as empty here and is reported by the boot-time
    // reader; this accessor is for showing an operator what is stored, not for
    // deciding what is in force.
    json read() const;

    enum class Result { Ok, Locked, NotAnObject, IoError };
    static std::string_view describe(Result r);

    // Validate, merge, write. Only the keys named are touched. A key that fails
    // validation is dropped with a reason rather than failing the whole patch —
    // the same rule the project settings registry keeps, and for the same
    // reason: a client one version ahead should not have an entire form
    // rejected over one field this server has not heard of.
    //
    // A null value REMOVES a key, which is how "stop pinning this, go back to
    // the built-in default" is said.
    Result patch(const json& p, json* out_file,
                 std::vector<std::string>* dropped = nullptr);

    // One value against the schema. nullopt drops it, with `why` explaining.
    static std::optional<json> validate(std::string_view key, const json& v,
                                        std::string& why);

private:
    bool write_locked(const json& doc) const;

    mutable std::mutex    mutex_;
    std::filesystem::path path_;
    bool                  locked_ = false;
};

} // namespace liveplay::core
