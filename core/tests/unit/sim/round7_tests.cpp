// Round 7: mitten sculpting, shovel scoop & fling, snowman skins, excluded applications.
#include <vector>

#include "core/skins.h"
#include "sim_test_util.h"
#include "surfaces/geometry.h"

using namespace simtest;
using T = InputEvent::Type;

namespace {
constexpr WindowId kWin = 10;
constexpr float kPi = 3.14159265f;
constexpr int kSculpt = 3, kBrush = 2, kScoop = 4;   // Impl::Gesture values

Settings quiet(Tool tool) {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = false;
    s.accumulation.shakeToClear = false;
    s.accumulation.shakeOffOnMove = false;
    s.accumulation.naturalCaps = false;
    s.festive.garland = false;
    s.interaction.tool = tool;
    return s;
}

void fillBar(World& w, float h, float maxX = 1e9f) {
    Surface* bar = surfaceOf(w, kTaskbarId);
    for (int i = 0; i < bar->hm.columns(); ++i) bar->hm.setHeight(i, bar->worldX(i) < maxX ? h : 0.0f);
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}

double barMass(World& w, float x0 = -1e9f, float x1 = 1e9f) {
    const Surface* bar = surfaceOf(w, kTaskbarId);
    double m = 0;
    for (int i = 0; i < bar->hm.columns(); ++i)
        if (bar->worldX(i) >= x0 && bar->worldX(i) < x1) m += bar->hm.height(i) * bar->hm.step();
    return m;
}

double pileMass(const Pile& p) {
    double m = 0;
    for (int i = 0; i < p.n; ++i) m += kPi * p.r[i] * p.r[i] / kPackFactor;
    return m;
}
}  // namespace

