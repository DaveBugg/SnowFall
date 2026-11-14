// In-app updater (macOS): check GitHub Releases, download + verify the signed release, replace
// the running bundle with a small helper script after the app has quit. Main thread only
// (network callbacks arrive on the main queue; the disk-image work runs on a background queue).
//
// Network use: only when the user agreed (settings.updates.check) or pressed «Check now» /
// «Update»; ephemeral NSURLSession (no cookies, cache or credentials), a "SnowFall/<version>"
// User-Agent, every URL and redirect checked with update::isAllowedUrl. Never in --selftest.
// Objective-C++ header.
#pragma once
#import <Foundation/Foundation.h>
#include <sys/types.h>

#include <string>

#include "update/update.h"

namespace snow::mac {

// Files of the updater: ~/Library/Caches/SnowFall/updates (download, helper log).
std::string updateWorkDirectory();

// The pieces of the installation, also used by the self-test with a local disk image.
// Mounts `dmg` read-only, copies its SnowFall.app to `stagedApp` (replaced if present), checks
// the copy (`codesign --verify --deep --strict`, CFBundleShortVersionString == expectedVersion,
// CFBundleIdentifier == expectedBundleId when not empty). On failure the copy is removed.
bool stageAppFromDiskImage(const std::string& dmg, const std::string& stagedApp, const std::string& expectedVersion,
                           const std::string& expectedBundleId, std::string* why);

struct SwapPlan {
    pid_t waitForPid = 0;
    std::string currentApp, stagedApp, backupApp;
    std::string logPath;
    bool reopen = true;
};
// Writes the helper script into a fresh temp directory and starts it detached (own session,
// no inherited descriptors). `helperPid` receives its pid (the self-test waits for it).
bool launchSwapHelper(const SwapPlan& plan, pid_t* helperPid, std::string* why);

}  // namespace snow::mac

@interface SFUpdater : NSObject
/// `networkAllowed` = NO: every request is refused (self-test, tests).
- (instancetype)initWithNetworkAllowed:(BOOL)networkAllowed;
@property(nonatomic, copy) void (^onStatus)(void);        // status changed
@property(nonatomic, copy) void (^onCheckAttempt)(void);  // a check starts (record lastCheck)
@property(nonatomic, copy) void (^onReadyToQuit)(void);   // helper started: quit normally now
@property(nonatomic, readonly) BOOL networkAllowed;
- (const snow::update::Status&)status;
- (BOOL)busy;   // checking / downloading / installing
/// "" if the running bundle can be replaced in place, otherwise why not (see update_support.h).
- (std::string)installBlocker;
/// Startup: log what the helper of a previous update reported, remove old downloads / staging.
- (void)cleanUpPreviousRun;
- (void)check;
- (void)install;
/// Opens the release page of the newer version (or the releases list) in the browser.
- (void)openReleasePage;
/// Shutdown: cancel transfers (an install in progress is abandoned and cleaned up).
- (void)cancel;
@end
