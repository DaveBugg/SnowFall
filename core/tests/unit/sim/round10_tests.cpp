// Round 10: snowballs (and flung snow) knock icicles down.
#include <vector>

#include "sim_test_util.h"

using namespace simtest;

namespace {
constexpr WindowId kWin = 10;
const RectI kWinRect{300, 300, 1100, 700};

Settings quiet() {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = false;
    s.accumulation.shakeOffOnMove = false;
    s.accumulation.naturalCaps = false;
    s.festive.garland = false;
    return s;
}

DesktopLayout layout(std::vector<WindowInfo> front = {}) {
    DesktopLayout l = basicLayout();
    l.windows.insert(l.windows.begin(), window(kWin, kWinRect));
    l.windows.insert(l.windows.begin(), front.begin(), front.end());
    return l;
}

// Three 30 px icicles under the window at local x 200, 300, 400.
IcicleSet& addIcicles(World& w, std::vector<float> xs = {200.0f, 300.0f, 400.0f}) {
    auto in = WorldTestAccess::get(w);
    in.icicles.sets().emplace_back();
    IcicleSet& set = in.icicles.sets().back();
    set.surface = surfaceOf(w, kWin)->id;
    set.seeded = true;
    set.capTime = 1000.0f;
    for (float x : xs) {
        Icicle ic;
        ic.localX = x;
        ic.len = 30.0f;
        ic.maxLen = 36.0f;
        ic.width = 4.0f;
        ic.rate = 0.05f;
        set.list.push_back(ic);
    }
    return set;
}

Ball& throwBall(World& w, float x, float y, float vx, float vy) {
    Ball b;
    b.x = x;
    b.y = y;
    b.vx = vx;
    b.vy = vy;
    b.r = 8.0f;
    b.collected = 3.14159265f * 64.0f / kPackFactor;
    auto& balls = WorldTestAccess::get(w).objects.balls();
    balls.push_back(b);
    return balls.back();
}

bool attachedAt(World& w, float localX) {
    for (const Icicle& ic : WorldTestAccess::get(w).icicles.sets()[0].list)
        if (ic.localX == localX) return true;
    return false;
}
// Still whole (not snapped at the root and no tip broken off)?
bool wholeAt(World& w, float localX) {
    for (const Icicle& ic : WorldTestAccess::get(w).icicles.sets()[0].list)
        if (ic.localX == localX) return ic.len >= 29.9f;
    return false;
}
}  // namespace

TEST(fast_ball_breaks_icicles_in_order_and_slows_down) {
    World w(1);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layout());
    addIcicles(w);
    auto in = WorldTestAccess::get(w);
    throwBall(w, 420.0f, 712.0f, 1500.0f, -200.0f);   // through the upper halves: roots break
    int brokeAt[3] = {-1, -1, -1};
    float vxAfter = 1500.0f;
    for (int k = 0; k < 30; ++k) {
        w.step(1.0f / 60.0f);
        for (int i = 0; i < 3; ++i)
            if (brokeAt[i] < 0 && !wholeAt(w, 200.0f + 100.0f * static_cast<float>(i))) brokeAt[i] = k;
        if (!in.objects.balls().empty() && k < 10) vxAfter = in.objects.balls()[0].vx;
    }
    printf("  icicles broken at steps %d %d %d, ball vx 1500 -> %.0f px/s\n", brokeAt[0], brokeAt[1], brokeAt[2], vxAfter);
    CHECK(brokeAt[0] >= 0 && brokeAt[1] >= 0 && brokeAt[2] >= 0);
    CHECK(brokeAt[0] <= brokeAt[1] && brokeAt[1] <= brokeAt[2]);
    CHECK(vxAfter < 0.6f * 1500.0f && vxAfter > 0.2f * 1500.0f);
    CHECK(in.icicles.hitBroken() == 3);   // (the ball sinks: the last one may lose only its tip)
    CHECK(in.icicles.attachedCount() == in.icicles.hitStubs());
    stepN(w, 120);
    CHECK(in.icicles.shattered() >= 3);   // the pieces shattered on landing
}

