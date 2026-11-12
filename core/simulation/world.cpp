#include "simulation/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "diagnostics/log.h"
#include "interaction/snow_objects.h"
#include "interaction/snowplow.h"
#include "interaction/tools.h"
#include "simulation/chunks.h"
#include "simulation/festive.h"
#include "simulation/particles.h"
#include "simulation/weather.h"
#include "simulation/world_internal.h"
#include "surfaces/geometry.h"
#include "surfaces/surface_registry.h"

namespace snow {

namespace {
constexpr float kMaxDt = 1.0f / 20.0f;
constexpr int kHitGapColumns = 4;      // snowy runs closer than this are merged into one hit rect
constexpr float kHitSnowMin = 0.5f;    // px of snow that counts as "there is snow here"
constexpr float kRingOpacity = 0.35f;  // brush-radius ring (hover only)
}  // namespace

struct World::Impl {
    explicit Impl(std::uint64_t seed)
        : particles(seed), weather(seed), eventRng(seed ^ 0xE7E2A5ull), chunks(seed), icicles(seed), splats(seed),
          fireworks(seed) {
        weather.configure(settings.weather);
        reg.setSeed(seed);
        objects.setChunks(&chunks);
        objects.setSplats(&splats);
        objects.setIcicles(&icicles);
        objects.setSeed(seed);
    }

    Settings settings;
    RuntimeTuning tuning;
    DesktopLayout layout;
    bool haveLayout = false;
    bool settingsApplied = false;

    SurfaceRegistry reg;
    ParticleSystem particles;
    Weather weather;
    ObjectSystem objects;
    LayoutEvents events;
    RelaxParams relaxParams;
    Snowplow plow;                          // the primary route's plow
    double plowStartedAt = 0;              // ev.time of the current run (stuck-run watchdog)
    std::vector<PlowRun> plowRuns;          // one per route (primary first)
    Snowplow& plowOf(PlowRun& r) { return r.main ? plow : r.own; }
    const Snowplow& plowOf(const PlowRun& r) const { return r.main ? plow : r.own; }

    // Rebuild the routes after a layout / settings change. A plow whose route changed or
    // vanished is stopped and drops its heap.
    void rebuildRoutes() {
        std::vector<PlowRoute> routes = buildPlowRoutes(reg, settings.accumulation.snowplowAllTaskbars);
        ParticleSystem* p = fx();
        std::vector<PlowRun> next;
        next.reserve(routes.size());
        for (std::size_t i = 0; i < routes.size(); ++i) {
            const PlowRoute& r = routes[i];
            const bool main = i == 0 && r.primary;
            PlowRun* old = nullptr;
            for (PlowRun& o : plowRuns)
                if (o.route.key == r.key) old = &o;
            PlowRun run;
            if (old && old->main == main) {
                run = *old;
                if (!run.route.sameAs(r)) plowOf(run).abortDump(p);   // its route changed under it
            } else {
                run.own = Snowplow(0x9100Bull ^ r.key);
                run.offset = r.primary ? 0.0f : eventRng.range(0.0f, kPlowRouteOffsetMax);
                if (old) {
                    run.fullSeconds = old->fullSeconds;
                    run.offset = old->offset;
                    run.nextDir = old->nextDir;
                }
            }
            run.route = r;
            run.main = main;
            next.push_back(run);
        }
        // Plows of routes that are gone (or changed role) stop where they are.
        for (PlowRun& o : plowRuns) {
            bool kept = false;
            for (const PlowRun& n : next) kept = kept || (n.route.key == o.route.key && n.main == o.main);
            if (!kept) plowOf(o).abortDump(p);
        }
        bool mainRoute = false;
        for (const PlowRun& n : next) mainRoute = mainRoute || n.main;
        if (!mainRoute) plow.abortDump(p);
        plowRuns.swap(next);
    }

    // "Mostly full" fraction of a route's drift.
    void routeFullness(const PlowRoute& r, float& avg, bool& isFull) const {
        int cols = 0, full = 0;
        double fill = 0;
        for (SurfaceId id : r.surfaces) {
            const Surface* s = reg.find(id);
            if (!s) continue;
            for (int i = 0; i < s->hm.columns(); ++i) {
                const float cap = s->hm.cap(i);
                if (cap < 1.0f) continue;
                const float f = std::min(s->hm.height(i) / cap, 1.0f);
                ++cols;
                fill += f;
                full += f >= kPlowFullColumnLevel ? 1 : 0;
            }
        }
        avg = cols ? static_cast<float>(fill / cols) : 0.0f;
        isFull = cols > 0 && (avg >= kPlowFullAverage || static_cast<float>(full) >= kPlowFullColumnShare * static_cast<float>(cols));
    }
    AutoEvents ev;
    Rng eventRng;
    ChunkSystem chunks;
    IcicleSystem icicles;
    SplatSystem splats;
    Fireworks fireworks;
    float wet = 0;
    float meltAcc = 0, dropAcc = 0;
    std::uint32_t dropPick = 0;
    // Ambient pointer (umbrella).
    float apX = 0, apY = 0, apPrevX = 0, apPrevY = 0;
    bool apValid = false, apHavePrev = false;
    // Local calendar.
    bool haveDate = false;
    int lastMonth = 0, lastDay = 0;
    int firedYear = -1;

    bool interaction = false;
    Tool tool = Tool::Mitten;
    Tool settingsTool = Tool::Mitten;
    bool hasPointer = false;
    bool brushing = false;   // a mitten / shovel press is in progress
    enum class Gesture { None, Pending, Brush, Sculpt, Scoop };
    Gesture gesture = Gesture::None;
    float pressX = 0, pressY = 0;
    double pressT = 0;
    float scoop = 0;
    float rubAcc = 0;
    std::vector<RectI> obstacleRects;
    float px = 0, py = 0;
    float ptrAccX = 0, ptrAccY = 0;   // pointer travel since the last step
    float ptrVx = 0, ptrVy = 0;       // smoothed pointer velocity, px/s

    float sinceLayout = 1.0f;
    std::uint64_t stepIndex = 0;
    float stepMs = 0;

    mutable std::vector<RectI> scratchA, scratchB, scratchC;

    bool animations() const { return tuning.animationsEnabled && settings.performance.animations; }
    ParticleSystem* fx() { return animations() ? &particles : nullptr; }

    float brushRadius(float dpi) const {
        return static_cast<float>(std::clamp(settings.interaction.brushRadiusPxAt96Dpi, 12, 48)) * dpi;
    }
    float shovelWidth(float dpi) const {
        return static_cast<float>(std::clamp(settings.interaction.shovelWidthPxAt96Dpi, 48, 160)) * dpi;
    }

    void updateTarget() {
        particles.setTargetCount(targetParticleCount(settings, tuning, reg.normalizedMonitorArea()));
    }

