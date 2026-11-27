#pragma once
// ============================================================================
// secrets.hpp - sealing credentials for the show file (audit F-23).
// ----------------------------------------------------------------------------
// Event connections travel with the show, so a show opened on another rig
// already knows its devices. Their credentials travel too, but sealed: each
// secret value is encrypted (libsodium secretbox) with a key that is made on
// first use, kept in the server's data folder (secret.key) and never leaves
// this machine. The file alone reveals nothing, and a show opened on another
// server can't open them: those connections ask the operator to sign in again
// (ProjectState::connection_auth_).
//
// Not hashing: LivePlay has to send the real value to the device, and a hash
// can't be turned back into it.
// ============================================================================
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace liveplay::core::secrets {

// Where the key lives. Set once at boot, before any show is loaded or saved.
void set_key_path(std::filesystem::path path);

// `plain` sealed as "lpenc:v1:<base64>", or nullopt when there is no key to
// seal with (the data folder can't be written).
std::optional<std::string> seal(std::string_view plain);

// A sealed value opened again, or nullopt when it isn't ours (another
// machine's key) or has been altered.
std::optional<std::string> open(std::string_view sealed);

}  // namespace liveplay::core::secrets
