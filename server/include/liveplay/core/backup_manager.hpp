// ============================================================================
// liveplay/core/backup_manager.hpp
// ----------------------------------------------------------------------------
// Periodic backup of the open project file.
//
// Every 10 minutes, while a project file is open, BackupManager copies the
// .liveplay file into a "backups" subfolder next to the project. At most
// MAX_BACKUPS files are retained; the oldest is removed when the cap is
// exceeded. All I/O runs on a background thread so playback is never blocked.
// ============================================================================
#pragma once

#include "liveplay/core/project_state.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace liveplay::core {

class BackupManager {
public:
    static constexpr std::size_t MAX_BACKUPS      = 20;
    static constexpr int         INTERVAL_MINUTES = 10;

    explicit BackupManager(ProjectState& project);
    ~BackupManager();

    void start();
    void stop();

private:
    void loop();
    void do_backup();

    ProjectState&      project_;
    std::atomic<bool>  running_{false};
    std::thread        thread_;
};

} // namespace liveplay::core
