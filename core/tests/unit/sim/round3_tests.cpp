// Round 3: believable snowplow heap (windrow) and snow caps breaking off in chunks.
#include <vector>

#include "sim_test_util.h"

using namespace simtest;

namespace {
constexpr WindowId kWin = 10;

Settings quiet() {
    Settings s;
    s.accumulation.fallingSnowmen = false;
    s.accumulation.snowplow = false;
    s.accumulation.shakeToClear = false;
    s.accumulation.shakeOffOnMove = true;
    return s;
}

DesktopLayout withWindows(std::vector<WindowInfo> front) {
    DesktopLayout l = basicLayout();
    l.windows.insert(l.windows.begin(), front.begin(), front.end());
    return l;
}

void fillToCap(World& w, Surface& s) {
    for (int i = 0; i < s.hm.columns(); ++i) s.hm.setHeight(i, s.hm.cap(i));
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}

void fillFlat(World& w, Surface& s, float h) {
    for (int i = 0; i < s.hm.columns(); ++i) s.hm.setHeight(i, h);
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}
}  // namespace

// ---------------------------------------------------------------- snowplow heap
TEST(plow_heap_is_a_windrow_against_the_blade) {
    World w(12);
    Settings st = quiet();
    st.accumulation.naturalCaps = false;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    Surface* bar = surfaceOf(w, kTaskbarId);
    if (!bar) {
        CHECK(bar != nullptr);
        return;
    }
    fillFlat(w, *bar, 2.0f);   // thin snow: the heap stays below the blade for a while
    w.triggerSnowplow();
    stepN(w, 120);   // 2 s
    const float H1 = in.plow.heapHeight(), L1 = in.plow.heapLength();
    stepN(w, 120);   // 4 s
    const float H2 = in.plow.heapHeight(), L2 = in.plow.heapLength();
    printf("  plow heap: H %.1f -> %.1f px, length %.1f -> %.1f px, blade %.1f px, carried %.0f px^2\n", H1, H2, L1,
           L2, in.plow.bladeHeight(), in.plow.carried());
    CHECK(H1 > 8.0f && H2 > H1 && L2 > L1);   // grows taller AND longer with the collected volume
    CHECK(H2 <= in.plow.bladeHeight() + 1e-3f);
    CHECK(L2 < 200.0f);                       // a heap, not a layer to the end of the taskbar

    FrameSnapshot snap;
    w.buildSnapshot(snap);
    const DriftStrip* d = driftOf(snap, bar->id);
    CHECK(d != nullptr);
    if (!d) return;
    const float step = d->step;
    const float front = in.plow.frontLocal();
    const int i0 = static_cast<int>(std::ceil(front / step));
    // Peak right at the blade, then down at the repose slope (some noise allowed).
    float peak = 0;
    int peakAt = 0;
    const int span = static_cast<int>(L2 / step) + 4;
    for (int k = 0; k < span; ++k) {
        const float h = d->heights[static_cast<std::size_t>(i0 + k)];
        if (h > peak) {
            peak = h;
            peakAt = k;
        }
    }
    CHECK(peakAt * step < in.plow.heapPlateau() + 0.5f * H2 / kReposeSlope + 3.0f * step);
    CHECK(peak <= in.plow.bladeHeight() * 1.03f + 0.5f);
    float maxDrop = 0, maxRise = 0;
    for (int k = peakAt; k + 1 < span; ++k) {
        const float a = d->heights[static_cast<std::size_t>(i0 + k)], b = d->heights[static_cast<std::size_t>(i0 + k + 1)];
        maxDrop = std::max(maxDrop, (a - b) / step);
        maxRise = std::max(maxRise, b - a);
    }
    CHECK(maxDrop <= 1.15f);    // repose 0.7 + lumps
    CHECK(maxDrop >= 0.5f);     // it really is a slope, not a slab
    CHECK(maxRise <= 0.8f);     // monotonic-ish
    // Union with the drift: never below it; the heap adds exactly what the plow carries.
    double extra = 0;
    for (std::size_t i = 0; i < d->heights.size(); ++i) {
        CHECK(d->heights[i] >= bar->hm.heights()[i] - 1e-4f);
        extra += (d->heights[i] - bar->hm.heights()[i]) * step;
    }
    CHECK_NEAR(extra, in.plow.carried(), in.plow.carried() * 0.04 + 2.0);
    // Mass bookkeeping of the run.
    const double sum = in.plow.carried() + in.plow.spilled() + in.plow.dumped() + in.plow.depositedAhead();
    CHECK_NEAR(sum, in.plow.collected(), in.plow.collected() * 1e-3 + 0.5);
    // The drift ahead of the toe is untouched.
    const int toe = i0 + static_cast<int>((L2 + 40.0f) / step);
    bool untouched = true;
    for (int i = toe; i < bar->hm.columns(); ++i) untouched = untouched && std::fabs(bar->hm.height(i) - 2.0f) < 0.02f;
    CHECK(untouched);
}