// ---------------------------------------------------------------- mitten: sculpting
TEST(mitten_hold_sculpts_a_snowman_stage_by_stage_from_the_drift) {
    World w(1);
    w.applySettings(quiet(Tool::Mitten), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    fillBar(w, 30.0f);
    const double m0 = barMass(w);
    w.setInteractionMode(true);
    input(w, T::PointerMove, 800.0f, 1000.0f);
    input(w, T::PointerDown, 800.0f, 1000.0f);
    stepN(w, 12);   // 0.2 s still: it is a sculpting press
    CHECK(WorldTestAccess::get(w).gesture == kSculpt);
    CHECK(in.objects.sculpt().active && in.objects.piles().size() == 1);
    int steps = 0, stage2At = -1;
    while (in.objects.sculpt().completed == 0 && steps < 1200) {
        w.step(1.0f / 60.0f);
        ++steps;
        if (stage2At < 0 && !in.objects.piles().empty() && in.objects.piles()[0].n == 2) stage2At = steps;
    }
    printf("  sculpt: middle ball after %.2f s, done after %.2f s (no rubbing)\n", stage2At / 60.0, steps / 60.0);
    CHECK(in.objects.sculpt().completed == 1);
    CHECK(in.objects.piles().size() == 1);
    if (in.objects.piles().size() != 1) return;
    const Pile& p = in.objects.piles()[0];
    CHECK(p.n == 3 && p.look.set);
    CHECK(p.r[0] >= kSculptBaseRadiusAt96 && p.r[0] < kSculptBaseRadiusAt96 * 1.1f);
    CHECK(p.r[1] >= kSculptBaseRadiusAt96 * kSculptMiddleShare && p.r[2] >= kSculptBaseRadiusAt96 * kSculptHeadShare);
    CHECK_NEAR(m0 - barMass(w), pileMass(p), pileMass(p) * 0.01);   // no snow from nothing
    input(w, T::PointerUp, 800.0f, 1000.0f);
    CHECK(in.objects.snowmanCount() == 1 && WorldTestAccess::get(w).gesture == 0);
    const int baseline = steps;

    // Rubbing back and forth speeds it up.
    World r(1);
    r.applySettings(quiet(Tool::Mitten), RuntimeTuning{});
    r.setEmissionEnabled(false);
    r.setLayout(basicLayout());
    fillBar(r, 30.0f);
    r.setInteractionMode(true);
    input(r, T::PointerMove, 800.0f, 1000.0f);
    input(r, T::PointerDown, 800.0f, 1000.0f);
    stepN(r, 12);
    auto ri = WorldTestAccess::get(r);
    steps = 0;
    while (ri.objects.sculpt().completed == 0 && steps < 1200) {
        input(r, T::PointerMove, (steps & 1) ? 790.0f : 810.0f, 1000.0f);
        r.step(1.0f / 60.0f);
        ++steps;
    }
    printf("  sculpt with rubbing: done after %.2f s\n", steps / 60.0);
    CHECK(ri.objects.sculpt().completed == 1 && steps < baseline * 0.6);
}

TEST(mitten_release_early_leaves_the_stage_reached_and_needs_snow) {
    World w(2);
    w.applySettings(quiet(Tool::Mitten), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    fillBar(w, 30.0f);
    w.setInteractionMode(true);
    input(w, T::PointerMove, 600.0f, 1000.0f);
    input(w, T::PointerDown, 600.0f, 1000.0f);
    stepN(w, 60);   // 1 s: base only
    input(w, T::PointerUp, 600.0f, 1000.0f);
    CHECK(in.objects.piles().size() == 1 && in.objects.piles()[0].n == 1 && in.objects.piles()[0].r[0] > 6.0f);
    CHECK(in.objects.snowmanCount() == 0);
    // A 2-ball snowman when released during the head stage is dressed too.
    input(w, T::PointerMove, 1200.0f, 1000.0f);
    input(w, T::PointerDown, 1200.0f, 1000.0f);
    int guard = 0;
    while (guard++ < 1200) {
        w.step(1.0f / 60.0f);
        const Pile& q = in.objects.piles().back();
        if (q.n == 3 && q.r[2] > 6.0f) break;
    }
    input(w, T::PointerUp, 1200.0f, 1000.0f);
    CHECK(in.objects.snowmanCount() == 1 && in.objects.piles().back().look.set);
    // Thin snow: growth stalls, it can't make more than the drift around it holds.
    World t(3);
    t.applySettings(quiet(Tool::Mitten), RuntimeTuning{});
    t.setEmissionEnabled(false);
    t.setLayout(basicLayout());
    fillBar(t, 1.5f);
    auto ti = WorldTestAccess::get(t);
    const double near = barMass(t, 800.0f - 70.0f, 800.0f + 70.0f);
    t.setInteractionMode(true);
    input(t, T::PointerMove, 800.0f, 1028.0f);
    input(t, T::PointerDown, 800.0f, 1028.0f);
    stepN(t, 600);
    CHECK(ti.objects.piles().size() == 1 && ti.objects.piles()[0].n == 1);
    CHECK(ti.objects.sculpt().area[0] <= near + 1.0);
}

TEST(mitten_fast_swipe_still_brushes_and_slow_drag_sculpts) {
    World w(4);
    w.applySettings(quiet(Tool::Mitten), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    fillBar(w, 20.0f);
    w.setInteractionMode(true);
    const double m0 = barMass(w);
    input(w, T::PointerMove, 500.0f, 1022.0f);
    input(w, T::PointerDown, 500.0f, 1022.0f);
    input(w, T::PointerMove, 700.0f, 1022.0f);   // 200 px in a frame: a swipe
    CHECK(WorldTestAccess::get(w).gesture == kBrush);
    CHECK(barMass(w, 520.0f, 680.0f) < 1.0);
    input(w, T::PointerUp, 700.0f, 1022.0f);
    CHECK(in.objects.piles().empty() && barMass(w) < m0);
    // Slow drag (25+ px over half a second): sculpting.
    input(w, T::PointerMove, 1200.0f, 1022.0f);
    input(w, T::PointerDown, 1200.0f, 1022.0f);
    for (int k = 0; k < 12; ++k) {   // past the 0.15 s decision window
        input(w, T::PointerMove, 1200.0f + 1.0f * static_cast<float>(k), 1022.0f);
        w.step(1.0f / 60.0f);
    }
    CHECK(WorldTestAccess::get(w).gesture == kSculpt);
    input(w, T::PointerUp, 1211.0f, 1022.0f);
}

// ---------------------------------------------------------------- shovel: scoop & fling
TEST(shovel_scoops_up_to_capacity_and_shows_the_load) {
    World w(5);
    w.applySettings(quiet(Tool::Shovel), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    fillBar(w, 30.0f);
    w.setInteractionMode(true);
    const double m0 = barMass(w);
    input(w, T::PointerMove, 300.0f, 1020.0f);
    input(w, T::PointerDown, 300.0f, 1020.0f);
    CHECK(WorldTestAccess::get(w).gesture == kScoop);
    for (int k = 1; k <= 30; ++k) {
        input(w, T::PointerMove, 300.0f + 10.0f * static_cast<float>(k), 1020.0f);
        w.step(1.0f / 60.0f);
    }
    const float cap = 96.0f * kScoopDepthAt96;
    CHECK_NEAR(WorldTestAccess::get(w).scoop, cap, 1.0);
    CHECK(m0 - barMass(w) > 1.5 * cap);   // wide clearing still works once it is full
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    bool heap = false, ring = false;
    for (const SpriteInstance& s : snap.cursor.sprites) {
        heap = heap || s.shape == Shape::SnowChunk;
        ring = ring || s.shape == Shape::Ring;
    }
    CHECK(heap && !ring);   // the load rides on the blade; no ring during a stroke
    // Held still, then let go: no fling, the load is pushed aside (old clear).
    for (int k = 0; k < 20; ++k) w.step(1.0f / 60.0f);
    input(w, T::PointerUp, 600.0f, 1020.0f);
    CHECK(WorldTestAccess::get(w).chunks.shedChunkMass() == 0.0 && WorldTestAccess::get(w).scoop == 0.0f);
}

TEST(shovel_fling_flies_in_an_arc_and_lands_as_a_mound) {
    World w(6);
    w.applySettings(quiet(Tool::Shovel), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    fillBar(w, 30.0f, 700.0f);   // snow on the left only
    w.setInteractionMode(true);
    input(w, T::PointerMove, 300.0f, 1020.0f);
    input(w, T::PointerDown, 300.0f, 1020.0f);
    for (int k = 1; k <= 12; ++k) {
        input(w, T::PointerMove, 300.0f + 8.0f * static_cast<float>(k), 1015.0f);
        w.step(1.0f / 60.0f);
    }
    const float load = WorldTestAccess::get(w).scoop;
    CHECK(load > 1000.0f);
    input(w, T::PointerMove, 420.0f, 1000.0f);   // flick up-right
    input(w, T::PointerUp, 420.0f, 1000.0f);
    CHECK(in.chunks.count() >= 2);
    float minVy = 0;
    for (const Chunk& c : in.chunks.chunks()) {
        CHECK(c.soft && c.vx > 0);
        minVy = std::min(minVy, c.vy);
    }
    CHECK(minVy < 0);   // thrown upwards: an arc
    float peak = 1e9f;
    int guard = 0;
    while (in.chunks.count() > 0 && guard++ < 600) {
        w.step(1.0f / 60.0f);
        for (const Chunk& c : in.chunks.chunks()) peak = std::min(peak, c.y);
    }
    CHECK(in.chunks.count() == 0 && peak < 990.0f);
    const double landed = barMass(w, 780.0f, 1920.0f);   // (the left drift's edge relaxes a little past 700)
    printf("  shovel fling: load %.0f px^2 -> %.0f px^2 mound beyond x=700 (deposited %.0f)\n", load, landed,
           in.chunks.deposited());
    CHECK(landed > 0.8 * load);
    CHECK(landed <= in.chunks.deposited() + 1.0);
}

TEST(shovel_fling_at_a_window_splats) {
    World w(7);
    w.applySettings(quiet(Tool::Shovel), RuntimeTuning{});
    w.setEmissionEnabled(false);
    DesktopLayout l = basicLayout();
    l.windows.insert(l.windows.begin(), window(kWin, RectI{300, 200, 1500, 900}));
    w.setLayout(l);
    auto in = WorldTestAccess::get(w);
    fillBar(w, 30.0f);
    w.setInteractionMode(true);
    input(w, T::PointerMove, 1600.0f, 1020.0f);
    input(w, T::PointerDown, 1600.0f, 1020.0f);
    for (int k = 1; k <= 10; ++k) {
        input(w, T::PointerMove, 1600.0f + 8.0f * static_cast<float>(k), 1015.0f);
        w.step(1.0f / 60.0f);
    }
    input(w, T::PointerMove, 1550.0f, 940.0f);   // fast flick up-left, at the window
    input(w, T::PointerUp, 1550.0f, 940.0f);
    CHECK(in.splats.mainCount() == 1);
}

// ---------------------------------------------------------------- skins
TEST(every_skin_dresses_the_snowman_with_its_parts_at_their_anchors) {
    for (int si = 0; si < kSnowmanSkinCount; ++si) {
        const SnowmanSkin& skin = kSnowmanSkins[si];
        World w(8);
        Settings st = quiet(Tool::Snowball);
        st.interaction.snowmanSkin = skin.id;
        w.applySettings(st, RuntimeTuning{});
        w.setLayout(basicLayout());
        auto in = WorldTestAccess::get(w);
        Pile& p = in.objects.addPile(*surfaceOf(w, kTaskbarId), 800.0f, 30.0f);
        p.r[p.n++] = 22.0f;
        p.r[p.n++] = 22.0f * 0.7f;
        p.look = in.objects.rollLook();
        CHECK(p.look.skin == si);
        FrameSnapshot snap;
        w.buildSnapshot(snap);
        if (snap.objects.empty()) {
            CHECK(!snap.objects.empty());
            continue;
        }
        const std::vector<SpriteInstance>& sp = snap.objects[0].sprites;
        // Head = the third snowball.
        float hx = 0, hy = 0, rh = 0;
        int balls = 0;
        for (const SpriteInstance& s : sp)
            if (s.shape == Shape::Snowball && ++balls == 3) {
                hx = s.x;
                hy = s.y;
                rh = s.halfW;
            }
        int expectAcc = skin.googlyEyes ? 2 : 0, gotAcc = 0, hats = 0, scarves = 0, eyes = 0;
        for (const SkinPart& part : skin.parts) {
            if (part.kind == SkinPart::Kind::Hat) ++hats;
            else if (part.kind == SkinPart::Kind::Scarf) ++scarves;
            else if (part.variant != kNoPart) ++expectAcc;
        }
        int hatSprites = 0, scarfSprites = 0, lastLayer = -1;
        bool ordered = true;
        for (const SpriteInstance& s : sp) {
            if (s.shape == Shape::Hat) {
                ++hatSprites;
                ordered = ordered && lastLayer <= 5;
                lastLayer = 5;
            }
            if (s.shape == Shape::Scarf) {
                ++scarfSprites;
                ordered = ordered && lastLayer <= 0;
                lastLayer = std::max(lastLayer, 0);
            }
            if (s.shape == Shape::Coal && std::fabs(s.y - (hy - 6.0f / 22.0f * rh)) < 0.5f) ++eyes;
            if (s.shape != Shape::Accessory) continue;
            ++gotAcc;
            const Accessory a = static_cast<Accessory>(s.variant);
            const AccessoryPlacement pl = accessoryPlacement(a, rh);
            CHECK_NEAR(s.halfW, pl.halfW, 1e-3);
            CHECK_NEAR(s.halfH, pl.halfH, 1e-3);
            if (a == Accessory::GooglyEye) {
                CHECK_NEAR(std::fabs(s.x - hx), pl.dx, 1e-3);
                CHECK_NEAR(s.y, hy + pl.dy, 1e-3);
            } else if (a == Accessory::Broom) {
                // The hand (0.14 halfH above the centre) is at the end of the +X arm.
                bool atArm = false;
                for (const SpriteInstance& st2 : sp)
                    if (st2.shape == Shape::Stick && std::cos(st2.rotation) > 0) {
                        const float ex = st2.x + std::cos(st2.rotation) * st2.halfW;
                        const float ey = st2.y + std::sin(st2.rotation) * st2.halfW;
                        atArm = atArm || (std::fabs(s.x - ex) < 0.01f && std::fabs(s.y - 0.14f * s.halfH - ey) < 0.01f);
                    }
                CHECK(atArm);
            } else if (a != Accessory::Flower) {
                CHECK_NEAR(s.x, hx + pl.dx, 1e-3);
                CHECK_NEAR(s.y, hy + pl.dy, 1e-3);
            }
            if (a == Accessory::GooglyEye) continue;   // part of the face, drawn before all parts
            const int layer = accessoryLayer(a);
            ordered = ordered && layer >= lastLayer;
            lastLayer = std::max(lastLayer, layer);
        }
        if (gotAcc != expectAcc || hatSprites != hats || scarfSprites != scarves || !ordered)
            printf("  skin %s: acc %d/%d hats %d/%d scarves %d/%d ordered %d\n", skin.id, gotAcc, expectAcc, hatSprites, hats,
                   scarfSprites, scarves, ordered ? 1 : 0);
        CHECK(gotAcc == expectAcc && hatSprites == hats && scarfSprites == scarves);
        CHECK(ordered);
        CHECK(eyes == (skin.googlyEyes ? 0 : 2));   // googly eyes replace the coal eyes
    }
}

TEST(skin_setting_random_and_fixed_are_deterministic) {
    auto skins = [](const char* setting, std::uint64_t seed, std::vector<int>& out) {
        World w(seed);
        Settings st;
        st.interaction.snowmanSkin = setting;
        w.applySettings(st, RuntimeTuning{});
        for (int i = 0; i < 100; ++i) out.push_back(WorldTestAccess::get(w).objects.rollLook().skin);
    };
    std::vector<int> a, b, pirate;
    skins("random", 9, a);
    skins("random", 9, b);
    skins("pirate", 9, pirate);
    CHECK(a == b);
    int randomParts = 0, distinct = 0;
    std::vector<bool> seen(kSnowmanSkinCount, false);
    for (int s : a) {
        randomParts += s < 0 ? 1 : 0;
        if (s >= 0 && !seen[static_cast<std::size_t>(s)]) {
            seen[static_cast<std::size_t>(s)] = true;
            ++distinct;
        }
    }
    CHECK(randomParts > 15 && randomParts < 45);   // ~30 %
    CHECK(distinct >= 8);
    for (int s : pirate) CHECK(s == skinIndex("pirate"));
    // Googly pupils jiggle when the snowman is moved.
    World w(10);
    Settings st = quiet(Tool::Snowball);
    st.interaction.snowmanSkin = "silly";
    w.applySettings(st, RuntimeTuning{});
    DesktopLayout l = basicLayout();
    l.windows.insert(l.windows.begin(), window(kWin, RectI{300, 400, 900, 800}));
    w.setLayout(l);
    auto in = WorldTestAccess::get(w);
    Pile& p = in.objects.addPile(*surfaceOf(w, kWin), 300.0f, 26.0f);
    p.r[p.n++] = 18.0f;
    in.objects.ensureLook(p);
    stepN(w, 5);
    for (int i = 1; i <= 8; ++i) {
        w.step(1.0f / 60.0f);
        l.windows[0] = window(kWin, RectI{300 + 6 * i, 400, 900 + 6 * i, 800});
        w.setLayout(l);
    }
    CHECK(in.objects.piles().size() == 1 && in.objects.piles()[0].jiggle > 0.1f);
}

// ---------------------------------------------------------------- excluded applications
TEST(excluded_windows_behind_mode_clips_particles) {
    World w(11);
    Settings st;
    st.system.excludedSnowMode = ExcludedSnowMode::Behind;
    w.applySettings(st, RuntimeTuning{});
    DesktopLayout l = basicLayout();
    WindowInfo ex = window(kWin, RectI{600, 300, 1300, 800});
    ex.snowAllowed = false;
    l.windows.insert(l.windows.begin(), ex);
    w.setLayout(l);
    stepN(w, 300);
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    CHECK(!snap.particles.clip.empty());
    for (const RectI& c : snap.particles.clip) CHECK(!c.intersects(ex.bounds));
    CHECK(totalArea(snap.particles.clip) == 1920LL * 1080 - 700LL * 500);
    w.setLayout(basicLayout());   // no excluded window: unclipped
    w.buildSnapshot(snap);
    CHECK(snap.particles.clip.empty());
}

TEST(excluded_windows_around_mode_deflects_flakes) {
    int inside[2]{};
    for (int mode = 0; mode < 2; ++mode) {
        World w(12);
        Settings st;
        st.weather.preset = WeatherPreset::Blizzard;
        st.weather.intensity = 100;
        st.performance.profile = PerfProfile::High;
        st.accumulation.fallingSnowmen = false;
        st.system.excludedSnowMode = mode == 0 ? ExcludedSnowMode::Around : ExcludedSnowMode::Behind;
        w.applySettings(st, RuntimeTuning{});
        DesktopLayout l = basicLayout();
        WindowInfo ex = window(kWin, RectI{600, 300, 1300, 800});
        ex.snowAllowed = false;
        l.windows.insert(l.windows.begin(), ex);
        w.setLayout(l);
        auto in = WorldTestAccess::get(w);
        for (int k = 0; k < 1500; ++k) {
            w.step(1.0f / 60.0f);
            if (k < 600) continue;
            for (const Particle& p : in.particles.particles())
                if (!(p.flags & Particle::kDust) && p.x > 602 && p.x < 1298 && p.y > 302 && p.y < 798) ++inside[mode];
        }
    }
    printf("  excluded window: flakes over it %d (around) vs %d (behind)\n", inside[0], inside[1]);
    CHECK(inside[0] == 0);
    CHECK(inside[1] > 1000);
}
