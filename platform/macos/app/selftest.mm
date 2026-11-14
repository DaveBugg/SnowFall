#include "app/selftest.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "app/app_host.h"
#include "app/strings_mac.h"
#include "app/updater.h"
#include "core/skins.h"
#include "diagnostics/log.h"
#include "overlay/overlay_system.h"
#include "render/metal_renderer.h"
#include "runtime/sim_runner.h"
#include "settings/hotkey.h"
#include "settings/json.h"
#include "settings/settings_store.h"
#include "system/cursor_hider.h"
#include "system/platform_services.h"
#include "system/text_util.h"
#include "tracker/window_tracker.h"
#include "update/update.h"

extern char** environ;

namespace snow::mac {
SimPlatformHooks makeMacSimHooks();   // app_host.mm

namespace {

int g_failed = 0, g_passed = 0, g_skipped = 0;

void report(const char* tag, const std::string& name, const std::string& detail) {
    std::printf("[%s] %s%s%s\n", tag, name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
    std::fflush(stdout);
}
void pass(const std::string& n, const std::string& d = {}) {
    ++g_passed;
    report(" ok ", n, d);
}
void fail(const std::string& n, const std::string& d = {}) {
    ++g_failed;
    report("FAIL", n, d);
}
void skip(const std::string& n, const std::string& d = {}) {
    ++g_skipped;
    report("SKIP", n, d);
}
void check(bool ok, const std::string& n, const std::string& d = {}) { ok ? pass(n, d) : fail(n, d); }

void pumpRunLoop(double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        @autoreleasepool {
            [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    }
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ---- offscreen rendering ---------------------------------------------------------------------
struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> bgra;
    const std::uint8_t* px(int x, int y) const { return &bgra[(static_cast<std::size_t>(y) * w + x) * 4]; }
    int alpha(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : px(x, y)[3]; }
    int maxAlpha(int cx, int cy, int r) const {
        int m = 0;
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) m = std::max(m, alpha(x, y));
        return m;
    }
};

bool renderOffscreen(MetalRenderer& r, const std::shared_ptr<const FrameSnapshot>& snap, const RectI& bounds, float scale,
                     Image& out, std::string& err) {
    id<MTLDevice> dev = r.device();
    const int w = static_cast<int>(bounds.width() * scale), h = static_cast<int>(bounds.height() * scale);
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MetalRenderer::pixelFormat()
                                                                                  width:static_cast<NSUInteger>(w)
                                                                                 height:static_cast<NSUInteger>(h)
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    const NSUInteger rowBytes = static_cast<NSUInteger>(w) * 4;
    id<MTLBuffer> buf = [dev newBufferWithLength:rowBytes * static_cast<NSUInteger>(h) options:MTLResourceStorageModeShared];
    if (!tex || !buf) {
        err = "texture/buffer allocation failed";
        return false;
    }
    r.beginFrame();
    r.prepare(snap);
    id<MTLCommandBuffer> cb = [r.queue() commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    RenderTarget t;
    t.bounds = bounds;
    t.widthPx = w;
    t.heightPx = h;
    t.scale = scale;
    r.encode(enc, t, 0.0f, 1.0f);
    [enc endEncoding];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(static_cast<NSUInteger>(w), static_cast<NSUInteger>(h), 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:rowBytes
        destinationBytesPerImage:rowBytes * static_cast<NSUInteger>(h)];
    [blit endEncoding];
    r.endFrame(cb);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        err = fmt("command buffer status %ld (%s)", static_cast<long>(cb.status),
                  cb.error.localizedDescription.UTF8String ?: "");
        return false;
    }
    out.w = w;
    out.h = h;
    out.bgra.assign(static_cast<const std::uint8_t*>(buf.contents),
                    static_cast<const std::uint8_t*>(buf.contents) + rowBytes * static_cast<NSUInteger>(h));
    return true;
}

SpriteInstance sprite(Shape shape, float x, float y, float hw, float hh, std::uint8_t variant = 0) {
    SpriteInstance s;
    s.x = x;
    s.y = y;
    s.halfW = hw;
    s.halfH = hh;
    s.shape = shape;
    s.variant = variant;
    s.opacity = 1;
    return s;
}

void renderTests(id<MTLDevice> device) {
    MetalRenderer r;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!r.initialize(device, &err)) {
        fail("metal: shaders compile + pipelines", err + " (see the MSL errors in the log above)");
        return;
    }
    pass("metal: shaders compile + pipelines",
         fmt("%.0f ms on %s", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
             device.name.UTF8String ?: "?"));

    // Synthetic scene, 320 x 200 points.
    auto snap = std::make_shared<FrameSnapshot>();
    snap->frame = 1;
    snap->taken = std::chrono::steady_clock::now();
    DriftStrip d;
    d.id = 1;
    d.x0 = 20;
    d.baseY = 150;
    d.step = 3;
    d.version = 1;
    d.heights.assign(60, 20.0f);   // x 20..197, top at y = 130
    d.clip = {RectI{0, 0, 320, 200}};
    snap->drifts.push_back(d);
    SpriteBatch freeBatch;
    freeBatch.sprites.push_back(sprite(Shape::Snowball, 60, 40, 12, 12));
    snap->objects.push_back(freeBatch);
    SpriteBatch clipped;   // its only clip rect does not contain the ball: nothing may be drawn
    clipped.clip = {RectI{0, 0, 100, 100}};
    clipped.sprites.push_back(sprite(Shape::Snowball, 250, 40, 12, 12));
    snap->objects.push_back(clipped);
    snap->particles.sprites.push_back(sprite(Shape::Flake, 150, 60, 3, 3));
    snap->cursor.sprites.push_back(sprite(Shape::Mitten, 280, 120, 14, 14));

    for (float scale : {1.0f, 2.0f}) {
        Image img;
        const std::string tag = fmt("render @%gx: ", scale);
        if (!renderOffscreen(r, snap, RectI{0, 0, 320, 200}, scale, img, err)) {
            fail(tag + "offscreen frame", err);
            continue;
        }
        auto P = [&](int x) { return static_cast<int>(x * scale); };
        const std::uint8_t* corner = img.px(P(5), P(5));
        check(corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 0, tag + "transparent stays transparent");
        const std::uint8_t* drift = img.px(P(100), P(142));
        check(drift[3] > 200 && drift[0] > drift[3] * 3 / 4 && drift[1] > drift[3] * 3 / 4 && drift[2] > drift[3] * 3 / 4,
              tag + "drift draws white", fmt("bgra %d,%d,%d,%d", drift[0], drift[1], drift[2], drift[3]));
        check(img.alpha(P(100), P(124)) == 0, tag + "nothing above the drift", fmt("alpha %d", img.alpha(P(100), P(124))));
        check(img.alpha(P(60), P(40)) > 230, tag + "snowball", fmt("alpha %d", img.alpha(P(60), P(40))));
        check(img.maxAlpha(P(250), P(40), P(14)) == 0, tag + "scissor clip removes the clipped batch");
        check(img.maxAlpha(P(150), P(60), P(2)) > 0, tag + "flake particle");
        check(img.maxAlpha(P(280), P(120), P(6)) > 0, tag + "cursor sprite");
    }

    // Every shape / variant draws something (catches translation mistakes that zero a branch).
    auto gallery = std::make_shared<FrameSnapshot>();
    gallery->taken = std::chrono::steady_clock::now();
    struct Item {
        Shape s;
        float hw, hh;
        int variants;
        const char* name;
    };
    const Item items[] = {
        {Shape::Flake, 9, 9, 4, "flake"},         {Shape::Snowball, 12, 12, 1, "snowball"},
        {Shape::Coal, 5, 5, 1, "coal"},           {Shape::Carrot, 12, 4, 1, "carrot"},
        {Shape::Stick, 14, 5, 1, "stick"},        {Shape::Mitten, 14, 14, 2, "mitten"},
        {Shape::Shovel, 14, 16, 1, "shovel"},     {Shape::Ring, 14, 14, 1, "ring"},
        {Shape::Snowplow, 17, 8, 2, "snowplow"},  {Shape::SnowChunk, 14, 7, 4, "chunk"},
        {Shape::Icicle, 4, 14, 4, "icicle"},      {Shape::Spark, 2, 2, 8, "spark"},
        {Shape::Bulb, 5, 8, 16, "bulb"},          {Shape::GarlandWire, 14, 3, 1, "garland"},
        {Shape::Splat, 14, 12, 4, "splat"},       {Shape::Droplet, 3, 6, 2, "droplet"},
        {Shape::Hat, 12, 10, 4, "hat"},           {Shape::Scarf, 12, 10, 4, "scarf"},
        {Shape::Accessory, 14, 8, static_cast<int>(Accessory::Count), "accessory"},
    };
    struct Cell {
        std::string name;
        int x, y;
    };
    std::vector<Cell> cells;
    SpriteBatch all;
    int idx = 0;
    for (const Item& it : items) {
        for (int v = 0; v < it.variants; ++v, ++idx) {
            const int cx = 20 + (idx % 16) * 40, cy = 20 + (idx / 16) * 40;
            SpriteInstance s = sprite(it.s, static_cast<float>(cx), static_cast<float>(cy), it.hw, it.hh,
                                      static_cast<std::uint8_t>(v));
            if (it.s == Shape::Spark) s.vx = 120;
            all.sprites.push_back(s);
            cells.push_back({fmt("%s/%d", it.name, v), cx, cy});
        }
    }
    gallery->objects.push_back(all);
    Image img;
    if (!renderOffscreen(r, gallery, RectI{0, 0, 640, 40 * ((idx + 15) / 16)}, 2.0f, img, err)) {
        fail("render: shape gallery", err);
        return;
    }
    std::string empty;
    for (const Cell& c : cells)
        if (img.maxAlpha(c.x * 2, c.y * 2, 18) == 0) empty += (empty.empty() ? "" : ", ") + c.name;
    check(empty.empty(), fmt("render: all %zu shape variants draw", cells.size()), empty.empty() ? "" : "empty: " + empty);
}

// ---- updates: the installer pieces, offline (a local disk image, dummy bundles) -------------------

bool pathExists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

std::string readText(const std::string& p) {
    NSString* t = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:p.c_str()] encoding:NSUTF8StringEncoding error:nil];
    return t ? std::string(t.UTF8String ?: "") : std::string();
}

