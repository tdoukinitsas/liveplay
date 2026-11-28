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
//
// A seal is bound to where its value may be sent (R2-02): the connection id,
// its base URL and the header name are the AEAD's associated data
// (secret_headers.hpp seal_binding). A sealed value copied onto another
// connection, another address or another header doesn't open. Sealed values
// are only ever opened from a show file on this machine's disk, never from a
// request body.
// ============================================================================
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace liveplay::core::secrets {

// Where the key lives. Set once at boot, before any show is loaded or saved.
void set_key_path(std::filesystem::path path);

// Reads the key (or, with `create`, makes it the first time), retrying a file
// that is briefly locked. Call it outside any lock that GO needs (before a
// load or a save): seal() and open() never wait for the file themselves.
// False when there is no usable key; credentials are then not written to disk
// at all (R2-03).
bool ensure_key(bool create = true);

// `plain` sealed as "lpenc:v2:<base64>" and bound to `binding`, or nullopt
// when there is no key to seal with.
std::optional<std::string> seal(std::string_view plain, std::string_view binding);

// A sealed value opened again, or nullopt when it isn't ours (another
// machine's key), has been altered, or is bound somewhere else. A v1 value
// (written by 2.6 RC2, unbound) opens whatever the binding.
std::optional<std::string> open(std::string_view sealed, std::string_view binding);

}  // namespace liveplay::core::secrets
