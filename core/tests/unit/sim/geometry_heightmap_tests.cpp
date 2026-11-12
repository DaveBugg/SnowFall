#define SNOW_TEST_MAIN
#include <vector>

#include "sim_test_util.h"
#include "simulation/rng.h"
#include "surfaces/geometry.h"
#include "surfaces/heightmap.h"

using namespace snow;

namespace {
bool anyOverlap(const std::vector<RectI>& v) {
    for (std::size_t i = 0; i < v.size(); ++i)
        for (std::size_t j = i + 1; j < v.size(); ++j)
            if (v[i].intersects(v[j])) return true;
    return false;
}
}  // namespace

TEST(rng_is_deterministic_and_uniform) {
    Rng a(42), b(42), c(43);
    bool same = true, differs = false;
    double sum = 0;
    for (int i = 0; i < 10000; ++i) {
        const float x = a.uniform();
        same = same && x == b.uniform();
        differs = differs || x != c.uniform();
        CHECK(x >= 0.0f && x < 1.0f);
        sum += x;
    }
    CHECK(same);
    CHECK(differs);
    CHECK_NEAR(sum / 10000.0, 0.5, 0.02);
    for (int i = 0; i < 1000; ++i) CHECK(a.below(7) < 7u);
}

TEST(rect_subtract_basic) {
    std::vector<RectI> out;
    const RectI a{0, 0, 100, 50};
    subtractRect(a, RectI{20, 10, 40, 30}, out);   // hole strictly inside -> 4 pieces
    CHECK(out.size() == 4);
    CHECK(totalArea(out) == 100 * 50 - 20 * 20);
    CHECK(!anyOverlap(out));
    out.clear();
    subtractRect(a, RectI{200, 0, 300, 50}, out);  // disjoint -> unchanged
    CHECK(out.size() == 1 && out[0] == a);
    out.clear();
    subtractRect(a, RectI{-5, -5, 500, 500}, out); // fully covered -> nothing
    CHECK(out.empty());
    out.clear();
    subtractRect(a, RectI{50, -10, 500, 500}, out);  // right half removed
    CHECK(out.size() == 1 && (out[0] == RectI{0, 0, 50, 50}));
}

TEST(rect_minus_rects_matches_brute_force) {
    Rng rng(7);
    std::vector<RectI> out, scratch, holes;
    for (int round = 0; round < 200; ++round) {
        const RectI base{-40, -20, 60, 30};
        holes.clear();
        const int n = static_cast<int>(rng.below(6));
        for (int i = 0; i < n; ++i) {
            const int l = static_cast<int>(rng.below(140)) - 70, t = static_cast<int>(rng.below(80)) - 40;
            holes.push_back(RectI{l, t, l + static_cast<int>(rng.below(60)), t + static_cast<int>(rng.below(40))});
        }
        rectMinusRects(base, holes, out, scratch);
        CHECK(!anyOverlap(out));
        long long expected = 0;
        bool consistent = true;
        for (int y = base.t; y < base.b; ++y) {
            for (int x = base.l; x < base.r; ++x) {
                const bool visible = !pointInAny(holes, x, y);
                expected += visible ? 1 : 0;
                consistent = consistent && (pointInAny(out, x, y) == visible);
            }
        }
        CHECK(consistent);
        CHECK(totalArea(out) == expected);
    }
}

TEST(clip_region_respects_monitors_and_holes) {
    std::vector<RectI> out, s1, s2;
    const std::vector<RectI> monitors{RectI{0, 0, 100, 100}, RectI{100, 0, 200, 100}};
    const std::vector<RectI> holes{RectI{90, 0, 110, 100}};
    clipRegion(RectI{50, 40, 250, 60}, monitors, holes, out, s1, s2);
    CHECK(!anyOverlap(out));
    CHECK(totalArea(out) == (40 + 90) * 20);   // [50,90) on monitor 0, [110,200) on monitor 1
    for (const RectI& r : out) CHECK(!r.intersects(holes[0]) && r.r <= 200);
}