TEST(plow_spills_over_a_full_blade_leaves_a_scraped_path_and_dumps_over_time) {
    World w(13);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    Surface* bar = surfaceOf(w, kTaskbarId);
    if (!bar) {
        CHECK(bar != nullptr);
        return;
    }
    fillToCap(w, *bar);
    const double m0 = bar->hm.mass();
    w.triggerSnowplow();
    stepN(w, 300);   // 750 px through a full drift
    CHECK_NEAR(in.plow.heapHeight(), in.plow.bladeHeight(), 1e-3);   // up against the top of the blade
    CHECK(in.plow.spilled() > 0);                                     // ... and over it
    {
        // On a full drift the heap stands clearly above the snow ahead of it.
        FrameSnapshot snap;
        w.buildSnapshot(snap);
        const DriftStrip* d = driftOf(snap, bar->id);
        CHECK(d != nullptr);
        if (d) {
            const int i0 = static_cast<int>(std::ceil(in.plow.frontLocal() / d->step));
            float above = 0;
            for (int k = 0; k < 20; ++k) {
                const std::size_t i = static_cast<std::size_t>(i0 + k);
                above = std::max(above, d->heights[i] - bar->hm.heights()[i]);
            }
            const float ahead = bar->hm.height(i0 + static_cast<int>(in.plow.heapLength() / d->step) + 3);
            printf("  plow on a full drift: heap %.1f px, %.1f px above the drift (drift ahead %.1f px), "
                   "spilled %.0f %% after 750 px\n",
                   in.plow.heapHeight(), above, ahead, 100.0 * in.plow.spilled() / in.plow.collected());
            CHECK(above >= 15.0f);
            CHECK(in.plow.bladeHeight() >= 52.0f && in.plow.bladeHeight() <= 54.0f);
        }
    }
    const double sum = in.plow.carried() + in.plow.spilled() + in.plow.dumped() + in.plow.depositedAhead();
    CHECK_NEAR(sum, in.plow.collected(), in.plow.collected() * 1e-3 + 0.5);
    // At the end: the heap falls off over time while the plow waits.
    int guard = 0;
    while (in.plow.phase() == Snowplow::Phase::Driving && guard++ < 2000) w.step(1.0f / 60.0f);
    CHECK(in.plow.phase() == Snowplow::Phase::Dumping);
    const float start = in.plow.carried();
    CHECK(start > 100.0f);
    const std::size_t particles0 = w.particleCount();
    int dumpSteps = 0;
    float prev = start;
    bool decreasing = true;
    while (in.plow.carried() > 0 && dumpSteps < 600) {
        w.step(1.0f / 60.0f);
        ++dumpSteps;
        decreasing = decreasing && in.plow.carried() <= prev + 1e-3f;
        prev = in.plow.carried();
    }
    printf("  plow dump: %.0f px^2 over %d steps; whole run: spilled %.0f of %.0f px^2 collected (%.0f %%)\n", start,
           dumpSteps, in.plow.spilled(), in.plow.collected(), 100.0 * in.plow.spilled() / in.plow.collected());
    CHECK(dumpSteps >= 25 && dumpSteps <= 70);   // ~0.7 s, not one frame
    CHECK(decreasing);
    CHECK(w.particleCount() > particles0);
    guard = 0;
    while (in.plow.active() && guard++ < 3000) w.step(1.0f / 60.0f);
    CHECK(!in.plow.active() && in.plow.lastExitLocal() > 1920.0f);
    // Scraped, not erased: a thin uneven residual with a few clods.
    double sumH = 0;
    int positive = 0, clods = 0;
    for (int i = 0; i < bar->hm.columns(); ++i) {
        const float h = bar->hm.height(i);
        sumH += h;
        positive += h > 0.05f ? 1 : 0;
        clods += h >= 2.0f ? 1 : 0;
    }
    const double avg = sumH / bar->hm.columns();
    printf("  plow residual: avg %.2f px, %d columns > 0, %d clods >= 2 px, left %.1f %% of the drift\n", avg, positive,
           clods, 100.0 * bar->hm.mass() / m0);
    CHECK(avg > 0.05 && avg <= 1.2);
    CHECK(positive > bar->hm.columns() / 3);
    CHECK(clods >= 3);
    CHECK(bar->hm.mass() <= 0.05 * m0);
}

