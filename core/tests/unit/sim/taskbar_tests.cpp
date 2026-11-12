// Regression: opening the Start menu makes Windows 11 drop Shell_TrayWnd from EnumWindows;
// the tracker then reports the taskbar as hidden. Its drift must survive and stay visible.
#include "sim_test_util.h"

using namespace simtest;

TEST(taskbar_hidden_keeps_visible_drift) {
    World w(7);
    Settings s;
    w.applySettings(s, RuntimeTuning{});
    DesktopLayout l = basicLayout();
    w.setLayout(l);
    Surface* tb = surfaceOf(w, kTaskbarId);
    CHECK(tb != nullptr);
    if (!tb) return;
    fill(w, *tb, 20.0f);
    const double mass = w.totalSnowMass();
    CHECK(mass > 0);

    // Start menu open: taskbar listed as hidden.
    DesktopLayout start = l;
    start.revision = 2;
    start.windows.clear();
    start.hiddenWindows.push_back(kTaskbarId);
    w.setLayout(start);
    w.step(1.0f / 60.0f);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    tb = surfaceOf(w, kTaskbarId);
    CHECK(tb != nullptr);
    CHECK(tb && driftOf(snap, tb->id) != nullptr);   // still drawn
    CHECK_NEAR(w.totalSnowMass(), mass, mass * 0.02);

    // Start menu closed again.
    l.revision = 3;
    w.setLayout(l);
    w.step(1.0f / 60.0f);
    CHECK(surfaceOf(w, kTaskbarId) != nullptr);
    CHECK_NEAR(w.totalSnowMass(), mass, mass * 0.03);
}
