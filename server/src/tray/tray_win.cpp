// =====================================================================
// tray_win.cpp — Shell_NotifyIcon tray for `--silent` (see tray.hpp)
// ---------------------------------------------------------------------
// The console is not hidden but RELEASED (FreeConsole): hiding only works on a
// classic conhost window, and on Windows 11 the console may be a Windows
// Terminal tab that ShowWindow cannot touch. Releasing it closes either kind.
// "Show console" allocates a fresh one and replays the recent log into it;
// "Hide console" releases it again.
//
// Closing that console with its own close button still stops the server, as
// it always has — Windows ends the process once the close handler returns, so
// there is no way to turn it into a hide. The close button is removed where
// the host allows it (conhost), and the tray's "Hide console" is the way out.
// =====================================================================
#include "liveplay/tray.hpp"
#include "liveplay/logger.hpp"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace liveplay::tray {
namespace {

constexpr UINT kMsgNotify = WM_APP + 1;   // the icon's mouse events
constexpr UINT kMsgStatus = WM_APP + 2;   // set_status from another thread
constexpr UINT kMsgDone   = WM_APP + 3;   // the server body returned

constexpr UINT kIdStatus  = 1;
constexpr UINT kIdConsole = 2;
constexpr UINT kIdLogs    = 3;
constexpr UINT kIdStop    = 4;

// How much of the log a newly shown console replays, so it does not open
// blank in the middle of a show.
constexpr std::size_t kReplayLines = 200;

struct State {
    std::mutex  mutex;
    std::string status;
    std::string log_file;
    std::function<void()> on_stop;

