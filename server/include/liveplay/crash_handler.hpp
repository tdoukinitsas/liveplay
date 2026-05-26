// ============================================================================
// liveplay/crash_handler.hpp
// ----------------------------------------------------------------------------
// Installs top-level crash handlers so an unhandled fault (segfault, access
// violation, uncaught exception, std::terminate) prints a stack trace + reason
// to stderr AND to a crash log file before the process exits. Without this the
// server window vanishes on a fault and the operator has nothing to go on.
// ============================================================================
#pragma once

#include <string>

namespace liveplay {

// Call once early in main(). Idempotent. `log_dir` is the directory where a
// crash-<timestamp>.log will be written; if empty, the working directory is used.
void install_crash_handlers(const std::string& log_dir = "");

} // namespace liveplay
