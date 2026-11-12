#include "app/preview_view.h"

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <chrono>
#include <memory>

#include "diagnostics/log.h"
#include "render/metal_renderer.h"
#include "settings/settings_store.h"
#include "simulation/world.h"

namespace {

constexpr int kDesktopW = 400;   // virtual desktop of the preview, points
constexpr int kDesktopH = 250;
constexpr float kSpeedUp = 4.0f;          // simulated seconds per real second (PreviewHint "4x")
constexpr float kStep = 1.0f / 30.0f;
constexpr int kMaxStepsPerTick = 12;
constexpr float kWarmUpSeconds = 360.0f;  // pre-simulated on open / reset
constexpr double kApplyInterval = 0.05;   // settings coalesced to <= 20 Hz

snow::RectI winA() { return snow::RectI{28, 84, 208, 196}; }
snow::RectI winB() { return snow::RectI{184, 46, 370, 152}; }
snow::RectI dock() { return snow::RectI{90, 228, 310, kDesktopH - 1}; }

snow::DesktopLayout previewLayout() {
    using namespace snow;
    DesktopLayout l;
    MonitorInfo m;
    m.bounds = RectI{0, 0, kDesktopW, kDesktopH};
    m.work = RectI{0, 10, kDesktopW, dock().t};
    m.primary = true;
    l.revision = 1;
    l.monitors.push_back(m);
    WindowInfo bar;
    bar.id = 3;
    bar.kind = WindowKind::Taskbar;
    bar.bounds = dock();
    WindowInfo front;
    front.id = 1;
    front.bounds = winB();
    front.cornerRadiusPx = 6;
    WindowInfo back;
    back.id = 2;
    back.bounds = winA();
    back.cornerRadiusPx = 6;
    l.windows = {bar, front, back};   // front to back
    return l;
}

}  // namespace

@implementation SFPreviewViewImpl {
    std::unique_ptr<snow::World> _world;
    std::unique_ptr<snow::mac::MetalRenderer> _renderer;
    snow::Settings _settings;
    snow::DesktopLayout _layout;
    CAMetalLayer* _metal;
    NSTimer* _timer;
    NSString* _pendingJSON;
    double _lastApply;
    float _carry;
    std::chrono::steady_clock::time_point _lastTick;
    BOOL _failed;
}

- (instancetype)initWithSettingsJSON:(NSString*)json {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, kDesktopW, kDesktopH)])) {
        _layout = previewLayout();
        if (json.length) snow::fromJson(json.UTF8String, _settings);
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
        id<MTLDevice> device = snow::mac::pickMetalDevice();
        _renderer = std::make_unique<snow::mac::MetalRenderer>();
        std::string err;
        if (!device || !_renderer->initialize(device, &err)) {
            SNOW_LOGW("preview: Metal renderer unavailable (%s)", err.c_str());
            _failed = YES;
        } else {
            _metal = [CAMetalLayer layer];
            _metal.device = device;
            _metal.pixelFormat = snow::mac::MetalRenderer::pixelFormat();
            _metal.opaque = NO;
            _metal.framebufferOnly = YES;
            CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            _metal.colorspace = srgb;
            CGColorSpaceRelease(srgb);
            [self.layer addSublayer:_metal];
        }
        [self resetPreview];
    }
    return self;
}

- (void)dealloc {
    [_timer invalidate];
}

- (BOOL)isFlipped {
    return YES;   // desktop coordinates grow downwards
}

// Fitted, centred desktop rectangle inside the view (points) and its scale.
- (NSRect)desktopRect:(CGFloat*)k {
    const NSRect b = self.bounds;
    const CGFloat s = std::max<CGFloat>(0.01, std::min(b.size.width / kDesktopW, b.size.height / kDesktopH));
    if (k) *k = s;
    const CGFloat w = kDesktopW * s, h = kDesktopH * s;
    return NSMakeRect(b.origin.x + (b.size.width - w) / 2, b.origin.y + (b.size.height - h) / 2, w, h);
}

- (void)layout {
    [super layout];
    [self updateMetalGeometry];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateMetalGeometry];
}

- (void)updateMetalGeometry {
    if (!_metal) return;
    CGFloat k = 1;
    const NSRect r = [self desktopRect:&k];
    const CGFloat backing = self.window ? self.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _metal.frame = r;
    _metal.contentsScale = backing;
    _metal.drawableSize = CGSizeMake(std::max<CGFloat>(1, r.size.width * backing), std::max<CGFloat>(1, r.size.height * backing));
    [CATransaction commit];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [_timer invalidate];
    _timer = nil;
    if (self.window && !_failed) {
        __weak SFPreviewViewImpl* weakSelf = self;
        _timer = [NSTimer timerWithTimeInterval:1.0 / 30.0
                                        repeats:YES
                                          block:^(NSTimer*) {
                                            [weakSelf tick];
                                          }];
        [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
        _lastTick = std::chrono::steady_clock::now();
        [self updateMetalGeometry];
    }
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateMetalGeometry];
}

