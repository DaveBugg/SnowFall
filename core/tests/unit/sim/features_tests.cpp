// Round 2 features: opacity, desktop layer, natural caps, shake to clear, falling snowmen,
// snowplow.
#include <vector>

#include "sim_test_util.h"
#include "surfaces/geometry.h"
#include "surfaces/heightmap.h"

using namespace simtest;
using T = InputEvent::Type;

namespace {
constexpr WindowId kWin = 10;
const RectI kWinRect{300, 400, 900, 800};

DesktopLayout layoutWithWindow(RectI r = kWinRect) {
    DesktopLayout l = basicLayout();
    l.windows.insert(l.windows.begin(), window(kWin, r));   // in front of the taskbar
    return l;
}

double stddev(const std::vector<float>& v, int from, int to) {
    double s = 0, s2 = 0;
    const int n = to - from;
    for (int i = from; i < to; ++i) {
        s += v[static_cast<std::size_t>(i)];
        s2 += static_cast<double>(v[static_cast<std::size_t>(i)]) * v[static_cast<std::size_t>(i)];
    }
    const double m = s / n;
    return std::sqrt(std::max(0.0, s2 / n - m * m));
}

// Animations on, no ambient snowfall (only what the tested feature produces).
Settings quietSettings() {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = false;
    s.accumulation.shakeOffOnMove = false;
    s.accumulation.shakeToClear = true;
    s.festive.garland = false;
    s.interaction.tool = Tool::Mitten;   // the tests cycle from the mitten
    return s;
}

void fillToCap(World& w, Surface& s) {
    for (int i = 0; i < s.hm.columns(); ++i) s.hm.setHeight(i, s.hm.cap(i));
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}

int countShape(const FrameSnapshot& snap, Shape shape, const SpriteInstance** found = nullptr) {
    int n = 0;
    for (const SpriteBatch& b : snap.objects)
        for (const SpriteInstance& sp : b.sprites)
            if (sp.shape == shape) {
                ++n;
                if (found) *found = &sp;
            }
    return n;
}
}  // namespace

// ---------------------------------------------------------------- opacity
TEST(opacity_settings_apply_to_flakes_and_drifts) {
    World w(3);
    Settings st;
    st.weather.opacityPercent = 40;
    st.accumulation.driftOpacityPercent = 55;
    w.applySettings(st, RuntimeTuning{});
    w.setLayout(layoutWithWindow());
    stepN(w, 400);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(!snap.particles.sprites.empty() && !snap.drifts.empty());
    float maxOp = 0;
    for (const SpriteInstance& s : snap.particles.sprites) maxOp = std::max(maxOp, s.opacity);
    CHECK(maxOp <= 0.4f + 1e-5f && maxOp > 0.3f);
    for (const DriftStrip& d : snap.drifts) CHECK_NEAR(d.opacity, 0.55, 1e-6);
    st.weather.opacityPercent = 100;
    st.accumulation.driftOpacityPercent = 100;
    w.applySettings(st, RuntimeTuning{});
    w.buildSnapshot(snap);
    maxOp = 0;
    for (const SpriteInstance& s : snap.particles.sprites) maxOp = std::max(maxOp, s.opacity);
    CHECK(maxOp > 0.9f);
    for (const DriftStrip& d : snap.drifts) CHECK(d.opacity == 1.0f);
}

