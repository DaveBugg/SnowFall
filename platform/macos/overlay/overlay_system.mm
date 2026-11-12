#include "overlay/overlay_system.h"

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "diagnostics/log.h"
#include "render/metal_renderer.h"

// CVDisplayLink is deprecated since macOS 15 in favour of NSView/NSScreen display links, which
// deliver on the main run loop; a CVDisplayLink still works everywhere from macOS 13 on and
// ticks on its own thread, which is what a dedicated render thread needs.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

@interface SFOverlayWindow : NSWindow
@end
@implementation SFOverlayWindow
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
// Borderless windows must be allowed to cover the menu bar area of their screen.
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen {
    (void)screen;
    return frameRect;
}
@end

namespace snow::mac {
namespace {

using Clock = std::chrono::steady_clock;

struct Target {
    std::uint64_t key = 0;
    CAMetalLayer* layer = nil;   // strong (ARC): stays alive while the render thread holds it
    RectI bounds;                // points
    int widthPx = 0, heightPx = 0;
    float scale = 1;
};

NSWindowLevel overlayLevel() { return CGWindowLevelForKey(kCGDockWindowLevelKey) - 1; }

}  // namespace

struct OverlaySystem::Impl {
    // ---- main thread -----------------------------------------------------------------
    struct UiOverlay {
        std::uint64_t key = 0;
        SFOverlayWindow* window = nil;
        CAMetalLayer* layer = nil;
        ScreenInfo screen;
    };
    std::vector<UiOverlay> wins;
    std::uint64_t nextKey = 1;
    bool initialized = false;
    const SnapshotExchange* source = nullptr;
    id<MTLDevice> device;
    std::string deviceName;

    // ---- shared ----------------------------------------------------------------------
    std::mutex m;
    std::condition_variable cv;
    bool woken = false;            // guarded by m
    bool tick = false;             // guarded by m: display link fired
    std::vector<Target> targets;   // guarded by m
    std::thread thread;
    std::atomic<bool> quit{false};
    std::atomic<bool> visibleFlag{true};
    std::atomic<bool> failed{false};
    std::atomic<int> fpsCap{60};
    std::atomic<float> opacity{1.0f};
    std::atomic<bool> badge{false};
    std::atomic<float> statFps{0}, statFrameMs{0};
    std::atomic<std::uint64_t> statFrames{0};
    std::atomic<int> statGpuErrors{0};
    std::atomic<long long> lastPresentMs{0};
    CVDisplayLinkRef link = nullptr;
    std::atomic<bool> linkRunning{false};

    // ---- render thread ----------------------------------------------------------------
    MetalRenderer renderer;

    void wake() {
        {
            std::lock_guard<std::mutex> lk(m);
            woken = true;
        }
        cv.notify_all();
    }

    static CVReturn linkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*,
                                 void* ctx) {
        auto* self = static_cast<Impl*>(ctx);
        {
            std::lock_guard<std::mutex> lk(self->m);
            self->tick = true;
        }
        self->cv.notify_all();
        return kCVReturnSuccess;
    }

    void setLinkRunning(bool on) {
        if (!link || linkRunning.load() == on) return;
        if (on) {
            CVDisplayLinkStart(link);
        } else {
            CVDisplayLinkStop(link);
        }
        linkRunning.store(on);
    }

    CAMetalLayer* makeLayer(const ScreenInfo& s) {
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MetalRenderer::pixelFormat();
        layer.framebufferOnly = YES;
        layer.opaque = NO;
        layer.backgroundColor = nil;
        layer.presentsWithTransaction = NO;
        layer.displaySyncEnabled = YES;
        layer.maximumDrawableCount = 3;
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer.colorspace = srgb;
        CGColorSpaceRelease(srgb);
        layer.contentsScale = s.backingScale;
        layer.drawableSize = CGSizeMake(s.frame.width() * s.backingScale, s.frame.height() * s.backingScale);
        return layer;
    }