- (void)applySettingsNow {
    snow::Settings s = _settings;
    s.interaction.enabled = false;           // no tool in the preview
    s.system.excludedProcesses.clear();      // exclusions / fullscreen pause do not apply
    snow::RuntimeTuning tuning;
    tuning.animationsEnabled = s.performance.animations;
    if (_world) _world->applySettings(s, tuning);
}

- (void)setSettingsJSON:(NSString*)json {
    _pendingJSON = [json copy];
}

- (void)resetPreview {
    _world = std::make_unique<snow::World>(0x5EED1234ull);
    _world->setLayout(_layout);
    [self applySettingsNow];
    _world->setEmissionEnabled(true);
    const int steps = static_cast<int>(kWarmUpSeconds / kStep + 0.5f);
    for (int i = 0; i < steps; ++i) _world->step(kStep);
    _carry = 0;
    if (_renderer) _renderer->reset();
}

- (void)tick {
    const auto now = std::chrono::steady_clock::now();
    const float realDt = std::chrono::duration<float>(now - _lastTick).count();
    _lastTick = now;
    if (_pendingJSON && CFAbsoluteTimeGetCurrent() - _lastApply >= kApplyInterval) {
        snow::Settings s;
        if (snow::fromJson(_pendingJSON.UTF8String, s)) {
            _settings = s;
            [self applySettingsNow];
        }
        _pendingJSON = nil;
        _lastApply = CFAbsoluteTimeGetCurrent();
    }
    if (!_world || !_renderer || !_metal) return;
    _carry += std::min(realDt, 0.25f) * kSpeedUp;
    int steps = 0;
    while (_carry >= kStep && steps < kMaxStepsPerTick) {
        _world->step(kStep);
        _carry -= kStep;
        ++steps;
    }
    if (steps == kMaxStepsPerTick) _carry = 0;
    auto snap = std::make_shared<snow::FrameSnapshot>();
    _world->buildSnapshot(*snap);
    snap->taken = now;

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [_metal nextDrawable];
        if (!drawable) return;
        _renderer->beginFrame();
        _renderer->prepare(snap);
        id<MTLCommandBuffer> cb = [_renderer->queue() commandBuffer];
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        snow::mac::RenderTarget t;
        t.bounds = snow::RectI{0, 0, kDesktopW, kDesktopH};
        t.widthPx = static_cast<int>(drawable.texture.width);
        t.heightPx = static_cast<int>(drawable.texture.height);
        t.scale = static_cast<float>(t.widthPx) / kDesktopW;
        _renderer->encode(enc, t, 0.0f, 1.0f);
        [enc endEncoding];
        [cb presentDrawable:drawable];
        _renderer->endFrame(cb);
        [cb commit];
    }
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    CGFloat k = 1;
    const NSRect r = [self desktopRect:&k];
    auto X = [&](double x) { return r.origin.x + x * k; };
    auto Y = [&](double y) { return r.origin.y + y * k; };
    // Wallpaper: night-blue gradient.
    NSGradient* sky = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithSRGBRed:0.10 green:0.16 blue:0.32 alpha:1]
                                                    endingColor:[NSColor colorWithSRGBRed:0.27 green:0.36 blue:0.58 alpha:1]];
    [sky drawInBezierPath:[NSBezierPath bezierPathWithRoundedRect:r xRadius:6 * k yRadius:6 * k] angle:90];
    // Menu bar.
    [[NSColor colorWithWhite:1 alpha:0.18] setFill];
    NSRectFill(NSMakeRect(X(0), Y(0), kDesktopW * k, 10 * k));
    // Windows (back to front).
    for (const snow::RectI& w : {winA(), winB()}) {
        const NSRect wr = NSMakeRect(X(w.l), Y(w.t), w.width() * k, w.height() * k);
        NSBezierPath* body = [NSBezierPath bezierPathWithRoundedRect:wr xRadius:6 * k yRadius:6 * k];
        [[NSColor colorWithSRGBRed:0.93 green:0.94 blue:0.96 alpha:1] setFill];
        [body fill];
        [[NSColor colorWithSRGBRed:0.80 green:0.82 blue:0.86 alpha:1] setFill];
        NSRectFill(NSMakeRect(wr.origin.x, wr.origin.y + 6 * k, wr.size.width, 10 * k));
        int i = 0;
        for (NSColor* c in @[ NSColor.systemRedColor, NSColor.systemYellowColor, NSColor.systemGreenColor ]) {
            [c setFill];
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(wr.origin.x + (7 + 9 * i) * k, wr.origin.y + 4 * k, 5 * k, 5 * k)] fill];
            ++i;
        }
        [[NSColor colorWithWhite:0.2 alpha:0.25] setStroke];
        [body stroke];
    }
    // Dock.
    const snow::RectI d = dock();
    NSBezierPath* dockPath = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(X(d.l), Y(d.t), d.width() * k, d.height() * k)
                                                             xRadius:5 * k
                                                             yRadius:5 * k];
    [[NSColor colorWithWhite:1 alpha:0.35] setFill];
    [dockPath fill];
}

@end
