// Round 6: icicles, umbrella cursor, thaw, garland, fireworks, splats, snowman looks.
#include <chrono>
#include <vector>

#include "sim_test_util.h"
#include "simulation/festive.h"

using namespace simtest;
using T = InputEvent::Type;

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

void fillToCap(World& w, Surface& s) {
    for (int i = 0; i < s.hm.columns(); ++i) s.hm.setHeight(i, s.hm.cap(i));
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}

int count(const FrameSnapshot& snap, Shape shape) {
    int n = 0;
    for (const SpriteBatch& b : snap.objects)
        for (const SpriteInstance& s : b.sprites) n += s.shape == shape ? 1 : 0;
    for (const SpriteInstance& s : snap.particles.sprites) n += s.shape == shape ? 1 : 0;
    return n;
}

const SpriteBatch* batchWith(const FrameSnapshot& snap, Shape shape) {
    for (const SpriteBatch& b : snap.objects)
        for (const SpriteInstance& s : b.sprites)
            if (s.shape == shape) return &b;
    return nullptr;
}
}  // namespace

// ---------------------------------------------------------------- icicles
TEST(icicles_grow_under_a_lasting_cap_ride_the_window_and_snap_off) {
    World w(1);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindow(RectI{300, 300, 1100, 700}));
    auto in = WorldTestAccess::get(w);
    fillToCap(w, *surfaceOf(w, kWin));
    stepN(w, 140 * 20, 1.0f / 20.0f);   // 140 s: not yet
    CHECK(in.icicles.attachedCount() == 0);
    stepN(w, 20 * 20, 1.0f / 20.0f);    // 160 s
    CHECK(in.icicles.attachedCount() >= 6);
    stepN(w, 400 * 20, 1.0f / 20.0f);   // grown
    const IcicleSet* set = in.icicles.setFor(surfaceOf(w, kWin)->id);
    CHECK(set != nullptr);
    if (!set) return;
    float maxLen = 0;
    for (std::size_t k = 0; k < set->list.size(); ++k) {
        const Icicle& ic = set->list[k];
        CHECK(ic.len > 0 && ic.len <= ic.maxLen + 1e-3f && ic.maxLen >= 6.0f && ic.maxLen <= 40.0f);
        CHECK(ic.width >= 2.0f && ic.width <= 6.0f);
        if (k > 0) {
            const float gap = ic.localX - set->list[k - 1].localX;
            CHECK(gap >= 25.0f - 1e-3f && gap <= 90.0f + 1e-3f);
        }
        maxLen = std::max(maxLen, ic.len);
    }
    CHECK(maxLen > 10.0f);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    const SpriteBatch* b = batchWith(snap, Shape::Icicle);
    CHECK(b != nullptr);
    float firstX = 0;
    if (b) {
        for (const RectI& c : b->clip) CHECK(c.t >= 700 && c.l >= 300 && c.r <= 1100);
        firstX = b->sprites[0].x;
        CHECK_NEAR(b->sprites[0].y - b->sprites[0].halfH, 700.0, 1e-3);   // root at the bottom edge
    }
    // Rides along with a gentle drag.
    for (int i = 1; i <= 10; ++i) {
        w.step(1.0f / 60.0f);
        w.setLayout(withWindow(RectI{300 + 3 * i, 300, 1100 + 3 * i, 700}));
    }
    w.buildSnapshot(snap);
    b = batchWith(snap, Shape::Icicle);
    CHECK(b && std::fabs(b->sprites[0].x - (firstX + 30.0f)) < 1e-3f);
    // Minimised: kept, not drawn.
    DesktopLayout hidden = basicLayout();
    hidden.hiddenWindows.push_back(kWin);
    const int attached = in.icicles.attachedCount();
    w.setLayout(hidden);
    w.buildSnapshot(snap);
    CHECK(count(snap, Shape::Icicle) == 0 && in.icicles.attachedCount() == attached);
    w.setLayout(withWindow(RectI{330, 300, 1130, 700}));
    // A sharp jerk snaps them off; they shatter on the taskbar.
    w.step(1.0f / 60.0f);
    w.setLayout(withWindow(RectI{390, 300, 1190, 700}));
    CHECK(in.icicles.attachedCount() < attached);
    CHECK(!in.icicles.falling().empty());
    stepN(w, 120);
    CHECK(in.icicles.shattered() >= 1);
}

