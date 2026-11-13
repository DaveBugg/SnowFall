#include "tracker/window_tracker.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#include <libproc.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>

#include "diagnostics/log.h"
#include "system/text_util.h"

namespace snow::mac {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kFastHz = 60, kNormalHz = 30, kIdleHz = 10;
constexpr double kIdleAfterSeconds = 2.0;
constexpr double kHiddenVerifySeconds = 1.0;

int overlayLevelValue() { return static_cast<int>(CGWindowLevelForKey(kCGDockWindowLevelKey)) - 1; }

std::vector<ScreenInfo> readScreens() {
    std::vector<ScreenInfo> out;
    NSArray<NSScreen*>* screens = NSScreen.screens;
    if (screens.count == 0) return out;
    const double primaryHeight = screens[0].frame.size.height;
    for (NSUInteger i = 0; i < screens.count; ++i) {
        NSScreen* s = screens[i];
        ScreenInfo si;
        si.displayId = [s.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        const NSRect f = s.frame, v = s.visibleFrame;
        si.frame = cocoaToGlobal(CocoaRect{f.origin.x, f.origin.y, f.size.width, f.size.height}, primaryHeight);
        si.visible = cocoaToGlobal(CocoaRect{v.origin.x, v.origin.y, v.size.width, v.size.height}, primaryHeight);
        si.backingScale = static_cast<float>(s.backingScaleFactor);
        si.primary = i == 0;
        out.push_back(si);
    }
    return out;
}

// Exact Dock tile-list geometry via the Accessibility API (only when we are trusted).
bool dockGeometryAX(int& width, int& centerX) {
    if (!AXIsProcessTrusted()) return false;
    NSArray<NSRunningApplication*>* docks = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.dock"];
    if (docks.count == 0) return false;
    AXUIElementRef app = AXUIElementCreateApplication(docks[0].processIdentifier);
    if (!app) return false;
    AXUIElementSetMessagingTimeout(app, 0.25f);
    bool ok = false;
    CFArrayRef children = nullptr;
    if (AXUIElementCopyAttributeValue(app, kAXChildrenAttribute, reinterpret_cast<CFTypeRef*>(&children)) ==
            kAXErrorSuccess &&
        children) {
        for (CFIndex i = 0; i < CFArrayGetCount(children) && !ok; ++i) {
            auto child = static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(children, i)));
            CFTypeRef role = nullptr;
            if (AXUIElementCopyAttributeValue(child, kAXRoleAttribute, &role) != kAXErrorSuccess || !role) continue;
            const bool isList = CFGetTypeID(role) == CFStringGetTypeID() &&
                                CFStringCompare(static_cast<CFStringRef>(role), kAXListRole, 0) == kCFCompareEqualTo;
            CFRelease(role);
            if (!isList) continue;
            CFTypeRef pos = nullptr, size = nullptr;
            CGPoint p{};
            CGSize sz{};
            if (AXUIElementCopyAttributeValue(child, kAXPositionAttribute, &pos) == kAXErrorSuccess && pos &&
                AXUIElementCopyAttributeValue(child, kAXSizeAttribute, &size) == kAXErrorSuccess && size &&
                AXValueGetValue(static_cast<AXValueRef>(pos), kAXValueTypeCGPoint, &p) &&
                AXValueGetValue(static_cast<AXValueRef>(size), kAXValueTypeCGSize, &sz) && sz.width > 20) {
                width = static_cast<int>(std::lround(sz.width));
                centerX = static_cast<int>(std::lround(p.x + sz.width / 2));
                ok = true;
            }
            if (pos) CFRelease(pos);
            if (size) CFRelease(size);
        }
        CFRelease(children);
    }
    CFRelease(app);
    return ok;
}