// ---------------------------------------------------------------- desktop layer
TEST(objects_live_on_the_primary_monitor_behind_all_windows) {
    World w(5);
    Settings st = quietSettings();
    st.accumulation.naturalCaps = false;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{1920, 0, 3840, 1080}));            // secondary listed first
    l.monitors.push_back(monitor(1, RectI{0, 0, 1920, 1080}, 1.0f, true));
    const RectI popup{1000, 900, 1100, 1000};
    const RectI winB{2200, 400, 2800, 800};
    l.windows.push_back(window(60, popup, WindowKind::Occluder));
    l.windows.push_back(window(kWin, kWinRect));
    l.windows.push_back(window(11, winB));
    l.windows.push_back(window(kTaskbarId, RectI{0, kTaskbarTop, 1920, 1080}, WindowKind::Taskbar));
    w.setLayout(l);
    auto in = WorldTestAccess::get(w);
    CHECK(in.surfaces.primaryRect() == (RectI{0, 0, 1920, 1080}));
    Surface* a = surfaceOf(w, kWin);
    Surface* b = surfaceOf(w, 11);
    Surface* bar = surfaceOf(w, kTaskbarId);
    CHECK(a && b && bar);
    if (!a || !b || !bar) return;
    in.objects.addPile(*a, 200.0f, 20.0f);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(snap.objects.size() == 1);
    if (snap.objects.size() != 1) return;
    CHECK(!snap.objects[0].clip.empty());
    for (const RectI& c : snap.objects[0].clip) {
        CHECK(c.l >= 0 && c.r <= 1920 && c.t >= 0 && c.b <= 1080);
        for (const WindowInfo& wi : l.windows) CHECK(!c.intersects(wi.bounds));
    }

    // Snowball tool: no ball on the secondary monitor, a ball on the primary one.
    fill(w, *b, 20.0f);
    fill(w, *bar, 20.0f);
    w.setInteractionMode(true);
    input(w, T::CycleTool);
    input(w, T::CycleTool);   // Mitten -> Shovel -> Snowball
    CHECK(WorldTestAccess::get(w).tool == Tool::Snowball);
    const double massB = b->hm.mass();
    input(w, T::PointerMove, 2500.0f, 392.0f);
    input(w, T::PointerDown, 2500.0f, 392.0f);
    CHECK(!in.objects.holding());
    CHECK(b->hm.mass() == massB);
    input(w, T::PointerUp, 2500.0f, 392.0f);
    input(w, T::PointerMove, 1500.0f, 1024.0f);
    input(w, T::PointerDown, 1500.0f, 1024.0f);
    CHECK(in.objects.holding());
    // ... dragged over to the secondary monitor: it falls apart.
    input(w, T::PointerMove, 1900.0f, 1024.0f);
    input(w, T::PointerMove, 2500.0f, 600.0f);
    w.step(1.0f / 60.0f);
    CHECK(!in.objects.holding() && in.objects.balls().empty());
    input(w, T::PointerUp, 2500.0f, 600.0f);
    // Mitten works on every monitor.
    input(w, T::CycleTool);
    input(w, T::PointerDown, 2500.0f, 392.0f);
    input(w, T::PointerUp, 2500.0f, 392.0f);
    CHECK(b->hm.mass() < massB);

    // The window carrying the pile moves to the secondary monitor: the pile breaks.
    CHECK(in.objects.piles().size() == 1);
    l.windows[1] = window(kWin, RectI{2000, 300, 2600, 700});
    w.setLayout(l);
    w.step(1.0f / 60.0f);
    CHECK(in.objects.piles().empty());
    w.buildSnapshot(snap);
    CHECK(snap.objects.empty() && snap.stats.snowballs == 0);
}

