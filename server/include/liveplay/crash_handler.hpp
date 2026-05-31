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

// Call once early in main(). Idempotent. `log_dir` is the fallback directory
// for crash logs when no project dir is known; if empty, cwd is used.
void install_crash_handlers(const std::string& log_dir = "");

// Tell the crash handler where the server executable lives and what arguments
// it was started with (so it can relaunch after a crash). Call once after
// argument parsing. `restart_args` should be the original argv[1..] joined by
// spaces (so the new instance inherits the same port / bind settings).
void set_crash_exe_info(const std::string& exe_path,
                        const std::string& restart_args);

// Update the crash handler's idea of which project folder is open. When set,
// crash logs are written to <project_dir>/logs/ instead of the fallback dir.
// Call whenever the open project changes.
void set_crash_project_dir(const std::string& project_dir);

// Update the playback state that the crash handler will persist so the new
// instance can resume from where playback stopped. Safe to call frequently
// (e.g. from the heartbeat loop). Pass empty strings / 0.0 to clear.
void update_crash_resume_state(const std::string& project_file,
                                const std::string& playing_item_uuid,
                                double             position_sec);

} // namespace liveplay
