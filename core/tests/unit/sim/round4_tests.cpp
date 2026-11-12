// Round 4: tool cursor as the real cursor, snowballs / snowmen falling off jerked windows.
#include <vector>

#include "interaction/tools.h"
#include "sim_test_util.h"

using namespace simtest;

namespace {
constexpr WindowId kWin = 10;
constexpr float kPi = 3.14159265f;

Settings quiet() {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = false;
    s.accumulation.shakeToClear = false;
    s.accumulation.shakeOffOnMove = true;
    s.accumulation.naturalCaps = false;
    s.festive.garland = false;
    return s;
}

DesktopLayout withWindow(RectI r) {
    DesktopLayout l = basicLayout();
    l.windows.insert(l.windows.begin(), window(kWin, r));
    return l;
}

// Snowman of 3 balls on the window; returns the total snow it is made of.
double placeSnowman(World& w, float localX) {
    auto in = WorldTestAccess::get(w);
    Pile& p = in.objects.addPile(*surfaceOf(w, kWin), localX, 26.0f);
    p.r[p.n++] = 19.0f;
    p.r[p.n++] = 13.0f;
    double m = 0;
    for (int i = 0; i < p.n; ++i) m += kPi * p.r[i] * p.r[i] / kPackFactor;
    return m;
}
}  // namespace

TEST(tool_cursor_hotspot_and_tilt) {
    for (float dpi : {1.0f, 1.5f}) {
        for (Tool t : {Tool::Mitten, Tool::Shovel, Tool::Snowball}) {
            const SpriteInstance c = toolCursorSprite(t, 500.0f, 300.0f, dpi);
            // Rotate the local hotspot by the sprite rotation around its centre: it must be the pointer.
            const Vec2 h = toolHotspotLocal(t, dpi);
            const float cs = std::cos(c.rotation), sn = std::sin(c.rotation);
            CHECK_NEAR(c.x + h.x * cs - h.y * sn, 500.0, 1e-3);
            CHECK_NEAR(c.y + h.x * sn + h.y * cs, 300.0, 1e-3);
        }
        const SpriteInstance m = toolCursorSprite(Tool::Mitten, 500.0f, 300.0f, dpi);
        const SpriteInstance s = toolCursorSprite(Tool::Shovel, 500.0f, 300.0f, dpi);
        const SpriteInstance b = toolCursorSprite(Tool::Snowball, 500.0f, 300.0f, dpi);
        CHECK(m.shape == Shape::Mitten && s.shape == Shape::Shovel && b.shape == Shape::Snowball);
        CHECK_NEAR(m.halfW, 26.0 * dpi, 1e-4);
        CHECK_NEAR(s.halfW, 30.0 * dpi, 1e-4);
        CHECK_NEAR(b.halfW, 14.0 * dpi, 1e-4);
        // Mitten fingertips point up-left (like the arrow), the sprite hangs down-right of the pointer.
        const float upX = std::sin(m.rotation), upY = -std::cos(m.rotation);
        CHECK(upX < -0.4f && upY < -0.7f);
        CHECK(m.x > 500.0f && m.y > 300.0f);
        // Shovel blade points down-left, handle up-right: the sprite sits up-right of the pointer.
        const float downX = -std::sin(s.rotation), downY = std::cos(s.rotation);
        CHECK(downX < -0.4f && downY > 0.7f);
        CHECK(s.x > 500.0f && s.y < 300.0f);
        CHECK(b.x == 500.0f && b.y == 300.0f && b.rotation == 0.0f);
    }
}

TEST(snowman_falls_off_a_jerked_window_and_shatters_on_the_taskbar) {
    World w(8);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindow(RectI{300, 300, 900, 700}));
    auto in = WorldTestAccess::get(w);
    const double ballMass = placeSnowman(w, 300.0f);
    const Surface* bar = surfaceOf(w, kTaskbarId);
    CHECK(bar && bar->hm.mass() == 0.0);
    w.step(1.0f / 60.0f);
    w.setLayout(withWindow(RectI{360, 300, 960, 700}));   // 3600 px/s jerk
    CHECK(in.objects.piles().empty());
    CHECK(in.objects.balls().size() == 3);
    CHECK(in.objects.debris().size() >= 5);   // eyes, carrot, buttons, arms
    for (const Ball& b : in.objects.balls()) {
        CHECK(b.vx > 0 && b.vx < 3600.0f * 0.35f);   // lags behind the window
        CHECK(b.x < 360.0f + 300.0f + 40.0f);        // starts where it stood (old position)
    }
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(snap.objects.size() == 1);   // still in the desktop layer while they are balls
    int coal = 0;
    for (const SpriteInstance& s : snap.objects[0].sprites) coal += s.shape == Shape::Coal ? 1 : 0;
    CHECK(coal == 5);
    int steps = 0;
    while ((!in.objects.balls().empty() || in.chunks.count() > 0) && steps < 900) {
        w.step(1.0f / 60.0f);
        ++steps;
    }
    CHECK(in.objects.balls().empty() && in.chunks.count() == 0);
    CHECK(in.objects.shattered() == 3);
    const double onBar = surfaceOf(w, kTaskbarId)->hm.mass();
    printf("  snowman shatter: %.0f px^2 of balls -> %.0f deposited on the taskbar, %.0f dust, %.0f lost\n",
           ballMass, onBar, in.chunks.dust(), in.chunks.lost());
    CHECK(onBar > 0.3 * ballMass);
    // Mass bookkeeping: balls -> chunks system (deposited + dust + lost).
    CHECK_NEAR(in.chunks.shedChunkMass(), ballMass, ballMass * 1e-3);
    CHECK_NEAR(in.chunks.deposited() + in.chunks.dust() + in.chunks.lost(), ballMass, ballMass * 1e-3);
    CHECK_NEAR(onBar, in.chunks.deposited(), in.chunks.deposited() * 1e-3 + 0.5);
    // Decorations landed and faded away.
    stepN(w, 180);
    CHECK(in.objects.debris().empty());
}