    // Turn (part of) a drift into falling flakes. `fraction` of the snow is represented by
    // at most `maxFlakes` flakes; offset shifts the spawn position (old window position).
    void spillFlakes(const Surface& s, float fraction, int maxFlakes, float offX, float offY, float baseVx = 0,
                     float baseVy = 0) {
        const float d2 = s.dpiScale * s.dpiScale;
        const int n = std::clamp(static_cast<int>(s.hm.mass() * fraction / (10.0 * d2)), 0, maxFlakes);
        const int cols = s.hm.columns();
        if (cols <= 0) return;
        Rng& rng = particles.rng();
        for (int i = 0; i < n; ++i) {
            int col = 0;
            float h = 0;
            for (int attempt = 0; attempt < 4 && h < 0.3f; ++attempt) {
                col = static_cast<int>(rng.below(static_cast<std::uint32_t>(cols)));
                h = s.hm.height(col);
            }
            if (h < 0.3f) continue;
            const bool clump = baseVx != 0 && rng.uniform() < 0.2f;   // shaken-off snow: some big clumps
            particles.addLoose(s.worldX(col) + offX, static_cast<float>(s.top) - rng.uniform() * h + offY,
                               baseVx * rng.range(0.3f, 0.7f) + rng.range(-15.0f, 15.0f) * s.dpiScale,
                               baseVy * rng.range(0.3f, 0.7f) + rng.range(0.0f, 40.0f) * s.dpiScale,
                               (clump ? rng.range(3.0f, 4.5f) : rng.range(1.2f, 2.6f)) * s.dpiScale);
        }
    }

    void rebuildSurfaces(float elapsed) {
        reg.update(layout, settings.accumulation, elapsed, events);
        const RectI scene = reg.sceneBounds();
        particles.setScene(scene, reg.monitors());
        weather.setScene(static_cast<float>(scene.t), static_cast<float>(scene.b));
        updateTarget();

        ParticleSystem* p = fx();
        const bool drop = settings.accumulation.dropSnowOnClose;
        if (p && drop) {
            for (Surface& s : events.removed) {
                if (!s.active) continue;
                if (ChunkSystem::substantial(s)) chunks.shed(s, 1.0f, 0, 0, 0, 0, p);   // breaks into slabs
                else spillFlakes(s, 1.0f, kMaxDropFlakes, 0, 0);
            }
        }
        objects.onSurfacesRemoved(events.removed, p, drop);
        icicles.onSurfacesRemoved(events.removed, drop);
        splats.onLayout(reg, elapsed, p ? &chunks : nullptr);
        for (const auto& r : events.rescaled) objects.onSurfaceRescaled(r.id, r.ratio);
        for (SurfaceId id : events.resized)
            if (const Surface* s = reg.find(id)) objects.onSurfaceResized(*s);
        for (const auto& m : events.moved) {
            Surface* s = reg.find(m.id);
            if (!s) continue;
            const float jump = static_cast<float>(std::max(std::abs(m.dx), std::abs(m.dy)));
            const bool teleport = jump > kTeleportJumpAt96 * s->dpiScale;
            objects.onSurfaceMoved(m, teleport, reg, p);
            if (s->kind == SurfaceKind::Window) {
                if (teleport) {
                    s->shake.reset();
                } else if (settings.accumulation.shakeToClear &&
                           s->shake.feed(m.dx, m.dy, ev.time, kShakeMinLegAt96 * s->dpiScale)) {
                    shakeClear(*s, velX(m), velY(m));
                    continue;
                }
            }
            const float threshold = kShakeSpeedAt96 * s->dpiScale;
            if (!teleport && settings.accumulation.shakeOffOnMove && m.speed > threshold) {
                // A moderate fast move sheds a few pieces, a sharp jerk most of the cap.
                const float f = std::clamp(0.25f + 0.33f * (m.speed / threshold - 1.0f), 0.25f, 0.95f);
                if (s->hm.peak() > 0)
                    shedCap(*s, f, velX(m), velY(m), -static_cast<float>(m.dx), -static_cast<float>(m.dy));
                objects.detachPilesOn(*s, f, velX(m), velY(m), -static_cast<float>(m.dx), -static_cast<float>(m.dy));
                icicles.snapOff(*s, f, velX(m), velY(m), -static_cast<float>(m.dx), -static_cast<float>(m.dy));
            }
        }
        events.removed.clear();   // release the heightmaps of closed windows
        updateObstacles();
        rebuildRoutes();
    }

    void updateObstacles() {
        obstacleRects.clear();
        const bool around = settings.system.excludedSnowMode == ExcludedSnowMode::Around;
        float dpi = 1;
        if (around) {
            for (const RectI& r : reg.excludedRects()) {
                dpi = reg.dpiScaleAt(0.5f * static_cast<float>(r.l + r.r), static_cast<float>(r.t));
                const int m = static_cast<int>(kExcludedMarginAt96 * dpi);
                obstacleRects.push_back(RectI{r.l - m, r.t - m, r.r + m, r.b});
            }
        }
        particles.setObstacles(obstacleRects, kExcludedRadiusAt96 * dpi, kExcludedBandAt96 * dpi);
    }

    // Mitten / shovel gestures.
    float scoopCapacity(float dpi) const { return shovelWidth(dpi) * kScoopDepthAt96 * dpi; }
    void releaseVelocity(float& vx, float& vy) const {
        // Blend the smoothed velocity with the travel since the last step (assumed 1/60 s).
        vx = 0.5f * ptrVx + 0.5f * ptrAccX * 60.0f;
        vy = 0.5f * ptrVy + 0.5f * ptrAccY * 60.0f;
    }
    void beginSculptOrBrush() {
        if (objects.sculptStart(reg, pressX, pressY)) {
            gesture = Gesture::Sculpt;
            rubAcc = 0;
        } else {
            gesture = Gesture::Brush;
            applyStroke(pressX, pressY, px, py);
        }
    }
    // Shovel released: fling the load, or (slow release) push it aside like the old shovel.
    void flingScoop() {
        const float load = scoop;
        scoop = 0;
        if (load <= 0.5f) return;
        const float dpi = reg.dpiScaleAt(px, py);
        ParticleSystem* p = fx();
        float vx, vy;
        releaseVelocity(vx, vy);
        float speed = std::sqrt(vx * vx + vy * vy);
        if (speed < kFlingMinAt96 * dpi) {
            if (p) emitDust(*p, px, py, load, dpi);
            return;
        }
        if (speed > kFlingMaxAt96 * dpi) {
            vx *= kFlingMaxAt96 * dpi / speed;
            vy *= kFlingMaxAt96 * dpi / speed;
            speed = kFlingMaxAt96 * dpi;
        }
        if (speed > kSplatThrowSpeedAt96 * dpi) {   // flung at a window pane: splat
            Ball b;
            b.x = px;
            b.y = py;
            b.r = std::clamp(std::sqrt(kPackFactor * load / 3.14159265f), 6.0f * dpi, 40.0f * dpi);
            b.collected = load;
            b.dpi = dpi;
            if (splats.tryThrow(reg, b, vx, vy, p)) return;
        }
        if (!p) return;
        Rng& rng = eventRng;
        const int n = std::clamp(static_cast<int>(load / (250.0f * dpi * dpi)), 2, 6);
        float w[6]{}, wsum = 0;
        for (int i = 0; i < n; ++i) wsum += (w[i] = rng.range(0.6f, 1.4f));
        for (int i = 0; i < n; ++i) {
            const float m = load * w[i] / wsum;
            const float halfW = std::clamp(0.55f * std::sqrt(m), 4.0f * dpi, 30.0f * dpi);
            const float halfH = std::clamp(1.2f * m / (4.0f * halfW), 3.0f * dpi, 14.0f * dpi);
            Chunk* c = chunks.spawnFree(px + rng.range(-8.0f, 8.0f) * dpi, py - (8.0f + rng.range(0.0f, 6.0f)) * dpi, halfW,
                                        halfH, m, vx * rng.range(0.85f, 1.05f) + rng.range(-40.0f, 40.0f) * dpi,
                                        vy * rng.range(0.85f, 1.05f) + rng.range(-40.0f, 20.0f) * dpi, dpi, 0);
            if (c) {
                c->soft = true;
                c->angVel = rng.range(-3.0f, 3.0f);
            }
        }
        for (int i = 0; i < 4; ++i)
            p->addDust(px, py - 6.0f * dpi, vx * 0.4f + rng.range(-60.0f, 60.0f) * dpi, vy * 0.4f - rng.range(20.0f, 80.0f) * dpi,
                       1.2f * dpi, 0.6f);
    }
    // The shovel hits a resting snowman / ball: knocked over in the stroke direction, harder
    // the faster the swing.
    void shovelKnock(float x0, float y0, float x1, float y1) {
        const float dpi = reg.dpiScaleAt(x1, y1);
        const float half = 0.5f * shovelWidth(dpi);
        const float dx = x1 - x0;
        const float speed = std::max(std::hypot(ptrVx, ptrVy), std::hypot(dx, y1 - y0) * 60.0f);
        for (std::size_t k = 0; k < objects.piles().size(); ++k) {
            const Pile& p = objects.piles()[k];
            if (p.knock != KnockBy::None) continue;
            const Surface* s = reg.find(p.surface);
            if (!s || !s->active) continue;
            const PileGeom g = pileGeom(p, *s);
            const float top = g.cy[g.n - 1] - g.r[g.n - 1];
            if (std::fabs(g.cx - x1) > half + g.r[0] || y1 < top - 20.0f * dpi || y1 > g.base + 20.0f * dpi) continue;
            const int dir = dx > 0.5f ? 1 : (dx < -0.5f ? -1 : (x1 < g.cx ? 1 : -1));
            objects.knock(k, *s, dir, speed, KnockBy::Shovel, fx());
        }
    }

