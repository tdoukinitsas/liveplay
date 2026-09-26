// ============================================================================
// liveplay/core/user_prefs.hpp
// ----------------------------------------------------------------------------
// What belongs to the PERSON rather than to the show or to the rig.
//
// Four values spent this project's life in the .liveplay document, where they
// were never really at home: the colour scheme, the meter's display unit,
// whether the playlist follows the playing cue, and the transport keymap. A
// document is a portable thing that gets mailed to a colleague, and every one
// of those values travelled with it — so opening someone else's show changed
// your theme and, worse, silently reassigned the keys your hands already knew.
// That is the bug this store exists to fix, and it is why R1 (one writer per
// value) matters more here than tidiness: the fix is not "copy it to a better
// place", it is "there is now exactly one place, and the document is not it".
//
// ---------------------------------------------------------------------------
// Only a signed-in person has a profile
// ---------------------------------------------------------------------------
// With no accounts configured — still the default, see user_store.hpp — there
// is no person for a preference to belong to, so there is no file here and the
// client keeps these values in its own machine store. That is deliberate. The
// obvious alternative, one shared "anonymous" profile, would put whatever the
// desk chose onto every tablet in the building: the same coupling the document
// had, moved to a new file and no better for the move.
//
// So the tier resolves by whether anyone is signed in:
//   no accounts   → this surface's preference, held by the client
//   signed in     → this person's preference, held here and following them to
//                   any surface they sign in on
//
// ---------------------------------------------------------------------------
// Sparse, like liveplay.json
// ---------------------------------------------------------------------------
// An absent key means NOT CHOSEN, not "off" — so an installation keeps taking
// improved defaults instead of being frozen at whatever the defaults were the
// first time someone opened a colour picker. Only what a person actually set
// is written.
//
// A profile is seeded once, on first sign-in, from whatever the open project
// was carrying in its legacy fields. That is the whole migration: it runs at
// the moment there is finally somewhere to put the values, it reads the state
// the operator can see in front of them, and it never runs twice.
//
// ---------------------------------------------------------------------------
// Validation is written out, not tabulated
// ---------------------------------------------------------------------------
// O1 built a typed registry for project settings because there are around
// forty of them. There are six here, one of them nested, so a table would be
// more machinery than the thing it validates. The rules are spelled out in
// sanitise() instead, where they can be read.
//
// One rule is worth stating up front: playbackKeys is stored, not understood.
// The server validates the SHAPE of a binding and accepts any action name the
// client cares to use, because the alternative — a server-side list of the
// actions that exist — is a list that goes stale the first time the client
// gains an action, and goes stale by silently dropping somebody's keybinding.
// meterMode is the opposite case and is checked against a real enum, because
// the server acts on it: it gates true-peak and loudness DSP.
// ============================================================================
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace liveplay::core {

using json = nlohmann::json;

// Bumped only if a stored profile ever needs rewriting on read. Sparse keys
// mean most additions do not need it.
inline constexpr int kUserPrefsSchemaVersion = 1;

class UserPrefs {
public:
    enum class Result {
        Ok,
        NoUser,     // empty user id — nobody is signed in, so nothing to store
        IoError,    // the profile could not be written
    };
    static std::string_view describe(Result r);

    // Where the per-user files live: <dir>/<user-id>.json. Set once at startup.
    // The directory is created on the first write, not here — an installation
    // that never turns authentication on should not grow an empty folder.
    void                  set_dir(std::filesystem::path p);
    std::filesystem::path dir() const;

    // The stored profile, or nothing if this user has never had one written.
    // A file that exists but cannot be parsed reads as nothing, and is moved
    // aside rather than overwritten the next time this user changes something
    // — losing a keymap silently is exactly the failure this store was built
    // to prevent, so the bad file stays on disk where it can be looked at.
    std::optional<json> find(const std::string& user_id) const;

    // The profile, creating it from `seed` if there is none. This is the
    // first-sign-in migration and the only thing that ever writes a seed.
    // `seed` is sanitised like any other input, so a legacy document's
    // nonsense cannot land in a profile.
    json get_or_seed(const std::string& user_id, const json& seed);

    // Merge. Unknown keys and malformed values are dropped rather than
    // refused: a client one version ahead should not have its whole patch
    // rejected over a key this server has never heard of. `out_profile`
    // receives the profile as it now stands.
    //
    // A null value REMOVES a key, which is how "stop choosing this, go back
    // to following the default" is expressed — necessary because sparse means
    // absent and present-but-default are genuinely different states.
    Result patch(const std::string& user_id, const json& p, json* out_profile);

    // Drop a profile from disk and from the cache. Called when the account
    // goes away, so a re-used name cannot inherit a stranger's keymap.
    Result forget(const std::string& user_id);

    // Apply the rules to a bag of candidate values, returning only what
    // belongs. Public because the seed path, the patch path and the tests all
    // need the same answer, and because it is the readable statement of what
    // this store will hold.
    static json sanitise(const json& in);

private:
    // Callers already holding mutex_ use these; the public methods lock and
    // delegate, the same discipline UserStore keeps.
    bool                  load_locked(const std::string& user_id) const;
    bool                  write_locked(const std::string& user_id, const json& doc) const;
    std::filesystem::path file_for(const std::string& user_id) const;

    mutable std::mutex                     mutex_;
    std::filesystem::path                  dir_;
    // Cache of what is on disk. mutable because find() is logically const and
    // faults a profile in on first read.
    mutable std::map<std::string, json>    cache_;
    // User ids whose file failed to parse. Kept so the move-aside happens once
    // rather than on every write.
    mutable std::map<std::string, bool>    corrupt_;
};

} // namespace liveplay::core