// ---------------------------------------------------------------- chunks
TEST(sharp_move_breaks_a_big_cap_into_long_and_short_chunks) {
    World w(1);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindows({window(kWin, RectI{300, 400, 1500, 800})}));
    auto in = WorldTestAccess::get(w);
    Surface* s = surfaceOf(w, kWin);
    if (!s) {
        CHECK(s != nullptr);
        return;
    }
    fillToCap(w, *s);
    w.step(1.0f / 60.0f);
    const double before = s->hm.mass();
    w.setLayout(withWindows({window(kWin, RectI{360, 400, 1560, 800})}));   // 60 px in one frame: a jerk
    s = surfaceOf(w, kWin);
    const double removed = before - s->hm.mass();
    int longOnes = 0, shortOnes = 0;
    for (const Chunk& c : in.chunks.chunks()) {
        longOnes += c.halfW >= 30.0f ? 1 : 0;
        shortOnes += c.halfW <= 20.0f ? 1 : 0;
        CHECK(c.halfW >= 4.0f && c.halfW <= 150.0f && c.halfH >= 3.0f && c.halfH <= 30.0f);
        CHECK(c.vx > 0 && c.vx < 3600.0f * 0.5f);   // lags behind the window (3600 px/s)
    }
    printf("  chunks: %zu (%d long, %d short), shed %.0f of %.0f px^2\n", in.chunks.count(), longOnes, shortOnes,
           removed, before);
    CHECK(longOnes >= 1 && shortOnes >= 2);
    CHECK(removed > 0.7 * before);
    CHECK_NEAR(in.chunks.shedChunkMass(), removed, removed * 0.02 + 1.0);
    // Snapshot: foreground slabs with the flake opacity.
    FrameSnapshot snap;
    w.buildSnapshot(snap);
    int sprites = 0;
    for (const SpriteInstance& sp : snap.particles.sprites)
        if (sp.shape == Shape::SnowChunk) {
            ++sprites;
            CHECK(sp.variant < 4 && sp.opacity == 1.0f);
        }
    CHECK(sprites == static_cast<int>(in.chunks.count()));
    // Long slabs crack mid-air; bookkeeping holds throughout.
    bool books = true;
    for (int k = 0; k < 240; ++k) {
        w.step(1.0f / 60.0f);
        const double total = in.chunks.liveMass() + in.chunks.deposited() + in.chunks.dust() + in.chunks.lost();
        books = books && std::fabs(total - in.chunks.shedChunkMass()) <= 1e-3 * in.chunks.shedChunkMass() + 0.5;
    }
    CHECK(books);
    CHECK(in.chunks.cracks() >= 1);
    CHECK(in.chunks.impacts() >= 1);
    CHECK(in.chunks.deposited() > 0);   // landed on the taskbar
}

TEST(moderate_fast_move_sheds_only_some_pieces) {
    World w(2);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindows({window(kWin, RectI{300, 400, 1500, 800})}));
    auto in = WorldTestAccess::get(w);
    Surface* s = surfaceOf(w, kWin);
    if (!s) {
        CHECK(s != nullptr);
        return;
    }
    fillToCap(w, *s);
    w.step(1.0f / 60.0f);
    const double before = s->hm.mass();
    w.setLayout(withWindows({window(kWin, RectI{325, 400, 1525, 800})}));   // 1500 px/s: just above the threshold
    s = surfaceOf(w, kWin);
    const double after = s->hm.mass();
    CHECK(after > 0.3 * before && after < 0.95 * before);
    CHECK(in.chunks.count() >= 1);
    // Gaps in the cap: an empty run between snowy columns.
    int gap = 0, best = 0;
    bool seenSnow = false;
    for (int i = 0; i < s->hm.columns(); ++i) {
        if (s->hm.height(i) > 1.0f) {
            if (seenSnow) best = std::max(best, gap);
            seenSnow = true;
            gap = 0;
        } else {
            ++gap;
        }
    }
    CHECK(best >= 5);
}