TEST(icicles_grow_faster_in_a_thaw_and_fall_when_the_window_closes) {
    World w(2);
    Settings st = quiet();
    st.weather.preset = WeatherPreset::Thaw;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindow(RectI{300, 300, 1100, 700}));
    auto in = WorldTestAccess::get(w);
    fillToCap(w, *surfaceOf(w, kWin));
    stepN(w, 35 * 20, 1.0f / 20.0f);   // 150 s / 5
    CHECK(in.icicles.attachedCount() > 0);
    stepN(w, 60 * 20, 1.0f / 20.0f);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(count(snap, Shape::Droplet) > 0);   // drips forming / falling
    w.setLayout(basicLayout());               // closed (drop on close)
    CHECK(in.icicles.attachedCount() == 0 && !in.icicles.falling().empty());
    // Disabled: none grow.
    World off(2);
    st.accumulation.icicles = false;
    off.applySettings(st, RuntimeTuning{});
    off.setEmissionEnabled(false);
    off.setLayout(withWindow(RectI{300, 300, 1100, 700}));
    fillToCap(off, *surfaceOf(off, kWin));
    stepN(off, 60 * 20, 1.0f / 20.0f);
    CHECK(WorldTestAccess::get(off).icicles.attachedCount() == 0);
}

// ---------------------------------------------------------------- umbrella
TEST(umbrella_keeps_flakes_out_of_the_dome_and_shelters_below) {
    World w(3);
    Settings st;
    st.weather.preset = WeatherPreset::Blizzard;
    st.weather.wind = 0;
    st.weather.intensity = 100;
    st.performance.profile = PerfProfile::High;
    st.accumulation.fallingSnowmen = false;
    w.applySettings(st, RuntimeTuning{});
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    const float ux = 960, uy = 400, R = kUmbrellaAt96;
    int inside = 0, shelter = 0, open = 0;
    for (int k = 0; k < 1500; ++k) {
        w.setAmbientPointer(ux, uy, true);
        w.step(1.0f / 60.0f);
        if (k < 600) continue;
        for (const Particle& p : in.particles.particles()) {
            if (p.flags & Particle::kDust) continue;
            const float dx = p.x - ux, dy = p.y - uy;
            if (dx * dx + dy * dy < 0.97f * 0.97f * R * R) ++inside;
            if (dy > 0 && dy < 120 && std::fabs(dx) < 0.4f * R) ++shelter;
            if (dy > 0 && dy < 120 && std::fabs(dx - 400) < 0.4f * R) ++open;
        }
    }
    printf("  umbrella: inside %d, sheltered column %d vs open column %d\n", inside, shelter, open);
    CHECK(inside == 0);
    CHECK(open > 100);
    CHECK(shelter < open / 3);
    // Disabled in the settings: flakes fall through.
    st.interaction.umbrellaCursor = false;
    w.applySettings(st, RuntimeTuning{});
    inside = 0;
    for (int k = 0; k < 300; ++k) {
        w.setAmbientPointer(ux, uy, true);
        w.step(1.0f / 60.0f);
        for (const Particle& p : in.particles.particles()) {
            const float dx = p.x - ux, dy = p.y - uy;
            if (!(p.flags & Particle::kDust) && dx * dx + dy * dy < 0.8f * R * R) ++inside;
        }
    }
    CHECK(inside > 0);
}

