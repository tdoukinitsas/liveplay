// ============================================================================
// liveplay/crash_handler.hpp
// ----------------------------------------------------------------------------
// Installs top-level crash handlers so an unhandled fault (segfault, access
// violation, uncaught exception, std::terminate) prints a stack trace + reason
// to stderr AND to a crash log file before the process exits. Without this the
// server window vanishes on a fault and the operator has nothing to go on.
// ============================================================================
#pragma once

#include <cstddef>
#include <string>

namespace liveplay {

// Call once early in main(). Idempotent. `log_dir` is the fallback directory
// for crash logs when no project dir is known; if empty, cwd is used.
void install_crash_handlers(const std::string& log_dir = "");

// Configure crash-loop protection. `counter_file` is a path where the
// consecutive-crash count is persisted across restarts; `consecutive_so_far`
// is the count read from that file at startup; `max_consecutive` is the number
// of back-to-back crashes after which the handler stops auto-restarting (so a
// deterministic crash can't relaunch forever). Call once at startup, after
// set_crash_exe_info().
void set_crash_restart_guard(const std::string& counter_file,
                             int consecutive_so_far,
                             int max_consecutive);

// Reset the consecutive-crash count to zero (in memory and on disk). Call once
// the process has been running healthily for a while, and on clean shutdown, so
// isolated crashes over a long session don't accumulate toward the give-up
// threshold.
void reset_crash_restart_guard();

// Delete all but the newest `keep` crash-log files in `dir`. Best-effort;
// safe to call at startup (never from a signal handler).
void prune_crash_logs(const std::string& dir, std::size_t keep);

// Tell the crash handler where the server executable lives and what arguments
// it was started with (so it can relaunch after a crash). Call once after
// argument parsing. `restart_args` should be the original argv[1..] joined by
// spaces (so the new instance inherits the same port / bind settings).
void set_crash_exe_info(const std::string& exe_path,
                        const std::string& restart_args);

// Where the crash-resume record is written. Defaults to .crash-resume.json
// beside the executable; main() points it at the server data directory, since
// the executable's folder is read-only on a packaged Linux install.
void set_crash_resume_file(const std::string& resume_file);

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