// Estimate from com.apple.dock preferences: tiles (pinned apps, running unpinned apps,
// recents, Finder, folders, Trash) x tile size + separators and padding.
int dockWidthEstimate() {
    auto pref = [](CFStringRef key) -> id { return CFBridgingRelease(CFPreferencesCopyAppValue(key, CFSTR("com.apple.dock"))); };
    double tile = 48;
    if (NSNumber* n = pref(CFSTR("tilesize")); [n isKindOfClass:[NSNumber class]] && n.doubleValue >= 16) tile = n.doubleValue;
    NSArray* apps = pref(CFSTR("persistent-apps"));
    NSArray* others = pref(CFSTR("persistent-others"));
    NSArray* recents = pref(CFSTR("recent-apps"));
    NSNumber* showRecents = pref(CFSTR("show-recents"));
    NSMutableSet<NSString*>* pinned = [NSMutableSet set];
    if ([apps isKindOfClass:[NSArray class]]) {
        for (NSDictionary* item in apps) {
            if (![item isKindOfClass:[NSDictionary class]]) continue;
            NSDictionary* td = item[@"tile-data"];
            NSString* bid = [td isKindOfClass:[NSDictionary class]] ? td[@"bundle-identifier"] : nil;
            if ([bid isKindOfClass:[NSString class]]) [pinned addObject:bid.lowercaseString];
        }
    }
    int count = 1 /* Finder */ + 1 /* Trash */ + static_cast<int>(pinned.count);
    if ([others isKindOfClass:[NSArray class]]) count += static_cast<int>(others.count);
    const bool recentsOn = ![showRecents isKindOfClass:[NSNumber class]] || showRecents.boolValue;
    int separators = 1;
    if (recentsOn && [recents isKindOfClass:[NSArray class]] && recents.count) {
        count += static_cast<int>(std::min<NSUInteger>(recents.count, 3));
        ++separators;
    }
    for (NSRunningApplication* a in NSWorkspace.sharedWorkspace.runningApplications) {
        if (a.activationPolicy != NSApplicationActivationPolicyRegular) continue;
        NSString* bid = a.bundleIdentifier.lowercaseString;
        if (!bid || [bid isEqualToString:@"com.apple.finder"] || [pinned containsObject:bid]) continue;
        ++count;
    }
    const double w = count * (tile + 4.0) + separators * 14.0 + 12.0;
    return static_cast<int>(w * 0.97);   // rather a little short than overhanging
}

std::vector<std::string> processNames(int pid) {
    std::vector<std::string> names;
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (proc_pidpath(pid, path, sizeof path) <= 0) return names;
    const std::string p = path;
    const std::size_t slash = p.find_last_of('/');
    names.push_back(toLowerAscii(slash == std::string::npos ? p : p.substr(slash + 1)));
    // ".../Name.app/Contents/MacOS/exe" -> also "name"
    const std::size_t app = p.rfind(".app/");
    if (app != std::string::npos) {
        const std::size_t start = p.rfind('/', app);
        if (start != std::string::npos) names.push_back(toLowerAscii(p.substr(start + 1, app - start - 1)));
    }
    return names;
}

}  // namespace

struct WindowTracker::Impl {
    // ---- shared (guarded by m) --------------------------------------------------------
    mutable std::mutex m;
    std::vector<ScreenInfo> screens;
    std::set<std::uint32_t> ignored;
    std::vector<std::string> excluded;
    int frontPid = 0;
    int dockWidth = 0, dockCenterX = 0;
    bool dockAX = false;
    bool forcePublish = false;
    std::shared_ptr<const DesktopLayout> current;
    TrackerStats stats;

    // ---- main thread ------------------------------------------------------------------
    LayoutCallback cb;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    id screenObserver = nil, activateObserver = nil;
    NSTimer* geometryTimer = nil;
    bool running = false;

