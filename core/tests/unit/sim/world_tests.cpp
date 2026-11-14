// World-level tests: surfaces, occlusion, particles, presets, lifecycle, benchmark.
#include <chrono>
#include <cstring>
#include <vector>

#include "sim_test_util.h"
#include "surfaces/geometry.h"

using namespace simtest;

namespace {
constexpr WindowId kWin = 10;
const RectI kWinRect{300, 400, 900, 800};

DesktopLayout layoutWithWindow(RectI r = kWinRect) {
    DesktopLayout l = basicLayout();
    l.windows.push_back(window(kWin, r));
    return l;
}

Settings staticSettings() {   // no falling snow: fully deterministic static scenes
    Settings s;
    s.performance.animations = false;
    s.accumulation.naturalCaps = false;
    s.accumulation.shakeOffOnMove = false;   // default is "Осыпать": keep drifts while moving
    return s;
}

struct ParticleStats {
    std::size_t count = 0;
    double meanVx = 0, meanVy = 0;
};
ParticleStats particleStats(World& w) {
    ParticleStats st;
    const auto& ps = WorldTestAccess::get(w).particles.particles();
    for (const Particle& p : ps) {
        st.meanVx += p.vx;
        st.meanVy += p.vy;
    }
    st.count = ps.size();
    if (st.count) {
        st.meanVx /= static_cast<double>(st.count);
        st.meanVy /= static_cast<double>(st.count);
    }
    return st;
}
}  // namespace

TEST(surfaces_are_built_from_the_layout) {
    World w;
    w.applySettings(Settings{}, RuntimeTuning{});
    DesktopLayout l = basicLayout();
    l.windows.push_back(window(kWin, kWinRect));
    WindowInfo maxed = window(11, RectI{0, 0, 1920, kTaskbarTop});
    maxed.maximized = true;
    l.windows.push_back(maxed);
    l.windows.push_back(window(12, RectI{1000, 200, 1300, 500}, WindowKind::Occluder));
    WindowInfo excluded = window(13, RectI{1400, 300, 1800, 700});
    excluded.snowAllowed = false;
    l.windows.push_back(excluded);
    l.windows.push_back(window(14, RectI{100, 1, 500, 300}));   // touches the monitor top
    w.setLayout(l);
    auto in = WorldTestAccess::get(w);
    CHECK(w.surfaceCount() == 2);   // taskbar + window 10; no screen-bottom under a taskbar
    CHECK(in.surfaces.findByWindow(kTaskbarId) && in.surfaces.findByWindow(kTaskbarId)->kind == SurfaceKind::Taskbar);
    CHECK(in.surfaces.findByWindow(kWin) && in.surfaces.findByWindow(kWin)->top == 400);
    CHECK(!in.surfaces.findByWindow(11) && !in.surfaces.findByWindow(12) && !in.surfaces.findByWindow(13));
    CHECK(!in.surfaces.findByWindow(14));

    // Without a taskbar the monitor's bottom edge becomes a surface.
    l.windows.erase(l.windows.begin());
    w.setLayout(l);
    CHECK(w.surfaceCount() == 2);
    CHECK(!in.surfaces.findByWindow(kTaskbarId));
    bool screen = false;
    for (const Surface& s : in.surfaces.surfaces()) screen = screen || (s.kind == SurfaceKind::Screen && s.top == 1080);
    CHECK(screen);

    // Settings switch surfaces off without restarting.
    Settings s;
    s.accumulation.onScreenBottom = false;
    s.accumulation.skipMaximized = false;
    w.applySettings(s, RuntimeTuning{});
    CHECK(in.surfaces.activeCount() == 2);            // window 10 + window 14 (1 px of room); screen now inactive
    CHECK(in.surfaces.findByWindow(14) != nullptr);
}

