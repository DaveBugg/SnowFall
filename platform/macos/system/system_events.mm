#include "system/system_events.h"

#import <AppKit/AppKit.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>

#include <vector>

#include "diagnostics/log.h"
#include "system/objc_util.h"

namespace snow::mac {
namespace {

bool batteryPowered() {
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return false;
    bool battery = false;
    if (CFStringRef type = IOPSGetProvidingPowerSourceType(info)) {
        battery = CFStringCompare(type, CFSTR(kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo;
    }
    CFRelease(info);
    return battery;
}

}  // namespace

struct SystemEvents::Impl {
    SystemState s;
    bool sleeping = false, displaysAsleep = false, screenLocked = false, sessionInactive = false;
    std::function<void()> onChange;
    std::function<void()>* onWake = nullptr;
    std::vector<id> observers;          // NSWorkspace notification center
    std::vector<SFBlockObserver*> distObservers;   // distributed notification center
    std::vector<id> localObservers;     // default center
    CFRunLoopSourceRef psSource = nullptr;

    void update() {
        SystemState n;
        n.locked = screenLocked || sessionInactive;
        n.suspended = sleeping || displaysAsleep;
        n.onBattery = batteryPowered();
        n.lowPower = NSProcessInfo.processInfo.lowPowerModeEnabled;
        const bool changed = n.locked != s.locked || n.suspended != s.suspended || n.onBattery != s.onBattery ||
                             n.lowPower != s.lowPower;
        if (changed) {
            SNOW_LOGI("system: locked=%d suspended=%d battery=%d lowPower=%d", n.locked ? 1 : 0, n.suspended ? 1 : 0,
                      n.onBattery ? 1 : 0, n.lowPower ? 1 : 0);
        }
        s = n;
        if (changed && onChange) onChange();
    }

    static void powerSourceChanged(void* ctx) { static_cast<Impl*>(ctx)->update(); }
};

SystemEvents::SystemEvents() : impl_(std::make_unique<Impl>()) {}
SystemEvents::~SystemEvents() { stop(); }

void SystemEvents::start(std::function<void()> onChange) {
    Impl* d = impl_.get();
    d->onChange = std::move(onChange);
    d->onWake = &onWake;
    NSNotificationCenter* ws = NSWorkspace.sharedWorkspace.notificationCenter;
    auto onWs = [&](NSNotificationName name, void (^block)(NSNotification*)) {
        d->observers.push_back([ws addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:block]);
    };
    onWs(NSWorkspaceWillSleepNotification, ^(NSNotification*) {
      d->sleeping = true;
      d->update();
    });
    onWs(NSWorkspaceDidWakeNotification, ^(NSNotification*) {
      d->sleeping = false;
      d->update();
      if (d->onWake && *d->onWake) (*d->onWake)();
    });
    onWs(NSWorkspaceScreensDidSleepNotification, ^(NSNotification*) {
      d->displaysAsleep = true;
      d->update();
    });
    onWs(NSWorkspaceScreensDidWakeNotification, ^(NSNotification*) {
      d->displaysAsleep = false;
      d->update();
      if (d->onWake && *d->onWake) (*d->onWake)();
    });
    onWs(NSWorkspaceSessionDidResignActiveNotification, ^(NSNotification*) {
      d->sessionInactive = true;
      d->update();
    });
    onWs(NSWorkspaceSessionDidBecomeActiveNotification, ^(NSNotification*) {
      d->sessionInactive = false;
      d->update();
    });

    // Screen lock: undocumented but long-standing distributed notifications.
    auto onDist = [&](NSString* name, void (^block)(NSNotification*)) {
        d->distObservers.push_back([SFBlockObserver observeDistributed:name block:block]);
    };
    onDist(@"com.apple.screenIsLocked", ^(NSNotification*) {
      d->screenLocked = true;
      d->update();
    });
    onDist(@"com.apple.screenIsUnlocked", ^(NSNotification*) {
      d->screenLocked = false;
      d->update();
      if (d->onWake && *d->onWake) (*d->onWake)();
    });

    d->localObservers.push_back([NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoPowerStateDidChangeNotification
                                                                                object:nil
                                                                                 queue:NSOperationQueue.mainQueue
                                                                            usingBlock:^(NSNotification*) {
                                                                              d->update();
                                                                            }]);
    d->psSource = IOPSNotificationCreateRunLoopSource(&Impl::powerSourceChanged, d);
    if (d->psSource) CFRunLoopAddSource(CFRunLoopGetMain(), d->psSource, kCFRunLoopDefaultMode);
    d->update();
}

void SystemEvents::stop() {
    Impl* d = impl_.get();
    for (id o : d->observers) [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:o];
    for (SFBlockObserver* o : d->distObservers) [o cancel];
    for (id o : d->localObservers) [NSNotificationCenter.defaultCenter removeObserver:o];
    d->observers.clear();
    d->distObservers.clear();
    d->localObservers.clear();
    if (d->psSource) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), d->psSource, kCFRunLoopDefaultMode);
        CFRelease(d->psSource);
        d->psSource = nullptr;
    }
    d->onChange = nullptr;
}

SystemState SystemEvents::state() const { return impl_->s; }

}  // namespace snow::mac