bool writeText(const std::string& p, const std::string& text) {
    FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return false;
    std::fputs(text.c_str(), f);
    std::fclose(f);
    return true;
}

std::string quoted(const std::string& p) { return "'" + p + "'"; }

pid_t spawnSleep(const char* seconds) {
    const char* argv[] = {"/bin/sleep", seconds, nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, "/bin/sleep", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) return 0;
    return pid;
}

bool waitChild(pid_t pid, double seconds, int* status) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        int st = 0;
        const pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (status) *status = st;
            return true;
        }
        if (r < 0) return false;
        ::usleep(20000);
    }
    return false;
}

void updateChecks(const std::string& dir) {
    // No network in the self-test: the updater refuses every request.
    {
        SFUpdater* u = [[SFUpdater alloc] initWithNetworkAllowed:NO];
        __block int attempts = 0, statuses = 0;
        u.onCheckAttempt = ^{
          ++attempts;
        };
        u.onStatus = ^{
          ++statuses;
        };
        [u check];
        [u install];
        pumpRunLoop(0.2);
        check(attempts == 0 && statuses == 0 && [u status].stage == update::Stage::Idle,
              "updates: updater without network makes no request");
        json::Value st;
        const std::string j = update::statusToJson([u status]);
        check(json::parse(j, st) && st.find("stage") && st.find("stage")->asString() == "idle", "updates: status JSON", j);
        const std::string blocker = [u installBlocker];
        pass("updates: in-place install of this build", blocker.empty() ? "possible" : blocker);
    }

    NSBundle* mb = NSBundle.mainBundle;
    const std::string bundle = mb.bundlePath.UTF8String ?: "";
    NSString* ver = [mb objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    const std::string version = [ver isKindOfClass:[NSString class]] ? std::string(ver.UTF8String) : std::string();
    const std::string bundleId = mb.bundleIdentifier.UTF8String ?: "";
    const std::string base = dir + "/updates";
    ::mkdir(base.c_str(), 0755);

    // Disk image -> staged app (hdiutil attach, ditto, codesign --verify, version check).
    if (bundle.size() < 5 || bundle.substr(bundle.size() - 4) != ".app" || version.empty()) {
        skip("updates: stage from a disk image", "not running from an app bundle");
    } else {
        const std::string src = base + "/src", dmg = base + "/test.dmg", dest = base + "/dest";
        ::mkdir(src.c_str(), 0755);
        ::mkdir(dest.c_str(), 0755);
        const bool copied = std::system(("/usr/bin/ditto " + quoted(bundle) + " " + quoted(src + "/SnowFall.app")).c_str()) == 0;
        bool made = false;
        for (int i = 0; copied && !made && i < 3; ++i) {   // hdiutil is sometimes "busy" on CI machines
            if (i > 0) ::sleep(2);
            made = std::system(("/usr/bin/hdiutil create -quiet -volname SnowFallTest -srcfolder " + quoted(src) +
                                " -ov -fs HFS+ -format UDZO " + quoted(dmg)).c_str()) == 0;
        }
        if (!made || !pathExists(dmg)) {
            skip("updates: stage from a disk image", "hdiutil create failed on this machine");
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            std::string why;
            const std::string staged = dest + "/.SnowFall-update.app";
            const bool ok = stageAppFromDiskImage(dmg, staged, version, bundleId, &why);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            check(ok && pathExists(staged + "/Contents/MacOS/SnowFall") && pathExists(staged + "/Contents/Info.plist"),
                  "updates: stage the app from a disk image (attach, copy, detach, codesign, version)",
                  ok ? fmt("%.1f s", secs) : why);
            std::string why2;
            const std::string staged2 = dest + "/other.app";
            const bool wrong = stageAppFromDiskImage(dmg, staged2, "99.0.0", bundleId, &why2);
            check(!wrong && !pathExists(staged2), "updates: a disk image with another version is refused", why2);
            std::system(("/bin/rm -rf " + quoted(src) + " " + quoted(dest) + " " + quoted(dmg)).c_str());
        }
    }

    // Bundle swap by the helper script (no relaunch): waits for the pid, swaps, removes the old one.
    {
        const std::string sw = base + "/swap";
        const std::string cur = sw + "/SnowFall.app", staged = sw + "/.SnowFall-update.app", old = sw + "/.SnowFall-old.app";
        const std::string log = sw + "/helper.log";
        ::mkdir(sw.c_str(), 0755);
        ::mkdir(cur.c_str(), 0755);
        ::mkdir(staged.c_str(), 0755);
        writeText(cur + "/marker", "old");
        writeText(staged + "/marker", "new");
        const pid_t sleeper = spawnSleep("0.6");
        SwapPlan plan;
        plan.waitForPid = sleeper;
        plan.currentApp = cur;
        plan.stagedApp = staged;
        plan.backupApp = old;
        plan.logPath = log;
        plan.reopen = false;
        pid_t helper = 0;
        std::string why;
        const bool launched = sleeper > 0 && launchSwapHelper(plan, &helper, &why);
        int st = -1;
        // The helper must still be waiting while the "app" runs.
        const bool waitedForApp = launched && ::waitpid(helper, &st, WNOHANG) == 0;
        if (sleeper > 0) waitChild(sleeper, 5, nullptr);   // reap it (a zombie would still answer kill -0)
        const bool finished = launched && waitChild(helper, 15, &st);
        check(launched && waitedForApp && finished && WIFEXITED(st) && WEXITSTATUS(st) == 0 && readText(cur + "/marker") == "new" &&
                  !pathExists(staged) && !pathExists(old),
              "updates: helper swaps the bundles after the app quits",
              launched ? readText(log).substr(0, 400) : why);

        // Staged app missing: nothing changes, the current app stays.
        const pid_t sleeper2 = spawnSleep("0.1");
        plan.waitForPid = sleeper2;
        pid_t helper2 = 0;
        const bool launched2 = sleeper2 > 0 && launchSwapHelper(plan, &helper2, &why);
        if (sleeper2 > 0) waitChild(sleeper2, 5, nullptr);
        int st2 = -1;
        const bool finished2 = launched2 && waitChild(helper2, 15, &st2);
        check(finished2 && WIFEXITED(st2) && WEXITSTATUS(st2) == 1 && readText(cur + "/marker") == "new" && !pathExists(old),
              "updates: helper keeps the current app when the staged one is missing");
        std::system(("/bin/rm -rf " + quoted(sw)).c_str());
    }
}

}  // namespace