    HWND   hwnd = nullptr;
    HICON  icon = nullptr;
    UINT   taskbar_created = 0;
    bool   stopping = false;
    // A console we share with a shell (the server was started from a command
    // prompt) is that shell's, not ours to release or allocate.
    bool   shared_console = false;
    std::atomic<bool> body_done{false};
};

State& state() {
    static State s;
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring status_line() {
    auto& s = state();
    std::lock_guard lock{s.mutex};
    if (s.stopping) return L"LivePlay Server — stopping…";
    return s.status.empty() ? std::wstring{L"LivePlay Server"}
                            : L"LivePlay Server — " + widen(s.status);
}

bool console_visible() {
    return GetConsoleWindow() != nullptr;
}

void notify_icon(DWORD message) {
    auto& s = state();
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = s.hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kMsgNotify;
    nid.hIcon            = s.icon;
    const std::wstring tip = status_line();
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(message, &nid);
}

void enable_vt(DWORD handle_id) {
    HANDLE h = GetStdHandle(handle_id);
    DWORD mode = 0;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
}

void show_console() {
    if (HWND con = GetConsoleWindow()) {
        // Ours and already up, or the shell's: either way, bring it forward.
        ShowWindow(con, SW_SHOWNORMAL);
        SetForegroundWindow(con);
        return;
    }
    if (!AllocConsole()) return;

    // The C runtime's streams still point at whatever stdout was at launch
    // (nothing, for a detached process). Point them at the new console; iostream
    // follows because it is synced with stdio.
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$",  "r", stdin);
    std::cout.clear();
    std::cerr.clear();

    SetConsoleOutputCP(CP_UTF8);
    enable_vt(STD_OUTPUT_HANDLE);
    enable_vt(STD_ERROR_HANDLE);
    Logger::set_color_enabled(true);
    SetConsoleTitleW(L"LivePlay Server");

    if (HWND con = GetConsoleWindow()) {
        auto& s = state();
        if (s.icon) {
            SendMessageW(con, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(s.icon));
            SendMessageW(con, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(s.icon));
        }
        if (HMENU sys = GetSystemMenu(con, FALSE)) DeleteMenu(sys, SC_CLOSE, MF_BYCOMMAND);
    }

    // Replay the tail of the log so the window opens on what just happened.
    const std::string history = Logger::dump_history();
    std::size_t start = history.size();
    for (std::size_t lines = 0; start > 0 && lines <= kReplayLines; ) {
        --start;
        if (history[start] == '\n') ++lines;
    }
    if (start > 0) ++start;
    std::cout << "\033[2m--- LivePlay Server (running in the system tray; "
                 "use the tray icon to hide this window) ---\033[0m\n"
              << std::string_view{history}.substr(start);
    std::cout.flush();
}

void hide_console() {
    if (state().shared_console) return;
    Logger::set_color_enabled(false);
    FreeConsole();
}

void open_log_folder() {
    std::string file;
    {
        std::lock_guard lock{state().mutex};
        file = state().log_file;
    }
    if (file.empty()) return;
    std::wstring dir = widen(file);
    if (const auto cut = dir.find_last_of(L"\\/"); cut != std::wstring::npos) dir.resize(cut);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void request_stop() {
    auto& s = state();
    std::function<void()> stop;
    {
        std::lock_guard lock{s.mutex};
        if (s.stopping) return;
        s.stopping = true;
        stop = s.on_stop;
    }
    if (stop) stop();
    notify_icon(NIM_MODIFY);
}

void show_menu(HWND hwnd) {
    auto& s = state();
    bool has_log = false;
    bool stopping = false;
    {
        std::lock_guard lock{s.mutex};
        has_log  = !s.log_file.empty();
        stopping = s.stopping;
    }
    const bool shown = console_visible();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kIdStatus, status_line().c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | ((shown && s.shared_console) ? MF_GRAYED : 0), kIdConsole,
                shown ? L"Hide console" : L"Show console");
    AppendMenuW(menu, MF_STRING | (has_log ? 0 : MF_GRAYED), kIdLogs, L"Open log folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (stopping ? MF_GRAYED : 0), kIdStop, L"Stop server");

    // The documented dance for a tray menu: without the foreground switch the
    // menu does not close when the user clicks elsewhere, and without the
    // WM_NULL it can need two clicks to open next time.
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                    pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case kIdConsole: shown ? hide_console() : show_console(); break;
        case kIdLogs:    open_log_folder(); break;
        case kIdStop:    request_stop(); break;
        default: break;
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto& s = state();
    if (s.taskbar_created != 0 && msg == s.taskbar_created) {
        // Explorer restarted and took every tray icon with it.
        notify_icon(NIM_ADD);
        return 0;
    }
    switch (msg) {
        case kMsgNotify:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) show_menu(hwnd);
            return 0;
        case kMsgStatus:
            notify_icon(NIM_MODIFY);
            return 0;
        case kMsgDone:
            notify_icon(NIM_DELETE);
            DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            // `taskkill` without /F, or anything else asking politely. The
            // window only goes once the server has stopped (kMsgDone).
            request_stop();
            return 0;
        case WM_DESTROY:
            s.hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
        case WM_ENDSESSION:
            // Log-off or shutdown. A process without a console never sees
            // CTRL_SHUTDOWN_EVENT, so this is its only notice: stop cleanly,
            // and hold the session open for as long as Windows allows while
            // the server finishes.
            if (wp) {
                request_stop();
                for (int i = 0; i < 40 && !s.body_done.load(); ++i) Sleep(100);
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int run(Options opts, std::function<int()> body) {
    auto& s = state();
    s.on_stop = std::move(opts.on_stop);

    const HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &wnd_proc;
    wc.hInstance     = hinst;
    wc.lpszClassName = L"LivePlayServerTray";
    RegisterClassExW(&wc);
    // A real (never shown) top-level window rather than a message-only one:
    // only top-level windows receive the TaskbarCreated broadcast.
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"LivePlay Server", 0,
                                0, 0, 0, 0, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) return body();

    s.hwnd = hwnd;
    s.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    // Resource ordinal 1 is IDI_APPICON from server.rc.
    s.icon = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXSMICON),
                                           GetSystemMetrics(SM_CYSMICON), 0));
    if (!s.icon) s.icon = LoadIconW(nullptr, IDI_APPLICATION);

    if (GetConsoleWindow()) {
        DWORD ids[2] = {};
        if (GetConsoleProcessList(ids, 2) <= 1) FreeConsole();
        else s.shared_console = true;
    }

    notify_icon(NIM_ADD);

    int result = 0;
    std::thread worker([&] {
        result = body();
        s.body_done.store(true);
        PostMessageW(hwnd, kMsgDone, 0, 0);
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker.join();
    return result;
}

void set_status(const std::string& text) {
    auto& s = state();
    HWND hwnd = nullptr;
    {
        std::lock_guard lock{s.mutex};
        s.status = text;
        hwnd = s.hwnd;
    }
    if (hwnd) PostMessageW(hwnd, kMsgStatus, 0, 0);
}

void set_log_file(const std::string& path) {
    std::lock_guard lock{state().mutex};
    state().log_file = path;
}

} // namespace liveplay::tray