    // ---- poll queue -------------------------------------------------------------------
    dispatch_queue_t queue = nullptr;
    dispatch_source_t timer = nullptr;
    LayoutBuilder builder;
    double intervalSec = 0;
    Clock::time_point lastChange = Clock::now();
    Clock::time_point lastHiddenCheck{};
    Clock::time_point statWindow = Clock::now();
    int statPolls = 0;
    double statMs = 0;
    std::map<int, std::vector<std::string>> nameCache;
    Clock::time_point nameCacheAt = Clock::now();
    float cornerRadius = 10;

    Impl() {
        queue = dispatch_queue_create("com.davebugg.snowfall.tracker", DISPATCH_QUEUE_SERIAL);
        builder.context.ownPid = getpid();
        builder.context.overlayLevel = overlayLevelValue();
        const NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
        cornerRadius = v.majorVersion >= 26 ? 16.0f : 10.0f;
    }

    void refreshGeometry() {   // main thread
        std::vector<ScreenInfo> s = readScreens();
        int w = 0, cx = 0;
        const bool ax = dockGeometryAX(w, cx);
        if (!ax) {
            w = dockWidthEstimate();
            cx = 0;
        }
        const pid_t front = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m);
            changed = s != screens || w != dockWidth || cx != dockCenterX;
            if (s != screens) {
                for (const ScreenInfo& si : s)
                    SNOW_LOGI("tracker: screen %u %dx%d at %d,%d scale %.1f visible %d,%d %dx%d%s", si.displayId,
                              si.frame.width(), si.frame.height(), si.frame.l, si.frame.t, si.backingScale,
                              si.visible.l, si.visible.t, si.visible.width(), si.visible.height(),
                              si.primary ? " primary" : "");
            }
            // Only worth a line when some screen has a bottom Dock (hidden / side Dock: no surface).
            bool bottomDock = false;
            for (const ScreenInfo& si : s) bottomDock = bottomDock || dockEdge(si) == DockEdge::Bottom;
            if (bottomDock && (ax != dockAX || std::abs(w - dockWidth) > 8))
                SNOW_LOGI("tracker: Dock width %d pt (%s)", w, ax ? "accessibility" : "estimate");
            screens = std::move(s);
            dockWidth = w;
            dockCenterX = cx;
            dockAX = ax;
            frontPid = front;
        }
        if (changed) requestPoll();
    }

    void requestPoll() {
        {
            std::lock_guard<std::mutex> lk(m);
            forcePublish = true;
        }
        dispatch_async(queue, ^{
          poll();
        });
    }

    void armTimer(double hz) {
        const double sec = 1.0 / hz;
        if (!timer || std::fabs(sec - intervalSec) < 1e-6) return;
        intervalSec = sec;
        const auto ns = static_cast<std::int64_t>(sec * NSEC_PER_SEC);
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, ns), static_cast<std::uint64_t>(ns),
                                  static_cast<std::uint64_t>(ns / 8));
    }

    std::set<std::uint32_t> existing(const std::vector<std::uint32_t>& numbers) {
        std::set<std::uint32_t> out;
        if (numbers.empty()) return out;
        std::vector<const void*> raw;
        raw.reserve(numbers.size());
        for (std::uint32_t n : numbers) raw.push_back(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(n)));
        CFArrayRef ids = CFArrayCreate(nullptr, raw.data(), static_cast<CFIndex>(raw.size()), nullptr);
        if (!ids) return out;
        NSArray* desc = CFBridgingRelease(CGWindowListCreateDescriptionFromArray(ids));
        CFRelease(ids);
        for (NSDictionary* d in desc) {
            NSNumber* n = d[(__bridge id)kCGWindowNumber];
            if (n) out.insert(n.unsignedIntValue);
        }
        return out;
    }

    std::vector<std::string> names(int pid) {
        if (Clock::now() - nameCacheAt > std::chrono::seconds(60)) {
            nameCache.clear();
            nameCacheAt = Clock::now();
        }
        auto it = nameCache.find(pid);
        if (it != nameCache.end()) return it->second;
        return nameCache[pid] = processNames(pid);
    }

    // Poll queue.
    void poll() {
        const Clock::time_point t0 = Clock::now();
        TrackerFrame f;
        bool force = false;
        {
            std::lock_guard<std::mutex> lk(m);
            f.screens = screens;
            f.frontPid = frontPid;
            f.dockWidth = dockWidth;
            f.dockCenterX = dockCenterX;
            builder.context.ignored = ignored;
            builder.excluded = excluded;
            force = forcePublish;
            forcePublish = false;
        }
        f.cornerRadius = cornerRadius;
        @autoreleasepool {
            NSArray* list = CFBridgingRelease(CGWindowListCopyWindowInfo(
                kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
            f.windows.reserve(list.count);
            for (NSDictionary* d in list) {
                RawWindow w;
                w.number = [d[(__bridge id)kCGWindowNumber] unsignedIntValue];
                w.pid = [d[(__bridge id)kCGWindowOwnerPID] intValue];
                w.layer = [d[(__bridge id)kCGWindowLayer] intValue];
                CGRect r = CGRectZero;
                NSDictionary* b = d[(__bridge id)kCGWindowBounds];
                if (b) CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)b, &r);
                w.bounds = roundRect(r.origin.x, r.origin.y, r.size.width, r.size.height);
                NSNumber* a = d[(__bridge id)kCGWindowAlpha];
                w.alpha = a ? a.floatValue : 1.0f;
                NSNumber* on = d[(__bridge id)kCGWindowIsOnscreen];
                w.onscreen = on ? on.boolValue : true;
                NSString* owner = d[(__bridge id)kCGWindowOwnerName];
                w.owner = owner.UTF8String ?: "";
                f.windows.push_back(std::move(w));
            }
        }
        const Clock::time_point now = Clock::now();
        const bool verify = now - lastHiddenCheck > std::chrono::duration<double>(kHiddenVerifySeconds);
        if (verify) lastHiddenCheck = now;
        DesktopLayout out;
        const bool changed = builder.build(
            f, [this](const std::vector<std::uint32_t>& n) { return existing(n); },
            [this](int pid) { return names(pid); }, out, verify);
        if (changed) lastChange = now;

        std::shared_ptr<const DesktopLayout> publishMe;
        if (changed || force) {
            auto layout = std::make_shared<DesktopLayout>(changed ? out : builder.last());
            std::lock_guard<std::mutex> lk(m);
            current = layout;
            publishMe = layout;
        }
        // Adaptive rate.
        const bool leftDown = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
        const double idle = std::chrono::duration<double>(now - lastChange).count();
        armTimer(leftDown ? kFastHz : (idle < kIdleAfterSeconds ? kNormalHz : kIdleHz));

        ++statPolls;
        statMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const double win = std::chrono::duration<double>(now - statWindow).count();
        if (win >= 2.0) {
            std::lock_guard<std::mutex> lk(m);
            stats.pollsPerSecond = statPolls / win;
            stats.avgPollMs = statPolls ? statMs / statPolls : 0;
            stats.windowsSeen = static_cast<int>(f.windows.size());
            stats.dockWidth = dockWidth;
            stats.dockFromAccessibility = dockAX;
            statWindow = now;
            statPolls = 0;
            statMs = 0;
        }
        if (publishMe) {
            std::shared_ptr<std::atomic<bool>> live = alive;
            Impl* self = this;
            dispatch_async(dispatch_get_main_queue(), ^{
              if (live->load() && self->cb) self->cb(publishMe);
            });
        }
    }
};

