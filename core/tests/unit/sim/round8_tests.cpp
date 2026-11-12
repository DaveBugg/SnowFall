// Round 8: snowmen knocked over by the snowplow / the shovel / a fast hit.
#include <vector>

#include "sim_test_util.h"

using namespace simtest;
using T = InputEvent::Type;

namespace {
constexpr float kPi = 3.14159265f;

Settings quiet(Tool tool) {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = false;
    s.accumulation.shakeToClear = false;
    s.accumulation.shakeOffOnMove = false;
    s.accumulation.naturalCaps = false;
    s.festive.garland = false;
    s.interaction.tool = tool;
    s.interaction.snowmanSkin = "classic";
    return s;
}

void fillBar(World& w, float h) {
    Surface* bar = surfaceOf(w, kTaskbarId);
    for (int i = 0; i < bar->hm.columns(); ++i) bar->hm.setHeight(i, h);
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}

double ballArea(float r) { return kPi * r * r / kPackFactor; }

// A dressed 3-ball snowman on the taskbar.
double placeSnowman(World& w, float localX) {
    auto in = WorldTestAccess::get(w);
    Pile& p = in.objects.addPile(*surfaceOf(w, kTaskbarId), localX, 26.0f);
    p.r[p.n++] = 19.0f;
    p.r[p.n++] = 13.0f;
    in.objects.ensureLook(p);
    return ballArea(26) + ballArea(19) + ballArea(13);
}

double liveObjectMass(World& w) {
    auto in = WorldTestAccess::get(w);
    double m = 0;
    for (const Ball& b : in.objects.balls()) m += b.collected;
    for (const Pile& p : in.objects.piles())
        for (int i = 0; i < p.n; ++i) m += ballArea(p.r[i]);
    return m;
}

struct PlowTrial {
    int contactStep = -1, ballSteps = 0, debrisAtContact = 0;
    float dipAfterContact = 1, ballVx = 0;
    std::vector<float> ballX;   // at contact + 40 steps (determinism)
};

PlowTrial runPlow(World& w, double& snowmanMass) {
    PlowTrial r;
    w.applySettings(quiet(Tool::Snowball), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    fillBar(w, 10.0f);
    snowmanMass = placeSnowman(w, 900.0f);
    auto in = WorldTestAccess::get(w);
    w.triggerSnowplow();
    for (int k = 0; k < 60 * 30 && in.plow.active(); ++k) {
        w.step(1.0f / 60.0f);
        if (r.contactStep < 0 && !in.objects.piles().empty() && in.objects.piles()[0].knock != KnockBy::None) {
            r.contactStep = k;
            r.dipAfterContact = in.plow.speedFactor();
            r.debrisAtContact = static_cast<int>(in.objects.debris().size());
        }
        if (!in.objects.balls().empty()) {
            ++r.ballSteps;
            for (const Ball& b : in.objects.balls()) r.ballVx = std::max(r.ballVx, b.vx);
        }
        if (r.contactStep >= 0 && k == r.contactStep + 40)
            for (const Ball& b : in.objects.balls()) r.ballX.push_back(b.x);
    }
    return r;
}
}  // namespace

TEST(snowplow_knocks_a_snowman_over_instead_of_vanishing_it) {
    World w(1);
    double mass = 0;
    const PlowTrial r = runPlow(w, mass);
    auto in = WorldTestAccess::get(w);
    printf("  plow vs snowman: contact at %.2f s, free balls for %.2f s, ball vx up to %.0f px/s, %d decorations, "
           "speed x%.2f on impact; heap got %.0f of %.0f px^2\n",
           r.contactStep / 60.0, r.ballSteps / 60.0, r.ballVx, r.debrisAtContact, r.dipAfterContact, in.plow.fromObjects(), mass);
    CHECK(r.contactStep > 0);
    CHECK(r.debrisAtContact >= 6);                        // hat, scarf, coal, carrot, sticks flew off
    CHECK(r.dipAfterContact < 0.8f && in.plow.impacts() >= 1);
    CHECK(r.ballSteps >= 18);                             // the balls are free bodies for >= 0.3 s
    CHECK(r.ballVx > 150.0f);                             // thrown forward (the plow drives right)
    CHECK(in.plow.fromObjects() > 0.6 * ballArea(26));   // the base went mostly into the heap
    CHECK(!in.plow.active());
    // Everything settles: debris faded / plowed under, chunks landed.
    stepN(w, 60 * 5);
    CHECK(in.objects.debris().empty() && in.chunks.count() == 0);
    const double accounted = in.plow.fromObjects() + in.chunks.shedChunkMass() + in.objects.crumbleDust() +
                             in.objects.lostBalls() + liveObjectMass(w);
    CHECK_NEAR(accounted, mass, mass * 0.02);
    // Deterministic.
    World w2(1);
    double mass2 = 0;
    const PlowTrial r2 = runPlow(w2, mass2);
    CHECK(r2.contactStep == r.contactStep && r2.ballX == r.ballX && !r.ballX.empty());
}

TEST(decorations_fly_off_bounce_and_fade) {
    World w(2);
    w.applySettings(quiet(Tool::Shovel), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    fillBar(w, 10.0f);
    placeSnowman(w, 900.0f);
    auto in = WorldTestAccess::get(w);
    const Surface* bar = surfaceOf(w, kTaskbarId);
    in.objects.knock(0, *bar, 1, 300.0f, KnockBy::Hit, &in.particles);
    bool hatUp = false, scarfSlow = true, carrotSpins = false;
    for (const Debris& d : in.objects.debris()) {
        if (d.shape == Shape::Hat) hatUp = d.vy < -200.0f && d.vx > 0;
        if (d.shape == Shape::Scarf) scarfSlow = d.gravity < 0.5f;
        if (d.shape == Shape::Carrot) carrotSpins = std::fabs(d.angVel) >= 10.0f;
    }
    CHECK(hatUp && scarfSlow && carrotSpins);
    stepN(w, 60);
    int resting = 0;
    for (const Debris& d : in.objects.debris()) resting += d.fade >= 0 ? 1 : 0;
    CHECK(resting > 0);
    stepN(w, 60 * 4);
    CHECK(in.objects.debris().empty());
}

TEST(shovel_knocks_snowmen_over_harder_when_swung_faster) {
    float vx[3]{};
    for (int k = 0; k < 3; ++k) {
        World w(3);
        w.applySettings(quiet(Tool::Shovel), RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(basicLayout());
        fillBar(w, 0.0f);
        placeSnowman(w, 900.0f);
        auto in = WorldTestAccess::get(w);
        w.setInteractionMode(true);
        const float dir = k == 2 ? -1.0f : 1.0f;
        const float stepPx = k == 0 ? 4.0f : 30.0f;   // slow push vs fast swing
        float x = 900.0f - dir * 120.0f;
        input(w, T::PointerMove, x, 1010.0f);
        input(w, T::PointerDown, x, 1010.0f);
        for (int s = 0; s < 40 && in.objects.balls().empty(); ++s) {
            x += dir * stepPx;
            input(w, T::PointerMove, x, 1010.0f);
            w.step(1.0f / 60.0f);
        }
        CHECK(in.objects.balls().size() == 2);
        for (const Ball& b : in.objects.balls()) vx[k] += b.vx * 0.5f;
        // The base's snow lands on the shovel load (the drift is bare).
        for (int s = 0; s < 40; ++s) w.step(1.0f / 60.0f);
        const float load = WorldTestAccess::get(w).scoop;
        CHECK_NEAR(load, ballArea(26) * (1.0 - kCrumblePieces - kCrumbleDust), ballArea(26) * 0.02);
        input(w, T::PointerUp, x, 1010.0f);
    }
    printf("  shovel knock: upper balls vx slow %.0f / fast %.0f / fast-left %.0f px/s\n", vx[0], vx[1], vx[2]);
    CHECK(vx[0] > 0 && vx[1] > 2.0f * vx[0] && vx[2] < -2.0f * vx[0]);
}

TEST(flung_load_or_fast_ball_knocks_a_snowman_over) {
    World w(4);
    w.applySettings(quiet(Tool::Snowball), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    placeSnowman(w, 900.0f);
    auto in = WorldTestAccess::get(w);
    Chunk* c = in.chunks.spawnFree(800.0f, 990.0f, 10.0f, 6.0f, 300.0f, 900.0f, 0.0f, 1.0f, 0);
    CHECK(c != nullptr);
    if (c) c->soft = true;
    stepN(w, 10);
    CHECK(!in.objects.piles().empty() || !in.objects.balls().empty());
    CHECK(in.objects.piles().empty() || in.objects.piles()[0].knock != KnockBy::None);
    stepN(w, 30);
    CHECK(in.objects.snowmanCount() == 0 && in.objects.balls().size() + static_cast<std::size_t>(in.objects.shattered()) >= 2);
}