TEST(fast_pointer_swipe_whips_up_a_gust_along_the_motion) {
    for (int dir = -1; dir <= 1; dir += 2) {
        World w(4);
        Settings st;
        st.weather.wind = 0;
        st.weather.intensity = 100;
        st.weather.preset = WeatherPreset::Blizzard;
        st.accumulation.fallingSnowmen = false;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(basicLayout());
        auto in = WorldTestAccess::get(w);
        for (int k = 0; k < 600; ++k) {
            w.setAmbientPointer(100, 100, true);   // umbrella parked in a corner
            w.step(1.0f / 60.0f);
        }
        auto meanVx = [&]() {
            double s = 0;
            int n = 0;
            for (const Particle& p : in.particles.particles()) {
                if (std::fabs(p.x - 960) < 100 && std::fabs(p.y - 500) < 100) {
                    s += p.vx;
                    ++n;
                }
            }
            return n ? s / n : 0.0;
        };
        const double before = meanVx();
        w.setAmbientPointer(960.0f - 30.0f * static_cast<float>(dir), 500, true);
        w.step(1.0f / 60.0f);
        w.setAmbientPointer(960.0f + 30.0f * static_cast<float>(dir), 500, true);   // 3600 px/s
        w.step(1.0f / 60.0f);
        w.step(1.0f / 60.0f);
        const double after = meanVx();
        printf("  swipe %+d: mean vx near the pointer %.0f -> %.0f px/s\n", dir, before, after);
        CHECK((after - before) * dir > 50.0);
    }
}

// ---------------------------------------------------------------- thaw
TEST(thaw_melts_drifts_wets_them_drips_and_shrinks_snowmen) {
    World w(5);
    Settings st = quiet();
    st.weather.preset = WeatherPreset::Thaw;
    st.accumulation.snowplow = true;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindow(RectI{300, 300, 1100, 700}));
    auto in = WorldTestAccess::get(w);
    Surface* s = surfaceOf(w, kWin);
    for (int i = 0; i < s->hm.columns(); ++i) s->hm.setHeight(i, i < 40 ? 4.0f : 30.0f);
    const float thin0 = s->hm.height(20), thick0 = s->hm.height(150);
    Surface* bar = surfaceOf(w, kTaskbarId);
    fillToCap(w, *bar);
    Pile& p = in.objects.addPile(*bar, 900.0f, 26.0f);
    p.r[p.n++] = 19.0f;
    p.r[p.n++] = 13.0f;
    stepN(w, 20 * 20, 1.0f / 20.0f);   // 20 s
    s = surfaceOf(w, kWin);
    const float thinDrop = thin0 - s->hm.height(20), thickDrop = thick0 - s->hm.height(150);
    printf("  thaw 20 s: thick column -%.2f px, thin column -%.2f px, wetness %.2f\n", thickDrop, thinDrop,
           WorldTestAccess::get(w).wetness);
    CHECK(thickDrop > 0.4f && thickDrop < 1.2f);   // ~1 px per 30 s
    CHECK(thinDrop > 1.3f * thickDrop);            // thin snow goes first
    CHECK(in.particles.splashes() > 0);            // drops fell off the window edges
    CHECK(in.objects.piles().size() == 1 && in.objects.piles()[0].r[0] < 26.0f);
    w.triggerSnowplow();
    CHECK(!in.plow.active());                      // no plow in a thaw
    stepN(w, 120 * 20, 1.0f / 20.0f);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(!snap.drifts.empty());
    for (const DriftStrip& d : snap.drifts) CHECK_NEAR(d.wetness, kWetnessMax, 0.02);
    stepN(w, 200 * 20, 1.0f / 20.0f);
    CHECK(in.objects.piles().empty() && in.objects.collapsed() == 1);   // slumped into the drift
    // Back to normal weather: dries within a minute.
    st.weather.preset = WeatherPreset::Normal;
    w.applySettings(st, RuntimeTuning{});
    stepN(w, 50 * 20, 1.0f / 20.0f);
    w.buildSnapshot(snap);
    for (const DriftStrip& d : snap.drifts) CHECK(d.wetness == 0.0f);
}

TEST(thaw_flakes_are_sparse_heavy_and_straight) {
    double count[2]{}, vy[2]{}, vxs[2]{};
    for (int k = 0; k < 2; ++k) {
        World w(6);
        Settings st;
        st.weather.preset = k == 0 ? WeatherPreset::Normal : WeatherPreset::Thaw;
        st.accumulation.fallingSnowmen = false;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(basicLayout());
        stepN(w, 2000);
        const auto& ps = WorldTestAccess::get(w).particles.particles();
        for (const Particle& p : ps) {
            vy[k] += p.vy;
            vxs[k] += std::fabs(p.vx - 0.0);
        }
        count[k] = static_cast<double>(ps.size());
        vy[k] /= std::max(1.0, count[k]);
        vxs[k] /= std::max(1.0, count[k]);
    }
    CHECK(count[1] < 0.6 * count[0]);
    CHECK(vy[1] > 1.6 * vy[0]);
}