WindowTracker::WindowTracker() : impl_(std::make_unique<Impl>()) {}

WindowTracker::~WindowTracker() {
    stop();
    impl_->alive->store(false);
    if (impl_->queue) dispatch_sync(impl_->queue, ^{});
}

bool WindowTracker::start(LayoutCallback cb) {
    Impl& d = *impl_;
    if (d.running) return true;
    d.cb = std::move(cb);
    d.refreshGeometry();
    Impl* self = &d;
    dispatch_sync(d.queue, ^{
      self->poll();
    });
    // Deliver the first layout synchronously.
    if (auto l = current(); l && d.cb) d.cb(l);

    d.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, d.queue);
    dispatch_source_set_event_handler(d.timer, ^{
      self->poll();
    });
    d.intervalSec = 0;
    d.armTimer(kNormalHz);
    dispatch_resume(d.timer);

    d.screenObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                                       object:nil
                                                                        queue:NSOperationQueue.mainQueue
                                                                   usingBlock:^(NSNotification*) {
                                                                     self->refreshGeometry();
                                                                     self->requestPoll();
                                                                   }];
    d.activateObserver =
        [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification
                                                                    object:nil
                                                                     queue:NSOperationQueue.mainQueue
                                                                usingBlock:^(NSNotification*) {
                                                                  {
                                                                      std::lock_guard<std::mutex> lk(self->m);
                                                                      self->frontPid = NSWorkspace.sharedWorkspace
                                                                                           .frontmostApplication.processIdentifier;
                                                                  }
                                                                  self->requestPoll();
                                                                }];
    d.geometryTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                      repeats:YES
                                                        block:^(NSTimer*) {
                                                          self->refreshGeometry();
                                                        }];
    d.running = true;
    return true;
}