    // A flung load (soft chunks) that flies into a snowman knocks it over.
    void chunkKnocks() {
        if (objects.piles().empty() || chunks.count() == 0) return;
        for (const Chunk& c : chunks.chunks()) {
            if (!c.soft) continue;
            const float v = std::hypot(c.vx, c.vy);
            if (v < 300.0f * c.dpi) continue;
            for (std::size_t k = 0; k < objects.piles().size(); ++k) {
                const Pile& p = objects.piles()[k];
                if (p.knock != KnockBy::None) continue;
                const Surface* s = reg.find(p.surface);
                if (!s || !s->active) continue;
                const PileGeom g = pileGeom(p, *s);
                const float top = g.cy[g.n - 1] - g.r[g.n - 1];
                if (std::fabs(c.x - g.cx) < g.r[0] + c.halfW && c.y > top - c.halfH && c.y < g.base + c.halfH)
                    objects.knock(k, *s, c.vx >= 0 ? 1 : -1, v * 0.5f, KnockBy::Hit, fx());
            }
        }
    }

    void endGesture() {
        if (gesture == Gesture::Pending) {
            applyStroke(pressX, pressY, pressX, pressY);   // a tap brushes
        } else if (gesture == Gesture::Sculpt) {
            objects.sculptEnd(reg, fx());
        } else if (gesture == Gesture::Scoop) {
            flingScoop();
        }
        gesture = Gesture::None;
        brushing = false;
    }

    static float velX(const LayoutEvents::Moved& m) {
        const float dist = std::sqrt(static_cast<float>(m.dx * m.dx + m.dy * m.dy));
        return dist > 0 ? static_cast<float>(m.dx) * m.speed / dist : 0.0f;
    }
    static float velY(const LayoutEvents::Moved& m) {
        const float dist = std::sqrt(static_cast<float>(m.dx * m.dx + m.dy * m.dy));
        return dist > 0 ? static_cast<float>(m.dy) * m.speed / dist : 0.0f;
    }

    // Shed `fraction` of a window's cap: a substantial cap breaks into slabs (pieces chosen at
    // random, leaving gaps), thin snow goes as flakes.
    void shedCap(Surface& s, float fraction, float vx, float vy, float offX, float offY) {
        ParticleSystem* p = fx();
        if (ChunkSystem::substantial(s)) {
            chunks.shed(s, fraction, vx, vy, offX, offY, p);
        } else {
            if (p) spillFlakes(s, fraction, kShakeMaxFlakes, offX, offY, vx, vy);
            s.hm.scaleAll(1.0f - fraction);
        }
    }

    // The user shook the window: throw most of its snow cap off, break what stands on it.
    void shakeClear(Surface& s, float vx, float vy) {
        if (s.hm.peak() > 0) shedCap(s, kShakeShedFraction, vx, vy, 0, 0);
        objects.detachPilesOn(s, 1.0f, vx, vy, 0, 0);
        icicles.snapOff(s, 1.0f, vx, vy, 0, 0);
    }

    // Heavy snow: >= kHeavyColumnShare of the visible drift columns on the primary monitor
    // are filled to >= kHeavyColumnLevel of their cap.
    bool evaluateHeavySnow() const {
        int total = 0, full = 0;
        for (const Surface& s : reg.surfaces()) {
            if (!s.active || s.clip.empty() || !reg.edgeInObjectArea(s)) continue;
            const float y = static_cast<float>(s.top) - 1.0f;
            for (int i = 0; i < s.hm.columns(); ++i) {
                const float cap = s.hm.cap(i);
                if (cap < 1.0f || s.hm.blocked(i) || !reg.inObjectArea(s.worldX(i), y)) continue;
                ++total;
                full += s.hm.height(i) >= kHeavyColumnLevel * cap ? 1 : 0;
            }
        }
        return total >= kHeavyMinColumns && static_cast<float>(full) >= kHeavyColumnShare * static_cast<float>(total);
    }

    // Pick a visible, full spot on the primary monitor and drop a snowman onto it.
    // A spot a snowman may land on: full drift (heavy-snow rule) or enough snow over a little width.
    bool landingColumn(const Surface& s, int i, float dpi, bool heavyRule) const {
        const float cap = s.hm.cap(i);
        if (heavyRule) return s.hm.height(i) >= kHeavyColumnLevel * cap;
        const float need = kLandingSnowAt96 * dpi;
        const int half = static_cast<int>(std::ceil(0.5f * kLandingWidthAt96 * dpi / s.hm.step()));
        for (int k = std::max(0, i - half); k <= std::min(s.hm.columns() - 1, i + half); ++k)
            if (s.hm.height(k) < need) return false;
        return true;
    }
    bool hasLandingSpot() {
        for (const Surface& s : reg.surfaces()) {
            if (!s.active || s.clip.empty() || !reg.edgeInObjectArea(s) || s.hm.peak() < kLandingSnowAt96 * s.dpiScale) continue;
            const float y = static_cast<float>(s.top) - 1.0f;
            for (int i = 0; i < s.hm.columns(); i += 2)
                if (!s.hm.blocked(i) && reg.inObjectArea(s.worldX(i), y) && landingColumn(s, i, s.dpiScale, false)) return true;
        }
        return false;
    }
    float snowmanWait() {
        const float minutes = static_cast<float>(std::clamp(settings.accumulation.snowmenIntervalMinutes, 1, 60));
        ev.lastWait = std::max(kSnowmanMinWait, minutes * 60.0f * eventRng.range(kSnowmanWaitJitterMin, kSnowmanWaitJitterMax));
        return ev.lastWait;
    }