TEST(chunks_land_on_a_lower_window_and_deposit_there) {
    World w(3);
    Settings st = quiet();
    st.accumulation.onTaskbar = false;
    st.accumulation.onScreenBottom = false;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    const WindowInfo lower = window(20, RectI{100, 600, 1900, 1000});
    w.setLayout(withWindows({window(kWin, RectI{300, 200, 900, 400}), lower}));
    auto in = WorldTestAccess::get(w);
    Surface* s = surfaceOf(w, kWin);
    if (!s) {
        CHECK(s != nullptr);
        return;
    }
    fillToCap(w, *s);
    w.step(1.0f / 60.0f);
    w.setLayout(withWindows({window(kWin, RectI{360, 200, 960, 400}), lower}));
    CHECK(in.chunks.count() >= 2);
    stepN(w, 120);
    CHECK(in.chunks.impacts() >= 1);
    const Surface* low = surfaceOf(w, 20);
    CHECK(low && low->hm.mass() > 0.0);
    CHECK(in.chunks.deposited() > 0);
    CHECK(in.chunks.lost() < in.chunks.shedChunkMass() * 0.5);
}

TEST(fast_chunk_does_not_tunnel) {
    World w(4);
    w.applySettings(quiet(), RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(basicLayout());
    auto in = WorldTestAccess::get(w);
    Chunk c;
    c.x = 900.0f;
    c.y = 100.0f;
    c.vy = 1.0e6f;
    c.halfW = 40.0f;
    c.halfH = 8.0f;
    c.mass = 400.0f;
    in.chunks.chunks().push_back(c);
    w.step(1.0f / 20.0f);
    CHECK(in.chunks.impacts() == 1);
    CHECK(in.chunks.lost() == 0.0);
    CHECK(surfaceOf(w, kTaskbarId)->hm.mass() > 100.0);
}

TEST(thin_snow_sheds_as_flakes_and_closing_drops_slabs) {
    World w(5);
    Settings st = quiet();
    st.accumulation.naturalCaps = false;
    w.applySettings(st, RuntimeTuning{});
    w.setEmissionEnabled(false);
    w.setLayout(withWindows({window(kWin, RectI{300, 400, 900, 800})}));
    auto in = WorldTestAccess::get(w);
    Surface* s = surfaceOf(w, kWin);
    if (!s) {
        CHECK(s != nullptr);
        return;
    }
    fillFlat(w, *s, 3.0f);
    w.step(1.0f / 60.0f);
    const double before = s->hm.mass();
    w.setLayout(withWindows({window(kWin, RectI{360, 400, 960, 800})}));
    CHECK(in.chunks.count() == 0);
    CHECK(in.particles.particles().size() > 5);
    CHECK(surfaceOf(w, kWin)->hm.mass() < 0.5 * before);

    // dropSnowOnClose with a thick cap: slabs.
    World c(6);
    c.applySettings(st, RuntimeTuning{});
    c.setEmissionEnabled(false);
    c.setLayout(withWindows({window(kWin, RectI{300, 400, 900, 800})}));
    fillFlat(c, *surfaceOf(c, kWin), 20.0f);
    c.setLayout(basicLayout());
    CHECK(WorldTestAccess::get(c).chunks.count() >= 2);
}

TEST(chunks_are_deterministic) {
    auto run = [](std::vector<Chunk>& out) {
        World w(77);
        w.applySettings(quiet(), RuntimeTuning{});
        w.setEmissionEnabled(false);
        w.setLayout(withWindows({window(kWin, RectI{300, 400, 1500, 800})}));
        fillToCap(w, *surfaceOf(w, kWin));
        w.step(1.0f / 60.0f);
        w.setLayout(withWindows({window(kWin, RectI{350, 400, 1550, 800})}));
        stepN(w, 30);
        out = WorldTestAccess::get(w).chunks.chunks();
    };
    std::vector<Chunk> a, b;
    run(a);
    run(b);
    CHECK(!a.empty() && a.size() == b.size());
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i)
        same = a[i].x == b[i].x && a[i].y == b[i].y && a[i].halfW == b[i].halfW && a[i].rotation == b[i].rotation;
    CHECK(same);
}
