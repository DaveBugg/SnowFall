// Tools, snowballs, snowmen, hit regions, cursor.
#include <vector>

#include "interaction/tools.h"
#include "sim_test_util.h"

using namespace simtest;
using T = InputEvent::Type;

namespace {
constexpr WindowId kWin = 10;
const RectI kWinRect{300, 400, 900, 800};
constexpr float kPi = 3.14159265f;

DesktopLayout layoutWithWindow(RectI r = kWinRect) {
    DesktopLayout l = basicLayout();
    l.windows.push_back(window(kWin, r));
    return l;
}

// Static world (no falling snow) with `h` px of snow on the taskbar.
void setup(World& w, Tool tool, float h, const DesktopLayout& l = basicLayout()) {
    Settings st;
    st.performance.animations = false;
    st.accumulation.naturalCaps = false;   // exact heights below
    st.festive.garland = false;
    st.interaction.tool = tool;
    w.applySettings(st, RuntimeTuning{});
    w.setLayout(l);
    Surface* bar = surfaceOf(w, kTaskbarId);
    if (bar && h > 0) fill(w, *bar, h);
}

int countShape(const SpriteBatch& b, Shape s) {
    int n = 0;
    for (const SpriteInstance& sp : b.sprites) n += sp.shape == s ? 1 : 0;
    return n;
}

// Roll a ball along the taskbar drift from startX until it reaches `targetR`, then lift it.
float rollBall(World& w, float startX, float targetR) {
    auto in = WorldTestAccess::get(w);
    const float y = static_cast<float>(kTaskbarTop) - 8.0f;
    input(w, T::PointerMove, startX, y);
    input(w, T::PointerDown, startX, y);
    float x = startX;
    while (in.objects.heldBall() && in.objects.heldBall()->r < targetR && x < 1900.0f) {
        x += 3.0f;
        input(w, T::PointerMove, x, y);
    }
    input(w, T::PointerMove, x, y - 150.0f);
    return in.objects.heldBall() ? in.objects.heldBall()->r : 0.0f;
}

// Build a 3-ball snowman on the window surface; returns false if something went wrong.
bool buildSnowman(World& w) {
    auto in = WorldTestAccess::get(w);
    w.setInteractionMode(true);
    const float targets[3] = {30.0f, 21.0f, 13.0f};
    const float starts[3] = {100.0f, 500.0f, 900.0f};
    for (int k = 0; k < 3; ++k) {
        const float r = rollBall(w, starts[k], targets[k]);
        if (r < targets[k]) return false;
        float tx = 600.0f, ty = 300.0f;   // first ball: dropped above the window's top edge
        if (k > 0) {
            if (in.objects.piles().size() != 1) return false;
            const Pile& p = in.objects.piles()[0];
            const Surface* s = in.surfaces.find(p.surface);
            if (!s) return false;
            const PileGeom g = pileGeom(p, *s);
            tx = g.cx + 2.0f;                                  // slightly off-centre: must auto-align
            ty = g.cy[g.n - 1] - g.r[g.n - 1] - r + 4.0f;
        }
        input(w, T::PointerMove, tx, ty);
        stepN(w, 40);                     // let the pointer velocity die down: a gentle release
        input(w, T::PointerUp, tx, ty);
        stepN(w, 120);
    }
    return true;
}
}  // namespace