TEST(moving_a_window_keeps_the_identical_drift) {
    World w;
    w.applySettings(staticSettings(), RuntimeTuning{});
    w.setLayout(layoutWithWindow());
    Surface* s = surfaceOf(w, kWin);
    CHECK(s != nullptr);
    if (!s) return;
    for (int i = 0; i < s->hm.columns(); ++i) s->hm.setHeight(i, static_cast<float>((i * 7) % 13));
    const SurfaceId id = s->id;
    const double mass = w.totalSnowMass();
    CHECK(mass > 0);
    FrameSnapshot a, b;
    w.buildSnapshot(a);
    w.step(1.0f / 60.0f);   // (relaxes a little; snapshot again so both sides saw the same state)
    w.buildSnapshot(a);
    RectI moved = kWinRect;
    moved.l += 300;
    moved.r += 300;
    moved.t += 40;
    moved.b += 40;
    w.setLayout(layoutWithWindow(moved));
    w.buildSnapshot(b);
    const DriftStrip* da = driftOf(a, id);
    const DriftStrip* db = driftOf(b, id);
    CHECK(da && db);
    if (!da || !db) return;
    CHECK(db->x0 == da->x0 + 300.0f && db->baseY == da->baseY + 40.0f);
    CHECK(db->version == da->version);
    CHECK(da->heights.size() == db->heights.size());
    CHECK(da->heights == db->heights);
    CHECK(w.surfaceCount() == 2);
    for (const RectI& c : db->clip) CHECK(c.l >= 600 && c.r <= 1200 && c.b <= 440);
}

TEST(resize_hide_restore_close_leave_no_stale_surfaces) {
    World w;
    w.applySettings(staticSettings(), RuntimeTuning{});
    w.setLayout(layoutWithWindow());
    Surface* s = surfaceOf(w, kWin);
    CHECK(s != nullptr);
    if (!s) return;
    fill(w, *s, 12.0f);
    const SurfaceId id = s->id;
    const int cols = s->hm.columns();
    const float h10 = s->hm.height(10);
    FrameSnapshot snap;

    // Shrink: columns stay anchored to the left edge.
    w.setLayout(layoutWithWindow(RectI{300, 400, 600, 800}));
    s = surfaceOf(w, kWin);
    CHECK(s && s->id == id);
    CHECK(s && s->hm.columns() == 101 && s->hm.columns() < cols);
    CHECK(s && s->hm.height(10) == h10);
    // Grow again: new columns start empty.
    w.setLayout(layoutWithWindow());
    s = surfaceOf(w, kWin);
    CHECK(s && s->hm.columns() == cols && s->hm.height(10) == h10 && s->hm.height(cols - 5) == 0.0f);
    const double mass = w.totalSnowMass();

    // Minimised: state kept, nothing drawn.
    DesktopLayout hidden = basicLayout();
    hidden.hiddenWindows.push_back(kWin);
    w.setLayout(hidden);
    CHECK(w.surfaceCount() == 2);
    CHECK_NEAR(w.totalSnowMass(), mass, 1e-6);
    w.buildSnapshot(snap);
    CHECK(driftOf(snap, id) == nullptr);
    CHECK(snap.stats.surfaces == 1);
    stepN(w, 30);
    CHECK_NEAR(w.totalSnowMass(), mass, 1e-6);   // hidden surfaces are not simulated

    // Restored (at another place): same surface, same snow.
    w.setLayout(layoutWithWindow(RectI{700, 200, 1300, 600}));
    w.buildSnapshot(snap);
    const DriftStrip* d = driftOf(snap, id);
    CHECK(d && d->x0 == 700.0f && d->baseY == 200.0f);
    CHECK_NEAR(w.totalSnowMass(), mass, 1e-6);

    // Closed: gone.
    w.setLayout(basicLayout());
    CHECK(w.surfaceCount() == 1);
    CHECK(w.totalSnowMass() == 0.0);
    w.buildSnapshot(snap);
    CHECK(snap.drifts.size() == 1 && driftOf(snap, id) == nullptr);

    // A new window with the same rect is a new surface with no snow.
    DesktopLayout again = basicLayout();
    again.windows.push_back(window(77, kWinRect));
    w.setLayout(again);
    Surface* fresh = surfaceOf(w, 77);
    CHECK(fresh && fresh->id != id && fresh->hm.mass() == 0.0);
}