// ---------------------------------------------------------------- garland
TEST(garland_hangs_along_every_taskbar_and_blinks) {
    World w(7);
    Settings st = quiet();
    st.festive.garland = true;
    st.performance.animations = false;
    w.applySettings(st, RuntimeTuning{});
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.monitors.push_back(monitor(1, RectI{1920, 0, 3840, 1080}));
    l.windows.push_back(window(kWin, RectI{300, 600, 900, 1050}));   // covers part of the taskbar
    l.windows.push_back(window(kTaskbarId, RectI{0, kTaskbarTop, 1920, 1080}, WindowKind::Taskbar));
    l.windows.push_back(window(2, RectI{1920, kTaskbarTop, 3840, 1080}, WindowKind::Taskbar));
    w.setLayout(l);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    int batches = 0;
    for (const SpriteBatch& b : snap.objects) {
        int bulbs = 0, wires = 0;
        float lastX = -1e9f, minGap = 1e9f, maxGap = 0;
        for (const SpriteInstance& s : b.sprites) {
            if (s.shape == Shape::Bulb) {
                ++bulbs;
                if (lastX > -1e8f) {
                    minGap = std::min(minGap, s.x - lastX);
                    maxGap = std::max(maxGap, s.x - lastX);
                }
                lastX = s.x;
                CHECK(s.y + s.halfH <= static_cast<float>(kTaskbarTop) + 0.5f);   // above the edge
                CHECK(s.variant < 16);
            }
            wires += s.shape == Shape::GarlandWire ? 1 : 0;
        }
        if (!bulbs) continue;
        ++batches;
        CHECK(bulbs >= 70 && wires > 40);
        CHECK_NEAR(minGap, kGarlandBulbSpacingAt96, 0.01);
        CHECK_NEAR(maxGap, kGarlandBulbSpacingAt96, 0.01);   // nothing buried on a bare taskbar
        bool onLeft = false;
        for (const RectI& c : b.clip) {
            onLeft = onLeft || c.r <= 1920;
            for (const WindowInfo& wi : l.windows) CHECK(!c.intersects(wi.bounds));   // behind windows
        }
        for (const RectI& c : b.clip) CHECK(onLeft ? c.r <= 1920 : c.l >= 1920);  // its own monitor
    }
    CHECK(batches == 2);
    // Partially buried by a lumpy drift: bulbs inside the snow are not drawn.
    Surface* bar = surfaceOf(w, kTaskbarId);
    for (int i = 0; i < bar->hm.columns(); ++i) bar->hm.setHeight(i, (i / 20) % 2 ? 30.0f : 2.0f);
    w.buildSnapshot(snap);
    for (const SpriteBatch& b : snap.objects)
        for (const SpriteInstance& s : b.sprites)
            if (s.shape == Shape::Bulb && s.x < 1920)
                CHECK(s.y <= static_cast<float>(kTaskbarTop) - bar->hm.heightAtLocalX(s.x) + 1.0f);
    // Patterns.
    CHECK(garlandPattern(0) == 0 && garlandPattern(21) == 1 && garlandPattern(41) == 2 && garlandPattern(61) == 3 &&
          garlandPattern(81) == 0);
    int onChase = 0, onAlt = 0, moved = 0;
    for (int k = 0; k < 40; ++k) {
        onChase += garlandBulbOn(0, k, 1.0f, 7) ? 1 : 0;
        onAlt += garlandBulbOn(1, k, 1.0f, 7) ? 1 : 0;
        moved += garlandBulbOn(0, k, 1.0f, 7) != garlandBulbOn(0, k, 1.13f, 7) ? 1 : 0;
    }
    CHECK(onChase == 10 && onAlt == 20 && moved > 0);
    // Off in the settings.
    st.festive.garland = false;
    w.applySettings(st, RuntimeTuning{});
    w.buildSnapshot(snap);
    CHECK(count(snap, Shape::Bulb) == 0);
}

