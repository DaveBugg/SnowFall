// Round 12: snowplow routes on every taskbar.
#include <vector>

#include "sim_test_util.h"

using namespace simtest;

namespace {
Settings quiet(bool allBars = true) {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = true;
    s.accumulation.snowplowAllTaskbars = allBars;
    s.accumulation.naturalCaps = false;
    s.festive.garland = false;
    return s;
}

DesktopLayout sideBySide() {
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.monitors.push_back(monitor(1, RectI{1920, 0, 3840, 1080}));
    l.windows.push_back(window(kTaskbarId, RectI{0, kTaskbarTop, 1920, 1080}, WindowKind::Taskbar));
    l.windows.push_back(window(2, RectI{1920, kTaskbarTop, 3840, 1080}, WindowKind::Taskbar));
    return l;
}

DesktopLayout stacked() {
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.monitors.push_back(monitor(1, RectI{0, 1080, 1920, 2160}));
    l.windows.push_back(window(kTaskbarId, RectI{0, kTaskbarTop, 1920, 1080}, WindowKind::Taskbar));
    l.windows.push_back(window(2, RectI{0, 2112, 1920, 2160}, WindowKind::Taskbar));
    return l;
}

void fill(World& w, WindowId bar, float h) {
    Surface* s = surfaceOf(w, bar);
    for (int i = 0; i < s->hm.columns(); ++i) s->hm.setHeight(i, h);
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}

}  // namespace

TEST(side_by_side_taskbars_form_one_route_crossed_seamlessly) {
    World w(1);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(sideBySide());
    auto in = WorldTestAccess::get(w);
    CHECK(in.plowRuns.size() == 1);
    if (in.plowRuns.size() != 1) return;
    CHECK(in.plowRuns[0].route.surfaces.size() == 2 && in.plowRuns[0].route.left == 0 && in.plowRuns[0].route.right == 3840);
    fill(w, kTaskbarId, 20.0f);
    fill(w, 2, 20.0f);
    const double m1 = surfaceOf(w, kTaskbarId)->hm.mass(), m2 = surfaceOf(w, 2)->hm.mass();
    w.triggerSnowplow();
    CHECK(in.plow.active());
    float maxX = 0;
    bool humpOnSecond = false;
    for (int k = 0; k < 60 * 60 && in.plow.active(); ++k) {
        w.step(1.0f / 60.0f);
        maxX = std::max(maxX, in.plow.centerLocal());
        if (k % 30 == 0 && in.plow.frontLocal() > 1920.0f && in.plow.frontLocal() < 3500.0f) {
            FrameSnapshot snap;
            w.buildSnapshot(snap);
            const DriftStrip* d = driftOf(snap, surfaceOf(w, 2)->id);
            if (d) {
                const int i = static_cast<int>((in.plow.frontLocal() + 4.0f - 1920.0f) / d->step);
                humpOnSecond = humpOnSecond || d->heights[static_cast<std::size_t>(i)] > 21.0f;
            }
        }
    }
    printf("  joined route: plow reached x=%.0f, left on bars %.1f %% / %.1f %%\n", maxX,
           100.0 * surfaceOf(w, kTaskbarId)->hm.mass() / m1, 100.0 * surfaceOf(w, 2)->hm.mass() / m2);
    CHECK(!in.plow.active() && in.plow.completedRuns() == 1);
    CHECK(maxX > 3840.0f && in.plow.lastExitLocal() > 3840.0f);
    CHECK(humpOnSecond);   // the heap is pushed on across the seam
    CHECK(surfaceOf(w, kTaskbarId)->hm.mass() < 0.05 * m1 && surfaceOf(w, 2)->hm.mass() < 0.05 * m2);
}

TEST(stacked_taskbars_are_separate_routes_with_their_own_timers) {
    World w(2);
    Settings st = quiet();
    st.accumulation.snowplowDelayMinutes = 1;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(stacked());
    auto in = WorldTestAccess::get(w);
    CHECK(in.plowRuns.size() == 2);
    if (in.plowRuns.size() != 2) return;
    CHECK(in.plowRuns[0].main && !in.plowRuns[1].main);
    CHECK(in.plowRuns[1].offset >= 0 && in.plowRuns[1].offset <= kPlowRouteOffsetMax);
    fill(w, 2, 36.0f);   // only the lower taskbar is full
    int lowerRuns = 0;
    for (int k = 0; k < 130 * 20; ++k) {
        w.step(1.0f / 20.0f);
        if (k % 20 == 0) fill(w, 2, 36.0f);   // it keeps snowing there
        lowerRuns = in.plowRuns[1].own.completedRuns() + (in.plowRuns[1].own.active() ? 1 : 0);
    }
    CHECK(lowerRuns >= 1);
    CHECK(in.plow.completedRuns() == 0 && !in.plow.active());   // the empty upper taskbar: no plow
}

TEST(manual_call_starts_every_route_staggered_and_plows_show_on_secondary_monitors) {
    World w(3);
    Settings st = quiet();
    st.interaction.objectsOnAllMonitors = false;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(stacked());
    auto in = WorldTestAccess::get(w);
    w.triggerSnowplow();
    CHECK(in.plow.active());
    CHECK(!in.plowRuns[1].own.active());
    CHECK(in.plowRuns[1].pending >= kPlowStaggerMin && in.plowRuns[1].pending <= kPlowStaggerMax);
    stepN(w, static_cast<int>(60 * (kPlowStaggerMax + 0.2f)));
    CHECK(in.plowRuns[1].own.active());
    stepN(w, 60);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    bool lowerVisible = false;
    for (const SpriteBatch& b : snap.objects)
        for (const SpriteInstance& s : b.sprites)
            if (s.shape == Shape::Snowplow && s.y > 1080.0f)
                for (const RectI& c : b.clip) lowerVisible = lowerVisible || c.contains(static_cast<int>(s.x), static_cast<int>(s.y));
    CHECK(lowerVisible);   // drawn on the secondary monitor although objects are primary-only
    // In a thaw nothing starts.
    World t(4);
    st.weather.preset = WeatherPreset::Thaw;
    t.applySettings(st, RuntimeTuning{});
    t.setLayout(stacked());
    t.triggerSnowplow();
    stepN(t, 300);
    auto ti = WorldTestAccess::get(t);
    CHECK(!ti.plow.active() && !ti.plowRuns[1].own.active());
}

TEST(hot_plug_rebuilds_routes_and_aborts_a_running_plow_cleanly) {
    World w(5);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(sideBySide());
    auto in = WorldTestAccess::get(w);
    fill(w, kTaskbarId, 20.0f);
    w.triggerSnowplow();
    stepN(w, 300);
    CHECK(in.plow.active() && in.plow.carried() > 0);
    DesktopLayout one = basicLayout();   // the second monitor is unplugged
    w.setLayout(one);
    CHECK(!in.plow.active());            // its route changed: stopped, heap dropped
    CHECK(in.plowRuns.size() == 1 && in.plowRuns[0].route.right == 1920);
    stepN(w, 60);
    w.triggerSnowplow();                 // the new route works
    CHECK(in.plow.active());
    // Primary-only setting: one route even with two taskbars.
    World p(6);
    p.applySettings(quiet(false), RuntimeTuning{});
    p.setLayout(stacked());
    CHECK(WorldTestAccess::get(p).plowRuns.size() == 1);
}