    bool spawnAutoSnowman() {
        const float r96 = eventRng.range(20.0f, 28.0f);   // bottom ball radius at 96 DPI
        const int n = eventRng.chance(0.7f) ? 3 : 2;
        const float k1 = eventRng.range(0.68f, 0.78f);
        const Surface* best = nullptr;
        int bestCol = 0;
        int seen = 0;
        for (const Surface& s : reg.surfaces()) {
            const float dpi = s.dpiScale;
            const float r0 = r96 * dpi;
            if (!s.active || s.clip.empty() || !reg.edgeInObjectArea(s) || s.width() < 6.0f * r0) continue;
            const float margin = 2.4f * r0;   // keep the arms on the edge
            const float y = static_cast<float>(s.top) - 1.0f;
            for (int i = 0; i < s.hm.columns(); i += 2) {
                const float lx = static_cast<float>(i) * s.hm.step();
                const float x = s.worldX(i);
                const float cap = s.hm.cap(i);
                if (lx < margin || lx > s.width() - margin || cap < 6.0f * dpi || s.hm.blocked(i)) continue;
                if (!reg.inObjectArea(x - margin, y) || !reg.inObjectArea(x + margin, y)) continue;
                if (!landingColumn(s, i, dpi, settings.accumulation.snowmenNeedHeavySnow)) continue;
                bool crowded = false;
                for (const Pile& p : objects.piles())
                    crowded = crowded || (p.surface == s.id && std::fabs(p.localX - lx) < 2.5f * (p.r[0] + r0));
                if (crowded) continue;
                if (eventRng.below(static_cast<std::uint32_t>(++seen)) == 0) {   // reservoir sampling
                    best = &s;
                    bestCol = i;
                }
            }
        }
        if (!best) return false;
        const float dpi = best->dpiScale;
        const float radii[3] = {r96 * dpi, r96 * dpi * k1, r96 * dpi * k1 * 0.7f};
        const RectI* mon = reg.objectRectAt(best->worldX(bestCol), static_cast<float>(best->top) - 1.0f);
        const float topY = mon ? static_cast<float>(mon->t) : static_cast<float>(reg.sceneBounds().t);
        objects.dropSnowman(*best, static_cast<float>(bestCol) * best->hm.step(), n, radii, topY - 12.0f * dpi, dpi,
                            eventRng);
        return true;
    }

    void scheduleEvents(float dt) {
        if (!animations() || !settings.accumulation.enabled) return;
        if (settings.accumulation.fallingSnowmen) {
            ev.snowmanEval += dt;
            if (ev.snowmanEval >= kSnowmanEvalPeriod) {
                ev.snowmanEval -= kSnowmanEvalPeriod;
                ev.heavySnow = evaluateHeavySnow();
                // The countdown runs while there is heavy snow (or, without that rule, while
                // there is somewhere to land).
                const bool gate = settings.accumulation.snowmenNeedHeavySnow ? ev.heavySnow : hasLandingSpot();
                if (gate) {
                    if (ev.snowmanCountdown < 0) ev.snowmanCountdown = snowmanWait();
                    ev.snowmanCountdown -= kSnowmanEvalPeriod;
                    if (ev.snowmanCountdown <= 0) {
                        const int maxAlive = std::clamp(settings.accumulation.snowmenMaxAlive, 1, 10);
                        if (objects.autoSnowmenAlive() < maxAlive && spawnAutoSnowman()) {
                            ev.snowmanCountdown = snowmanWait();
                            ++ev.snowmenDropped;
                        } else {
                            ev.snowmanCountdown = 2.0f * kSnowmanEvalPeriod;   // retry soon
                        }
                    }
                }
            }
        }
        if (settings.accumulation.snowplow && !thaw()) {
            ev.plowEval += dt;
            if (ev.plowEval >= kPlowEvalPeriod) {
                ev.plowEval -= kPlowEvalPeriod;
                const float delay = 60.0f * static_cast<float>(std::clamp(settings.accumulation.snowplowDelayMinutes, 1, 120));
                for (PlowRun& run : plowRuns) {   // each route has its own timer
                    if (plowOf(run).active() || run.pending >= 0) {
                        run.fullSeconds = 0;
                        continue;
                    }
                    float avg = 0;
                    bool isFull = false;
                    routeFullness(run.route, avg, isFull);
                    if (isFull) run.fullSeconds += kPlowEvalPeriod;
                    else if (avg < kPlowEmptyAverage) run.fullSeconds = 0;   // cleared / melted away
                    else run.fullSeconds = std::max(0.0f, run.fullSeconds - 2.0f * kPlowEvalPeriod);
                    if (run.fullSeconds >= delay + run.offset) startRun(run);
                    if (run.main) ev.plowFullSeconds = run.fullSeconds;
                }
            }
        }
    }

    bool thaw() const { return settings.weather.preset == WeatherPreset::Thaw; }

    // Umbrella dome over the ambient pointer, and a gust when it is swiped fast.
    void updatePointer(float dt) {
        const bool on = apValid && settings.interaction.umbrellaCursor;
        const float dpi = reg.dpiScaleAt(apX, apY);
        particles.setUmbrella(apX, apY, (interaction ? kUmbrellaInteractAt96 : kUmbrellaAt96) * dpi, on);
        if (on && apHavePrev && dt > 0) {
            const float vx = (apX - apPrevX) / dt, vy = (apY - apPrevY) / dt;
            const float speed = std::sqrt(vx * vx + vy * vy);
            if (speed > kSwipeSpeedAt96 * dpi) {
                const float k = std::min(speed * 0.35f, kSwipeGustMaxAt96 * dpi) / speed;
                particles.setGust(apX, apY, vx * k, vy * k, kSwipeRadiusAt96 * dpi);
            }
        }
        apPrevX = apX;
        apPrevY = apY;
        apHavePrev = apValid;
    }

    // Thaw: wet look, slow melting, drops falling off window edges.
    void thawStep(float dt) {
        const bool t = thaw();
        if (t) wet = std::min(kWetnessMax, wet + dt * kWetnessMax / kWetnessRiseSeconds);
        else wet = std::max(0.0f, wet - dt * kWetnessMax / kWetnessFallSeconds);
        if (!t) return;
        meltAcc += dt;
        if (meltAcc >= 0.5f) {
            const float span = meltAcc;
            meltAcc = 0;
            for (Surface& s : reg.surfaces()) {
                if (!s.active || s.hm.peak() <= 0) continue;
                const float d = s.dpiScale;
                const float base = d / kMeltSecondsPerPx * span;
                for (int i = 0; i < s.hm.columns(); ++i) {
                    const float h = s.hm.height(i);
                    if (h <= 0) continue;
                    const float thin = std::clamp(1.0f - h / (8.0f * d), 0.0f, 1.0f);   // thin snow goes first
                    s.hm.lowerTo(i, h - base * (1.0f + 1.5f * thin));
                }
            }
        }
        ParticleSystem* p = fx();
        if (!p) return;
        int snowy = 0;
        for (const Surface& s : reg.surfaces()) snowy += (s.active && s.kind == SurfaceKind::Window && s.hm.peak() > 1.0f) ? 1 : 0;
        if (snowy == 0) return;
        dropAcc += dt * kDripRatePerWindow * static_cast<float>(snowy);
        while (dropAcc >= 1.0f) {
            dropAcc -= 1.0f;
            int pick = static_cast<int>(dropPick++ % static_cast<std::uint32_t>(snowy));
            for (const Surface& s : reg.surfaces()) {
                if (!(s.active && s.kind == SurfaceKind::Window && s.hm.peak() > 1.0f)) continue;
                if (pick-- > 0) continue;
                // A lower end of the drift: the first snowy column from either end.
                const bool fromLeft = (dropPick & 1u) != 0;
                const int n = s.hm.columns();
                for (int k = 0; k < n; ++k) {
                    const int i = fromLeft ? k : n - 1 - k;
                    if (s.hm.height(i) > 1.0f * s.dpiScale) {
                        p->addDroplet(s.worldX(i), static_cast<float>(s.top) + 1.0f, 0.0f, 10.0f * s.dpiScale, s.dpiScale);
                        break;
                    }
                }
                break;
            }
        }
    }