// ---------------------------------------------------------------- natural caps
TEST(natural_caps_are_lumpy_deterministic_and_local) {
    Heightmap a, b, c;
    for (Heightmap* h : {&a, &b, &c}) h->reset(400, 3.0f);
    a.setLimits(36.0f, 0.0f, 1197.0f, true, 42, 1.0f);
    b.setLimits(36.0f, 0.0f, 1197.0f, true, 42, 1.0f);
    c.setLimits(36.0f, 0.0f, 1197.0f, true, 43, 1.0f);
    bool same = true, differs = false;
    float lo = 1e9f, hi = 0;
    std::vector<float> caps;
    for (int i = 0; i < 400; ++i) {
        same = same && a.cap(i) == b.cap(i);
        differs = differs || a.cap(i) != c.cap(i);
        lo = std::min(lo, a.cap(i));
        hi = std::max(hi, a.cap(i));
        caps.push_back(a.cap(i));
    }
    CHECK(same && differs);
    CHECK(lo >= kNaturalCapMin * 36.0f - 1e-3f && hi <= 36.0f + 1e-3f);
    CHECK(hi - lo > 8.0f);
    CHECK(stddev(caps, 0, 400) > 2.5);
    float maxStep = 0;   // smooth: soft mounds, not spikes
    for (int i = 0; i + 1 < 400; ++i) maxStep = std::max(maxStep, std::fabs(a.cap(i) - a.cap(i + 1)));
    CHECK(maxStep < 3.0f);
    // Same profile at 150 % DPI (wavelengths scale with DPI).
    Heightmap d;
    d.reset(240, 5.0f);   // 1200 px at 150 % ~ 800 px at 100 %
    d.setLimits(54.0f, 0.0f, 1195.0f, true, 42, 1.5f);
    CHECK_NEAR(d.cap(60) / 54.0, naturalCapFactor(42, 200.0f), 1e-4);

    // Saturation: the full drift keeps the lumpy profile; relaxation never flattens it.
    Heightmap h;
    h.reset(400, 3.0f);
    h.setLimits(36.0f, 0.0f, 1197.0f, true, 7, 1.0f);
    RelaxParams rp;
    rp.openLeft = rp.openRight = false;
    double deposited = 0;
    for (int k = 0; k < 40000; ++k) {
        deposited += h.deposit(static_cast<float>((k * 37) % 1197), 6.0f);
        if (k % 4 == 0) h.relax(rp);
    }
    for (int k = 0; k < 3000; ++k) h.relax(rp);
    CHECK_NEAR(h.mass(), deposited, deposited * 1e-4);
    int full = 0;
    for (int i = 0; i < 400; ++i) {
        CHECK(h.height(i) <= h.cap(i) + 1e-3f);
        full += h.height(i) >= 0.95f * h.cap(i) ? 1 : 0;
    }
    CHECK(full > 360);
    CHECK(stddev(h.heights(), 0, 400) > 2.5);
}

TEST(natural_caps_stable_under_move_and_resize) {
    World w;
    w.applySettings(quietSettings(), RuntimeTuning{});
    w.setLayout(layoutWithWindow());
    Surface* s = surfaceOf(w, kWin);
    CHECK(s != nullptr);
    if (!s) return;
    std::vector<float> caps;
    for (int i = 0; i < s->hm.columns(); ++i) caps.push_back(s->hm.cap(i));
    const SurfaceId id = s->id;
    w.setLayout(layoutWithWindow(RectI{600, 300, 1200, 700}));   // moved
    s = surfaceOf(w, kWin);
    bool same = s && s->id == id;
    for (int i = 0; same && i < s->hm.columns(); ++i) same = s->hm.cap(i) == caps[static_cast<std::size_t>(i)];
    CHECK(same);
    w.setLayout(layoutWithWindow(RectI{600, 300, 1000, 700}));   // narrower
    s = surfaceOf(w, kWin);
    same = s != nullptr;
    for (int i = 0; same && i < s->hm.columns(); ++i) same = s->hm.cap(i) == caps[static_cast<std::size_t>(i)];
    CHECK(same);
    w.setLayout(layoutWithWindow(RectI{600, 300, 1400, 700}));   // wider: old part unchanged
    s = surfaceOf(w, kWin);
    same = s != nullptr;
    for (std::size_t i = 0; same && i < caps.size(); ++i) same = s->hm.cap(static_cast<int>(i)) == caps[i];
    CHECK(same);
    // Another window gets another profile (per-surface seed, not the WindowId).
    DesktopLayout two = layoutWithWindow();
    two.windows.insert(two.windows.begin(), window(99, RectI{1000, 200, 1600, 300}));
    w.setLayout(two);
    const Surface* o = surfaceOf(w, 99);
    const Surface* me = surfaceOf(w, kWin);
    CHECK(o && me && o->capSeed != me->capSeed);
    // Natural caps off: flat.
    Settings flat = quietSettings();
    flat.accumulation.naturalCaps = false;
    w.applySettings(flat, RuntimeTuning{});
    s = surfaceOf(w, kWin);
    CHECK(s && s->hm.cap(10) == 36.0f && s->hm.cap(100) == 36.0f);
}