    SFOverlayWindow* makeWindow(const ScreenInfo& s, double primaryHeight, CAMetalLayer* layer) {
        const CocoaRect c = globalToCocoa(s.frame, primaryHeight);
        const NSRect frame = NSMakeRect(c.x, c.y, c.w, c.h);
        SFOverlayWindow* w = [[SFOverlayWindow alloc] initWithContentRect:frame
                                                                styleMask:NSWindowStyleMaskBorderless
                                                                  backing:NSBackingStoreBuffered
                                                                    defer:NO];
        w.releasedWhenClosed = NO;
        w.opaque = NO;
        w.backgroundColor = NSColor.clearColor;
        w.hasShadow = NO;
        w.ignoresMouseEvents = YES;
        w.level = overlayLevel();
        w.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary |
                               NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
        w.animationBehavior = NSWindowAnimationBehaviorNone;
        w.excludedFromWindowsMenu = YES;
        w.title = @"SnowFall overlay";
        NSView* v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, c.w, c.h)];
        v.layer = layer;   // layer-hosting view
        v.wantsLayer = YES;
        w.contentView = v;
        [w setFrame:frame display:NO];
        return w;
    }

    void publish() {
        std::vector<Target> t;
        for (const UiOverlay& o : wins) {
            Target x;
            x.key = o.key;
            x.layer = o.layer;
            x.bounds = o.screen.frame;
            x.scale = o.screen.backingScale > 0 ? o.screen.backingScale : 1.0f;
            x.widthPx = static_cast<int>(std::lround(o.screen.frame.width() * x.scale));
            x.heightPx = static_cast<int>(std::lround(o.screen.frame.height() * x.scale));
            t.push_back(x);
        }
        {
            std::lock_guard<std::mutex> lk(m);
            targets = std::move(t);
            woken = true;
        }
        cv.notify_all();
    }

    // ---- render thread ----------------------------------------------------------------
    void clearAll(const std::vector<Target>& ts) {
        @autoreleasepool {
            renderer.beginFrame();
            id<MTLCommandBuffer> cb = [renderer.queue() commandBuffer];
            bool any = false;
            for (const Target& t : ts) {
                id<CAMetalDrawable> drawable = [t.layer nextDrawable];
                if (!drawable) continue;
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = drawable.texture;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc endEncoding];
                [cb presentDrawable:drawable];
                any = true;
            }
            renderer.endFrame(any ? cb : nil);
            if (any) [cb commit];
        }
    }

    void threadMain() {
        pthread_setname_np("SnowFall render");
        bool wasActive = false;
        bool force = true;
        float lastOpacity = -1, lastDt = -1;
        std::shared_ptr<const FrameSnapshot> lastSnap;
        Clock::time_point lastPresent{};
        Clock::time_point windowStart = Clock::now();
        int windowFrames = 0;
        double windowCpuMs = 0;
        int cap = 60;
        std::vector<Target> ts;
        while (!quit.load()) {
            {
                std::lock_guard<std::mutex> lk(m);
                ts = targets;
            }
            const bool active = visibleFlag.load() && !failed.load() && !ts.empty();
            if (!active) {
                setLinkRunning(false);
                if (wasActive && !ts.empty()) clearAll(ts);   // nothing stale flashes on the next show
                wasActive = false;
                lastSnap.reset();
                statFps.store(0);
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return woken || quit.load(); });
                woken = false;
                continue;
            }
            if (!wasActive) {
                wasActive = true;
                force = true;
                windowStart = Clock::now();
                windowFrames = 0;
                windowCpuMs = 0;
                setLinkRunning(true);
            }
            // Pace on the display link (fallback: ~120 Hz timer when it could not be created).
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait_for(lk, std::chrono::milliseconds(link ? 50 : 8), [&] { return tick || woken || quit.load(); });
                if (woken) force = true;
                tick = false;
                woken = false;
            }
            if (quit.load()) break;
            const int want = fpsCap.load() <= 30 ? 30 : 60;
            if (want != cap) {
                cap = want;
                windowStart = Clock::now();
                windowFrames = 0;
                windowCpuMs = 0;
            }
            const Clock::time_point now = Clock::now();
            const double since = std::chrono::duration<double>(now - lastPresent).count();
            if (!force && since < 1.0 / cap - 0.004) continue;

            const float globalOpacity = opacity.load();
            std::shared_ptr<const FrameSnapshot> snap = source->latest();
            float dt = 0;
            bool moving = false;
            if (snap && snap->taken != Clock::time_point{}) {
                dt = std::clamp(std::chrono::duration<float>(now - snap->taken).count(), 0.0f, 0.05f);
                moving = !snap->particles.sprites.empty() || !snap->objects.empty() || !snap->cursor.sprites.empty();
            }
            const bool newSnap = snap != lastSnap;
            if (!(force || newSnap || globalOpacity != lastOpacity || (moving && dt != lastDt))) continue;

            const Clock::time_point t0 = Clock::now();
            @autoreleasepool {
                renderer.setCursorBadgeOffset(badge.load() ? 14.0f : 0.0f, badge.load() ? 18.0f : 0.0f);
                renderer.beginFrame();
                renderer.prepare(snap);
                lastSnap = snap;
                id<MTLCommandBuffer> cb = [renderer.queue() commandBuffer];
                bool any = false;
                for (const Target& t : ts) {
                    id<CAMetalDrawable> drawable = [t.layer nextDrawable];
                    if (!drawable) continue;
                    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = drawable.texture;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                    RenderTarget rt;
                    rt.bounds = t.bounds;
                    // The drawable may lag a resize by a frame: draw for its actual size.
                    rt.widthPx = static_cast<int>(drawable.texture.width);
                    rt.heightPx = static_cast<int>(drawable.texture.height);
                    rt.scale = t.bounds.width() > 0 ? static_cast<float>(rt.widthPx) / static_cast<float>(t.bounds.width())
                                                    : t.scale;
                    if (snap) renderer.encode(enc, rt, dt, globalOpacity);
                    [enc endEncoding];
                    [cb presentDrawable:drawable];
                    any = true;
                }
                if (any) {
                    std::atomic<int>* errors = &statGpuErrors;
                    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                      if (done.status == MTLCommandBufferStatusError) {
                          const int n = errors->fetch_add(1) + 1;
                          if (n <= 5 || n % 100 == 0)
                              SNOW_LOGW("render: command buffer failed (%ld), %d so far",
                                        static_cast<long>(done.error.code), n);
                      }
                    }];
                }
                renderer.endFrame(any ? cb : nil);
                if (any) [cb commit];
            }
            force = false;
            lastOpacity = globalOpacity;
            lastDt = dt;
            lastPresent = Clock::now();
            const double cpuMs = std::chrono::duration<double, std::milli>(lastPresent - t0).count();
            statFrames.fetch_add(1);
            ++windowFrames;
            windowCpuMs += cpuMs;
            lastPresentMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(lastPresent.time_since_epoch()).count());
            const double windowSec = std::chrono::duration<double>(lastPresent - windowStart).count();
            if (windowSec >= 1.0) {
                statFps.store(static_cast<float>(windowFrames / windowSec));
                statFrameMs.store(static_cast<float>(windowCpuMs / windowFrames));
                windowStart = lastPresent;
                windowFrames = 0;
                windowCpuMs = 0;
            }
        }
        setLinkRunning(false);
    }
};

