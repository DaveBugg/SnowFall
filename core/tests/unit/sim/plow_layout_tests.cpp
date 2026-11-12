// Regression: the snowplow must start and be drawn on a realistic 3-monitor layout where the
// primary taskbar is not the first window and other monitors have taskbars too.
#include <cstdio>

#include "sim_test_util.h"

using namespace simtest;

TEST(plow_runs_on_three_monitor_layout) {
    World w(11);
    Settings s;
    w.applySettings(s, RuntimeTuning{});
    DesktopLayout l;
    l.revision = 1;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.monitors.push_back(monitor(1, RectI{1920, -34, 3360, 2526}, 1.0f, false));
    l.monitors.push_back(monitor(2, RectI{0, 1080, 1920, 2160}, 1.0f, false));
    WindowInfo a = window(7, RectI{1920, -34, 3360, 2478});
    a.maximized = true;
    l.windows.push_back(a);
    l.windows.push_back(window(8, RectI{333, 278, 1417, 906}));
    l.windows.push_back(window(10, RectI{262, 229, 1638, 764}));
    WindowInfo f = window(12, RectI{0, 1080, 1920, 2112});
    f.maximized = true;
    l.windows.push_back(f);
    l.windows.push_back(window(41, RectI{1920, 2478, 3360, 2526}, WindowKind::Taskbar));
    l.windows.push_back(window(42, RectI{0, 2112, 1920, 2160}, WindowKind::Taskbar));
    l.windows.push_back(window(44, RectI{0, 1032, 1920, 1080}, WindowKind::Taskbar));
    w.setLayout(l);
    stepN(w, 10);
    w.triggerSnowplow();
    int seen = 0;
    FrameSnapshot snap;
    for (int i = 0; i < 300; ++i) {
        w.step(1.0f / 60.0f);
        w.buildSnapshot(snap);
        for (const auto& b : snap.objects)
            for (const auto& sp : b.sprites)
                if (sp.shape == Shape::Snowplow) {
                    bool inClip = b.clip.empty();
                    for (const RectI& c : b.clip)
                        if (c.contains(static_cast<int>(sp.x), static_cast<int>(sp.y))) inClip = true;
                    if (inClip) ++seen;
                    if (i == 150) printf("  plow at %.0f,%.0f clip rects %zu inClip %d\n", sp.x, sp.y, b.clip.size(), inClip);
                }
    }
    printf("  frames with a visible plow: %d / 300\n", seen);
    CHECK(seen > 100);
}