TEST(mitten_removes_snow_only_near_the_stroke) {
    World w;
    setup(w, Tool::Mitten, 20.0f);
    const float y = static_cast<float>(kTaskbarTop) - 10.0f;
    const double mass0 = w.totalSnowMass();

    // Without interaction mode input changes nothing.
    input(w, T::PointerMove, 500.0f, y);
    input(w, T::PointerDown, 500.0f, y);
    input(w, T::PointerMove, 700.0f, y);
    input(w, T::PointerUp, 700.0f, y);
    CHECK(w.totalSnowMass() == mass0);

    w.setInteractionMode(true);
    input(w, T::PointerMove, 480.0f, y);   // hovering (button up) does nothing either
    input(w, T::PointerMove, 500.0f, y);
    CHECK(w.totalSnowMass() == mass0);
    input(w, T::PointerDown, 500.0f, y);
    input(w, T::PointerMove, 700.0f, y);   // one fast 200 px move: no gaps allowed
    input(w, T::PointerUp, 700.0f, y);
    input(w, T::PointerMove, 1200.0f, y);  // released: no more brushing
    CHECK(w.totalSnowMass() < mass0);

    const Surface* bar = surfaceOf(w, kTaskbarId);
    const float R = 28.0f;   // default brush radius at 100 %
    int cleared = 0, untouched = 0;
    for (int i = 0; i < bar->hm.columns(); ++i) {
        const float x = bar->worldX(i);
        const float d = x < 500.0f ? 500.0f - x : (x > 700.0f ? x - 700.0f : 0.0f);
        if (d > R + 1.0f) {
            CHECK(bar->hm.height(i) == 20.0f);
            ++untouched;
        } else if (d < 0.5f * R) {
            CHECK(bar->hm.height(i) == 0.0f);
            ++cleared;
        } else {
            CHECK(bar->hm.height(i) <= 20.0f);
        }
    }
    CHECK(cleared > 60 && untouched > 400);

    // A stroke far above the drift touches nothing.
    const double mass1 = w.totalSnowMass();
    input(w, T::PointerDown, 1000.0f, 500.0f);
    input(w, T::PointerMove, 1400.0f, 500.0f);
    input(w, T::PointerUp, 1400.0f, 500.0f);
    CHECK(w.totalSnowMass() == mass1);
}