    bool startRun(PlowRun& run) {
        if (thaw()) return false;   // no snowplow in a thaw
        run.pending = -1;
        if (!plowOf(run).start(run.route, run.nextDir)) return false;
        run.startedAt = ev.time;
        if (run.main) {
            plowStartedAt = ev.time;
            ev.plowNextDir = -run.nextDir;
            ev.plowFullSeconds = 0;
        }
        run.nextDir = -run.nextDir;   // alternate the side it comes from
        run.fullSeconds = 0;          // the delay restarts after the run
        return true;
    }
    bool startPlow() {   // the primary route (compatibility)
        for (PlowRun& run : plowRuns)
            if (run.main) return startRun(run);
        return false;
    }

    void applyStroke(float x0, float y0, float x1, float y1) {
        const float dpi = reg.dpiScaleAt(x1, y1);
        if (tool == Tool::Mitten) mittenStroke(reg, x0, y0, x1, y1, brushRadius(dpi), fx(), dpi);
        else if (tool == Tool::Shovel) shovelStroke(reg, x0, y0, x1, y1, shovelWidth(dpi), fx(), dpi);
    }

    float toolReach(float dpi) const {
        if (tool == Tool::Mitten) return brushRadius(dpi);
        if (tool == Tool::Shovel) return 0.5f * shovelWidth(dpi);
        return 0.0f;
    }

    void releaseHeld() {
        if (!objects.holding()) return;
        // Blend the smoothed velocity with the travel since the last step (assumed 1/60 s).
        const float vx = 0.5f * ptrVx + 0.5f * ptrAccX * 60.0f;
        const float vy = 0.5f * ptrVy + 0.5f * ptrAccY * 60.0f;
        objects.release(vx, vy, reg, fx());
    }