// ---------------------------------------------------------------- shake to clear
TEST(shaking_a_window_throws_its_snow_off) {
    World w(9);
    w.applySettings(quietSettings(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layoutWithWindow());
    Surface* s = surfaceOf(w, kWin);
    if (!s) {
        CHECK(s != nullptr);
        return;
    }
    fill(w, *s, 15.0f);
    auto in = WorldTestAccess::get(w);
    Pile& p = in.objects.addPile(*s, 300.0f, 25.0f);
    p.r[p.n++] = 16.0f;
    w.step(1.0f / 60.0f);
    const double mass = surfaceOf(w, kWin)->hm.mass();
    int x = 300;
    const int dirs[5] = {1, -1, 1, -1, 1};
    double after = mass;
    for (int leg = 0; leg < 5; ++leg) {
        for (int k = 0; k < 3; ++k) {
            x += 20 * dirs[leg];
            w.step(1.0f / 60.0f);
            w.setLayout(layoutWithWindow(RectI{x, 400, x + 600, 800}));
        }
        if (leg == 1) after = surfaceOf(w, kWin)->hm.mass();   // two legs (one reversal): nothing yet
    }
    CHECK_NEAR(after, mass, mass * 0.01);
    s = surfaceOf(w, kWin);
    CHECK(s && s->hm.mass() < 0.2 * mass);
    CHECK(in.objects.piles().empty());
    // Thrown-off snow falls as flakes moving with the window.
    CHECK(in.chunks.count() >= 2);   // a 15 px cap comes off as slabs
    double vx = 0;
    for (const Chunk& q : in.chunks.chunks()) vx += q.vx;
    CHECK(vx < 0);   // the 3rd reversal is confirmed during the 4th leg, which goes left
}

TEST(shake_detection_ignores_drags_jitter_and_snaps) {
    auto run = [](auto&& moves) {
        World w(9);
        Settings st = quietSettings();
        st.performance.animations = false;
        st.accumulation.naturalCaps = false;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(layoutWithWindow());
        Surface* s = surfaceOf(w, kWin);
        // Snow away from the (open) ends, so nothing slides off by itself during the test.
        for (int i = 30; i < s->hm.columns() - 30; ++i) s->hm.setHeight(i, 15.0f);
        w.step(1.0f / 60.0f);
        const double mass = w.totalSnowMass();
        moves(w);
        return w.totalSnowMass() / mass;
    };
    // A long straight drag.
    CHECK(run([](World& w) {
              for (int k = 1; k <= 200; ++k) {
                  w.step(1.0f / 60.0f);
                  w.setLayout(layoutWithWindow(RectI{300 + 5 * k, 400, 900 + 5 * k, 800}));
              }
          }) > 0.99);
    // A drag with mouse jitter (tiny reversals).
    CHECK(run([](World& w) {
              int x = 300;
              for (int k = 1; k <= 120; ++k) {
                  x += (k % 4 == 0) ? -2 : 9;
                  w.step(1.0f / 60.0f);
                  w.setLayout(layoutWithWindow(RectI{x, 400, x + 600, 800}));
              }
          }) > 0.99);
    // Win+Arrow snaps back and forth (teleports).
    CHECK(run([](World& w) {
              for (int k = 0; k < 8; ++k) {
                  const int x = (k % 2) ? 1200 : 100;
                  w.step(1.0f / 60.0f);
                  w.setLayout(layoutWithWindow(RectI{x, 400, x + 600, 800}));
              }
          }) > 0.99);
    // Slow back-and-forth (each reversal ~1.3 s apart).
    CHECK(run([](World& w) {
              int x = 300;
              for (int leg = 0; leg < 5; ++leg)
                  for (int k = 0; k < 80; ++k) {
                      x += (leg % 2) ? -1 : 1;
                      w.step(1.0f / 60.0f);
                      w.setLayout(layoutWithWindow(RectI{x, 400, x + 600, 800}));
                  }
          }) > 0.99);
    // The real thing, sanity check of the helper.
    CHECK(run([](World& w) {
              int x = 300;
              for (int leg = 0; leg < 5; ++leg)
                  for (int k = 0; k < 3; ++k) {
                      x += (leg % 2) ? -20 : 20;
                      w.step(1.0f / 60.0f);
                      w.setLayout(layoutWithWindow(RectI{x, 400, x + 600, 800}));
                  }
          }) < 0.2);
    // Disabled in the settings: nothing happens.
    World w(9);
    Settings st = quietSettings();
    st.accumulation.shakeToClear = false;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layoutWithWindow());
    fill(w, *surfaceOf(w, kWin), 15.0f);
    w.step(1.0f / 60.0f);
    const double m0 = surfaceOf(w, kWin)->hm.mass();
    int x = 300;
    for (int leg = 0; leg < 5; ++leg)
        for (int k = 0; k < 3; ++k) {
            x += (leg % 2) ? -20 : 20;
            w.step(1.0f / 60.0f);
            w.setLayout(layoutWithWindow(RectI{x, 400, x + 600, 800}));
        }
    CHECK(surfaceOf(w, kWin)->hm.mass() > 0.97 * m0);
}