TEST(mitten_radius_follows_settings_and_dust_flies) {
    double removed[2]{};
    for (int k = 0; k < 2; ++k) {
        World w;
        Settings st;
        st.interaction.tool = Tool::Mitten;
        st.interaction.brushRadiusPxAt96Dpi = k == 0 ? 12 : 48;
        w.applySettings(st, RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(basicLayout());
        fill(w, *surfaceOf(w, kTaskbarId), 20.0f);
        const double mass0 = w.totalSnowMass();
        w.setInteractionMode(true);
        input(w, T::PointerDown, 800.0f, static_cast<float>(kTaskbarTop) - 10.0f);
        input(w, T::PointerUp, 800.0f, static_cast<float>(kTaskbarTop) - 10.0f);
        removed[k] = mass0 - w.totalSnowMass();
        // Dust: short-lived cosmetic particles that never deposit.
        const auto& ps = WorldTestAccess::get(w).particles.particles();
        CHECK(!ps.empty());
        for (const Particle& p : ps) CHECK((p.flags & Particle::kDust) && p.vy < 0);
        const double mass1 = w.totalSnowMass();
        stepN(w, 200);
        CHECK(w.particleCount() == 0);
        CHECK(w.totalSnowMass() <= mass1 + 1e-6);
    }
    CHECK(removed[0] > 0);
    CHECK(removed[1] > 2.5 * removed[0]);
}

TEST(shovel_clears_a_wide_band_to_the_base) {
    World w;
    setup(w, Tool::Shovel, 20.0f);
    w.setInteractionMode(true);
    const float y = static_cast<float>(kTaskbarTop) - 10.0f;
    input(w, T::PointerDown, 1000.0f, y);
    input(w, T::PointerUp, 1000.0f, y);
    const Surface* bar = surfaceOf(w, kTaskbarId);
    for (int i = 0; i < bar->hm.columns(); ++i) {
        const float d = std::fabs(bar->worldX(i) - 1000.0f);
        if (d <= 48.0f) CHECK(bar->hm.height(i) == 0.0f);        // default width 96
        else CHECK(bar->hm.height(i) == 20.0f);
    }
    // One drag clears everything along the way.
    input(w, T::PointerDown, 1200.0f, y);
    input(w, T::PointerMove, 1700.0f, y);
    input(w, T::PointerUp, 1700.0f, y);
    for (int i = 0; i < bar->hm.columns(); ++i) {
        const float x = bar->worldX(i);
        if (x >= 1152.0f && x <= 1748.0f) CHECK(bar->hm.height(i) == 0.0f);
        if (x > 1752.0f || (x > 1052.0f && x < 1148.0f)) CHECK(bar->hm.height(i) == 20.0f);
    }
}

TEST(tool_selection_cycle_and_settings) {
    World w;
    Settings st;
    st.interaction.tool = Tool::Shovel;
    w.applySettings(st, RuntimeTuning{});
    CHECK(WorldTestAccess::get(w).tool == Tool::Shovel);
    input(w, T::CycleTool);
    CHECK(WorldTestAccess::get(w).tool == Tool::Snowball);
    input(w, T::CycleTool);
    CHECK(WorldTestAccess::get(w).tool == Tool::Mitten);
    st.weather.wind = 50;                 // unrelated settings change keeps the cycled tool
    w.applySettings(st, RuntimeTuning{});
    CHECK(WorldTestAccess::get(w).tool == Tool::Mitten);
    st.interaction.tool = Tool::Snowball; // the tool setting itself changed: switch
    w.applySettings(st, RuntimeTuning{});
    CHECK(WorldTestAccess::get(w).tool == Tool::Snowball);
}

TEST(snowball_conserves_mass_and_respects_size_limits) {
    World w;
    setup(w, Tool::Snowball, 20.0f);
    auto in = WorldTestAccess::get(w);
    w.setInteractionMode(true);
    const double mass0 = w.totalSnowMass();
    const float y = static_cast<float>(kTaskbarTop) - 8.0f;
    input(w, T::PointerDown, 300.0f, y);
    const Ball* b = in.objects.heldBall();
    CHECK(b != nullptr);
    if (!b) return;
    CHECK(b->r >= 6.0f);
    float prevR = b->r;
    for (float x = 300.0f; x <= 348.0f; x += 4.0f) {
        input(w, T::PointerMove, x, y);
        b = in.objects.heldBall();
        CHECK(b->r >= prevR);   // grows monotonically while rolling
        prevR = b->r;
    }
    const double taken = mass0 - w.totalSnowMass();
    CHECK(taken > 300.0);
    CHECK_NEAR(b->collected, taken, taken * 1e-3);                       // no snow from nothing
    CHECK_NEAR(kPi * b->r * b->r, kPackFactor * b->collected, 1.0);     // area = pack factor * collected
    CHECK(b->r > 6.0f && b->r < 40.0f);

    // Keep rolling: the ball stops growing (and stops eating snow) at 80 px diameter.
    for (float x = 348.0f; x <= 1500.0f; x += 4.0f) input(w, T::PointerMove, x, y);
    b = in.objects.heldBall();
    CHECK_NEAR(b->r, 40.0f, 1e-3);
    const double taken2 = mass0 - w.totalSnowMass();
    CHECK_NEAR(b->collected, taken2, taken2 * 1e-3);
    CHECK(kPackFactor * taken2 <= kPi * 40.0f * 40.0f * 1.001);
    const Surface* bar = surfaceOf(w, kTaskbarId);
    CHECK(bar->hm.heightAtLocalX(1400.0f) == 20.0f);   // full ball left the rest of the drift alone

    // Released: a free body that lands on the taskbar and comes to rest there.
    input(w, T::PointerMove, 1500.0f, y - 60.0f);
    stepN(w, 30);
    input(w, T::PointerUp, 1500.0f, y - 60.0f);
    CHECK(!in.objects.holding() && in.objects.balls().size() == 1);
    stepN(w, 240);
    CHECK(in.objects.balls().empty());
    CHECK(in.objects.piles().size() == 1);
    if (in.objects.piles().size() == 1) {
        const Pile& p = in.objects.piles()[0];
        CHECK(p.n == 1 && p.r[0] == 40.0f && p.surface == bar->id);
        const PileGeom g = pileGeom(p, *bar);
        CHECK(g.cy[0] + g.r[0] <= static_cast<float>(kTaskbarTop) + 0.01f);   // rests on top, not inside
    }
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(snap.stats.snowballs == 1 && snap.stats.snowmen == 0);
}

TEST(snowball_from_nothing_is_impossible) {
    {   // no snow at all
        World w;
        setup(w, Tool::Snowball, 0.0f);
        auto in = WorldTestAccess::get(w);
        w.setInteractionMode(true);
        input(w, T::PointerDown, 300.0f, 1025.0f);
        input(w, T::PointerMove, 900.0f, 1025.0f);
        input(w, T::PointerUp, 900.0f, 1025.0f);
        stepN(w, 60);
        CHECK(in.objects.balls().empty() && in.objects.piles().empty());
    }
    {   // a dusting: too little for the minimal ball
        World w;
        setup(w, Tool::Snowball, 0.6f);
        auto in = WorldTestAccess::get(w);
        w.setInteractionMode(true);
        input(w, T::PointerDown, 300.0f, 1028.0f);
        input(w, T::PointerUp, 300.0f, 1028.0f);
        CHECK(in.objects.balls().empty() && in.objects.piles().empty());
    }
    {   // a ball that falls past every surface leaves the scene
        World w;
        Settings st;
        st.performance.animations = false;
        st.accumulation.onTaskbar = false;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(basicLayout());
        auto in = WorldTestAccess::get(w);
        Ball b;
        b.x = 500.0f;
        b.y = 300.0f;
        b.r = 20.0f;
        in.objects.balls().push_back(b);
        stepN(w, 600);
        CHECK(in.objects.balls().empty() && in.objects.piles().empty());
    }
}

TEST(three_balls_make_a_snowman_bound_to_its_window) {
    World w;
    setup(w, Tool::Snowball, 30.0f, layoutWithWindow());
    auto in = WorldTestAccess::get(w);
    const bool built = buildSnowman(w);
    CHECK(built);
    CHECK(in.objects.piles().size() == 1 && in.objects.balls().empty());
    if (!built || in.objects.piles().size() != 1) return;
    const Surface* ws = in.surfaces.findByWindow(kWin);
    const Pile pile = in.objects.piles()[0];
    CHECK(pile.n == 3 && pile.surface == ws->id);
    CHECK(pile.r[0] > pile.r[1] && pile.r[1] > pile.r[2]);
    CHECK_NEAR(pile.localX, 300.0f, 1.0f);   // released at x=600 on a window starting at 300

    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(snap.stats.snowmen == 1 && snap.stats.snowballs == 0);
    CHECK(snap.objects.size() == 1);
    if (snap.objects.size() != 1) return;
    const SpriteBatch& batch = snap.objects[0];
    CHECK(!batch.clip.empty());
    CHECK(countShape(batch, Shape::Snowball) == 3);
    CHECK(countShape(batch, Shape::Carrot) == 1);
    CHECK(countShape(batch, Shape::Coal) == 5 || countShape(batch, Shape::Coal) == 10);   // eyes + buttons (+ smile)
    CHECK(countShape(batch, Shape::Stick) == 2);
    // Auto-aligned on one centre line, stacked upwards from the window's top edge.
    float xs[3]{}, ys[3]{};
    int k = 0;
    for (const SpriteInstance& sp : batch.sprites)
        if (sp.shape == Shape::Snowball && k < 3) {
            xs[k] = sp.x;
            ys[k] = sp.y;
            ++k;
        }
    CHECK(xs[0] == xs[1] && xs[1] == xs[2]);
    CHECK(ys[0] > ys[1] && ys[1] > ys[2]);
    CHECK_NEAR(ys[0] + pile.r[0], 400.0f, 0.01f);
    // Grabbable: a hit rect covers it.
    bool covered = false;
    for (const RectI& r : snap.hitRegions) covered = covered || r.contains(static_cast<int>(xs[2]), static_cast<int>(ys[2]));
    CHECK(covered);

    // The window is dragged (at a human pace): the snowman rides along.
    for (int i = 1; i <= 10; ++i) {
        w.step(1.0f / 60.0f);
        w.setLayout(layoutWithWindow(RectI{300 + 5 * i, 400 + 2 * i, 900 + 5 * i, 800 + 2 * i}));
    }
    FrameSnapshot moved;
    w.buildSnapshot(moved);
    CHECK(moved.objects.size() == 1 && moved.stats.snowmen == 1);
    if (moved.objects.size() == 1) {
        CHECK_NEAR(moved.objects[0].sprites[0].x, xs[0] + 50.0f, 1e-3);
        CHECK_NEAR(moved.objects[0].sprites[0].y, ys[0] + 20.0f, 1e-3);
    }

    // A window in front hides the part it covers.
    DesktopLayout covering = basicLayout();
    covering.windows.push_back(window(60, RectI{560, 200, 760, 600}, WindowKind::Occluder));
    covering.windows.push_back(window(kWin, RectI{350, 420, 950, 820}));
    w.setLayout(covering);
    w.buildSnapshot(moved);
    CHECK(moved.objects.size() == 1);   // desktop layer: clipped by every window
    if (moved.objects.size() == 1)
        for (const RectI& c : moved.objects[0].clip)
            CHECK(!c.intersects(RectI{560, 200, 760, 600}) && !c.intersects(RectI{350, 420, 950, 820}));
    for (const RectI& r : moved.hitRegions) CHECK(!r.intersects(RectI{560, 200, 760, 600}));

    // Minimised: kept but not drawn. Restored: back.
    DesktopLayout hidden = basicLayout();
    hidden.hiddenWindows.push_back(kWin);
    w.setLayout(hidden);
    w.buildSnapshot(moved);
    CHECK(moved.objects.empty() && in.objects.piles().size() == 1);
    w.setLayout(layoutWithWindow());
    w.buildSnapshot(moved);
    CHECK(moved.objects.size() == 1 && moved.stats.snowmen == 1);

    // Taking the head off again with any tool.
    input(w, T::CycleTool);
    input(w, T::PointerDown, xs[2], ys[2]);
    CHECK(in.objects.holding());
    CHECK(in.objects.piles().size() == 1 && in.objects.piles()[0].n == 2);
    input(w, T::PointerMove, xs[2] + 1.0f, ys[2] - 4.0f);
    stepN(w, 30);
    input(w, T::PointerUp, xs[2] + 1.0f, ys[2] - 4.0f);   // and putting it back
    CHECK(in.objects.piles().size() == 1 && in.objects.piles()[0].n == 3);

    // Window closed: the snowman comes apart and its balls fall (drop on close), then go.
    w.setLayout(basicLayout());
    CHECK(in.objects.piles().empty() && in.objects.balls().size() == 3);
    w.buildSnapshot(moved);
    CHECK(moved.stats.snowmen == 0);
    stepN(w, 300);
    CHECK(in.objects.balls().empty() && in.objects.piles().empty());
}

TEST(stacking_rules_and_falling_balls) {
    World w;
    setup(w, Tool::Mitten, 0.0f);
    auto in = WorldTestAccess::get(w);
    const Surface* bar = surfaceOf(w, kTaskbarId);
    in.objects.addPile(*bar, 500.0f, 30.0f);
    auto drop = [&](float x, float r) {
        Ball b;
        b.x = x;
        b.y = static_cast<float>(kTaskbarTop) - 200.0f;
        b.r = r;
        b.collected = kPi * r * r / kPackFactor;
        in.objects.balls().push_back(b);
        stepN(w, 400);
    };
    drop(505.0f, 20.0f);   // fits: stacks, centred
    CHECK(in.objects.piles().size() == 1 && in.objects.piles()[0].n == 2 && in.objects.balls().empty());
    drop(500.0f, 5.0f);    // far too small for a 20 px ball below (< 45 %): bounces off, rests beside
    CHECK(in.objects.piles().size() == 2 && in.objects.piles()[0].n == 2);
    drop(500.0f, 14.0f);
    CHECK(in.objects.piles()[0].n == 3);
    drop(500.0f, 9.0f);    // a 4th ball never stacks
    CHECK(in.objects.piles()[0].n == 3 && in.objects.piles().size() == 3);
    drop(1200.0f, 25.0f);  // nothing there: its own pile
    CHECK(in.objects.piles().size() == 4 && in.objects.snowmanCount() == 1 && in.objects.snowballCount() == 3);
    CHECK(in.objects.balls().empty());
}

TEST(snowman_breaks_on_strong_impact_or_violent_window_move) {
    {   // fast thrown ball
        World w;
        setup(w, Tool::Mitten, 0.0f);
        auto in = WorldTestAccess::get(w);
        const Surface* bar = surfaceOf(w, kTaskbarId);
        Pile& p = in.objects.addPile(*bar, 500.0f, 30.0f);
        p.r[p.n++] = 20.0f;
        CHECK(in.objects.snowmanCount() == 1);
        Ball b;
        b.x = 200.0f;
        b.y = static_cast<float>(kTaskbarTop) - 40.0f;
        b.vx = 2800.0f;
        b.r = 12.0f;
        in.objects.balls().push_back(b);
        stepN(w, 30);
        CHECK(in.objects.snowmanCount() == 0);
    }
    {   // window yanked away
        World w;
        Settings st;   // animations on: the snowman turns into flakes
        w.applySettings(st, RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(layoutWithWindow());
        auto in = WorldTestAccess::get(w);
        const Surface* ws = in.surfaces.findByWindow(kWin);
        Pile& p = in.objects.addPile(*ws, 300.0f, 30.0f);
        p.r[p.n++] = 20.0f;
        w.step(1.0f / 60.0f);
        w.setLayout(layoutWithWindow(RectI{320, 400, 920, 800}));   // 1200 px/s: fine
        CHECK(in.objects.snowmanCount() == 1);
        w.step(1.0f / 60.0f);
        w.setLayout(layoutWithWindow(RectI{520, 400, 1120, 800}));  // 12000 px/s: shattered
        CHECK(in.objects.snowmanCount() == 0 && in.objects.piles().empty());
        CHECK(w.particleCount() > 0);
        // A snap / restore (one huge jump) is not a shake.
        const Surface* ws2 = in.surfaces.findByWindow(kWin);
        in.objects.addPile(*ws2, 300.0f, 30.0f);
        w.step(1.0f / 60.0f);
        w.setLayout(layoutWithWindow(RectI{1200, 100, 1800, 500}));
        CHECK(in.objects.piles().size() == 1);
    }
}

TEST(clear_all_snow_removes_drifts_and_objects_but_not_weather) {
    World w(4);
    w.applySettings(Settings{}, RuntimeTuning{});
    w.setLayout(layoutWithWindow());
    stepN(w, 600);
    auto in = WorldTestAccess::get(w);
    fill(w, *surfaceOf(w, kWin), 10.0f);
    in.objects.addPile(*surfaceOf(w, kTaskbarId), 400.0f, 20.0f);
    Ball b;
    b.x = 100.0f;
    b.y = 100.0f;
    in.objects.balls().push_back(b);
    // Also snow on a hidden window.
    DesktopLayout hidden = basicLayout();
    hidden.hiddenWindows.push_back(kWin);
    w.setLayout(hidden);
    CHECK(w.totalSnowMass() > 0);
    const std::size_t flakes = w.particleCount();
    CHECK(flakes > 50);
    w.clearAllSnow();
    CHECK(w.totalSnowMass() == 0.0);
    CHECK(in.objects.piles().empty() && in.objects.balls().empty());
    CHECK(w.particleCount() == flakes);
    CHECK(w.surfaceCount() == 2);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    for (const DriftStrip& d : snap.drifts)
        for (float h : d.heights) CHECK(h == 0.0f);
    stepN(w, 600);                       // it keeps snowing
    CHECK(w.totalSnowMass() > 0);
}

TEST(hit_regions_and_cursor_only_in_interaction_mode) {
    World w;
    Settings st;
    st.performance.animations = false;
    st.interaction.tool = Tool::Snowball;   // tight regions first
    st.festive.garland = false;
    w.applySettings(st, RuntimeTuning{});
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    const RectI popup{400, 900, 500, 1080};
    l.windows.push_back(window(50, popup, WindowKind::Occluder));   // in front of the taskbar
    l.windows.push_back(window(kTaskbarId, RectI{0, kTaskbarTop, 1920, 1080}, WindowKind::Taskbar));
    w.setLayout(l);
    Surface* bar = surfaceOf(w, kTaskbarId);
    for (int i = 100; i <= 200; ++i) bar->hm.setHeight(i, 10.0f);   // x = 300..600
    for (int i = 400; i <= 420; ++i) bar->hm.setHeight(i, 4.0f);    // x = 1200..1260
    WorldTestAccess::get(w).surfaces.refreshBlocked(*bar);

    FrameSnapshot snap;
    input(w, T::PointerMove, 350.0f, 1020.0f);
    w.buildSnapshot(snap);
    CHECK(snap.hitRegions.empty() && snap.cursor.sprites.empty());

    w.setInteractionMode(true);
    w.buildSnapshot(snap);
    CHECK(snap.hitRegions.size() >= 3);   // run split by the popup + the second run
    auto hitAt = [&](int x, int y) {
        for (const RectI& r : snap.hitRegions)
            if (r.contains(x, y)) return true;
        return false;
    };
    CHECK(hitAt(350, 1027) && hitAt(550, 1027) && hitAt(1230, 1030));
    CHECK(!hitAt(450, 1027));    // under the popup
    CHECK(!hitAt(800, 1027));    // no snow there
    CHECK(!hitAt(350, 1040));    // never below the edge (that is the taskbar itself)
    CHECK(!hitAt(350, 900));
    for (const RectI& r : snap.hitRegions) CHECK(!r.intersects(popup) && r.b <= kTaskbarTop);
    const DriftStrip* d = driftOf(snap, bar->id);
    CHECK(d != nullptr);
    if (d)
        for (const RectI& c : d->clip) CHECK(!c.intersects(popup));
    // Snowball tool: just the ball preview, no ring; hotspot = centre.
    CHECK(snap.cursor.sprites.size() == 1);
    if (snap.cursor.sprites.size() == 1) {
        const SpriteInstance& c = snap.cursor.sprites[0];
        CHECK(c.shape == Shape::Snowball && c.x == 350.0f && c.y == 1020.0f && c.halfW == 14.0f);
    }

    // Holding a ball: the ball is drawn in the objects batch; the cursor batch carries a faint
    // foreground marker of the same size at the pointer (visible over windows).
    input(w, T::PointerDown, 350.0f, 1026.0f);
    w.buildSnapshot(snap);
    CHECK(snap.cursor.sprites.size() == 2);
    if (snap.cursor.sprites.size() == 2) {
        const Ball* hb = WorldTestAccess::get(w).objects.heldBall();
        const SpriteInstance& ring = snap.cursor.sprites[0];
        const SpriteInstance& mark = snap.cursor.sprites[1];
        CHECK(hb != nullptr);
        CHECK(ring.shape == Shape::Ring && ring.opacity == 0.25f);
        CHECK(mark.shape == Shape::Snowball && mark.opacity == 0.45f && mark.x == 350.0f && mark.y == 1026.0f);
        if (hb) CHECK(mark.halfW == hb->r && ring.halfW == hb->r + 3.0f);
    }
    CHECK(snap.objects.size() == 1 && !snap.objects[0].clip.empty() && snap.objects[0].sprites.size() == 1);
    if (snap.objects.size() == 1)
        for (const RectI& c : snap.objects[0].clip) CHECK(!c.intersects(popup) && c.b <= 1080);
    input(w, T::PointerUp, 350.0f, 1026.0f);

    // Mitten: regions grow by the brush radius in every direction (presses next to the snow
    // must not reach the desktop), even over the popup / the taskbar below the edge.
    input(w, T::CycleTool);
    CHECK(WorldTestAccess::get(w).tool == Tool::Mitten);
    for (int i = 100; i <= 200; ++i) bar->hm.setHeight(i, 10.0f);
    input(w, T::PointerMove, 700.0f, 1000.0f);
    w.buildSnapshot(snap);
    CHECK(hitAt(350, 1027) && hitAt(420, 1027));        // reaches over the popup's edge too
    CHECK(hitAt(300 - 25, 1027) && hitAt(600 + 25, 1027));
    CHECK(hitAt(350, 1032 + 20) && hitAt(350, 1022 - 25));
    CHECK(!hitAt(800, 1027) && !hitAt(350, 900));
    for (const RectI& r : snap.hitRegions) CHECK(r.l >= 0 && r.r <= 1920 && r.t >= 0 && r.b <= 1080);
    // Hover: subtle ring + the mitten cursor with its fingertips on the pointer.
    CHECK(snap.cursor.sprites.size() == 2);
    if (snap.cursor.sprites.size() == 2) {
        CHECK(snap.cursor.sprites[0].shape == Shape::Ring && snap.cursor.sprites[0].halfW == 28.0f);
        CHECK(snap.cursor.sprites[0].opacity < 0.5f);
        CHECK(snap.cursor.sprites[1].shape == Shape::Mitten);
        CHECK(snap.cursor.sprites[1].x > 700.0f && snap.cursor.sprites[1].y > 1000.0f);   // hangs down-right
    }
    // During a stroke: no ring at all.
    input(w, T::PointerDown, 700.0f, 1000.0f);
    w.buildSnapshot(snap);
    CHECK(snap.cursor.sprites.size() == 1 && snap.cursor.sprites[0].shape == Shape::Mitten);
    input(w, T::PointerUp, 700.0f, 1000.0f);
    w.buildSnapshot(snap);
    CHECK(snap.cursor.sprites.size() == 2);
    // Shovel: wider reach.
    input(w, T::CycleTool);
    w.buildSnapshot(snap);
    CHECK(snap.cursor.sprites.size() == 2 && snap.cursor.sprites[1].shape == Shape::Shovel &&
          snap.cursor.sprites[0].halfW == 48.0f);
    CHECK(hitAt(300 - 45, 1027) && !hitAt(300 - 60, 1027));

    // Leaving interaction mode hides everything interactive.
    w.setInteractionMode(false);
    CHECK(!WorldTestAccess::get(w).objects.holding());
    w.buildSnapshot(snap);
    CHECK(snap.hitRegions.empty() && snap.cursor.sprites.empty());
}
