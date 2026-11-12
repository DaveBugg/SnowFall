// Round 11: a sky per monitor; snowman arms (shoulders, wave).
#include <vector>

#include "sim_test_util.h"

using namespace simtest;

namespace {
Settings quiet(bool perMonitor) {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = false;
    s.festive.garland = false;
    s.weather.skyPerMonitor = perMonitor;
    s.weather.wind = 0;
    return s;
}

DesktopLayout stacked() {   // primary above a second monitor, a taskbar on each
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.monitors.push_back(monitor(1, RectI{0, 1080, 1920, 2160}));
    l.windows.push_back(window(kTaskbarId, RectI{0, kTaskbarTop, 1920, 1080}, WindowKind::Taskbar));
    l.windows.push_back(window(2, RectI{0, 2112, 1920, 2160}, WindowKind::Taskbar));
    return l;
}

int countIn(World& w, float y0, float y1, float x0 = -1e9f, float x1 = 1e9f) {
    int n = 0;
    for (const Particle& p : WorldTestAccess::get(w).particles.particles())
        if (!(p.flags & Particle::kDust) && p.y >= y0 && p.y < y1 && p.x >= x0 && p.x < x1) ++n;
    return n;
}
}  // namespace

TEST(sky_per_monitor_snows_on_a_lower_monitor_and_never_crosses) {
    World w(1);
    w.applySettings(quiet(true), RuntimeTuning{});
    w.setLayout(stacked());
    auto in = WorldTestAccess::get(w);
    int crossings = 0;
    for (int k = 0; k < 1500; ++k) {
        w.step(1.0f / 60.0f);
        for (const Particle& p : in.particles.particles()) {
            if (p.home == 255) continue;
            const RectI r = p.home == 0 ? RectI{0, 0, 1920, 1080} : RectI{0, 1080, 1920, 2160};
            if (p.x < r.l - 1.5f || p.x > r.r + 1.5f || p.y > r.b + 1.5f) ++crossings;
        }
    }
    const int lower = countIn(w, 1080, 2160);
    const double bar2 = surfaceOf(w, 2)->hm.mass();
    printf("  stacked, sky per monitor: %d flakes on the lower monitor, lower taskbar %.0f px^2, crossings %d\n", lower, bar2,
           crossings);
    CHECK(crossings == 0);
    CHECK(lower > 100);
    CHECK(bar2 > 100.0);
    FrameSnapshot snap;   // flakes near a monitor edge fade instead of popping
    w.buildSnapshot(snap);
    for (const SpriteInstance& s : snap.particles.sprites) CHECK(s.opacity > 0 && s.opacity <= 1);
}

TEST(single_sky_lets_foreground_flakes_reach_a_lower_monitor) {
    World w(2);
    w.applySettings(quiet(false), RuntimeTuning{});
    w.setLayout(stacked());
    stepN(w, 1500);
    const int lower = countIn(w, 1150, 2160);
    printf("  stacked, single sky: %d flakes below the upper taskbar\n", lower);
    CHECK(lower > 20);
    // Live switch to a sky per monitor.
    w.applySettings(quiet(true), RuntimeTuning{});
    stepN(w, 600);
    CHECK(countIn(w, 1080, 2160) > 100);
}

TEST(sky_budget_split_by_area_and_offset_monitors_start_at_their_own_top) {
    World w(3);
    w.applySettings(quiet(true), RuntimeTuning{});
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.monitors.push_back(monitor(1, RectI{1920, -300, 4480, 1140}));   // 2560x1440, offset upwards
    w.setLayout(l);
    auto in = WorldTestAccess::get(w);
    stepN(w, 1800);
    const double ratio = static_cast<double>(in.particles.skyCount(1)) / std::max(1, in.particles.skyCount(0));
    printf("  budget split: %d / %d flakes (ratio %.2f, area ratio 1.78)\n", in.particles.skyCount(0), in.particles.skyCount(1),
           ratio);
    CHECK(ratio > 1.45 && ratio < 2.15);
    CHECK(std::abs(in.particles.skyTarget(0) + in.particles.skyTarget(1) - in.particles.targetCount()) <= 1);
    // The offset monitor's snow starts at its own top; nothing from monitor 0 strays over.
    CHECK(countIn(w, -300, -100, 1920, 4480) > 10);
    bool stray = false;
    for (const Particle& p : in.particles.particles())
        if (p.home == 0 && p.x > 1921.5f) stray = true;
    CHECK(!stray);
}

