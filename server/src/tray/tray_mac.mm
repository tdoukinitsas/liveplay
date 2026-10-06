// =====================================================================
// tray_mac.mm — menu-bar status item for `--silent` (see tray.hpp)
// ---------------------------------------------------------------------
// The server is a bare executable, not an .app, so it becomes an "accessory"
// application: a menu-bar item, no Dock icon, no menu bar of its own.
// AppKit only works on the main thread, which is why run() keeps that thread
// for [NSApp run] and starts the server on a worker with a main-thread-sized
// stack (secondary threads get 512 KiB by default on macOS).
//
// A detached process has no terminal to reveal, so "Show console" opens the
// log file in Terminal with `tail -F`, which is the same stream the console
// would have shown.
// =====================================================================
#include "liveplay/tray.hpp"

#import <Cocoa/Cocoa.h>
#include <pthread.h>

#include <mutex>
#include <string>

namespace liveplay::tray {
namespace {

struct State {
    std::mutex  mutex;
    std::string status;
    std::string log_file;
    std::function<void()> on_stop;
    std::function<int()>  body;
    int  result   = 0;
    bool stopping = false;
    bool running  = false;
};

State& state() {
    static State s;
    return s;
}

NSString* ns(const std::string& s) {
    NSString* str = [NSString stringWithUTF8String:s.c_str()];
    return str ? str : @"";
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

} // namespace
} // namespace liveplay::tray

using liveplay::tray::state;

@interface LPTrayController : NSObject <NSMenuDelegate>
@property(strong) NSStatusItem* item;
@property(strong) NSMenuItem*   statusRow;
@property(strong) NSMenuItem*   logsRow;
@property(strong) NSMenuItem*   stopRow;
- (void)refresh;
@end

@implementation LPTrayController

- (instancetype)init {
    if ((self = [super init])) {
        _item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        NSImage* image = nil;
        if (@available(macOS 11.0, *)) {
            image = [NSImage imageWithSystemSymbolName:@"waveform"
                              accessibilityDescription:@"LivePlay Server"];
        }
        if (image) {
            // Follows the light/dark menu bar. A message, not `image.template`:
            // `template` is a C++ keyword, so Objective-C++ cannot spell it as
            // a property.
            [image setTemplate:YES];
            _item.button.image = image;
        } else {
            _item.button.title = @"LivePlay";
        }

        NSMenu* menu = [[NSMenu alloc] init];
        menu.autoenablesItems = NO;
        menu.delegate = self;

        _statusRow = [[NSMenuItem alloc] initWithTitle:@"LivePlay Server" action:nil keyEquivalent:@""];
        _statusRow.enabled = NO;
        [menu addItem:_statusRow];
        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* console = [[NSMenuItem alloc] initWithTitle:@"Show console"
                                                         action:@selector(showConsole:)
                                                  keyEquivalent:@""];
        console.target = self;
        [menu addItem:console];

        _logsRow = [[NSMenuItem alloc] initWithTitle:@"Open log folder"
                                              action:@selector(openLogs:)
                                       keyEquivalent:@""];
        _logsRow.target = self;
        [menu addItem:_logsRow];
        [menu addItem:[NSMenuItem separatorItem]];

        _stopRow = [[NSMenuItem alloc] initWithTitle:@"Stop server"
                                              action:@selector(stopServer:)
                                       keyEquivalent:@""];
        _stopRow.target = self;
        [menu addItem:_stopRow];

        _item.menu = menu;
        [self refresh];
    }
    return self;
}

- (void)refresh {
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
    NSString* line = stopping       ? @"LivePlay Server — stopping…"
                   : status.empty() ? @"LivePlay Server"
                   : [@"LivePlay Server — " stringByAppendingString:liveplay::tray::ns(status)];
    self.statusRow.title       = line;
    self.item.button.toolTip   = line;
    self.logsRow.enabled       = has_log;
    self.stopRow.enabled       = !stopping;
}

- (void)menuWillOpen:(NSMenu*)menu {
    (void)menu;
    [self refresh];
}

- (NSString*)logFile {
    std::lock_guard lock{state().mutex};
    return liveplay::tray::ns(state().log_file);
}

- (void)showConsole:(id)sender {
    (void)sender;
    NSString* file = [self logFile];
    if (file.length == 0) return;
    const std::string cmd = "clear; tail -n 200 -F " + liveplay::tray::shell_quote(file.UTF8String);
    // AppleScript string literal: escape backslashes and quotes.
    NSString* escaped = [[liveplay::tray::ns(cmd)
        stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
        stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    NSString* source = [NSString stringWithFormat:
        @"tell application \"Terminal\"\n  do script \"%@\"\n  activate\nend tell", escaped];
    NSAppleScript* script = [[NSAppleScript alloc] initWithSource:source];
    [script executeAndReturnError:nil];
}

- (void)openLogs:(id)sender {
    (void)sender;
    NSString* file = [self logFile];
    if (file.length == 0) return;
    NSURL* dir = [[NSURL fileURLWithPath:file] URLByDeletingLastPathComponent];
    [[NSWorkspace sharedWorkspace] openURL:dir];
}

- (void)stopServer:(id)sender {
    (void)sender;
    auto& s = state();
    std::function<void()> stop;
    {
        std::lock_guard lock{s.mutex};
        if (s.stopping) return;
        s.stopping = true;
        stop = s.on_stop;
    }
    if (stop) stop();
    [self refresh];
}

@end

namespace liveplay::tray {
namespace {

LPTrayController* g_controller = nil;

void* body_thread(void*) {
    auto& s = state();
    s.result = s.body();
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_controller) {
            [[NSStatusBar systemStatusBar] removeStatusItem:g_controller.item];
            g_controller = nil;
        }
        [NSApp stop:nil];
        // -stop: only takes effect once the run loop handles an event; give it one.
        NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSZeroPoint
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:0
                                            context:nil
                                            subtype:0
                                              data1:0
                                              data2:0];
        [NSApp postEvent:wake atStart:YES];
    });
    return nullptr;
}

} // namespace

int run(Options opts, std::function<int()> body) {
    auto& s = state();
    s.on_stop = std::move(opts.on_stop);
    s.body    = std::move(body);

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        g_controller = [[LPTrayController alloc] init];
        if (!g_controller.item) {
            g_controller = nil;
            return s.body();
        }
        {
            std::lock_guard lock{s.mutex};
            s.running = true;
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u);
        pthread_t worker{};
        const bool started = pthread_create(&worker, &attr, &body_thread, nullptr) == 0;
        pthread_attr_destroy(&attr);
        if (!started) {
            [[NSStatusBar systemStatusBar] removeStatusItem:g_controller.item];
            g_controller = nil;
            return s.body();
        }

        [NSApp run];
        pthread_join(worker, nullptr);
    }
    std::lock_guard lock{s.mutex};
    s.running = false;
    return s.result;
}

void set_status(const std::string& text) {
    auto& s = state();
    bool running = false;
    {
        std::lock_guard lock{s.mutex};
        s.status = text;
        running  = s.running;
    }
    if (running) dispatch_async(dispatch_get_main_queue(), ^{ [g_controller refresh]; });
}

void set_log_file(const std::string& path) {
    auto& s = state();
    bool running = false;
    {
        std::lock_guard lock{s.mutex};
        s.log_file = path;
        running    = s.running;
    }
    if (running) dispatch_async(dispatch_get_main_queue(), ^{ [g_controller refresh]; });
}

} // namespace liveplay::tray
