// Round 5: interaction.objectsOnAllMonitors.
#include "sim_test_util.h"

using namespace simtest;
using T = InputEvent::Type;

namespace {
constexpr WindowId kWinB = 11;
const RectI kWinBRect{2200, 400, 2800, 800};   // on the secondary monitor

Settings quiet(bool all) {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = false;
    s.accumulation.naturalCaps = false;
    s.accumulation.shakeOffOnMove = false;
    s.interaction.tool = Tool::Snowball;
    s.interaction.objectsOnAllMonitors = all;
    s.festive.garland = false;
    return s;
}

DesktopLayout twoMonitors() {
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.monitors.push_back(monitor(1, RectI{1920, 0, 3840, 1080}));
    l.windows.push_back(window(kWinB, kWinBRect));
    l.windows.push_back(window(kTaskbarId, RectI{0, kTaskbarTop, 1920, 1080}, WindowKind::Taskbar));
    return l;
}

void fillToCap(World& w, Surface& s) {
    for (int i = 0; i < s.hm.columns(); ++i) s.hm.setHeight(i, s.hm.cap(i));
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}
}  // namespace

TEST(objects_on_all_monitors_roll_rest_and_draw_on_the_secondary) {
    World w(3);
    Settings st = quiet(true);
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(twoMonitors());
    auto in = WorldTestAccess::get(w);
    Surface* b = surfaceOf(w, kWinB);
    if (!b) {
        CHECK(b != nullptr);
        return;
    }
    fillToCap(w, *b);
    const double m0 = b->hm.mass();
    w.setInteractionMode(true);
    input(w, T::PointerMove, 2300.0f, 392.0f);
    input(w, T::PointerDown, 2300.0f, 392.0f);   // snowball tool on the secondary monitor
    CHECK(in.objects.holding());
    for (float x = 2300.0f; x <= 2500.0f; x += 4.0f) input(w, T::PointerMove, x, 392.0f);
    CHECK(b->hm.mass() < m0);                    // it gathered snow there
    input(w, T::PointerMove, 2500.0f, 330.0f);
    stepN(w, 30);
    input(w, T::PointerUp, 2500.0f, 330.0f);
    stepN(w, 240);
    CHECK(in.objects.piles().size() == 1);       // rests on the secondary-monitor window ...
    if (in.objects.piles().size() == 1) CHECK(in.objects.piles()[0].surface == b->id);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(!snap.objects.empty());
    bool secondary = false;
    for (const SpriteBatch& batch : snap.objects)
        for (const RectI& c : batch.clip) {
            secondary = secondary || c.l >= 1920;
            for (const WindowInfo& wi : twoMonitors().windows) CHECK(!c.intersects(wi.bounds));
        }
    CHECK(secondary);                            // ... and is drawn there
    // A held ball dragged across the monitor boundary survives.
    fillToCap(w, *surfaceOf(w, kTaskbarId));
    input(w, T::PointerDown, 1500.0f, 1028.0f);
    CHECK(in.objects.holding());
    for (float x = 1500.0f; x <= 2100.0f; x += 20.0f) input(w, T::PointerMove, x, 700.0f);
    w.step(1.0f / 60.0f);
    CHECK(in.objects.holding());
    input(w, T::PointerUp, 2100.0f, 700.0f);

    // Live switch to "primary only": the snowman/ball on the secondary monitor puffs into dust.
    w.applySettings(quiet(false), RuntimeTuning{});
    w.step(1.0f / 60.0f);
    bool anySecondary = false;
    for (const Pile& p : in.objects.piles()) anySecondary = anySecondary || p.surface == b->id;
    for (const Ball& ball : in.objects.balls()) anySecondary = anySecondary || ball.x >= 1920.0f;
    CHECK(!anySecondary);
    w.buildSnapshot(snap);
    for (const SpriteBatch& batch : snap.objects)
        for (const RectI& c : batch.clip) CHECK(c.r <= 1920);
}

TEST(objects_primary_only_mode_rejects_the_secondary) {
    World w(4);
    w.applySettings(quiet(false), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(twoMonitors());
    auto in = WorldTestAccess::get(w);
    fillToCap(w, *surfaceOf(w, kWinB));
    w.setInteractionMode(true);
    input(w, T::PointerMove, 2300.0f, 392.0f);
    input(w, T::PointerDown, 2300.0f, 392.0f);
    CHECK(!in.objects.holding());
    CHECK(!WorldTestAccess::dropSnowman(w));   // the only full drift is on the secondary monitor
    CHECK(!WorldTestAccess::heavySnow(w));
}

TEST(falling_snowmen_may_target_any_monitor_in_all_monitor_mode) {
    World w(5);
    w.applySettings(quiet(true), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(twoMonitors());
    auto in = WorldTestAccess::get(w);
    Surface* b = surfaceOf(w, kWinB);
    fillToCap(w, *b);
    CHECK(WorldTestAccess::dropSnowman(w));
    CHECK(in.objects.falling().size() == 1);
    if (in.objects.falling().size() == 1) {
        CHECK(in.objects.falling()[0].x >= 1920.0f && in.objects.falling()[0].y < 0.0f);
    }
    int steps = 0;
    while (!in.objects.falling().empty() && steps++ < 600) w.step(1.0f / 60.0f);
    CHECK(in.objects.piles().size() == 1 && in.objects.piles()[0].surface == b->id);
    stepN(w, 60);
    CHECK(in.objects.piles().size() == 1);   // stays: the secondary monitor is part of the layer
    // The snowplow stays on the primary taskbar only.
    w.triggerSnowplow();
    CHECK(in.plow.active());
    const Surface* plowOn = in.surfaces.find(in.plow.surface());
    CHECK(plowOn && plowOn->kind == SurfaceKind::Taskbar && plowOn->right <= 1920);
}