OverlaySystem::OverlaySystem() : impl_(std::make_unique<Impl>()) {}
OverlaySystem::~OverlaySystem() { shutdown(); }

bool OverlaySystem::initialize(const SnapshotExchange* source, std::string* error) {
    Impl& d = *impl_;
    if (d.initialized) return true;
    if (!source) return false;
    d.source = source;
    d.device = pickMetalDevice();
    if (!d.device) {
        if (error) *error = "Metal is not available";
        SNOW_LOGE("overlay: no Metal device");
        return false;
    }
    d.deviceName = d.device.name.UTF8String ?: "?";
    const auto t0 = Clock::now();
    if (!d.renderer.initialize(d.device, error)) {
        SNOW_LOGE("overlay: renderer initialisation failed: %s", error ? error->c_str() : "");
        return false;
    }
    SNOW_LOGI("render: Metal device \"%s\", shaders compiled in %.0f ms", d.deviceName.c_str(),
              std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    if (CVDisplayLinkCreateWithActiveCGDisplays(&d.link) == kCVReturnSuccess && d.link) {
        CVDisplayLinkSetOutputCallback(d.link, &Impl::linkCallback, &d);
    } else {
        d.link = nullptr;
        SNOW_LOGW("render: CVDisplayLink unavailable, using a timer");
    }
    d.quit.store(false);
    d.thread = std::thread([&d] { d.threadMain(); });
    d.initialized = true;
    return true;
}

void OverlaySystem::setScreens(const std::vector<ScreenInfo>& screens) {
    Impl& d = *impl_;
    if (!d.initialized) return;
    double primaryHeight = 0;
    for (const ScreenInfo& s : screens)
        if (s.primary) primaryHeight = s.frame.b;
    if (primaryHeight <= 0 && !screens.empty()) primaryHeight = screens.front().frame.b;

    std::vector<Impl::UiOverlay> old = std::move(d.wins);
    d.wins.clear();
    std::vector<bool> used(old.size(), false);
    const bool show = d.visibleFlag.load() && !d.failed.load();
    bool changed = false;
    for (const ScreenInfo& s : screens) {
        if (s.frame.empty()) continue;
        int pick = -1;
        for (std::size_t j = 0; j < old.size() && pick < 0; ++j)
            if (!used[j] && old[j].screen.displayId == s.displayId) pick = static_cast<int>(j);
        for (std::size_t j = 0; j < old.size() && pick < 0; ++j)
            if (!used[j]) pick = static_cast<int>(j);
        Impl::UiOverlay o;
        if (pick >= 0) {
            used[static_cast<std::size_t>(pick)] = true;
            o = old[static_cast<std::size_t>(pick)];
            if (!(o.screen == s)) {
                const CocoaRect c = globalToCocoa(s.frame, primaryHeight);
                [o.window setFrame:NSMakeRect(c.x, c.y, c.w, c.h) display:NO];
                o.layer.contentsScale = s.backingScale;
                o.layer.drawableSize = CGSizeMake(s.frame.width() * s.backingScale, s.frame.height() * s.backingScale);
                changed = true;
            }
        } else {
            o.key = d.nextKey++;
            o.layer = d.makeLayer(s);
            o.window = d.makeWindow(s, primaryHeight, o.layer);
            if (show) [o.window orderFrontRegardless];
            changed = true;
        }
        o.screen = s;
        d.wins.push_back(o);
    }
    std::vector<SFOverlayWindow*> doomed;
    for (std::size_t j = 0; j < old.size(); ++j)
        if (!used[j]) doomed.push_back(old[j].window);
    if (!changed && doomed.empty()) return;
    d.publish();   // the render thread keeps its own strong layer references until it re-reads
    for (SFOverlayWindow* w : doomed) [w orderOut:nil];
    SNOW_LOGI("overlay: %zu screen(s), %zu overlay(s), %zu removed", screens.size(), d.wins.size(), doomed.size());
}

void OverlaySystem::setVisible(bool visible) {
    Impl& d = *impl_;
    d.visibleFlag.store(visible);
    if (!d.initialized) return;
    const bool show = visible && !d.failed.load();
    for (const Impl::UiOverlay& o : d.wins) {
        if (show) {
            [o.window orderFrontRegardless];
        } else {
            [o.window orderOut:nil];
        }
    }
    d.wake();
}

bool OverlaySystem::visible() const { return impl_->visibleFlag.load() && !impl_->failed.load(); }

void OverlaySystem::setFpsCap(int fps) {
    impl_->fpsCap.store(fps <= 30 ? 30 : 60);
    impl_->wake();
}

void OverlaySystem::setGlobalOpacity(float o) {
    impl_->opacity.store(std::clamp(o, 0.1f, 1.0f));
    impl_->wake();
}

void OverlaySystem::setCursorBadge(bool on) {
    impl_->badge.store(on);
    impl_->wake();
}

std::set<std::uint32_t> OverlaySystem::windowNumbers() const {
    std::set<std::uint32_t> s;
    for (const Impl::UiOverlay& o : impl_->wins)
        if (o.window.windowNumber > 0) s.insert(static_cast<std::uint32_t>(o.window.windowNumber));
    return s;
}

RenderStats OverlaySystem::stats() const {
    const Impl& d = *impl_;
    RenderStats s;
    s.overlays = static_cast<int>(d.wins.size());
    s.failed = d.failed.load();
    s.frameMs = d.statFrameMs.load();
    s.frames = d.statFrames.load();
    s.gpuErrors = d.statGpuErrors.load();
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    const bool stale = nowMs - d.lastPresentMs.load() > 1500;
    s.fps = (stale || s.failed) ? 0.0f : d.statFps.load();
    return s;
}

std::string OverlaySystem::deviceName() const { return impl_->deviceName; }

void OverlaySystem::shutdown() {
    Impl& d = *impl_;
    if (!d.initialized) return;
    d.initialized = false;
    d.quit.store(true);
    d.wake();
    if (d.thread.joinable()) d.thread.join();
    if (d.link) {
        CVDisplayLinkStop(d.link);
        CVDisplayLinkRelease(d.link);
        d.link = nullptr;
    }
    for (const Impl::UiOverlay& o : d.wins) [o.window orderOut:nil];
    d.wins.clear();
    {
        std::lock_guard<std::mutex> lk(d.m);
        d.targets.clear();
    }
    SNOW_LOGI("overlay: shut down");
}

}  // namespace snow::mac
