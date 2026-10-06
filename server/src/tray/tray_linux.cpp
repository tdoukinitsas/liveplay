// =====================================================================
// tray_linux.cpp — AppIndicator tray for `--silent` (see tray.hpp)
// ---------------------------------------------------------------------
// GTK and libayatana-appindicator (or the older libappindicator) are opened
// with dlopen rather than linked, for two reasons: the server must still start
// on a headless box that has neither, and a link-time GTK dependency would make
// every Linux build need its development headers for one optional icon. The
// handful of entry points used are declared here by hand against opaque
// pointers — they are stable C ABI and have been for over a decade.
//
// No library or no display: run() logs why and the server runs headless, which
// is all `--silent` meant on Linux before there was a tray.
//
// AppIndicator has no tooltip, so the status line is the menu's first row.
// "Show console" opens the log in a terminal emulator with `tail -F`, since a
// detached process has no terminal of its own.
// =====================================================================
#include "liveplay/tray.hpp"
#include "liveplay/logger.hpp"

#include "tray_icon_png.hpp"   // generated: kTrayIconPng[], from assets/tray-icon.png

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace liveplay::tray {
namespace {

// ---- the slice of GTK / GLib / AppIndicator we call ---------------------
using gboolean  = int;
using gpointer  = void*;
using GCallback = void (*)();
using GSourceFunc = gboolean (*)(gpointer);

struct Api {
    gboolean (*gtk_init_check)(int*, char***);
    void     (*gtk_main)();
    void     (*gtk_main_quit)();
    void*    (*gtk_menu_new)();
    void*    (*gtk_menu_item_new_with_label)(const char*);
    void*    (*gtk_separator_menu_item_new)();
    void     (*gtk_menu_shell_append)(void*, void*);
    void     (*gtk_menu_item_set_label)(void*, const char*);
    void     (*gtk_widget_set_sensitive)(void*, gboolean);
    void     (*gtk_widget_show_all)(void*);
    unsigned long (*g_signal_connect_data)(gpointer, const char*, GCallback, gpointer, void*, int);
    unsigned (*g_idle_add)(GSourceFunc, gpointer);
    void*    (*app_indicator_new)(const char*, const char*, int);
    void     (*app_indicator_set_status)(void*, int);
    void     (*app_indicator_set_menu)(void*, void*);
    void     (*app_indicator_set_icon_theme_path)(void*, const char*);
    void     (*app_indicator_set_title)(void*, const char*);
};

constexpr int kCategoryApplicationStatus = 0;
constexpr int kStatusActive  = 1;
constexpr int kStatusPassive = 0;

struct State {
    std::mutex  mutex;
    std::string status;
    std::string log_file;
    std::function<void()> on_stop;
    bool stopping = false;
    bool running  = false;

    Api   api{};
    void* indicator  = nullptr;
    void* status_row = nullptr;
    void* logs_row   = nullptr;
    void* stop_row   = nullptr;
};

State& state() {
    static State s;
    return s;
}

template <typename F>
bool resolve(void* lib, const char* name, F& out) {
    out = reinterpret_cast<F>(dlsym(lib, name));
    return out != nullptr;
}

// Loads the indicator library and resolves everything from it: it links GTK,
// GObject and GLib itself, so their symbols are reachable through the same
// handle (RTLD_GLOBAL, so GTK's own lookups see one copy).
bool load(Api& a, std::string& why) {
    void* lib = nullptr;
    for (const char* name : {"libayatana-appindicator3.so.1", "libappindicator3.so.1"}) {
        if ((lib = dlopen(name, RTLD_NOW | RTLD_GLOBAL))) break;
    }
    if (!lib) {
        why = "libayatana-appindicator3 is not installed";
        return false;
    }
    const bool ok =
        resolve(lib, "gtk_init_check", a.gtk_init_check) &&
        resolve(lib, "gtk_main", a.gtk_main) &&
        resolve(lib, "gtk_main_quit", a.gtk_main_quit) &&
        resolve(lib, "gtk_menu_new", a.gtk_menu_new) &&
        resolve(lib, "gtk_menu_item_new_with_label", a.gtk_menu_item_new_with_label) &&
        resolve(lib, "gtk_separator_menu_item_new", a.gtk_separator_menu_item_new) &&
        resolve(lib, "gtk_menu_shell_append", a.gtk_menu_shell_append) &&
        resolve(lib, "gtk_menu_item_set_label", a.gtk_menu_item_set_label) &&
        resolve(lib, "gtk_widget_set_sensitive", a.gtk_widget_set_sensitive) &&
        resolve(lib, "gtk_widget_show_all", a.gtk_widget_show_all) &&
        resolve(lib, "g_signal_connect_data", a.g_signal_connect_data) &&
        resolve(lib, "g_idle_add", a.g_idle_add) &&
        resolve(lib, "app_indicator_new", a.app_indicator_new) &&
        resolve(lib, "app_indicator_set_status", a.app_indicator_set_status) &&
        resolve(lib, "app_indicator_set_menu", a.app_indicator_set_menu) &&
        resolve(lib, "app_indicator_set_icon_theme_path", a.app_indicator_set_icon_theme_path) &&
        resolve(lib, "app_indicator_set_title", a.app_indicator_set_title);
    if (!ok) why = "the appindicator library is missing expected symbols";
    return ok;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

// Writes the embedded icon somewhere the indicator host can read it, and
// returns that directory (an icon theme path) — empty on failure.
std::string write_icon() {
    std::string dir;
    if (const char* rt = std::getenv("XDG_RUNTIME_DIR"); rt && *rt) dir = std::string{rt} + "/liveplay-server";
    else dir = "/tmp/liveplay-server-" + std::to_string(::getuid());
    ::mkdir(dir.c_str(), 0700);
    std::ofstream f{dir + "/liveplay-server-tray.png", std::ios::binary | std::ios::trunc};
    if (!f) return {};
    f.write(reinterpret_cast<const char*>(kTrayIconPng), sizeof(kTrayIconPng));
    return f ? dir : std::string{};
}

void run_detached(const std::string& cmd) {
    // setsid + & so the child outlives nothing of ours and system() returns now.
    const std::string line = "setsid sh -c " + shell_quote(cmd) + " >/dev/null 2>&1 &";
    [[maybe_unused]] const int rc = std::system(line.c_str());
}

std::string log_file() {
    std::lock_guard lock{state().mutex};
    return state().log_file;
}

// ---- GTK-thread side ----------------------------------------------------
void refresh() {
    auto& s = state();
    std::string status;
    bool has_log = false;
    bool stopping = false;
    {
        std::lock_guard lock{s.mutex};
        status   = s.status;
        has_log  = !s.log_file.empty();
        stopping = s.stopping;
    }
    const std::string line = stopping       ? "LivePlay Server — stopping…"
                           : status.empty() ? "LivePlay Server"
                           : "LivePlay Server — " + status;
    s.api.gtk_menu_item_set_label(s.status_row, line.c_str());
    s.api.gtk_widget_set_sensitive(s.logs_row, has_log);
    s.api.gtk_widget_set_sensitive(s.stop_row, !stopping);
}

gboolean refresh_idle(gpointer) {
    refresh();
    return 0;   // G_SOURCE_REMOVE
}

gboolean quit_idle(gpointer) {
    auto& s = state();
    // Passive hides the icon at once rather than leaving it until the host
    // notices the bus name went away.
    s.api.app_indicator_set_status(s.indicator, kStatusPassive);
    s.api.gtk_main_quit();
    return 0;
}

void on_show_console(void*, gpointer) {
    const std::string file = log_file();
    if (file.empty()) return;
    const std::string tail = "tail -n 200 -F " + shell_quote(file);
    // Each emulator spells "run this" its own way; take the first one present.
    run_detached(
        "for t in x-terminal-emulator gnome-terminal konsole xfce4-terminal kitty alacritty xterm; do "
        "  command -v \"$t\" >/dev/null 2>&1 || continue; "
        "  case \"$t\" in "
        "    gnome-terminal) exec \"$t\" --title='LivePlay Server' -- sh -c " + shell_quote(tail) + " ;; "
        "    xfce4-terminal) exec \"$t\" --title='LivePlay Server' -x sh -c " + shell_quote(tail) + " ;; "
        "    *) exec \"$t\" -e sh -c " + shell_quote(tail) + " ;; "
        "  esac; "
        "done");
}

void on_open_logs(void*, gpointer) {
    std::string file = log_file();
    if (file.empty()) return;
    if (const auto cut = file.find_last_of('/'); cut != std::string::npos) file.resize(cut);
    run_detached("xdg-open " + shell_quote(file));
}

void on_stop(void*, gpointer) {
    auto& s = state();
    std::function<void()> stop;
    {
        std::lock_guard lock{s.mutex};
        if (s.stopping) return;
        s.stopping = true;
        stop = s.on_stop;
    }
    if (stop) stop();
    refresh();
}

void* add_item(const char* label, void (*handler)(void*, gpointer), void* menu) {
    auto& a = state().api;
    void* item = a.gtk_menu_item_new_with_label(label);
    if (handler)
        a.g_signal_connect_data(item, "activate", reinterpret_cast<GCallback>(handler), nullptr, nullptr, 0);
    a.gtk_menu_shell_append(menu, item);
    return item;
}

} // namespace

int run(Options opts, std::function<int()> body) {
    auto& s = state();
    s.on_stop = std::move(opts.on_stop);

    std::string why;
    if (!load(s.api, why)) {
        Logger::info("--silent: no tray icon ({}); running headless.", why);
        return body();
    }
    if (!s.api.gtk_init_check(nullptr, nullptr)) {
        Logger::info("--silent: no display to put a tray icon on; running headless.");
        return body();
    }

    auto& a = s.api;
    const std::string icon_dir = write_icon();
    s.indicator = a.app_indicator_new("liveplay-server",
                                      icon_dir.empty() ? "audio-x-generic" : "liveplay-server-tray",
                                      kCategoryApplicationStatus);
    if (!s.indicator) return body();
    if (!icon_dir.empty()) a.app_indicator_set_icon_theme_path(s.indicator, icon_dir.c_str());
    a.app_indicator_set_title(s.indicator, "LivePlay Server");

    void* menu = a.gtk_menu_new();
    s.status_row = add_item("LivePlay Server", nullptr, menu);
    a.gtk_widget_set_sensitive(s.status_row, 0);
    a.gtk_menu_shell_append(menu, a.gtk_separator_menu_item_new());
    add_item("Show console", &on_show_console, menu);
    s.logs_row = add_item("Open log folder", &on_open_logs, menu);
    a.gtk_menu_shell_append(menu, a.gtk_separator_menu_item_new());
    s.stop_row = add_item("Stop server", &on_stop, menu);
    a.gtk_widget_show_all(menu);
    a.app_indicator_set_menu(s.indicator, menu);
    a.app_indicator_set_status(s.indicator, kStatusActive);
    refresh();
    {
        std::lock_guard lock{s.mutex};
        s.running = true;
    }

    int result = 0;
    std::thread worker([&] {
        result = body();
        a.g_idle_add(&quit_idle, nullptr);   // g_idle_add is safe from any thread
    });
    a.gtk_main();
    worker.join();
    {
        std::lock_guard lock{s.mutex};
        s.running = false;
    }
    return result;
}

void set_status(const std::string& text) {
    auto& s = state();
    bool running = false;
    {
        std::lock_guard lock{s.mutex};
        s.status = text;
        running  = s.running;
    }
    if (running) s.api.g_idle_add(&refresh_idle, nullptr);
}

void set_log_file(const std::string& path) {
    auto& s = state();
    bool running = false;
    {
        std::lock_guard lock{s.mutex};
        s.log_file = path;
        running    = s.running;
    }
    if (running) s.api.g_idle_add(&refresh_idle, nullptr);
}

} // namespace liveplay::tray
