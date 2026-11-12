// SnowFall for macOS — entry point: command line, single instance, logging, crash reports,
// NSApplication (accessory: menu-bar only, no Dock icon) and the AppHost.
//
//   SnowFall [--data-dir <dir>] [--settings] [--exit] [--selftest]
//   --data-dir   portable / test data directory (settings + log inside it)
//   --settings   open the settings window (also what a second launch does)
//   --exit       ask the running instance to quit
//   --selftest   CI self-test (see app/selftest.mm), exits with 0 = pass / skip
#import <AppKit/AppKit.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>

#include "app/app_host.h"
#include "app/icon_images.h"
#include "app/selftest.h"
#include "diagnostics/log.h"
#include "settings/settings_store.h"
#include "system/objc_util.h"
#include "system/platform_services.h"
#include "system/text_util.h"

namespace {

NSString* const kRemoteNotification = @"com.davebugg.snowfall.remote";

std::unique_ptr<snow::mac::AppHost> g_host;

std::string homeDir() {
    const char* h = getenv("HOME");
    return h ? h : "/tmp";
}

}  // namespace

@interface SFAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) snow::mac::LaunchOptions options;
@property(nonatomic, strong) SFBlockObserver* remote;
@property(nonatomic, copy) NSString* dataDir;
@end

@implementation SFAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)note {
    (void)note;
    NSApp.applicationIconImage = snow::mac::makeAppIconImage();
    g_host = std::make_unique<snow::mac::AppHost>();
    if (!g_host->initialize(self.options)) {
        SNOW_LOGE("initialization failed");
        NSAlert* a = [NSAlert new];
        a.messageText = @"SnowFall";
        a.informativeText = @"SnowFall не удалось запустить. Подробности в журнале.\nSnowFall could not start; see the log.";
        [a runModal];
        [NSApp terminate:nil];
        return;
    }
    NSString* myDir = self.dataDir;
    self.remote = [SFBlockObserver observeDistributed:kRemoteNotification
                                                block:^(NSNotification* n) {
                                                  NSString* dir = n.userInfo[@"dataDir"];
                                                  NSString* cmd = n.userInfo[@"cmd"];
                                                  if (![dir isEqual:myDir] || ![cmd isKindOfClass:[NSString class]]) return;
                                                  if (g_host) g_host->remoteCommand(cmd.UTF8String);
                                                }];
}

- (void)applicationWillTerminate:(NSNotification*)note {
    (void)note;
    [self.remote cancel];
    if (g_host) g_host->shutdown();
    g_host.reset();
    snow::log::shutdown();
}

// Double-clicking the app in Finder while it runs: open the settings.
- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible {
    (void)app;
    (void)visible;
    if (g_host) g_host->openSettings();
    return NO;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    (void)app;
    return NO;
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        snow::mac::LaunchOptions opt;
        bool exitOther = false, selftest = false;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--settings") opt.openSettings = true;
            else if (a == "--exit") exitOther = true;
            else if (a == "--selftest") selftest = true;
            else if (a == "--data-dir" && i + 1 < argc) {
                snow::SettingsStore::setDataDirectoryOverride(snow::mac::fromUtf8(argv[++i]));
                opt.customDataDir = true;
            }
            // Anything else (e.g. -psn_0_xxx from old launchers, -NSDocumentRevisionsDebugMode) is ignored.
        }
        if (selftest) return snow::mac::runSelfTest();

        const std::string dataDir = snow::mac::toUtf8(snow::SettingsStore::dataDirectory());
        NSString* nsDataDir = [NSString stringWithUTF8String:dataDir.c_str()] ?: @"";

        // ---- single instance (per data directory) -------------------------------------
        const std::string lockPath = dataDir + "/.instance.lock";
        const int lockFd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lockFd >= 0 && ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            NSString* cmd = exitOther ? @"exit" : @"settings";
            [NSDistributedNotificationCenter.defaultCenter postNotificationName:kRemoteNotification
                                                                         object:nil
                                                                       userInfo:@{@"cmd" : cmd, @"dataDir" : nsDataDir}
                                                             deliverImmediately:YES];
            return 0;
        }
        if (exitOther) return 0;

        // ---- logging -------------------------------------------------------------------
        std::string logDir = opt.customDataDir ? dataDir : homeDir() + "/Library/Logs/SnowFall";
        ::mkdir(logDir.c_str(), 0755);
        opt.logPath = logDir + "/snowfall.log";
        snow::log::init(snow::mac::fromUtf8(opt.logPath));
        snow::mac::installCrashHandler(logDir);

        struct stat st{};
        opt.firstRun = ::stat((dataDir + "/settings.json").c_str(), &st) != 0;

        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyAccessory;   // also LSUIElement in Info.plist
        SFAppDelegate* delegate = [SFAppDelegate new];
        delegate.options = opt;
        delegate.dataDir = nsDataDir;
        app.delegate = delegate;
        [app run];
        // Not reached normally: -terminate: exits from inside -run.
        if (lockFd >= 0) ::close(lockFd);
    }
    return 0;
}