    // Mitten / shovel: every snowy run expanded by the tool's reach in all directions (so a
    // press slightly off the snow is still ours, not a desktop rubber-band selection);
    // snowball: tight rects of visible snow only.
    void appendDriftHitRects(std::vector<RectI>& out) const {
        for (const Surface& s : reg.surfaces()) {
            if (!s.active || s.clip.empty() || s.hm.peak() < kHitSnowMin) continue;
            const int n = s.hm.columns();
            const int pad = static_cast<int>(4.0f * s.dpiScale);
            const int padUp = static_cast<int>(6.0f * s.dpiScale);
            int runStart = -1, runEnd = -1;
            float runMax = 0;
            auto flush = [&]() {
                if (runStart < 0) return;
                const RectI r{static_cast<int>(s.worldX(runStart)) - pad,
                              s.top - static_cast<int>(std::ceil(runMax)) - padUp,
                              static_cast<int>(s.worldX(runEnd)) + pad + 1, s.top};
                const int reach = static_cast<int>(std::ceil(toolReach(s.dpiScale)));
                if (reach > 0) {
                    const RectI e{r.l - reach, r.t - reach, r.r + reach, r.b + reach};
                    for (const RectI& m : reg.monitorRects()) {
                        const RectI x = e.intersect(m);
                        if (!x.empty()) out.push_back(x);
                    }
                } else {
                    for (const RectI& c : s.clip) {
                        const RectI x = r.intersect(c);
                        if (!x.empty()) out.push_back(x);
                    }
                }
                runStart = -1;
            };
            for (int i = 0; i < n; ++i) {
                const bool snowy = s.hm.height(i) > kHitSnowMin && !s.hm.blocked(i);
                if (!snowy) continue;
                if (runStart >= 0 && i - runEnd > kHitGapColumns) flush();
                if (runStart < 0) {
                    runStart = i;
                    runMax = 0;
                }
                runEnd = i;
                runMax = std::max(runMax, s.hm.height(i));
            }
            flush();
        }
    }
};

WorldInternals WorldTestAccess::get(World& w) {
    World::Impl& i = *w.impl_;
    return WorldInternals{i.reg,      i.particles, i.objects,   i.weather, i.plow,  i.ev, i.plowRuns,
                          i.chunks,   i.icicles,   i.splats,    i.fireworks, i.wet, i.scoop,
                          static_cast<int>(i.gesture), i.tool, i.interaction};
}
bool WorldTestAccess::dropSnowman(World& w) { return w.impl_->spawnAutoSnowman(); }
bool WorldTestAccess::heavySnow(World& w) { return w.impl_->evaluateHeavySnow(); }
float WorldTestAccess::snowmanWait(World& w) { return w.impl_->snowmanWait(); }

World::World(std::uint64_t seed) : impl_(std::make_unique<Impl>(seed)) {}
World::~World() = default;

void World::applySettings(const Settings& settings, const RuntimeTuning& tuning) {
    Impl& d = *impl_;
    const bool accChanged = !d.settingsApplied || !(settings.accumulation == d.settings.accumulation);
    if (settings.accumulation.snowmenIntervalMinutes != d.settings.accumulation.snowmenIntervalMinutes ||
        settings.accumulation.snowmenNeedHeavySnow != d.settings.accumulation.snowmenNeedHeavySnow)
        d.ev.snowmanCountdown = -1;   // don't wait out an old (long) countdown
    if (!d.settingsApplied || settings.interaction.tool != d.settingsTool) {
        d.settingsTool = settings.interaction.tool;
        d.tool = d.settingsTool;
    }
    d.settings = settings;
    d.tuning = tuning;
    d.settingsApplied = true;
    d.weather.configure(settings.weather);
    d.particles.setSkyPerMonitor(settings.weather.skyPerMonitor);
    d.reg.setObjectsOnAllMonitors(settings.interaction.objectsOnAllMonitors);
    d.objects.setSkin(skinIndex(settings.interaction.snowmanSkin));
    d.updateObstacles();
    if (!d.animations()) {
        d.particles.clear();
        d.chunks.clear();
        d.fireworks.clear();
    }
    if (d.haveLayout && accChanged) d.rebuildSurfaces(1.0f);
    d.updateTarget();
}

void World::setLayout(const DesktopLayout& layout) {
    Impl& d = *impl_;
    d.layout = layout;
    d.haveLayout = true;
    d.rebuildSurfaces(std::max(d.sinceLayout, 1.0f / 60.0f));
    d.sinceLayout = 0;
}

void World::setInteractionMode(bool active) {
    Impl& d = *impl_;
    if (d.interaction == active) return;
    d.interaction = active;
    if (!active) {
        d.endGesture();
        d.releaseHeld();
    }
}

void World::handleInput(const InputEvent& ev) {
    Impl& d = *impl_;
    using G = Impl::Gesture;
    if (ev.type == InputEvent::Type::CycleTool) {
        d.endGesture();
        d.tool = d.tool == Tool::Mitten ? Tool::Shovel : (d.tool == Tool::Shovel ? Tool::Snowball : Tool::Mitten);
        return;
    }
    const float prevX = d.hasPointer ? d.px : ev.x;
    const float prevY = d.hasPointer ? d.py : ev.y;
    d.px = ev.x;
    d.py = ev.y;
    d.hasPointer = true;
    d.ptrAccX += ev.x - prevX;
    d.ptrAccY += ev.y - prevY;
    if (!d.interaction) return;
    const float dpi = d.reg.dpiScaleAt(ev.x, ev.y);

    switch (ev.type) {
        case InputEvent::Type::PointerMove:
            if (d.objects.holding()) {
                d.objects.moveHeld(prevX, prevY, ev.x, ev.y, d.reg);
                break;
            }
            switch (d.gesture) {
                case G::Pending: {   // mitten: swipe (brush) or press-and-hold (sculpt)?
                    const float travel = std::hypot(ev.x - d.pressX, ev.y - d.pressY);
                    if (travel > kGestureTravelAt96 * dpi) {
                        const float elapsed = static_cast<float>(d.ev.time - d.pressT);
                        if (travel / std::max(elapsed, 1.0f / 60.0f) > kSwipeBrushSpeedAt96 * dpi) {
                            d.gesture = G::Brush;
                            d.applyStroke(d.pressX, d.pressY, ev.x, ev.y);
                        } else {
                            d.beginSculptOrBrush();
                        }
                    }
                    break;
                }
                case G::Brush:
                    d.applyStroke(prevX, prevY, ev.x, ev.y);
                    break;
                case G::Sculpt: {
                    d.rubAcc += std::hypot(ev.x - prevX, ev.y - prevY);
                    const SculptState& sc = d.objects.sculpt();
                    const Surface* s = d.reg.find(sc.surface);
                    const float sx = s ? static_cast<float>(s->left) + sc.localX : d.pressX;
                    if (std::fabs(ev.x - sx) > kSculptLeaveAt96 * dpi || std::fabs(ev.y - d.pressY) > kSculptLeaveAt96 * dpi) {
                        d.objects.sculptEnd(d.reg, d.fx());   // dragged away: back to brushing
                        d.gesture = G::Brush;
                        d.applyStroke(prevX, prevY, ev.x, ev.y);
                    }
                    break;
                }
                case G::Scoop:
                    d.shovelKnock(prevX, prevY, ev.x, ev.y);
                    scoopStroke(d.reg, prevX, prevY, ev.x, ev.y, d.shovelWidth(dpi), d.scoop, d.scoopCapacity(dpi), d.fx(), dpi);
                    break;
                default:
                    break;
            }
            break;
        case InputEvent::Type::PointerDown:
            if (d.objects.holding() || d.gesture != G::None) break;
            if (d.objects.grabAt(ev.x, ev.y, d.reg)) break;   // any tool can pick up a ball
            if (d.tool == Tool::Snowball) {
                if (!d.reg.inObjectArea(ev.x, ev.y)) break;   // objects live in the desktop layer only
                d.objects.startBall(ev.x, ev.y, dpi);
                d.objects.moveHeld(ev.x, ev.y, ev.x, ev.y, d.reg);   // gather at the contact point
            } else if (d.tool == Tool::Mitten) {
                d.brushing = true;
                d.gesture = G::Pending;
                d.pressX = ev.x;
                d.pressY = ev.y;
                d.pressT = d.ev.time;
            } else {
                d.brushing = true;
                d.gesture = G::Scoop;
                d.scoop = 0;
                d.shovelKnock(ev.x, ev.y, ev.x, ev.y);
                scoopStroke(d.reg, ev.x, ev.y, ev.x, ev.y, d.shovelWidth(dpi), d.scoop, d.scoopCapacity(dpi), d.fx(), dpi);
            }
            break;
        case InputEvent::Type::PointerUp:
            d.endGesture();
            d.releaseHeld();
            break;
        default:
            break;
    }
}

void World::setEmissionEnabled(bool on) { impl_->particles.setEmission(on); }

void World::clearAllSnow() {
    Impl& d = *impl_;
    d.reg.clearAllSnow();
    d.objects.clear();
    for (PlowRun& run : d.plowRuns) {
        d.plowOf(run).abort();
        run.pending = -1;
    }
    d.plow.abort();
    d.splats.clear();
}

void World::triggerSnowplow() {
    Impl& d = *impl_;
    if (d.thaw()) {
        SNOW_LOGI("snowplow: not started (thaw)");
        return;
    }
    if (d.plowRuns.empty()) {
        int taskbars = 0, active = 0;
        for (const Surface& s : d.reg.surfaces())
            if (s.kind == SurfaceKind::Taskbar) {
                ++taskbars;
                if (s.active) ++active;
            }
        const RectI p = d.reg.primaryRect();
        SNOW_LOGI("snowplow: not started - no taskbar route (primary monitor %d,%d %dx%d); taskbar surfaces %d, active %d",
                  p.l, p.t, p.width(), p.height(), taskbars, active);
        return;
    }
    // Every idle route, the primary first, the others staggered by 1.5-4 s.
    float delay = 0;
    for (std::size_t i = 0; i < d.plowRuns.size(); ++i) {
        PlowRun& run = d.plowRuns[i];
        const Snowplow& pl = d.plowOf(run);
        if (pl.active()) {
            SNOW_LOGI("snowplow: route %zu (%.0f..%.0f) already running (phase %d, %.1f s)", i, run.route.left,
                      run.route.right, static_cast<int>(pl.phase()), d.ev.time - run.startedAt);
            continue;
        }
        if (run.pending >= 0) {
            SNOW_LOGI("snowplow: route %zu already queued (%.1f s)", i, run.pending);
            continue;
        }
        if (delay <= 0) {
            SNOW_LOGI("snowplow: route %zu (%.0f..%.0f%s) %s", i, run.route.left, run.route.right,
                      run.main ? ", primary" : "", d.startRun(run) ? "started" : "start refused");
            delay = d.eventRng.range(kPlowStaggerMin, kPlowStaggerMax);
        } else {
            run.pending = delay;
            SNOW_LOGI("snowplow: route %zu (%.0f..%.0f) starts in %.1f s", i, run.route.left, run.route.right, delay);
            delay += d.eventRng.range(kPlowStaggerMin, kPlowStaggerMax);
        }
    }
}

void World::setAmbientPointer(float x, float y, bool valid) {
    Impl& d = *impl_;
    d.apX = x;
    d.apY = y;
    if (!valid) d.apHavePrev = false;
    d.apValid = valid;
}

void World::triggerFireworks() {
    Impl& d = *impl_;
    if (!d.animations()) return;
    d.fireworks.start(kManualShowSeconds, d.reg.primaryRect(), d.reg.primaryDpi());
}

void World::setLocalTime(int year, int month, int day, int secondsOfDay) {
    Impl& d = *impl_;
    d.objects.setFestiveSeason((month == 12 && day >= 20) || (month == 1 && day <= 7));
    const bool newYear = month == 1 && day == 1;
    // Midnight: the date rolled from Dec 31 to Jan 1, or we are running during the first minute.
    const bool rolled = d.haveDate && d.lastMonth == 12 && d.lastDay == 31 && newYear;
    const bool firstMinute = newYear && secondsOfDay < 60;
    if ((rolled || firstMinute) && d.firedYear != year && d.settings.festive.fireworks && d.animations()) {
        d.firedYear = year;
        d.fireworks.start(kNewYearShowSeconds, d.reg.primaryRect(), d.reg.primaryDpi());
    }
    d.haveDate = true;
    d.lastMonth = month;
    d.lastDay = day;
}

void World::step(float dt) {
    Impl& d = *impl_;
    const auto t0 = std::chrono::steady_clock::now();
    if (!(dt > 0)) dt = 0;
    dt = std::min(dt, kMaxDt);
    d.sinceLayout += dt;
    d.ev.time += dt;
    ++d.stepIndex;

    d.weather.update(dt);
    d.updatePointer(dt);
    ParticleSystem* fx = d.fx();
    if (fx) d.particles.step(dt, d.weather, d.settings.accumulation.enabled ? &d.reg : nullptr);

    // Relaxation only where something changed since the surface last came to rest.
    for (Surface& s : d.reg.surfaces()) {
        if (!s.active || !s.hm.dirty()) continue;
        RelaxParams rp = d.relaxParams;
        rp.openLeft = rp.openRight = s.kind == SurfaceKind::Window;
        const RelaxResult r = s.hm.relax(rp);
        if (!fx) continue;
        // Snow sliding off the ends of a window edge falls as a flake of matching size.
        for (int side = 0; side < 2; ++side) {
            const float lost = side == 0 ? r.lostLeft : r.lostRight;
            if (lost < 0.3f) continue;
            const float radius = std::clamp(std::sqrt(lost / ParticleSystem::kDepositArea), 1.0f, 3.0f * s.dpiScale);
            const float dir = side == 0 ? -1.0f : 1.0f;
            fx->addLoose(side == 0 ? static_cast<float>(s.left) - 2.0f : static_cast<float>(s.right) + 2.0f,
                         static_cast<float>(s.top) - 1.0f, dir * 20.0f * s.dpiScale, 20.0f * s.dpiScale, radius);
        }
    }
    // After relaxation, so the drawn heap matches the heights it was solved against.
    // Watchdog: a run normally takes well under a minute (screen width / speed + dump pause);
    // never let a stuck run block the snowplow forever.
    for (std::size_t i = 0; i < d.plowRuns.size(); ++i) {
        PlowRun& run = d.plowRuns[i];
        Snowplow& pl = d.plowOf(run);
        if (run.pending >= 0 && (run.pending -= dt) < 0) {   // staggered manual start
            if (!d.thaw()) SNOW_LOGI("snowplow: route %zu %s", i, d.startRun(run) ? "started" : "start refused");
            run.pending = -1;
        }
        if (pl.active() && d.ev.time - run.startedAt > kPlowMaxRunSeconds) {
            SNOW_LOGW("snowplow: route %zu run exceeded %.0f s (phase %d) - aborted", i, kPlowMaxRunSeconds,
                      static_cast<int>(pl.phase()));
            pl.abort();
        }
        pl.step(dt, d.reg, d.objects, fx);
    }
    if ((d.stepIndex & 31u) == 0) d.reg.refreshBlockedAll();

    if (d.gesture == Impl::Gesture::Pending && d.ev.time - d.pressT >= kSculptDecideSeconds) d.beginSculptOrBrush();
    if (d.gesture == Impl::Gesture::Sculpt && dt > 0) {
        const float dpi = d.reg.dpiScaleAt(d.pressX, d.pressY);
        const float rub = 1.0f + std::min(2.0f, d.rubAcc / dt / (kSculptRubFullAt96 * dpi));
        d.rubAcc = 0;
        d.objects.sculptStep(dt, d.reg, fx, rub);
    }
    d.objects.setMelt(d.thaw() ? 1.0f : 0.0f);
    d.objects.step(dt, d.reg, fx);
    if (fx) d.chunks.step(dt, d.reg, fx);
    d.chunkKnocks();
    if (fx && d.chunks.count() > 0 && d.icicles.attachedCount() > 0) {   // a flung load breaks icicles too
        for (Chunk& c : d.chunks.chunks()) {
            if (!c.soft) continue;
            const int n = d.icicles.hitSegment(d.reg, c.x - c.vx * dt, c.y - c.vy * dt, c.x, c.y, 0.5f * c.halfW, c.vx, c.vy,
                                               kIcicleBreakSpeedAt96 * c.dpi, fx);
            for (int i = 0; i < n; ++i) {
                c.vx *= 0.75f;
                c.vy *= 0.75f;
            }
        }
    }
    {   // a snowman's base crumbled by the shovel lands on the load (up to capacity)
        const float m = d.objects.takeCrumbled(KnockBy::Shovel);
        if (m > 0) {
            const float dpi = d.reg.dpiScaleAt(d.px, d.py);
            float rest = m;
            if (d.gesture == Impl::Gesture::Scoop) {
                const float add = std::min(rest, std::max(0.0f, d.scoopCapacity(dpi) - d.scoop));
                d.scoop += add;
                rest -= add;
            }
            if (rest > 0.5f && fx) {
                Chunk* c = d.chunks.spawnFree(d.px, d.py - 6.0f * dpi, std::clamp(0.5f * std::sqrt(rest), 4.0f * dpi, 25.0f * dpi),
                                              4.0f * dpi, rest, 0.0f, 0.0f, dpi, 0);
                if (c) c->soft = true;
            }
        }
        d.objects.takeCrumbled(KnockBy::Hit);   // (Hit: everything already went into pieces)
    }
    d.icicles.step(dt, d.reg, fx, d.settings.accumulation.icicles && d.settings.accumulation.enabled, d.thaw());
    d.splats.step(dt, d.reg, fx ? &d.chunks : nullptr, fx);
    if (fx) d.fireworks.step(dt);
    d.thawStep(dt);
    d.scheduleEvents(dt);

    if (dt > 0) {
        d.ptrVx = 0.5f * d.ptrVx + 0.5f * d.ptrAccX / dt;
        d.ptrVy = 0.5f * d.ptrVy + 0.5f * d.ptrAccY / dt;
        d.ptrAccX = d.ptrAccY = 0;
    }
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    d.stepMs = d.stepIndex <= 1 ? ms : d.stepMs + (ms - d.stepMs) * 0.02f;
}

void World::buildSnapshot(FrameSnapshot& out) const {
    const Impl& d = *impl_;

    // Drifts.
    std::size_t nd = 0;
    for (const Surface& s : d.reg.surfaces())
        if (s.active && !s.clip.empty()) ++nd;
    out.drifts.resize(nd);
    nd = 0;
    const float driftOpacity = static_cast<float>(std::clamp(d.settings.accumulation.driftOpacityPercent, 10, 100)) / 100.0f;
    const float flakeOpacity = static_cast<float>(std::clamp(d.settings.weather.opacityPercent, 10, 100)) / 100.0f;
    for (const Surface& s : d.reg.surfaces()) {
        if (!s.active || s.clip.empty()) continue;
        DriftStrip& ds = out.drifts[nd++];
        ds.id = s.id;
        ds.x0 = static_cast<float>(s.left);
        ds.baseY = static_cast<float>(s.top);
        ds.step = s.hm.step();
        ds.dpiScale = s.dpiScale;
        ds.opacity = driftOpacity;
        ds.wetness = d.wet;
        ds.heights.assign(s.hm.heights().begin(), s.hm.heights().end());
        ds.version = s.hm.version();
        for (const PlowRun& run : d.plowRuns) {   // the snow pushed ahead of a plow's blade
            const Snowplow& pl = d.plowOf(run);
            if (!pl.onSurface(s.id)) continue;
            pl.applyHump(s, ds.heights);
            ds.version = s.hm.version() + pl.renderVersion();
        }
        ds.clip.assign(s.clip.begin(), s.clip.end());
    }

    // Desktop layer: snowballs, snowmen, snowplow. One batch, behind every window, on the
    // primary monitor only.
    {
        const std::vector<RectI>& clip = d.reg.desktopClip();
        if (out.objects.empty()) out.objects.emplace_back();
        SpriteBatch& batch = out.objects.front();
        batch.clip.assign(clip.begin(), clip.end());
        batch.sprites.clear();
        if (!clip.empty()) d.objects.appendSprites(d.reg, batch.sprites);
        out.objects.resize(batch.sprites.empty() ? 0 : 1);
    }
    // Snowplows: on their route's monitors (even when other objects are primary-only),
    // behind all windows.
    for (const PlowRun& run : d.plowRuns) {
        const Snowplow& pl = d.plowOf(run);
        if (!pl.active()) continue;
        out.objects.emplace_back();
        SpriteBatch& b = out.objects.back();
        clipRegion(d.reg.sceneBounds(), run.route.monitors, d.reg.windowRects(), b.clip, d.scratchA, d.scratchB);
        if (!b.clip.empty()) pl.appendSprite(d.reg, b.sprites);
        if (b.sprites.empty()) out.objects.pop_back();
    }
    // Garland along every taskbar (desktop layer of its monitor).
    if (d.settings.festive.garland) {
        for (const Surface& s : d.reg.surfaces()) {
            if (s.kind != SurfaceKind::Taskbar || !s.active || s.deskClip.empty()) continue;
            out.objects.emplace_back();
            SpriteBatch& g = out.objects.back();
            g.clip = s.deskClip;
            float plowX = std::numeric_limits<float>::quiet_NaN();
            for (const PlowRun& run : d.plowRuns)
                if (d.plowOf(run).onSurface(s.id)) plowX = d.plowOf(run).centerLocal();
            buildGarland(s, static_cast<float>(d.ev.time), plowX, g.sprites);
            if (g.sprites.empty()) out.objects.pop_back();
        }
    }
    d.icicles.appendBatches(d.reg, out.objects);   // below their window, behind windows in front of it
    d.splats.appendBatches(d.reg, out.objects);    // on the window glass

    // Falling flakes and dust.
    out.particles.clip.clear();
    out.particles.sprites.clear();
    for (const Particle& p : d.particles.particles()) {
        SpriteInstance sp;
        sp.x = p.x;
        sp.y = p.y;
        sp.vx = p.vx;
        sp.vy = p.vy;
        sp.halfW = sp.halfH = p.radius;
        sp.rotation = p.rotation;
        sp.opacity = flakeOpacity * d.particles.edgeFade(p) *
                     ((p.flags & Particle::kDust) ? p.opacity * std::clamp(1.5f * p.life / p.maxLife, 0.0f, 1.0f) : p.opacity);
        sp.shape = Shape::Flake;
        sp.variant = p.variant;
        if (p.flags & Particle::kDroplet) {
            sp.shape = Shape::Droplet;
            sp.variant = 0;
            sp.halfW = 0.8f * p.radius;
            sp.halfH = 1.4f * p.radius;
            sp.rotation = 0;
        }
        out.particles.sprites.push_back(sp);
    }
    d.icicles.appendFalling(out.particles.sprites, flakeOpacity);
    if (d.settings.system.excludedSnowMode == ExcludedSnowMode::Behind && !d.reg.excludedRects().empty()) {
        clipRegion(d.reg.sceneBounds(), d.reg.monitorRects(), d.reg.excludedRects(), out.particles.clip, d.scratchA,
                   d.scratchB);   // flakes, dust, chunks, sparks: never drawn over excluded applications
        // Fully covered (e.g. an excluded fullscreen app): an empty clip would mean "unclipped",
        // so keep one empty rect, which draws nothing.
        if (out.particles.clip.empty()) out.particles.clip.push_back(RectI{});
    }
    d.fireworks.appendSprites(out.particles.sprites);
    d.chunks.appendSprites(out.particles.sprites, flakeOpacity);   // broken-off cap slabs

    // Tool cursor.
    out.cursor.clip.clear();
    out.cursor.sprites.clear();
    out.hitRegions.clear();
    if (d.interaction) {
        if (d.hasPointer) {
            const float dpi = d.reg.dpiScaleAt(d.px, d.py);
            const bool held = d.objects.holding();
            // A subtle brush ring only while hovering (never during a stroke: no "selection").
            if (!held && !d.brushing && d.tool != Tool::Snowball) {
                SpriteInstance ring;
                ring.x = d.px;
                ring.y = d.py;
                ring.halfW = ring.halfH = d.toolReach(dpi);
                ring.opacity = kRingOpacity;
                ring.shape = Shape::Ring;
                out.cursor.sprites.push_back(ring);
            }
            // The tool sprite is the cursor (hotspot on the pointer). A held ball is drawn
            // itself in the objects batch.
            if (!held) {
                out.cursor.sprites.push_back(toolCursorSprite(d.tool, d.px, d.py, dpi));
            if (d.gesture == Impl::Gesture::Scoop && d.scoop > 0.5f) {   // the load riding on the blade
                SpriteInstance heap;
                heap.halfW = std::clamp(0.5f * std::sqrt(d.scoop), 4.0f * dpi, 22.0f * dpi);
                heap.halfH = std::clamp(d.scoop / (3.2f * heap.halfW), 3.0f * dpi, 14.0f * dpi);
                heap.x = d.px + 6.0f * dpi;
                heap.y = d.py - heap.halfH - 1.0f * dpi;
                heap.rotation = 0.3f * kShovelCursorRotation;
                heap.shape = Shape::SnowChunk;
                heap.variant = 1;
                out.cursor.sprites.push_back(heap);
            }
            } else if (const Ball* b = d.objects.heldBall()) {
                // The held ball lives behind windows: a faint foreground marker shows the pointer.
                SpriteInstance m;
                m.x = d.px;
                m.y = d.py;
                m.halfW = m.halfH = b->r;
                m.opacity = 0.45f;
                m.shape = Shape::Snowball;
                SpriteInstance ring = m;
                ring.halfW = ring.halfH = b->r + 3.0f * b->dpi;
                ring.opacity = 0.25f;
                ring.shape = Shape::Ring;
                out.cursor.sprites.push_back(ring);
                out.cursor.sprites.push_back(m);
            }
        }
        d.appendDriftHitRects(out.hitRegions);
        d.objects.appendHitRects(d.reg, out.hitRegions);
    }

    out.stats.particles = static_cast<std::uint32_t>(d.particles.particles().size() + d.chunks.count());
    out.stats.surfaces = static_cast<std::uint32_t>(d.reg.activeCount());
    out.stats.snowballs = static_cast<std::uint32_t>(d.objects.snowballCount());
    out.stats.snowmen = static_cast<std::uint32_t>(d.objects.snowmanCount());
    out.stats.stepMs = d.stepMs;
}

std::size_t World::particleCount() const { return impl_->particles.particles().size() + impl_->chunks.count(); }
std::size_t World::surfaceCount() const { return impl_->reg.surfaces().size(); }
double World::totalSnowMass() const { return impl_->reg.totalMass(); }

}  // namespace snow