// ---------------------------------------------------------------- arms
namespace {
struct ArmEnds {
    float shoulderX[2], shoulderY[2], tipX[2], tipY[2], rot[2];
};
// The two stick arms of the only snowman: [0] = -X arm, [1] = +X arm.
bool arms(const FrameSnapshot& snap, float cx, ArmEnds& a) {
    int n = 0;
    for (const SpriteBatch& b : snap.objects)
        for (const SpriteInstance& s : b.sprites) {
            if (s.shape != Shape::Stick) continue;
            const float ex = std::cos(s.rotation) * s.halfW, ey = std::sin(s.rotation) * s.halfW;
            const bool firstInner = std::fabs(s.x - ex - cx) < std::fabs(s.x + ex - cx);
            const int side = s.x > cx ? 1 : 0;
            a.shoulderX[side] = firstInner ? s.x - ex : s.x + ex;
            a.shoulderY[side] = firstInner ? s.y - ey : s.y + ey;
            a.tipX[side] = firstInner ? s.x + ex : s.x - ex;
            a.tipY[side] = firstInner ? s.y + ey : s.y - ey;
            a.rot[side] = s.rotation;
            ++n;
        }
    return n == 2;
}
}  // namespace

TEST(snowman_arms_hang_from_the_shoulders_and_wave_clear_of_the_face) {
    World w(4);
    Settings st = quiet(true);
    st.performance.animations = false;
    w.applySettings(st, RuntimeTuning{});
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    Pile& p = in.objects.addPile(*surfaceOf(w, kTaskbarId), 800.0f, 26.0f);
    p.r[p.n++] = 19.0f;
    p.r[p.n++] = 13.0f;
    p.look.set = true;
    const Surface* bar = surfaceOf(w, kTaskbarId);
    for (int pose = 0; pose < 3; ++pose) {
        in.objects.piles()[0].look.arms = static_cast<std::uint8_t>(pose);
        in.objects.piles()[0].waveT = 0;
        FrameSnapshot snap;
        w.buildSnapshot(snap);
        const PileGeom g = pileGeom(in.objects.piles()[0], *bar);
        ArmEnds a{};
        CHECK(arms(snap, g.cx, a));
        // Symmetric shoulders on the ball below the head, at its upper third.
        CHECK_NEAR(a.shoulderX[0] + a.shoulderX[1], 2.0 * g.cx, 0.01);
        CHECK_NEAR(a.shoulderY[0], a.shoulderY[1], 0.01);
        CHECK(a.shoulderY[1] < g.cy[1] && a.shoulderY[1] > g.cy[1] - g.r[1]);
        CHECK(a.shoulderY[1] > g.cy[2] + 0.5f * g.r[2]);   // never on the head
        for (int s = 0; s < 2; ++s) CHECK(std::hypot(a.tipX[s] - g.cx, a.tipY[s] - g.cy[2]) >= 1.24f * g.r[2]);
        if (pose != 1) CHECK_NEAR(a.rot[1], kArmRestAngle, 1e-4);   // at rest, -20 degrees
    }
    // A waving snowman: the arm swings -5..-55 degrees in 2-3 sine swings, then pauses 4-8 s,
    // and its tip never comes near the face.
    Pile& wp = in.objects.piles()[0];
    wp.look.arms = 2;
    wp.waveIn = 0.01f;
    Settings anim = quiet(true);
    w.applySettings(anim, RuntimeTuning{});
    w.setEmissionEnabled(false);
    float lo = 1e9f, hi = -1e9f, minClear = 1e9f;
    int waves = 0;
    bool waving = false;
    float pauseSeen = 0;
    for (int k = 0; k < 60 * 30; ++k) {
        w.step(1.0f / 60.0f);
        const Pile& q = in.objects.piles()[0];
        if (q.waveT > 0 && !waving) {
            ++waves;
            CHECK(q.wavePeriod >= 1.4f && q.wavePeriod <= 1.8f);
            const float cycles = q.waveDur / q.wavePeriod;
            CHECK(std::fabs(cycles - std::round(cycles)) < 1e-3 && cycles >= 2 && cycles <= 3);
        }
        if (waving && q.waveT <= 0) pauseSeen = q.waveIn;
        waving = q.waveT > 0;
        FrameSnapshot snap;
        w.buildSnapshot(snap);
        const PileGeom g = pileGeom(q, *bar);
        ArmEnds a{};
        if (!arms(snap, g.cx, a)) continue;
        lo = std::min(lo, a.rot[1]);
        hi = std::max(hi, a.rot[1]);
        for (int s = 0; s < 2; ++s) minClear = std::min(minClear, std::hypot(a.tipX[s] - g.cx, a.tipY[s] - g.cy[2]) / g.r[2]);
    }
    printf("  wave: arm angle %.0f..%.0f deg, %d waves in 30 s, last pause %.1f s, tip clearance >= %.2f head radii\n",
           lo * 57.2958f, hi * 57.2958f, waves, pauseSeen, minClear);
    CHECK(lo < -0.9f && lo > -1.05f);   // up to ~55 deg (+ a little flick)
    CHECK(hi > -0.12f && hi < -0.05f);  // down to ~5 deg
    CHECK(waves >= 2);
    CHECK(pauseSeen >= 4.0f && pauseSeen <= 8.0f);
    CHECK(minClear >= 1.24f);
}