// ---------------------------------------------------------------- falling snowmen
TEST(falling_snowman_lands_with_a_crater_and_rides_the_window) {
    World w(21);
    w.applySettings(quietSettings(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layoutWithWindow());
    auto in = WorldTestAccess::get(w);
    CHECK(!WorldTestAccess::heavySnow(w));
    CHECK(!WorldTestAccess::dropSnowman(w));   // nowhere full to land
    Surface* s = surfaceOf(w, kWin);
    if (!s) {
        CHECK(s != nullptr);
        return;
    }
    fillToCap(w, *s);
    w.step(1.0f / 60.0f);
    CHECK(WorldTestAccess::heavySnow(w) == false);   // the taskbar is still empty: < 60 % of the columns
    fillToCap(w, *surfaceOf(w, kTaskbarId));
    CHECK(WorldTestAccess::heavySnow(w));
    w.clearAllSnow();
    s = surfaceOf(w, kWin);
    fillToCap(w, *s);   // only the window is a valid target now

    CHECK(WorldTestAccess::dropSnowman(w));
    CHECK(in.objects.falling().size() == 1 && in.objects.autoSnowmenAlive() == 1);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(countShape(snap, Shape::Carrot) == 1);   // drawn while falling ...
    CHECK(snap.stats.snowmen == 0);                // ... but not yet a standing snowman
    const SpriteInstance* carrot = nullptr;
    countShape(snap, Shape::Carrot, &carrot);
    CHECK(carrot && carrot->y < 0);                // starts above the primary monitor

    double massBefore = s->hm.mass();
    int steps = 0;
    while (!in.objects.falling().empty() && steps < 600) {
        massBefore = surfaceOf(w, kWin)->hm.mass();
        w.step(1.0f / 60.0f);
        ++steps;
    }
    CHECK(in.objects.falling().empty() && steps > 20);
    CHECK(in.objects.piles().size() == 1);
    if (in.objects.piles().size() != 1) return;
    const Pile pile = in.objects.piles()[0];
    s = surfaceOf(w, kWin);
    CHECK(pile.surface == s->id && pile.autoSpawned && pile.n >= 2);
    // Crater + rims: most of the displaced snow stays in the drift, the rest splashed.
    const CraterResult cr = in.objects.lastCrater();
    CHECK(cr.removed > 50.0f);
    CHECK(cr.rims >= 0.5f * cr.removed && cr.rims <= cr.removed);
    const double lost = massBefore - s->hm.mass();
    CHECK_NEAR(lost, cr.removed - cr.rims, 0.02 * cr.removed + 1.0);
    const int col = s->hm.columnAt(pile.localX);
    CHECK(s->hm.height(col) < 0.6f * s->hm.cap(col));   // crater
    const int rim = col + static_cast<int>(std::ceil(pile.r[0] / s->hm.step())) + 2;
    CHECK(s->hm.height(rim) > s->hm.cap(rim));          // rim pushed above the cap
    CHECK(w.particleCount() > 15);                      // splash
    w.buildSnapshot(snap);
    CHECK(snap.stats.snowmen == 1);

    // It came upright and now rides the window.
    stepN(w, 120);
    CHECK(std::fabs(in.objects.piles()[0].tilt) < 0.01f);
    const PileGeom g0 = pileGeom(in.objects.piles()[0], *surfaceOf(w, kWin));
    for (int i = 1; i <= 10; ++i) {
        w.step(1.0f / 60.0f);
        w.setLayout(layoutWithWindow(RectI{300 + 4 * i, 400 + 2 * i, 900 + 4 * i, 800 + 2 * i}));
    }
    CHECK(in.objects.piles().size() == 1);
    if (in.objects.piles().size() == 1) {
        const PileGeom g1 = pileGeom(in.objects.piles()[0], *surfaceOf(w, kWin));
        CHECK_NEAR(g1.cx, g0.cx + 40.0f, 1e-3);
        CHECK_NEAR(g1.base, g0.base + 20.0f, 0.5);
    }
}

TEST(falling_snowmen_schedule_needs_heavy_snow_and_is_limited) {
    for (int heavy = 0; heavy < 2; ++heavy) {
        World w(33);
        Settings st;
        st.accumulation.snowplow = false;
        st.accumulation.snowmenIntervalMinutes = 1;   // waits of 60..84 s
        w.applySettings(st, RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(layoutWithWindow());
        auto in = WorldTestAccess::get(w);
        if (heavy) {
            fillToCap(w, *surfaceOf(w, kWin));
            fillToCap(w, *surfaceOf(w, kTaskbarId));
        }
        stepN(w, 130 * 20, 1.0f / 20.0f);   // 130 s
        if (heavy) CHECK(in.objects.autoSnowmenAlive() >= 1);
        else CHECK(in.objects.autoSnowmenAlive() == 0 && in.objects.piles().empty());
        if (!heavy) continue;
        int maxAlive = 0;
        for (int k = 0; k < 600; ++k) {   // 10 more minutes
            stepN(w, 20, 1.0f / 20.0f);
            maxAlive = std::max(maxAlive, in.objects.autoSnowmenAlive());
        }
        CHECK(maxAlive <= kMaxAutoSnowmen);
        CHECK(in.objects.snowmanCount() >= 2);
        // Disabled: no new ones.
        st.accumulation.fallingSnowmen = false;
        w.applySettings(st, RuntimeTuning{});
        w.clearAllSnow();
        fillToCap(w, *surfaceOf(w, kWin));
        fillToCap(w, *surfaceOf(w, kTaskbarId));
        stepN(w, 200 * 20, 1.0f / 20.0f);
        CHECK(in.objects.autoSnowmenAlive() == 0);
    }
}

// ---------------------------------------------------------------- snowplow
TEST(snowplow_clears_the_primary_taskbar_and_drives_off) {
    World w(4);
    w.applySettings(quietSettings(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    Surface* bar = surfaceOf(w, kTaskbarId);
    if (!bar) {
        CHECK(bar != nullptr);
        return;
    }
    fillToCap(w, *bar);
    Pile& p = in.objects.addPile(*bar, 900.0f, 22.0f);
    p.r[p.n++] = 15.0f;
    const double m0 = bar->hm.mass();
    w.triggerSnowplow();
    CHECK(in.plow.active() && in.plow.dir() == 1);
    w.triggerSnowplow();   // already running: ignored
    CHECK(in.plow.dir() == 1);

    FrameSnapshot snap;
    stepN(w, 300);   // 5 s: about 750 px along
    w.buildSnapshot(snap);
    const SpriteInstance* plow = nullptr;
    CHECK(countShape(snap, Shape::Snowplow, &plow) == 1);
    if (plow) {
        CHECK(plow->variant == 0 && plow->vx > 0);
        CHECK(std::fabs(plow->y + plow->halfH - static_cast<float>(kTaskbarTop)) < 2.0f);   // on the base line
        CHECK_NEAR(plow->halfW, 46.0, 1e-3);
        CHECK_NEAR(plow->halfH, 30.0, 1e-3);
        CHECK(plow->x > 500.0f && plow->x < 1000.0f);
    }
    const DriftStrip* d = driftOf(snap, bar->id);
    CHECK(d != nullptr);
    if (d && plow) {
        // Behind the plow: clear. Just ahead of the blade: the pushed pile rises above the drift.
        const int behind = static_cast<int>((plow->x - 100.0f) / d->step);
        CHECK(d->heights[static_cast<std::size_t>(behind)] <= 5.0f + 1e-3f);   // scraped (thin residual / clods)
        const int ahead = static_cast<int>((plow->x + plow->halfW + 6.0f) / d->step);
        CHECK(d->heights[static_cast<std::size_t>(ahead)] >= bar->hm.height(ahead));
        CHECK(in.plow.heapHeight() > 20.0f && in.plow.heapHeight() <= in.plow.bladeHeight() + 1e-3f);
    }
    int steps = 0;
    while (in.plow.active() && steps < 3000) {
        w.step(1.0f / 60.0f);
        ++steps;
    }
    CHECK(!in.plow.active() && in.plow.completedRuns() == 1);
    CHECK(in.plow.lastExitLocal() > 1920.0f + 30.0f);   // drove off-screen
    CHECK(bar->hm.mass() <= 0.05 * m0);
    CHECK(in.objects.piles().empty());                  // snowman pushed and broken
    w.buildSnapshot(snap);
    CHECK(countShape(snap, Shape::Snowplow) == 0);
    // Next run comes from the other side.
    w.triggerSnowplow();
    CHECK(in.plow.active() && in.plow.dir() == -1);
    stepN(w, 60);
    w.buildSnapshot(snap);
    plow = nullptr;
    countShape(snap, Shape::Snowplow, &plow);
    CHECK(plow && plow->variant == 1 && plow->vx < 0);
}

TEST(snowplow_timer_fires_after_the_delay_with_a_full_drift_only) {
    World w(6);
    Settings st = quietSettings();
    st.accumulation.snowplow = true;
    st.accumulation.snowplowDelayMinutes = 1;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    stepN(w, 200 * 20, 1.0f / 20.0f);   // empty drift: never
    CHECK(!in.plow.active() && in.plow.completedRuns() == 0);

    fillToCap(w, *surfaceOf(w, kTaskbarId));
    stepN(w, 50 * 20, 1.0f / 20.0f);
    CHECK(!in.plow.active() && in.plow.completedRuns() == 0);
    stepN(w, 15 * 20, 1.0f / 20.0f);
    CHECK(in.plow.active() || in.plow.completedRuns() == 1);
    stepN(w, 30 * 20, 1.0f / 20.0f);
    CHECK(!in.plow.active() && in.plow.completedRuns() == 1);

    // Cooldown: the delay starts over after the run.
    fillToCap(w, *surfaceOf(w, kTaskbarId));
    stepN(w, 40 * 20, 1.0f / 20.0f);
    CHECK(!in.plow.active() && in.plow.completedRuns() == 1);
    stepN(w, 25 * 20, 1.0f / 20.0f);
    CHECK(in.plow.active() || in.plow.completedRuns() == 2);

    // Clearing the drift resets the full-time.
    stepN(w, 30 * 20, 1.0f / 20.0f);
    fillToCap(w, *surfaceOf(w, kTaskbarId));
    stepN(w, 40 * 20, 1.0f / 20.0f);
    w.clearAllSnow();
    stepN(w, 2 * 20, 1.0f / 20.0f);
    fillToCap(w, *surfaceOf(w, kTaskbarId));
    stepN(w, 40 * 20, 1.0f / 20.0f);
    CHECK(!in.plow.active() && in.plow.completedRuns() == 2);

    // No taskbar on the primary monitor: the manual trigger does nothing.
    DesktopLayout noBar;
    noBar.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    w.setLayout(noBar);
    w.triggerSnowplow();
    CHECK(!in.plow.active());
}