TEST(closing_a_window_drops_its_snow_as_flakes_when_enabled) {
    for (int pass = 0; pass < 2; ++pass) {
        World w;
        Settings st;
        st.accumulation.dropSnowOnClose = pass == 0;
        w.applySettings(st, RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(layoutWithWindow());
        Surface* s = surfaceOf(w, kWin);
        if (!s) {
            CHECK(s != nullptr);
            return;
        }
        fill(w, *s, 15.0f);
        CHECK(w.particleCount() == 0);
        w.setLayout(basicLayout());
        if (pass == 0) CHECK(WorldTestAccess::get(w).chunks.count() >= 2);   // a 15 px cap breaks into slabs
        else CHECK(w.particleCount() == 0);
    }
}

TEST(occluded_columns_get_no_snow_and_clip_excludes_the_occluder) {
    World w(123);
    Settings st;
    st.weather.preset = WeatherPreset::Blizzard;
    st.weather.intensity = 100;
    st.performance.profile = PerfProfile::High;
    w.applySettings(st, RuntimeTuning{});
    DesktopLayout l = basicLayout();
    const RectI front{500, 300, 800, 700};
    const RectI back{200, 500, 1000, 900};
    l.windows.push_back(window(21, front));   // in front of ...
    l.windows.push_back(window(20, back));    // ... this one
    w.setLayout(l);
    auto in = WorldTestAccess::get(w);
    const Surface* sb = in.surfaces.findByWindow(20);
    const Surface* sf = in.surfaces.findByWindow(21);
    CHECK(sb && sf);
    if (!sb || !sf) return;

    // Clip: the strip above the back window minus the front window.
    CHECK(!sb->clip.empty());
    long long area = 0;
    for (const RectI& c : sb->clip) {
        CHECK(!c.intersects(front));
        CHECK(c.b <= 500 && c.l >= 200 && c.r <= 1000);
        area += static_cast<long long>(c.width()) * c.height();
    }
    CHECK(area == static_cast<long long>(800 - 300) * 38);   // maxHeight 36 + 2 px headroom
    CHECK(sf->clip.size() == 1);

    // Sweeps: under the front window the back surface is transparent.
    SurfaceHit hit;
    CHECK(in.surfaces.sweep(650.0f, 0.0f, 650.0f, 480.0f, true, hit));
    CHECK(in.surfaces.surfaces()[static_cast<std::size_t>(hit.surfaceIndex)].id == sf->id && hit.y == 300.0f);
    CHECK(!in.surfaces.sweep(650.0f, 350.0f, 650.0f, 600.0f, true, hit));   // starts below the front edge
    CHECK(in.surfaces.sweep(250.0f, 350.0f, 250.0f, 600.0f, true, hit));
    CHECK(in.surfaces.surfaces()[static_cast<std::size_t>(hit.surfaceIndex)].id == sb->id && hit.y == 500.0f);

    stepN(w, 4000);
    sb = in.surfaces.findByWindow(20);
    double open = 0, covered = 0;
    for (int i = 0; i < sb->hm.columns(); ++i) {
        const float x = sb->worldX(i);
        if (x >= 500.0f && x < 800.0f) covered += sb->hm.height(i);
        else open += sb->hm.height(i);
    }
    CHECK(open > 20.0);
    CHECK(covered == 0.0);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    const DriftStrip* d = driftOf(snap, sb->id);
    CHECK(d != nullptr);
    if (d)
        for (const RectI& c : d->clip) CHECK(!c.intersects(front));
}

TEST(very_fast_flake_does_not_tunnel) {
    World w;
    w.applySettings(Settings{}, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(layoutWithWindow());
    auto in = WorldTestAccess::get(w);
    SurfaceHit hit;
    CHECK(in.surfaces.sweep(600.0f, 0.0f, 600.0f, 1.0e6f, true, hit));
    CHECK(hit.y == 400.0f);
    CHECK(in.surfaces.sweep(100.0f, 0.0f, 2000.0f, 1900.0f, true, hit));   // diagonal: crosses y=400 at x=500
    CHECK(hit.y == 400.0f && std::fabs(hit.x - 500.0f) < 1.0f);

    for (int k = 0; k < 2; ++k) {
        Particle* p = in.particles.spawnAmbient(in.weather);
        CHECK(p != nullptr);
        if (!p) return;
        p->x = k == 0 ? 600.0f : 1500.0f;   // above the window / above the taskbar only
        p->y = 100.0f;
        p->vx = 0;
        p->vy = 1.0e6f;                     // ~40000 px in one step
        p->flags = Particle::kCollidesWindows;
        p->swayAmp = 0;
    }
    CHECK(w.totalSnowMass() == 0.0);
    w.step(1.0f / 20.0f);
    CHECK(w.particleCount() == 0);
    const Surface* win = in.surfaces.findByWindow(kWin);
    const Surface* bar = in.surfaces.findByWindow(kTaskbarId);
    CHECK(win && win->hm.mass() > 0.0);
    CHECK(bar && bar->hm.mass() > 0.0);
    CHECK(in.particles.landedCount() == 2);
}

TEST(same_seed_gives_identical_state) {
    auto run = [](std::uint64_t seed, std::vector<Particle>& out, double& mass) {
        World w(seed);
        Settings st;
        st.weather.wind = 35;
        st.weather.gustiness = 60;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(layoutWithWindow());
        stepN(w, 400);
        w.setLayout(layoutWithWindow(RectI{350, 420, 950, 820}));
        stepN(w, 400);
        out = WorldTestAccess::get(w).particles.particles();
        mass = w.totalSnowMass();
    };
    std::vector<Particle> a, b, c;
    double ma = 0, mb = 0, mc = 0;
    run(99, a, ma);
    run(99, b, mb);
    run(100, c, mc);
    CHECK(a.size() > 100);
    CHECK(a.size() == b.size());
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i)
        same = a[i].id == b[i].id && a[i].x == b[i].x && a[i].y == b[i].y && a[i].vx == b[i].vx && a[i].vy == b[i].vy &&
               a[i].radius == b[i].radius && a[i].rotation == b[i].rotation && a[i].variant == b[i].variant;
    CHECK(same);
    CHECK(ma == mb);
    bool differs = a.size() != c.size();
    for (std::size_t i = 0; !differs && i < a.size(); ++i) differs = a[i].x != c[i].x;
    CHECK(differs);
}

TEST(presets_are_clearly_different) {
    std::size_t count[3]{};
    double fall[3]{}, mass[3]{}, speedSpread[3]{};
    for (int p = 0; p < 3; ++p) {
        World w(5);
        Settings st;
        st.weather.preset = static_cast<WeatherPreset>(p);
        st.weather.wind = 0;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(basicLayout());
        stepN(w, 3000);
        const ParticleStats ps = particleStats(w);
        count[p] = ps.count;
        fall[p] = ps.meanVy;
        mass[p] = w.totalSnowMass();
        double var = 0;
        for (const Particle& q : WorldTestAccess::get(w).particles.particles()) var += static_cast<double>(q.vx) * q.vx;
        speedSpread[p] = std::sqrt(var / static_cast<double>(std::max<std::size_t>(ps.count, 1)));
    }
    printf("  presets: count %zu/%zu/%zu  fall %.0f/%.0f/%.0f px/s  drift %.0f/%.0f/%.0f px^2  vx-rms %.1f/%.1f/%.1f\n",
           count[0], count[1], count[2], fall[0], fall[1], fall[2], mass[0], mass[1], mass[2], speedSpread[0],
           speedSpread[1], speedSpread[2]);
    for (int p = 0; p < 2; ++p) {
        CHECK(static_cast<double>(count[p + 1]) > 1.3 * static_cast<double>(count[p]));
        CHECK(fall[p + 1] > 1.3 * fall[p]);
        CHECK(mass[p + 1] > 1.5 * mass[p]);
        CHECK(speedSpread[p + 1] > 1.1 * speedSpread[p]);
    }
    CHECK(count[0] > 50);
}

TEST(wind_sign_moves_flakes_the_right_way_and_sliders_apply) {
    double vx[2]{};
    for (int k = 0; k < 2; ++k) {
        World w(3);
        Settings st;
        st.weather.wind = k == 0 ? 80 : -80;
        st.weather.gustiness = 0;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(basicLayout());
        stepN(w, 900);
        vx[k] = particleStats(w).meanVx;
        // Upwind side spawning: flakes exist near the upwind edge well below the top.
        int nearEdge = 0;
        for (const Particle& p : WorldTestAccess::get(w).particles.particles())
            if (p.y > 700.0f && (k == 0 ? p.x < 300.0f : p.x > 1620.0f)) ++nearEdge;
        CHECK(nearEdge > 0);
    }
    CHECK(vx[0] > 60.0);
    CHECK(vx[1] < -60.0);

    // Fall speed and size sliders.
    double vy[2]{}, radius[2]{};
    for (int k = 0; k < 2; ++k) {
        World w(3);
        Settings st;
        st.weather.fallSpeedPercent = k == 0 ? 50 : 200;
        st.weather.sizePercent = k == 0 ? 50 : 200;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(basicLayout());
        stepN(w, 600);
        vy[k] = particleStats(w).meanVy;
        const auto& ps = WorldTestAccess::get(w).particles.particles();
        for (const Particle& p : ps) radius[k] += p.radius;
        radius[k] /= static_cast<double>(std::max<std::size_t>(ps.size(), 1));
    }
    CHECK(vy[1] > 3.0 * vy[0]);
    CHECK(radius[1] > 3.0 * radius[0]);
}

TEST(particle_budget_follows_profile_area_and_tuning) {
    Settings st;
    st.weather.intensity = 100;
    const double area = 1920.0 * 1080.0;
    st.performance.profile = PerfProfile::Low;
    const int low = targetParticleCount(st, RuntimeTuning{}, area);
    st.performance.profile = PerfProfile::Balanced;
    const int mid = targetParticleCount(st, RuntimeTuning{}, area);
    st.performance.profile = PerfProfile::High;
    const int high = targetParticleCount(st, RuntimeTuning{}, area);
    CHECK(low < mid && mid < high);
    CHECK(mid == 900);   // 1200 * 0.75 (Normal) at 100 %
    CHECK(targetParticleCount(st, RuntimeTuning{}, 2 * area) == 2 * high);
    RuntimeTuning half;
    half.particleScale = 0.5f;
    CHECK(targetParticleCount(st, half, area) == high / 2);
    CHECK(targetParticleCount(st, RuntimeTuning{}, 1000 * area) == kMaxParticles);
    RuntimeTuning off;
    off.animationsEnabled = false;
    CHECK(targetParticleCount(st, off, area) == 0);
    st.weather.intensity = 0;
    CHECK(targetParticleCount(st, RuntimeTuning{}, area) == 0);
}

TEST(animations_off_and_emission_off) {
    World w;
    w.applySettings(Settings{}, RuntimeTuning{});
    w.setLayout(basicLayout());
    stepN(w, 300);
    CHECK(w.particleCount() > 50);
    const std::size_t before = w.particleCount();

    w.setEmissionEnabled(false);          // existing flakes keep falling, no new ones
    stepN(w, 60);
    CHECK(w.particleCount() <= before);
    stepN(w, 3000);
    CHECK(w.particleCount() == 0);
    w.setEmissionEnabled(true);
    stepN(w, 300);
    CHECK(w.particleCount() > 50);

    const double mass = w.totalSnowMass();
    RuntimeTuning off;
    off.animationsEnabled = false;
    w.applySettings(Settings{}, off);     // governor switches animations off
    CHECK(w.particleCount() == 0);
    stepN(w, 120);
    CHECK(w.particleCount() == 0);
    CHECK_NEAR(w.totalSnowMass(), mass, 0.05);   // drifts stay (they may still settle a little)
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(snap.particles.sprites.empty() && !snap.drifts.empty());

    Settings st;
    st.performance.animations = false;    // same through the user setting
    w.applySettings(st, RuntimeTuning{});
    stepN(w, 120);
    CHECK(w.particleCount() == 0);
}

TEST(accumulation_settings_are_respected) {
    {   // disabled: flakes pass through
        World w;
        Settings st;
        st.weather.preset = WeatherPreset::Blizzard;
        st.accumulation.enabled = false;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(layoutWithWindow());
        stepN(w, 1500);
        CHECK(w.particleCount() > 100);
        CHECK(w.totalSnowMass() == 0.0);
    }
    {   // max height 0: surfaces exist but never grow
        World w;
        Settings st;
        st.weather.preset = WeatherPreset::Blizzard;
        st.accumulation.maxHeightPxAt96Dpi = 0;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(layoutWithWindow());
        stepN(w, 1500);
        CHECK(w.totalSnowMass() == 0.0);
    }
    {   // low limit: drifts grow up to it and no further
        World w;
        Settings st;
        st.weather.preset = WeatherPreset::Blizzard;
        st.weather.intensity = 100;
        st.performance.profile = PerfProfile::High;
        st.accumulation.maxHeightPxAt96Dpi = 2;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(layoutWithWindow());
        double prev = -1;
        for (int k = 0; k < 6; ++k) {
            stepN(w, 1500);
            CHECK(w.totalSnowMass() >= prev);   // (window ends are open, tiny losses are negligible here)
            prev = w.totalSnowMass();
        }
        CHECK(prev > 100.0);
        FrameSnapshot snap;
        w.buildSnapshot(snap);
        CHECK(snap.drifts.size() == 2);
        float peak = 0;
        for (const DriftStrip& d : snap.drifts)
            for (float h : d.heights) peak = std::max(peak, h);
        CHECK(peak <= 2.0f + 1e-3f);
        CHECK(peak > 1.0f);
    }
}

TEST(shake_off_on_fast_move_only_when_enabled) {
    for (int pass = 0; pass < 2; ++pass) {
        World w;
        Settings st = staticSettings();
        st.accumulation.shakeOffOnMove = pass == 1;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(layoutWithWindow());
        Surface* s = surfaceOf(w, kWin);
        if (!s) {
            CHECK(s != nullptr);
            return;
        }
        fill(w, *s, 3.0f);   // gentle profile: nothing avalanches
        w.step(1.0f / 60.0f);
        const double mass = w.totalSnowMass();
        w.setLayout(layoutWithWindow(RectI{400, 400, 1000, 800}));   // 100 px in one step = 6000 px/s
        if (pass == 0) CHECK_NEAR(w.totalSnowMass(), mass, 1e-6);
        else CHECK(w.totalSnowMass() < 0.85 * mass);
        // A slow drag never sheds anything.
        const double after = w.totalSnowMass();
        for (int k = 1; k <= 20; ++k) {
            w.step(1.0f / 60.0f);
            w.setLayout(layoutWithWindow(RectI{400 + 4 * k, 400, 1000 + 4 * k, 800}));
        }
        CHECK(w.totalSnowMass() > 0.98 * after);
    }
}

TEST(two_monitors_negative_coordinates_and_mixed_dpi) {
    World w(11);
    w.applySettings(Settings{}, RuntimeTuning{});
    DesktopLayout l;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.monitors.push_back(monitor(1, RectI{-2560, -200, 0, 1240}, 1.5f));
    l.windows.push_back(window(30, RectI{-400, 300, 400, 700}));                        // spans both monitors
    l.windows.push_back(window(31, RectI{-2000, 100, -1000, 600}, WindowKind::App, 1.5f));
    w.setLayout(l);
    auto in = WorldTestAccess::get(w);
    CHECK(w.surfaceCount() == 4);   // 2 windows + 2 screen bottoms: ONE surface for the spanning window
    const Surface* span = in.surfaces.findByWindow(30);
    const Surface* hi = in.surfaces.findByWindow(31);
    CHECK(span && hi);
    if (!span || !hi) return;
    CHECK(span->clip.size() == 2);   // split per monitor
    CHECK(totalArea(span->clip) == 800LL * 38);
    CHECK(hi->hm.step() == 5.0f && span->hm.step() == 3.0f);       // 3 px * 1.5 -> 5
    CHECK_NEAR(hi->hm.maxHeight(), 54.0f, 1e-3);                   // 36 px at 96 DPI * 1.5
    CHECK(in.surfaces.sceneBounds() == (RectI{-2560, -200, 1920, 1240}));
    CHECK(in.surfaces.dpiScaleAt(-100.0f, 500.0f) == 1.5f && in.surfaces.dpiScaleAt(100.0f, 500.0f) == 1.0f);

    stepN(w, 1500);
    int left = 0, right = 0;
    for (const Particle& p : in.particles.particles()) (p.x < 0 ? left : right)++;
    CHECK(left > 20 && right > 20);
    // Moving the hi-DPI window onto the 100 % monitor rescales its drift instead of dropping it.
    Surface* s = surfaceOf(w, 31);
    if (!s) return;
    fill(w, *s, 30.0f);
    const SurfaceId id = s->id;
    l.windows[1] = window(31, RectI{500, 100, 1167, 433});   // 1000x500 @150 % -> 667x333 @100 %
    w.setLayout(l);
    s = surfaceOf(w, 31);
    CHECK(s && s->id == id && s->hm.step() == 3.0f);
    CHECK(s && std::fabs(s->hm.height(s->hm.columns() / 2) - 20.0f) < 0.5f);
}

TEST(step_clamps_dt) {
    World a(8), b(8);
    for (World* w : {&a, &b}) {
        w->applySettings(Settings{}, RuntimeTuning{});
        w->setLayout(basicLayout());
        stepN(*w, 200);
    }
    a.step(1.0f / 20.0f);
    b.step(3600.0f);    // "woke up from sleep": must not simulate the missed hour
    b.step(-5.0f);      // negative / NaN are treated as 0
    a.step(0.0f);
    const auto& pa = WorldTestAccess::get(a).particles.particles();
    const auto& pb = WorldTestAccess::get(b).particles.particles();
    CHECK(pa.size() == pb.size());
    bool same = pa.size() == pb.size();
    for (std::size_t i = 0; same && i < pa.size(); ++i) same = pa[i].x == pb[i].x && pa[i].y == pb[i].y;
    CHECK(same);
}

TEST(snapshot_is_complete_and_reusable) {
    World w(2);
    Settings st;
    st.festive.garland = false;
    w.applySettings(st, RuntimeTuning{});
    w.setLayout(layoutWithWindow());
    stepN(w, 600);
    FrameSnapshot snap;
    snap.frame = 77;
    w.buildSnapshot(snap);
    CHECK(snap.frame == 77);   // left to the caller
    CHECK(snap.drifts.size() == 2);
    // Every flake except those still entering their monitor through its top edge (sky per
    // monitor: invisible until fully inside, see ParticleSystem::edgeFade).
    CHECK(snap.particles.sprites.size() <= w.particleCount() && snap.particles.sprites.size() * 10 >= w.particleCount() * 9 &&
          snap.particles.clip.empty());
    CHECK(snap.stats.particles == w.particleCount() && snap.stats.surfaces == 2);
    CHECK(snap.hitRegions.empty() && snap.cursor.sprites.empty() && snap.objects.empty());
    bool variants[kFlakeVariants]{};
    for (const SpriteInstance& s : snap.particles.sprites) {
        CHECK(s.shape == Shape::Flake && s.variant < kFlakeVariants && s.opacity > 0 && s.opacity <= 1 && s.halfW > 0);
        variants[s.variant] = true;
    }
    for (bool v : variants) CHECK(v);
    for (const DriftStrip& d : snap.drifts) {
        CHECK(!d.clip.empty() && !d.heights.empty() && d.step == 3.0f && d.id != 0);
    }
    // Version only moves when heights change.
    const std::uint32_t v0 = snap.drifts[0].version;
    stepN(w, 600);
    const float* data = snap.drifts[0].heights.data();
    w.buildSnapshot(snap);
    CHECK(snap.drifts[0].version != v0);
    CHECK(snap.drifts[0].heights.data() == data);   // capacity reused, no reallocation
}

TEST(benchmark_3000_flakes_40_surfaces) {
    World w(1);
    Settings st;
    st.weather.preset = WeatherPreset::Blizzard;
    st.weather.intensity = 100;
    st.performance.profile = PerfProfile::High;
    RuntimeTuning tuning;
    tuning.particleScale = 1.2f;   // 2500 * 1.2 (Blizzard) * 1.2 = 3600 budget -> ~3000+ alive
    w.applySettings(st, tuning);
    DesktopLayout l = basicLayout();
    for (int i = 0; i < 40; ++i) {
        const int x = 20 + i * 36, y = 60 + (i % 8) * 90;
        l.windows.push_back(window(static_cast<WindowId>(100 + i), RectI{x, y, x + 420, y + 300}));
    }
    w.setLayout(l);
    stepN(w, 2000);   // reach the steady state
    const int kSteps = 3000;
    const auto t0 = std::chrono::steady_clock::now();
    stepN(w, kSteps);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kSteps;
    FrameSnapshot snap;
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) w.buildSnapshot(snap);
    const double snapUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / 200;
    printf("  benchmark: %zu flakes, %zu surfaces: step avg %.1f us, buildSnapshot avg %.1f us (stats.stepMs %.4f)\n",
           w.particleCount(), w.surfaceCount(), us, snapUs, static_cast<double>(snap.stats.stepMs));
    CHECK(w.surfaceCount() == 41);
    CHECK(w.particleCount() >= 3000);
    CHECK(us < 1000.0);
    CHECK(w.totalSnowMass() > 0);
}
