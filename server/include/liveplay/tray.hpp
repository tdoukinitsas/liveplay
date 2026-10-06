// =====================================================================
// tray.hpp
// ---------------------------------------------------------------------
// The system-tray presence `--silent` swaps the console window for.
//
// The console exists so an operator can SEE that the audio engine is still
// running after the desktop app has quit or crashed. Silent mode keeps that
// promise with an icon instead: it belongs to the server process, not to the
// client, so it is there exactly as long as the engine is.
//
//   Windows  Shell_NotifyIcon. The console is released at start and can be
//            brought back (and hidden again) from the menu.
//   macOS    an NSStatusItem in the menu bar. "Show console" opens the live
//            log in Terminal, since a detached process has no window to show.
//   Linux    an AppIndicator / StatusNotifierItem, loaded with dlopen so the
//            binary carries no GTK dependency. No libayatana-appindicator (or
//            no display) means no icon: the server runs headless and says so.
//
// AppKit and GTK both insist on the main thread, so run() hands that thread to
// the tray and runs the server itself on a worker. Every platform does it the
// same way, so there is one shape to reason about rather than three.
// =====================================================================
#pragma once

#include <functional>
#include <string>

namespace liveplay::tray {

struct Options {
    // Asked to stop the server — the menu's "Stop server". Must only flip a
    // flag; the server's own loop does the shutdown and then returns.
    std::function<void()> on_stop;
};

// Runs `body` with a tray icon up, and returns what `body` returned. The tray
// goes away when `body` returns. If the platform cannot show one (no display,
// no indicator library) `body` simply runs on the calling thread.
int run(Options opts, std::function<int()> body);

// Thread-safe, and no-ops when no tray is up — so the server can call them
// unconditionally.
//
// One line saying what this server is, e.g. "Port 4480 · PID 1234". Shown as
// the first (disabled) menu item and, where the platform has one, the tooltip.
void set_status(const std::string& text);
// The file the server's log is mirrored to. "Open log folder" opens its
// directory; on macOS and Linux "Show console" follows the file itself.
void set_log_file(const std::string& path);

} // namespace liveplay::tray
