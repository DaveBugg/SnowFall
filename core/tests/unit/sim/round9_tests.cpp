// Round 9: configurable falling-snowmen frequency.
#include <vector>

#include "sim_test_util.h"

using namespace simtest;

namespace {
Settings sched(int minutes, bool heavy, int maxAlive) {
    Settings s;
    s.accumulation.snowplow = false;
    s.accumulation.naturalCaps = false;
    s.festive.garland = false;
    s.accumulation.snowmenIntervalMinutes = minutes;
    s.accumulation.snowmenNeedHeavySnow = heavy;
    s.accumulation.snowmenMaxAlive = maxAlive;
    return s;
}

void fillBar(World& w, float h) {
    Surface* bar = surfaceOf(w, kTaskbarId);
    for (int i = 0; i < bar->hm.columns(); ++i) bar->hm.setHeight(i, h);
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}

// Runs `seconds` with 1/20 s steps; returns the sim times at which snowmen were dropped.
std::vector<double> run(World& w, float seconds) {
    std::vector<double> drops;
    auto in = WorldTestAccess::get(w);
    int seen = in.events.snowmenDropped;
    for (int k = 0; k < static_cast<int>(seconds * 20.0f); ++k) {
        w.step(1.0f / 20.0f);
        if (in.events.snowmenDropped != seen) {
            seen = in.events.snowmenDropped;
            drops.push_back(in.events.time);
        }
    }
    return drops;
}

World* make(std::uint64_t seed, const Settings& st, float fill) {
    World* w = new World(seed);
    w->applySettings(st, RuntimeTuning{});
    w->setEmissionEnabled(false);
    w->setLayout(basicLayout());
    fillBar(*w, fill);
    return w;
}
}  // namespace

TEST(snowman_wait_is_interval_times_0_6_to_1_4_and_at_least_a_minute) {
    for (int minutes : {1, 5, 60}) {
        World w(1);
        w.applySettings(sched(minutes, true, 3), RuntimeTuning{});
        float lo = 1e9f, hi = 0;
        for (int i = 0; i < 500; ++i) {
            const float t = WorldTestAccess::snowmanWait(w);
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        const float m = 60.0f * static_cast<float>(minutes);
        printf("  interval %d min: waits %.0f..%.0f s\n", minutes, lo, hi);
        CHECK(lo >= std::max(60.0f, 0.6f * m) - 1e-3f && hi <= std::max(60.0f, 1.4f * m) + 1e-3f);
        if (minutes > 1) CHECK(lo < 0.7f * m && hi > 1.3f * m);   // really randomised
        else CHECK(lo == 60.0f);                                 // 0.6 min clamped up to a minute
    }
}

TEST(snowmen_drop_on_schedule_without_the_heavy_snow_rule) {
    World* w = make(2, sched(1, false, 10), 12.0f);   // a moderate drift, not "heavy"
    CHECK(!WorldTestAccess::heavySnow(*w));
    const std::vector<double> drops = run(*w, 600.0f);
    printf("  1 min interval, 10 min: %zu snowmen\n", drops.size());
    CHECK(drops.size() >= 6 && drops.size() <= 10);
    for (std::size_t i = 1; i < drops.size(); ++i) CHECK(drops[i] - drops[i - 1] >= 57.0);
    delete w;
    // Too little snow to land on: they wait.
    World* thin = make(3, sched(1, false, 10), 4.0f);
    CHECK(run(*thin, 300.0f).empty());
    delete thin;
}

TEST(heavy_snow_rule_gates_the_countdown) {
    World* w = make(4, sched(1, true, 10), 12.0f);   // 12 of 36 px: not heavy
    CHECK(run(*w, 400.0f).empty());
    fillBar(*w, 36.0f);   // now it is
    CHECK(WorldTestAccess::heavySnow(*w));
    CHECK(!run(*w, 120.0f).empty());
    delete w;
}

TEST(max_alive_limit_and_live_interval_change) {
    World* w = make(5, sched(1, false, 2), 12.0f);
    auto in = WorldTestAccess::get(*w);
    int peak = 0;
    for (int k = 0; k < 600 * 20; ++k) {
        w->step(1.0f / 20.0f);
        peak = std::max(peak, in.objects.autoSnowmenAlive());
    }
    CHECK(peak == 2);
    delete w;

    // A long interval armed, then changed live to 1 minute: no waiting out the old countdown.
    World* l = make(6, sched(60, false, 10), 12.0f);
    auto li = WorldTestAccess::get(*l);
    run(*l, 10.0f);
    CHECK(li.events.snowmanCountdown > 1000.0f);
    l->applySettings(sched(1, false, 10), RuntimeTuning{});
    const std::vector<double> drops = run(*l, 100.0f);
    CHECK(drops.size() == 1);
    delete l;
}