void WindowTracker::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.running = false;
    [d.geometryTimer invalidate];
    d.geometryTimer = nil;
    if (d.screenObserver) [NSNotificationCenter.defaultCenter removeObserver:d.screenObserver];
    if (d.activateObserver) [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:d.activateObserver];
    d.screenObserver = d.activateObserver = nil;
    if (d.timer) {
        dispatch_source_cancel(d.timer);
        d.timer = nullptr;
    }
    dispatch_sync(d.queue, ^{});
    d.cb = nullptr;
}

void WindowTracker::setOptions(const TrackerOptions& options) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->excluded.clear();
    for (const std::string& e : options.excludedProcesses) {
        std::string l = toLowerAscii(e);
        while (!l.empty() && l.back() == ' ') l.pop_back();
        if (!l.empty()) impl_->excluded.push_back(l);
    }
}

void WindowTracker::setIgnoredWindows(const std::set<std::uint32_t>& windowNumbers) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->ignored = windowNumbers;
}

void WindowTracker::refreshScreens() { impl_->refreshGeometry(); }
void WindowTracker::requestResync() { impl_->requestPoll(); }

std::vector<ScreenInfo> WindowTracker::screens() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->screens;
}

std::shared_ptr<const DesktopLayout> WindowTracker::current() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->current;
}

TrackerStats WindowTracker::stats() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->stats;
}

std::shared_ptr<const DesktopLayout> WindowTracker::captureOnce() {
    Impl& d = *impl_;
    d.refreshGeometry();
    {
        std::lock_guard<std::mutex> lk(d.m);
        d.forcePublish = true;
    }
    Impl* self = &d;
    dispatch_sync(d.queue, ^{
      self->poll();
    });
    return current();
}

std::vector<std::string> runningAppNamesWithWindows() {
    std::set<int> pids;
    NSArray* list = CFBridgingRelease(
        CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    const int own = getpid();
    for (NSDictionary* d in list) {
        if ([d[(__bridge id)kCGWindowLayer] intValue] != 0) continue;
        const int pid = [d[(__bridge id)kCGWindowOwnerPID] intValue];
        if (pid != own) pids.insert(pid);
    }
    std::set<std::string> names;
    for (int pid : pids) {
        NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        NSString* exe = app.executableURL.lastPathComponent;
        if (exe.length) names.insert(exe.UTF8String);
    }
    return std::vector<std::string>(names.begin(), names.end());
}

}  // namespace snow::mac