TEST(visible_intervals_split_by_occluders) {
    std::vector<IntervalI> out;
    const std::vector<RectI> occ{RectI{10, 0, 20, 10}, RectI{50, 0, 200, 10}, RectI{30, 50, 40, 60}};
    visibleIntervals(0, 100, 5, occ, out);
    CHECK(out.size() == 2);
    CHECK(out.size() == 2 && (out[0] == IntervalI{0, 10}) && (out[1] == IntervalI{20, 50}));
    visibleIntervals(0, 100, 55, occ, out);
    CHECK(out.size() == 2 && (out[0] == IntervalI{0, 30}) && (out[1] == IntervalI{40, 100}));
}

TEST(heightmap_deposit_adds_mass_and_respects_max_height) {
    Heightmap hm;
    hm.reset(100, 3.0f);
    hm.setLimits(36.0f, 0.0f, 297.0f);
    const float got = hm.deposit(150.0f, 10.0f);
    CHECK_NEAR(got, 10.0f, 1e-4);
    CHECK_NEAR(hm.mass(), 10.0, 1e-4);
    CHECK(hm.height(50) > hm.height(49) && hm.height(49) > 0 && hm.height(48) == 0);   // 3-column kernel

    // Keep dumping snow at one spot: nothing may exceed the cap, the excess rolls to neighbours.
    double deposited = got;
    for (int i = 0; i < 5000; ++i) deposited += hm.deposit(150.0f, 12.0f);
    float peak = 0;
    for (float h : hm.heights()) peak = std::max(peak, h);
    CHECK(peak <= 36.0f + 1e-3f);
    CHECK_NEAR(hm.mass(), deposited, deposited * 1e-4);
    CHECK(hm.height(50 + Heightmap::kRollRange) > 35.0f);     // reachable neighbours filled up
    CHECK(hm.height(50 + Heightmap::kRollRange + 3) == 0.0f); // beyond the roll range: untouched
    CHECK(hm.deposit(150.0f, 12.0f) == 0.0f);                 // everything nearby is full: discarded

    Heightmap zero;
    zero.reset(50, 3.0f);
    zero.setLimits(0.0f, 0.0f, 147.0f);   // maxHeight 0 disables growth
    CHECK(zero.deposit(60.0f, 10.0f) == 0.0f);
    CHECK(zero.mass() == 0.0);
}

TEST(heightmap_blocked_columns_receive_nothing) {
    Heightmap hm;
    hm.reset(60, 3.0f);
    hm.setLimits(36.0f, 0.0f, 177.0f);
    for (int i = 20; i < 40; ++i) hm.setBlocked(i, true);
    CHECK(hm.deposit(30 * 3.0f, 10.0f) == 0.0f);          // lands on a blocked column
    for (int k = 0; k < 2000; ++k) hm.deposit(19 * 3.0f, 8.0f);   // right next to the blocked range
    RelaxParams rp;
    for (int k = 0; k < 500; ++k) hm.relax(rp);
    for (int i = 20; i < 40; ++i) CHECK(hm.height(i) == 0.0f);
    CHECK(hm.height(19) > 1.0f);
}

TEST(heightmap_corner_taper) {
    Heightmap hm;
    hm.reset(101, 3.0f);
    hm.setLimits(30.0f, 12.0f, 300.0f);
    CHECK(hm.cap(0) == 0.0f);
    CHECK(hm.cap(100) == 0.0f);
    CHECK_NEAR(hm.cap(2), 15.0f, 1e-4);
    CHECK(hm.cap(4) == 30.0f && hm.cap(50) == 30.0f);
    for (int k = 0; k < 3000; ++k) hm.deposit(0.0f, 6.0f);
    CHECK(hm.height(0) == 0.0f);
    for (int i = 0; i < hm.columns(); ++i) CHECK(hm.height(i) <= hm.cap(i) + 1e-3f);
}