// ---------------------------------------------------------------- fireworks
TEST(fireworks_at_new_year_midnight_once_and_on_demand) {
    World w(8);
    Settings st = quiet();
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    w.setLocalTime(2026, 12, 31, 86398);
    w.step(1.0f / 60.0f);
    CHECK(!in.fireworks.active());
    w.setLocalTime(2027, 1, 1, 2);   // rolled over
    CHECK(in.fireworks.active() && in.fireworks.shows() == 1);
    CHECK(in.fireworks.showLeft() > 80.0f);
    w.setLocalTime(2027, 1, 1, 3);
    w.setLocalTime(2027, 1, 1, 30);
    CHECK(in.fireworks.shows() == 1);   // only once
    std::size_t maxSparks = 0;
    for (int k = 0; k < 100 * 60; ++k) {
        w.step(1.0f / 60.0f);
        maxSparks = std::max(maxSparks, in.fireworks.sparkCount());
    }
    printf("  fireworks: %d rockets, %d bursts, peak %zu sparks\n", in.fireworks.launched(), in.fireworks.bursts(),
           maxSparks);
    CHECK(!in.fireworks.active());
    CHECK(in.fireworks.launched() > 90 && in.fireworks.bursts() > 90);
    CHECK(maxSparks <= static_cast<std::size_t>(kMaxSparks) && maxSparks > 200);
    // Later the same day / a normal day: nothing.
    w.setLocalTime(2027, 1, 1, 3600);
    w.setLocalTime(2027, 6, 1, 10);
    CHECK(in.fireworks.shows() == 1);
    // Started during the first minute of a new year (app launched at 00:00:20).
    World late(9);
    late.applySettings(st, RuntimeTuning{});
    late.setLayout(basicLayout());
    late.setLocalTime(2028, 1, 1, 20);
    CHECK(WorldTestAccess::get(late).fireworks.shows() == 1);
    // Disabled: no midnight show; the manual trigger always works.
    World off(10);
    st.festive.fireworks = false;
    off.applySettings(st, RuntimeTuning{});
    off.setLayout(basicLayout());
    off.setLocalTime(2026, 12, 31, 86399);
    off.setLocalTime(2027, 1, 1, 1);
    auto oi = WorldTestAccess::get(off);
    CHECK(oi.fireworks.shows() == 0);
    off.triggerFireworks();
    CHECK(oi.fireworks.active() && oi.fireworks.showLeft() <= kManualShowSeconds);
    stepN(off, 120);
    FrameSnapshot snap;
    off.buildSnapshot(snap);
    CHECK(count(snap, Shape::Spark) > 0);
    for (const SpriteInstance& s : snap.particles.sprites)
        if (s.shape == Shape::Spark) CHECK(s.variant < 8 && s.x >= 0 && s.x < 1920);
}

// ---------------------------------------------------------------- splats
namespace {
// Rolls a ball on the taskbar drift and holds it at (x, y) after a few frames.
void makeBall(World& w, float x, float y) {
    input(w, T::PointerMove, 1500.0f, 1026.0f);
    input(w, T::PointerDown, 1500.0f, 1026.0f);
    for (float px = 1500.0f; px <= 1700.0f; px += 4.0f) input(w, T::PointerMove, px, 1026.0f);
    for (int k = 0; k < 20; ++k) {
        input(w, T::PointerMove, x, y);
        w.step(1.0f / 60.0f);
    }
}
}  // namespace