TEST(snowballs_fall_when_their_window_closes_or_is_shaken) {
    {   // closed (drop on close): falls, breaks on the taskbar
        World w(9);
        w.applySettings(quiet(), RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(withWindow(RectI{300, 300, 900, 700}));
        auto in = WorldTestAccess::get(w);
        in.objects.addPile(*surfaceOf(w, kWin), 200.0f, 24.0f);
        w.setLayout(basicLayout());
        CHECK(in.objects.piles().empty() && in.objects.balls().size() == 1);
        CHECK(in.objects.balls()[0].vx == 0.0f || std::fabs(in.objects.balls()[0].vx) < 30.0f);
        stepN(w, 300);
        CHECK(in.objects.balls().empty() && in.objects.shattered() == 1);
        CHECK(surfaceOf(w, kTaskbarId)->hm.mass() > 0);
    }
    {   // closed without drop on close: gone
        World w(9);
        Settings st = quiet();
        st.accumulation.dropSnowOnClose = false;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(withWindow(RectI{300, 300, 900, 700}));
        auto in = WorldTestAccess::get(w);
        in.objects.addPile(*surfaceOf(w, kWin), 200.0f, 24.0f);
        w.setLayout(basicLayout());
        CHECK(in.objects.piles().empty() && in.objects.balls().empty());
    }
    {   // a small, slow drop onto a drift just lands and stays a ball
        World w(9);
        w.applySettings(quiet(), RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(withWindow(RectI{300, 900, 900, 1030}));   // edge 132 px above the taskbar
        auto in = WorldTestAccess::get(w);
        in.objects.addPile(*surfaceOf(w, kWin), 590.0f, 10.0f);   // near the right end
        w.setLayout(basicLayout());
        stepN(w, 300);
        CHECK(in.objects.shattered() == 0);
        CHECK(in.objects.piles().size() == 1);
    }
    {   // shaken (shake to clear on): all of it comes off
        World w(10);
        Settings st = quiet();
        st.accumulation.shakeOffOnMove = false;
        st.accumulation.shakeToClear = true;
        w.applySettings(st, RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(withWindow(RectI{300, 300, 900, 700}));
        auto in = WorldTestAccess::get(w);
        placeSnowman(w, 300.0f);
        int x = 300;
        for (int leg = 0; leg < 5; ++leg)
            for (int k = 0; k < 3; ++k) {
                x += (leg % 2) ? -20 : 20;
                w.step(1.0f / 60.0f);
                w.setLayout(withWindow(RectI{x, 300, x + 600, 700}));
            }
        CHECK(in.objects.piles().empty());
        CHECK(in.objects.balls().size() + static_cast<std::size_t>(in.objects.shattered()) == 3);
    }
}

TEST(falling_balls_are_deterministic) {
    auto run = [](std::vector<Ball>& balls, std::vector<Chunk>& chunks) {
        World w(42);
        w.applySettings(quiet(), RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(withWindow(RectI{300, 300, 900, 700}));
        placeSnowman(w, 300.0f);
        w.step(1.0f / 60.0f);
        w.setLayout(withWindow(RectI{360, 300, 960, 700}));
        stepN(w, 45);
        auto in = WorldTestAccess::get(w);
        balls = in.objects.balls();
        chunks = in.chunks.chunks();
    };
    std::vector<Ball> a, b;
    std::vector<Chunk> ca, cb;
    run(a, ca);
    run(b, cb);
    CHECK(a.size() == b.size() && ca.size() == cb.size());
    CHECK(!ca.empty() || !a.empty());
    bool same = a.size() == b.size() && ca.size() == cb.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) same = a[i].x == b[i].x && a[i].y == b[i].y;
    for (std::size_t i = 0; same && i < ca.size(); ++i) same = ca[i].x == cb[i].x && ca[i].y == cb[i].y;
    CHECK(same);
}
