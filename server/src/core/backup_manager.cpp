#include "liveplay/core/backup_manager.hpp"
#include "liveplay/logger.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <system_error>
#include <vector>

namespace liveplay::core {

namespace fs = std::filesystem;

BackupManager::BackupManager(ProjectState& project)
    : project_(project) {}

BackupManager::~BackupManager() {
    stop();
}

void BackupManager::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&BackupManager::loop, this);
}

void BackupManager::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void BackupManager::loop() {
    using namespace std::chrono_literals;
    constexpr auto interval = std::chrono::minutes(INTERVAL_MINUTES);
    constexpr auto tick     = 500ms;

    auto next_backup = std::chrono::steady_clock::now() + interval;

    while (running_.load()) {
        std::this_thread::sleep_for(tick);
        if (!running_.load()) break;
        if (std::chrono::steady_clock::now() >= next_backup) {
            do_backup();
            next_backup = std::chrono::steady_clock::now() + interval;
        }
    }
}

void BackupManager::do_backup() {
    const fs::path src = project_.project_file_path();
    if (src.empty() || !fs::exists(src)) return;

    const fs::path backup_dir = src.parent_path() / "backups";
    std::error_code ec;
    fs::create_directories(backup_dir, ec);
    if (ec) {
        Logger::warn("Backup: could not create backups directory '{}': {}",
                     backup_dir.string(), ec.message());
        return;
    }

    // Timestamp: YYYY-MM-DD_HH-MM
    const auto now    = std::chrono::system_clock::now();
    const auto t      = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    const std::string stem      = src.stem().string();
    const std::string timestamp = std::format("{:04d}-{:02d}-{:02d}_{:02d}-{:02d}",
                                              tm_buf.tm_year + 1900,
                                              tm_buf.tm_mon  + 1,
                                              tm_buf.tm_mday,
                                              tm_buf.tm_hour,
                                              tm_buf.tm_min);
    const fs::path dst = backup_dir / (stem + "_backup_" + timestamp + ".liveplay");

    // Prune oldest backups if we're at the cap.
    {
        std::vector<fs::path> existing;
        for (const auto& entry : fs::directory_iterator(backup_dir, ec)) {
            if (!ec && entry.is_regular_file()) {
                const auto& p = entry.path();
                if (p.extension() == ".liveplay") existing.push_back(p);
            }
        }
        std::sort(existing.begin(), existing.end());  // lexicographic = chronological
        while (existing.size() >= MAX_BACKUPS) {
            fs::remove(existing.front(), ec);
            if (ec) {
                Logger::warn("Backup: could not remove old backup '{}': {}",
                             existing.front().string(), ec.message());
            }
            existing.erase(existing.begin());
        }
    }

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        Logger::warn("Backup: failed to write '{}': {}", dst.string(), ec.message());
    } else {
        Logger::info("Backup saved: {}", dst.filename().string());
    }
}

} // namespace liveplay::core