TEST(fast_throw_splats_on_the_window_slides_down_and_falls_off) {
    World w(11);
    Settings st = quiet();
    st.interaction.tool = Tool::Snowball;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    const RectI win{300, 200, 1500, 900};
    w.setLayout(withWindow(win));
    auto in = WorldTestAccess::get(w);
    fillToCap(w, *surfaceOf(w, kTaskbarId));
    w.setInteractionMode(true);
    makeBall(w, 1600.0f, 1000.0f);
    CHECK(in.objects.holding());
    const float ballMass = in.objects.heldBall()->collected;
    input(w, T::PointerMove, 1200.0f, 600.0f);   // a fast fling up-left
    input(w, T::PointerUp, 1200.0f, 600.0f);
    CHECK(in.objects.balls().empty());
    CHECK(in.splats.mainCount() == 1);
    if (in.splats.mainCount() != 1) return;
    const Splat sp = in.splats.splats()[0];
    CHECK(sp.window == kWin && sp.halfW > 0);
    CHECK_NEAR(in.splats.splatted(), ballMass, 1e-3);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    const SpriteBatch* b = batchWith(snap, Shape::Splat);
    CHECK(b != nullptr);
    if (b)
        for (const RectI& c : b->clip) CHECK(c.l >= win.l && c.r <= win.r && c.t >= win.t && c.b <= win.b);
    // Sticks, rides along with the window.
    for (int i = 1; i <= 10; ++i) {
        w.step(1.0f / 60.0f);
        w.setLayout(withWindow(RectI{300 + 2 * i, 200, 1500 + 2 * i, 900}));
    }
    CHECK_NEAR(in.splats.splats()[0].ly, sp.ly, 1e-3);
    w.buildSnapshot(snap);
    b = batchWith(snap, Shape::Splat);
    CHECK(b && std::fabs(b->sprites[0].x - (320.0f + sp.lx)) < 1e-3f);
    // Then slides down with a trail and falls off the bottom as a chunk.
    int steps = 0, maxBits = 0;
    while (in.splats.mainCount() > 0 && steps++ < 60 * 30) {
        w.step(1.0f / 60.0f);
        maxBits = std::max(maxBits, static_cast<int>(in.splats.splats().size()) - in.splats.mainCount());
    }
    CHECK(in.splats.mainCount() == 0 && maxBits >= 3);
    CHECK(in.chunks.shedChunkMass() > 0);
    CHECK_NEAR(in.splats.dropped() + in.splats.dust() + in.splats.liveMass(), in.splats.splatted(), 1e-2);
}

TEST(slow_throw_does_not_splat_and_splats_go_with_their_window) {
    World w(12);
    Settings st = quiet();
    st.interaction.tool = Tool::Snowball;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindow(RectI{300, 200, 1500, 900}));
    auto in = WorldTestAccess::get(w);
    fillToCap(w, *surfaceOf(w, kTaskbarId));
    w.setInteractionMode(true);
    makeBall(w, 1200.0f, 600.0f);
    input(w, T::PointerUp, 1200.0f, 600.0f);   // at rest
    CHECK(in.splats.mainCount() == 0 && in.objects.balls().size() == 1);
    stepN(w, 120);
    // A fast one, then the window is minimised: the splat goes with it.
    makeBall(w, 1600.0f, 1000.0f);
    input(w, T::PointerMove, 1000.0f, 500.0f);
    input(w, T::PointerUp, 1000.0f, 500.0f);
    CHECK(in.splats.mainCount() == 1);
    DesktopLayout hidden = basicLayout();
    hidden.hiddenWindows.push_back(kWin);
    w.setLayout(hidden);
    CHECK(in.splats.splats().empty());
    // ... and a jerk knocks one off as a chunk.
    w.setLayout(withWindow(RectI{300, 200, 1500, 900}));
    makeBall(w, 1600.0f, 1000.0f);
    input(w, T::PointerMove, 1000.0f, 500.0f);
    input(w, T::PointerUp, 1000.0f, 500.0f);
    CHECK(in.splats.mainCount() == 1);
    const double before = in.chunks.shedChunkMass();
    w.setLayout(withWindow(RectI{300, 200, 1500, 900}));   // the tracker reports continuously
    w.step(1.0f / 60.0f);
    w.setLayout(withWindow(RectI{360, 200, 1560, 900}));
    CHECK(in.splats.splats().empty() && in.chunks.shedChunkMass() > before);
}