TEST(heightmap_relax_is_stable_limits_slope_and_conserves_mass) {
    Heightmap hm;
    hm.reset(200, 3.0f);
    hm.setLimits(1000.0f, 0.0f, 597.0f);
    // A nasty profile: spikes, a cliff and a pit.
    for (int i = 0; i < 200; ++i) hm.setHeight(i, (i % 7 == 0) ? 60.0f : 0.0f);
    for (int i = 80; i < 120; ++i) hm.setHeight(i, 90.0f);
    for (int i = 95; i < 100; ++i) hm.setHeight(i, 0.0f);
    const double mass0 = hm.mass();
    RelaxParams rp;
    rp.openLeft = rp.openRight = false;
    float prevPeak = 1e9f;
    int settledAt = -1;
    for (int k = 0; k < 20000; ++k) {
        hm.relax(rp);
        float peak = 0;
        for (float h : hm.heights()) {
            peak = std::max(peak, h);
            CHECK(h >= 0.0f);
        }
        if (peak > prevPeak + 1e-4f) {   // the maximum never grows: no oscillation / overshoot
            CHECK(peak <= prevPeak + 1e-4f);
            break;
        }
        prevPeak = peak;
        if (!hm.dirty()) {
            settledAt = k;
            break;
        }
    }
    CHECK(settledAt > 0);
    CHECK_NEAR(hm.mass(), mass0, mass0 * 1e-4);
    const float limit = rp.maxSlope * hm.step();
    float worst = 0;
    for (int i = 0; i + 1 < hm.columns(); ++i) worst = std::max(worst, std::fabs(hm.height(i) - hm.height(i + 1)));
    CHECK(worst <= limit * 1.05f);
    // Once settled, further passes barely move anything.
    const std::vector<float> before = hm.heights();
    for (int k = 0; k < 100; ++k) hm.relax(rp);
    float drift = 0;
    for (int i = 0; i < hm.columns(); ++i) drift = std::max(drift, std::fabs(hm.height(i) - before[static_cast<std::size_t>(i)]));
    CHECK(drift < 0.2f);

    // Open ends: what leaves the map is reported, so mass + lost is conserved.
    Heightmap open;
    open.reset(40, 3.0f);
    open.setLimits(1000.0f, 0.0f, 117.0f);
    for (int i = 0; i < 40; ++i) open.setHeight(i, 50.0f);
    const double m0 = open.mass();
    double lost = 0;
    RelaxParams ro;
    for (int k = 0; k < 5000; ++k) {
        const RelaxResult r = open.relax(ro);
        lost += r.lostLeft + r.lostRight;
    }
    CHECK(lost > 0);
    CHECK_NEAR(open.mass() + lost, m0, m0 * 1e-3);
    CHECK(open.height(0) <= ro.maxSlope * 3.0f * 1.05f);
}

TEST(heightmap_resize_keeps_left_columns_and_resample_scales) {
    Heightmap hm;
    hm.reset(50, 3.0f);
    hm.setLimits(36.0f, 0.0f, 147.0f);
    for (int i = 0; i < 50; ++i) hm.setHeight(i, static_cast<float>(i % 10));
    const std::vector<float> before = hm.heights();
    hm.resizeColumns(30);
    for (int i = 0; i < 30; ++i) CHECK(hm.height(i) == before[static_cast<std::size_t>(i)]);
    hm.resizeColumns(70);
    CHECK(hm.columns() == 70);
    for (int i = 0; i < 30; ++i) CHECK(hm.height(i) == before[static_cast<std::size_t>(i)]);
    for (int i = 30; i < 70; ++i) CHECK(hm.height(i) == 0.0f);

    Heightmap flat;
    flat.reset(100, 3.0f);
    flat.setLimits(100.0f, 0.0f, 297.0f);
    for (int i = 0; i < 100; ++i) flat.setHeight(i, 10.0f);
    const double m = flat.mass();
    flat.resample(90, 5.0f, 1.5f);   // 100 % -> 150 % DPI
    CHECK_NEAR(flat.height(10), 15.0f, 1e-3);
    CHECK_NEAR(flat.mass(), m * 1.5 * 1.5, m * 1.5 * 1.5 * 0.05);
}