int runSelfTest() {
    // Watchdog: a hang must fail the CI job, not stall it.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 240 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
      std::printf("[FAIL] selftest watchdog: still running after 240 s\n");
      std::fflush(stdout);
      _exit(3);
    });
    setenv("SNOW_LOG_STDERR", "1", 1);
    char tmpl[] = "/tmp/snowfall-selftest-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        std::printf("[FAIL] mkdtemp\n");
        return 1;
    }
    SettingsStore::setDataDirectoryOverride(fromUtf8(dir));
    log::init(fromUtf8(std::string(dir) + "/snowfall.log"), log::Level::Info);
    std::printf("SnowFall %s self-test (data in %s)\n", SNOW_VERSION, dir);

    NSApplication* app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyAccessory;
    [app finishLaunching];

    // ---- settings store -------------------------------------------------------------------
    {
        SettingsStore store(SettingsStore::defaultPath());
        Settings s;
        s.weather.intensity = 77;
        s.hotkeys.pauseToggle = "Ctrl+Alt+P";
        s.system.excludedProcesses = {"Safari", "Google Chrome"};
        Settings expected;   // what any save/load cycle yields: the sanitized form
        fromJson(toJson(s), expected);
        Settings back;
        const bool saved = store.save(s);
        const bool loaded = store.load(back);
        const std::string path = toUtf8(store.path());
        check(saved && loaded && back == expected, "settings: save/load round trip",
              fmt("saved=%d loaded=%d equal=%d", saved, loaded, back == expected));
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fputs("{ not json", f);
            std::fclose(f);
        }
        Settings c;
        const bool loadedCorrupt = store.load(c);
        check(!loadedCorrupt && c == Settings{} && access((path + ".corrupt").c_str(), F_OK) == 0,
              "settings: corrupt file -> defaults + .corrupt kept");
    }

    // ---- strings ---------------------------------------------------------------------------
    {
        NSDictionary* t = uiStringTable();
        check(t.count == static_cast<NSUInteger>(Str::Count) && t[@"TabSnow"] != nil && t[@"MenuExit"] != nil,
              "strings: table complete", fmt("%lu entries", static_cast<unsigned long>(t.count)));
    }

    // ---- hotkeys ---------------------------------------------------------------------------
    {
        HotkeyManager hk;
        Hotkey k;
        parseHotkey("Ctrl+Alt+Shift+F18", k);
        const bool ok = hk.registerHotkey(1, k);
        hk.unregisterAll();
        check(ok, "hotkeys: Carbon RegisterEventHotKey");
    }

    // ---- Metal -------------------------------------------------------------------------------
    id<MTLDevice> device = pickMetalDevice();
    if (!device) {
        skip("metal", "MTLCreateSystemDefaultDevice() returned nil on this machine");
    } else {
        renderTests(device);
    }

    // ---- window tracker --------------------------------------------------------------------
    WindowTracker tracker;
    std::shared_ptr<const DesktopLayout> layout = tracker.captureOnce();
    const std::vector<ScreenInfo> screens = tracker.screens();
    if (NSScreen.screens.count == 0) {
        skip("tracker", "no screens (headless session)");
    } else if (!layout) {
        fail("tracker: layout published");
    } else {
        bool sane = !layout->monitors.empty();
        for (const MonitorInfo& m : layout->monitors) sane = sane && !m.bounds.empty() && !m.work.empty();
        std::set<WindowId> ids;
        int apps = 0, occluders = 0, bars = 0;
        for (const WindowInfo& w : layout->windows) {
            sane = sane && !w.bounds.empty() && ids.insert(w.id).second;
            (w.kind == WindowKind::App ? apps : w.kind == WindowKind::Taskbar ? bars : occluders)++;
        }
        check(sane, "tracker: >= 1 monitor and a sane layout",
              fmt("%zu monitor(s), %d app / %d occluder / %d dock window(s), %zu hidden, fullscreen=%d",
                  layout->monitors.size(), apps, occluders, bars, layout->hiddenWindows.size(),
                  layout->fullscreenAppActive ? 1 : 0));
        for (const ScreenInfo& s : screens)
            std::printf("       screen %u: %dx%d at %d,%d, scale %.1f, visible %dx%d at %d,%d\n", s.displayId,
                        s.frame.width(), s.frame.height(), s.frame.l, s.frame.t, s.backingScale, s.visible.width(),
                        s.visible.height(), s.visible.l, s.visible.t);
    }

    // ---- simulation thread -------------------------------------------------------------------
    SnapshotExchange exchange;
    {
        SimRunner runner(exchange, makeMacSimHooks());
        Settings s;
        RuntimeTuning tuning;
        runner.start(s, tuning);
        if (layout) runner.setLayout(layout);
        pumpRunLoop(1.0);
        auto snap = exchange.latest();
        runner.stop();
        check(snap && snap->frame > 2, "simulation: SimRunner steps and publishes",
              snap ? fmt("frame %llu, %u particles", static_cast<unsigned long long>(snap->frame), snap->stats.particles)
                   : std::string("no snapshot"));
    }

    // ---- overlay windows + render thread ----------------------------------------------------
    if (!device || screens.empty()) {
        skip("overlay", !device ? "no Metal device" : "no screens");
    } else {
        OverlaySystem overlay;
        std::string err;
        if (!overlay.initialize(&exchange, &err)) {
            fail("overlay: initialize", err);
        } else {
            overlay.setScreens(screens);
            overlay.setVisible(true);
            auto empty = std::make_shared<FrameSnapshot>();   // nothing visible is drawn on the CI screen
            empty->taken = std::chrono::steady_clock::now();
            exchange.publish(empty);
            pumpRunLoop(1.5);
            const RenderStats st = overlay.stats();
            const std::set<std::uint32_t> numbers = overlay.windowNumbers();
            check(st.overlays == static_cast<int>(screens.size()) && numbers.size() == screens.size(),
                  "overlay: one window per screen", fmt("%d overlay(s)", st.overlays));
            check(st.frames > 0 && st.gpuErrors == 0, "overlay: render thread presents frames",
                  fmt("%llu frame(s), %d GPU error(s)", static_cast<unsigned long long>(st.frames), st.gpuErrors));
            // Our overlays must never show up as windows in the layout.
            tracker.setIgnoredWindows({});
            auto l2 = tracker.captureOnce();
            int coverOwn = 0;
            if (l2)
                for (const WindowInfo& w : l2->windows)
                    if (w.kind != WindowKind::Taskbar && coversScreen(w.bounds, screens) && w.kind == WindowKind::Occluder)
                        ++coverOwn;
            check(coverOwn == 0, "tracker: overlays are not reported as windows");
            overlay.setVisible(false);
            pumpRunLoop(0.2);
            overlay.shutdown();
        }
    }

    // ---- updates (offline) ---------------------------------------------------------------------
    updateChecks(dir);

    // ---- the whole app: status item, UI module, every subsystem wired together ----------------
    if (screens.empty()) {
        skip("app", "no screens");
    } else {
        auto host = std::make_unique<AppHost>();
        LaunchOptions o;
        o.customDataDir = true;
        o.quiet = true;
        o.logPath = std::string(dir) + "/snowfall.log";
        const bool started = host->initialize(o);
        check(started, "app: AppHost starts (status item, overlays, tracker, simulation, hotkeys)");
        if (started) {
            pumpRunLoop(1.5);
            json::Value diag;
            const std::string dj = host->diagnosticsJson();
            const bool parsed = json::parse(dj, diag) && diag.find("fps") && diag.find("monitors") && diag.find("version");
            check(parsed, "app: diagnosticsJSON", dj.substr(0, 240));
            check(parsed && diag.find("updateStage") && !host->updaterNetworkAllowed(),
                  "app: updater present, network disabled in the self-test");
            host->checkForUpdates();   // must be a no-op here
            json::Value us;
            const std::string usj = host->updateStatusJson();
            check(json::parse(usj, us) && us.find("stage") && us.find("stage")->asString() == "idle" &&
                      host->settings().updates.lastCheckUnix == 0,
                  "app: «Check now» makes no request in the self-test", usj);
            // Settings JSON from the UI is applied and comes back sanitized.
            Settings s = host->settings();
            s.weather.intensity = 31;
            std::string sent = toJson(s);   // Mac spelling from a UI is accepted and stored portably
            const std::string from = "\"pauseToggle\": \"" + s.hotkeys.pauseToggle + "\"";
            if (const std::size_t at = sent.find(from); at != std::string::npos)
                sent.replace(at, from.size(), "\"pauseToggle\": \"Ctrl+Option+P\"");
            host->applySettingsJson(sent);
            check(host->settings().weather.intensity == 31 && host->settings().hotkeys.pauseToggle == "Ctrl+Alt+P",
                  "app: applySettingsJSON", host->settings().hotkeys.pauseToggle);
            check(host->validateHotkey("Ctrl+Option+Shift+F17").empty(), "app: validateHotkey accepts a free combination");
            check(!host->validateHotkey("Ctrl+Pause").empty(), "app: validateHotkey rejects keys a Mac does not have");
#if defined(SNOW_HAS_SWIFT_UI)
            check(NSClassFromString(@"SFUIFactory") != nil && host->hasSettingsWindow() && host->hasQuickPanel(),
                  "ui: SwiftUI module linked (SFUIFactory)");
            host->openSettings();
            pumpRunLoop(1.5);
            check(host->settingsWindowOpen(), "ui: settings window opens");
            host->closeUi();
            pumpRunLoop(0.3);
#else
            skip("ui", "built without the SwiftUI module (Unix Makefiles generator)");
#endif
            // Tool cursor, both modes. Whether the private "hide in background" call works on
            // this machine is reported, not judged; the cursor must be visible afterwards.
            {   // pointer away from the menu bar / Dock (where the arrow is always shown)
                const RectI& f = screens.front().frame;
                CGWarpMouseCursorPosition(CGPointMake(f.l + f.width() / 2.0, f.t + f.height() / 2.0));
            }
            for (bool hideCursor : {false, true}) {
                Settings cs = host->settings();
                cs.interaction.enabled = true;
                cs.interaction.hideSystemCursor = hideCursor;
                host->applySettingsJson(toJson(cs));
                host->remoteCommand("interaction");   // toggle on
                pumpRunLoop(0.4);
                const std::string mode = host->cursorModeName();
                const bool hiddenNow = CursorHider::hidden();
                host->remoteCommand("interaction");   // toggle off
                pumpRunLoop(0.3);
                if (!hideCursor) {
                    check(mode == "hold" && !hiddenNow && CursorHider::balance() == 0,
                          "cursor: hold mode keeps the system arrow", mode);
                } else {
                    check((mode == "replace" && hiddenNow) || (mode == "replace-unavailable" && !hiddenNow),
                          "cursor: replace mode (hide system cursor)",
                          mode == "replace" ? "private CGS call works here: cursor was hidden"
                                            : "private CGS call unavailable here: fell back to hold");
                    check(CursorHider::balance() == 0 && !CursorHider::hidden(),
                          "cursor: visible again after leaving interaction mode (hide/show balance 0)");
                }
            }
            host->shutdown();
            check(CursorHider::balance() == 0, "app: clean shutdown (cursor visible)");
        }
    }

    std::printf("\nself-test: %d passed, %d failed, %d skipped\n", g_passed, g_failed, g_skipped);
    log::shutdown();
    return g_failed ? 1 : 0;
}

}  // namespace snow::mac