TEST(slow_ball_bumps_off_icicles) {
    World w(2);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layout());
    addIcicles(w);
    auto in = WorldTestAccess::get(w);
    throwBall(w, 480.0f, 712.0f, 200.0f, -150.0f);
    float minVx = 1e9f;
    for (int k = 0; k < 20; ++k) {
        w.step(1.0f / 60.0f);
        if (!in.objects.balls().empty()) minVx = std::min(minVx, in.objects.balls()[0].vx);
    }
    CHECK(in.icicles.attachedCount() == 3 && in.icicles.hitBroken() == 0);
    CHECK(minVx < 0);   // bounced back
}

TEST(low_hit_leaves_a_stub_that_regrows) {
    World w(3);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layout());
    addIcicles(w, {300.0f});
    auto in = WorldTestAccess::get(w);
    throwBall(w, 520.0f, 728.0f, 1200.0f, -60.0f);   // near the tip
    stepN(w, 10);
    CHECK(in.icicles.hitStubs() == 1 && in.icicles.attachedCount() == 1);
    const float stub = in.icicles.sets()[0].list[0].len;
    CHECK(stub > 5.0f && stub < 26.0f);
    // A lasting cap regrows it.
    Surface* s = surfaceOf(w, kWin);
    for (int i = 0; i < s->hm.columns(); ++i) s->hm.setHeight(i, s->hm.cap(i));
    stepN(w, 60 * 20, 1.0f / 20.0f);
    CHECK(in.icicles.sets()[0].list[0].len > stub + 3.0f);
}

TEST(icicles_break_before_the_ball_splats_on_a_window) {
    World w(4);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layout({window(11, RectI{1110, 600, 1800, 1000})}));
    addIcicles(w, {760.0f});   // world x 1060, between the ball and window 11
    auto in = WorldTestAccess::get(w);
    Ball& b = throwBall(w, 1000.0f, 712.0f, 0.0f, 0.0f);
    b.held = true;
    in.objects.release(2500.0f, 0.0f, in.surfaces, &in.particles);
    CHECK(in.icicles.hitBroken() == 1 && in.icicles.attachedCount() == 0);
    CHECK(in.splats.mainCount() == 1 && in.objects.balls().empty());
    CHECK(!in.splats.splats().empty() && in.splats.splats()[0].window == 11);
}

TEST(flung_snow_and_a_dragged_ball_break_icicles) {
    World w(5);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layout());
    addIcicles(w);
    auto in = WorldTestAccess::get(w);
    Chunk* c = in.chunks.spawnFree(420.0f, 712.0f, 8.0f, 5.0f, 200.0f, 900.0f, 0.0f, 1.0f, 0);
    if (c) c->soft = true;
    stepN(w, 8);
    CHECK(!attachedAt(w, 200.0f));
    // Dragging a held ball through the rest (gentle, but they still snap).
    Ball& b = throwBall(w, 560.0f, 712.0f, 0, 0);
    b.held = true;
    in.objects.moveHeld(560.0f, 712.0f, 720.0f, 712.0f, in.surfaces);
    CHECK(in.icicles.attachedCount() == 0);
    // Deterministic: same seed, same result.
    World w2(5);
    w2.applySettings(quiet(), RuntimeTuning{});
    w2.setEmissionEnabled(false);
    w2.setLayout(layout());
    addIcicles(w2);
    throwBall(w2, 420.0f, 712.0f, 1500.0f, -200.0f);
    World w3(5);
    w3.applySettings(quiet(), RuntimeTuning{});
    w3.setEmissionEnabled(false);
    w3.setLayout(layout());
    addIcicles(w3);
    throwBall(w3, 420.0f, 712.0f, 1500.0f, -200.0f);
    stepN(w2, 20);
    stepN(w3, 20);
    const auto& f2 = WorldTestAccess::get(w2).icicles.falling();
    const auto& f3 = WorldTestAccess::get(w3).icicles.falling();
    bool same = f2.size() == f3.size() && !f2.empty();
    for (std::size_t i = 0; same && i < f2.size(); ++i) same = f2[i].x == f3[i].x && f2[i].y == f3[i].y;
    CHECK(same);
}