// ---------------------------------------------------------------- snowman looks
TEST(snowman_looks_are_varied_deterministic_and_festive) {
    auto looks = [](std::uint64_t seed, bool season, int n, std::vector<SnowmanLook>& out) {
        World w(seed);
        w.setLocalTime(2026, season ? 12 : 6, season ? 28 : 15, 43200);
        for (int i = 0; i < n; ++i) out.push_back(WorldTestAccess::get(w).objects.rollLook());
    };
    std::vector<SnowmanLook> a, b, c;
    looks(5, false, 200, a);
    looks(5, false, 200, b);
    looks(5, true, 200, c);
    bool same = true;
    int hats = 0, scarves = 0, santaOff = 0, santaOn = 0, smiles = 0, waving = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        same = same && a[i].hat == b[i].hat && a[i].scarf == b[i].scarf && a[i].arms == b[i].arms && a[i].nose == b[i].nose;
        hats += (a[i].hat >= 0 || a[i].hatAcc != kNoPart) ? 1 : 0;
        scarves += a[i].scarf >= 0 ? 1 : 0;
        santaOff += a[i].hat == 3 ? 1 : 0;
        santaOn += c[i].hat == 3 ? 1 : 0;
        smiles += a[i].smile ? 1 : 0;
        waving += a[i].arms == 2 ? 1 : 0;
        CHECK(a[i].nose >= 0.35f && a[i].nose <= 0.7f);
    }
    printf("  looks (200): hats %d, scarves %d, smiles %d, waving %d, santa %d off-season / %d in season\n", hats, scarves,
           smiles, waving, santaOff, santaOn);
    CHECK(same);
    CHECK(hats > 130 && hats < 190);   // most skins and ~85 % of random looks wear something on the head
    CHECK(scarves > 80 && scarves < 140);
    CHECK(santaOn > 2 * santaOff && santaOn > 70);
    CHECK(smiles > 60 && waving > 40);

    // A built snowman wears its look; knocked off a window, hat and scarf fall separately.
    World w(13);
    Settings st = quiet();
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindow(RectI{300, 300, 900, 700}));
    auto in = WorldTestAccess::get(w);
    Pile& p = in.objects.addPile(*surfaceOf(w, kWin), 300.0f, 26.0f);
    p.r[p.n++] = 19.0f;
    p.r[p.n++] = 13.0f;
    p.look = in.objects.rollLook();
    p.look.hat = 1;
    p.look.hatAcc = kNoPart;
    p.look.scarf = 2;
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(count(snap, Shape::Hat) == 1 && count(snap, Shape::Scarf) == 1);
    w.step(1.0f / 60.0f);
    w.setLayout(withWindow(RectI{360, 300, 960, 700}));
    bool hat = false, scarf = false;
    for (const Debris& d : in.objects.debris()) {
        hat = hat || d.shape == Shape::Hat;
        scarf = scarf || d.shape == Shape::Scarf;
    }
    CHECK(hat && scarf);
}

TEST(benchmark_with_fireworks_and_umbrella) {
    World w(1);
    Settings st;
    st.weather.preset = WeatherPreset::Blizzard;
    st.weather.intensity = 100;
    st.performance.profile = PerfProfile::High;
    RuntimeTuning tuning;
    tuning.particleScale = 1.2f;
    w.applySettings(st, tuning);
    DesktopLayout l = basicLayout();
    for (int i = 0; i < 40; ++i) {
        const int x = 20 + i * 36, y = 60 + (i % 8) * 90;
        l.windows.push_back(window(static_cast<WindowId>(100 + i), RectI{x, y, x + 420, y + 300}));
    }
    w.setLayout(l);
    stepN(w, 2000);
    w.triggerFireworks();
    for (int k = 0; k < 600; ++k) {   // let the show build up
        w.setAmbientPointer(900.0f + 300.0f * std::sin(static_cast<float>(k) * 0.01f), 500, true);
        w.step(1.0f / 60.0f);
    }
    auto in = WorldTestAccess::get(w);
    const std::size_t sparks = in.fireworks.sparkCount();
    const int kSteps = 600;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < kSteps; ++k) {
        w.setAmbientPointer(900.0f + 300.0f * std::sin(static_cast<float>(k) * 0.01f), 500, true);
        w.step(1.0f / 60.0f);
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kSteps;
    FrameSnapshot snap;
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) w.buildSnapshot(snap);
    const double snapUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / 100;
    printf("  benchmark+fireworks+garland+umbrella: %zu particles (%zu sparks), step avg %.1f us, snapshot %.1f us\n",
           w.particleCount(), sparks, us, snapUs);
    CHECK(sparks > 100);
    CHECK(us < 1000.0);
}
