#include "interaction/snowplow.h"

#include <algorithm>
#include <cmath>

#include "interaction/snow_objects.h"
#include "simulation/particles.h"
#include "surfaces/surface_registry.h"

namespace snow {

namespace {
float hash01(std::uint64_t seed, long long idx) {
    std::uint64_t z = seed ^ (static_cast<std::uint64_t>(idx) * 0xD1B54A32D192ED03ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
}
// Smooth value noise in [0, 1].
float vnoise(std::uint64_t seed, float u) {
    const float fl = std::floor(u);
    const long long i = static_cast<long long>(fl);
    const float f = u - fl;
    const float s = f * f * (3.0f - 2.0f * f);
    const float a = hash01(seed, i), b = hash01(seed, i + 1);
    return a + (b - a) * s;
}
bool usableTaskbar(const Surface& s) { return s.kind == SurfaceKind::Taskbar && s.active && s.hm.columns() > 0; }
}  // namespace

const Surface* Snowplow::primaryTaskbar(const SurfaceRegistry& reg) {
    const RectI prim = reg.primaryRect();
    if (prim.empty()) return nullptr;
    for (const Surface& s : reg.surfaces()) {
        if (!usableTaskbar(s)) continue;
        if (s.top > prim.t && s.top <= prim.b && s.left < prim.r && s.right > prim.l) return &s;
    }
    return nullptr;
}

std::vector<PlowRoute> buildPlowRoutes(const SurfaceRegistry& reg, bool all) {
    std::vector<PlowRoute> routes;
    const Surface* prim = Snowplow::primaryTaskbar(reg);
    std::vector<const Surface*> bars;
    for (const Surface& s : reg.surfaces())
        if (usableTaskbar(s) && (all || &s == prim)) bars.push_back(&s);
    std::sort(bars.begin(), bars.end(), [](const Surface* a, const Surface* b) { return a->left < b->left; });
    std::vector<bool> used(bars.size(), false);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (used[i]) continue;
        PlowRoute r;
        const Surface* cur = bars[i];
        used[i] = true;
        r.surfaces.push_back(cur->id);
        r.left = static_cast<float>(cur->left);
        r.right = static_cast<float>(cur->right);
        r.top = static_cast<float>(cur->top);
        r.dpi = cur->dpiScale;
        r.primary = cur == prim;
        // Extend to the right with taskbars at the same height that touch this one.
        for (bool grew = true; grew;) {
            grew = false;
            for (std::size_t j = 0; j < bars.size(); ++j) {
                if (used[j]) continue;
                const Surface* n = bars[j];
                const float tol = kRouteJoinYAt96 * std::max(n->dpiScale, cur->dpiScale);
                if (std::fabs(static_cast<float>(n->top) - r.top) <= tol &&
                    static_cast<float>(n->left) >= r.right - kRouteJoinGap - 1.0f &&
                    static_cast<float>(n->left) - r.right <= kRouteJoinGap) {
                    used[j] = true;
                    r.surfaces.push_back(n->id);
                    r.right = std::max(r.right, static_cast<float>(n->right));
                    r.dpi = std::max(r.dpi, n->dpiScale);
                    r.primary = r.primary || n == prim;
                    cur = n;
                    grew = true;
                }
            }
        }
        std::uint64_t key = 0xC0FFEEull;
        for (SurfaceId id : r.surfaces) key = (key ^ id) * 0x100000001B3ull;
        r.key = key;
        for (const MonitorInfo& m : reg.monitors()) {
            const RectI& b = m.bounds;
            if (b.empty() || static_cast<float>(b.l) >= r.right || static_cast<float>(b.r) <= r.left) continue;
            if (r.top > static_cast<float>(b.t) && r.top <= static_cast<float>(b.b)) r.monitors.push_back(b);
        }
        routes.push_back(r);
    }
    // The primary route first.
    std::stable_sort(routes.begin(), routes.end(), [](const PlowRoute& a, const PlowRoute& b) { return a.primary && !b.primary; });
    return routes;
}

bool Snowplow::start(const SurfaceRegistry& reg, int dir) {
    const std::vector<PlowRoute> routes = buildPlowRoutes(reg, false);
    if (routes.empty()) return false;
    return start(routes.front(), dir);
}

bool Snowplow::start(const PlowRoute& route, int dir) {
    if (active() || route.surfaces.empty() || route.right <= route.left) return false;
    route_ = route;
    dpi_ = route.dpi > 0.1f ? route.dpi : 1.0f;
    step_ = static_cast<float>(SurfaceRegistry::stepFor(dpi_));
    halfW_ = kPlowHalfWAt96 * dpi_;
    halfH_ = kPlowHalfHAt96 * dpi_;
    speed_ = kPlowSpeedAt96 * dpi_;
    bladeH_ = kBladeHeightShare * 2.0f * halfH_;
    dir_ = dir >= 0 ? 1 : -1;
    x_ = dir_ > 0 ? route.left - halfW_ - 2.0f : route.right + halfW_ + 2.0f;
    H_ = P_ = 0;
    carried_ = collected_ = spilled_ = dumped_ = deposited_ = 0;
    objectsIn_ = dipT_ = 0;
    spillQueue_ = spillBudget_ = rollAcc_ = 0;
    dumpTimer_ = dumpRate_ = dumpTotal_ = 0;
    bounce_ = 0;
    runSeed_ = rng_.next();
    phase_ = Phase::Driving;
    ++renderVersion_;
    return true;
}

bool Snowplow::onSurface(SurfaceId id) const {
    if (!active()) return false;
    for (SurfaceId s : route_.surfaces)
        if (s == id) return true;
    return false;
}

void Snowplow::abortDump(ParticleSystem* fx) {
    if (!active()) return;
    if (carried_ > 0) emitFallAt(frontLocal(), carried_, 1.0f, fx);
    dumped_ += carried_;
    abort();
}

float Snowplow::heapLength() const {
    if (H_ <= 0) return 0;
    const float R = 0.5f * H_ / kReposeSlope;   // crest rounding
    const float T = 0.2f * H_;                  // soft toe
    return P_ + R + (H_ - kReposeSlope * 0.5f * R) / kReposeSlope + T / kReposeSlope;
}

float Snowplow::profile(float d, float H, float P) const {
    if (H <= 0 || d < 0) return 0;
    const float s = kReposeSlope;
    float h;
    if (d <= P) {
        h = H;
    } else {
        const float u = d - P;
        const float R = 0.5f * H / s;   // rounded crest: parabola blending into the slope
        if (u < R) {
            h = H - s * u * u / (2.0f * R);
        } else {
            h = H - s * (u - 0.5f * R);
        }
        const float T = 0.2f * H;       // soft toe: quadratic blend into the ground
        if (h < T) h = h <= -T ? 0.0f : (h + T) * (h + T) / (4.0f * T);
    }
    if (h <= 0) return 0;
    // Lumpy face, fixed relative to the blade (keyed by the distance from it).
    const float amp = std::min(0.08f * H, 2.0f * dpi_) * std::clamp(d / (4.0f * dpi_), 0.0f, 1.0f);
    const float n = (vnoise(runSeed_, d / (12.0f * dpi_)) - 0.5f) * 2.0f * amp;
    return std::max(0.0f, h + n * (h / H));
}

float Snowplow::heightAt(const SurfaceRegistry& reg, float x, bool& inRoute) const {
    (void)reg;
    inRoute = x >= route_.left && x < route_.right;
    if (!inRoute) return 0;
    for (const Surface* s : segs_)   // (resolved once per step)
        if (x >= static_cast<float>(s->left) && x < static_cast<float>(s->right))
            return s->hm.heightAtLocalX(x - static_cast<float>(s->left));
    return 0;   // a seam gap: bare
}

float Snowplow::heapArea(const SurfaceRegistry& /*reg*/, float H, float P, float* offEnd) const {
    // Summed over the columns the drift is drawn with (each surface's own grid), so the drawn
    // heap adds exactly what the plow carries.
    const float front = frontLocal();
    const float fdir = static_cast<float>(dir_);
    const float reach = P + 2.2f * H / kReposeSlope + 6.0f * dpi_;
    const float lo = dir_ > 0 ? front : front - reach, hi = dir_ > 0 ? front + reach : front;
    float area = 0, off = 0;
    for (const Surface* s : segs_) {
        const float step = s->hm.step();
        const float l = static_cast<float>(s->left);
        const int i0 = std::max(0, static_cast<int>(std::ceil((lo - l) / step)));
        const int i1 = std::min(s->hm.columns() - 1, static_cast<int>(std::floor((hi - l) / step)));
        for (int i = i0; i <= i1; ++i) {
            const float wx = l + static_cast<float>(i) * step;
            if (wx < route_.left || wx >= route_.right) continue;
            const float h = profile((wx - front) * fdir, H, P);
            if (h > 0) area += std::max(0.0f, h - s->hm.height(i)) * step;
        }
    }
    // The part sticking out past the end of the route.
    const float endX = dir_ > 0 ? route_.right : route_.left;
    for (float d = std::max(0.0f, (endX - front) * fdir); d <= reach; d += step_) off += profile(d, H, P) * step_;
    if (offEnd) *offEnd = off;
    return area + off;
}

void Snowplow::solve(const SurfaceRegistry& reg, float& overflow, float& offEnd) {
    overflow = 0;
    offEnd = 0;
    if (carried_ <= 0.01f) {
        H_ = P_ = 0;
        return;
    }
    const float Pmax = kPlateauMaxShare * bladeH_;
    if (heapArea(reg, bladeH_, 0, nullptr) >= carried_) {   // grows taller
        float lo = 0, hi = bladeH_;
        for (int it = 0; it < 22; ++it) {
            const float mid = 0.5f * (lo + hi);
            (heapArea(reg, mid, 0, nullptr) < carried_ ? lo : hi) = mid;
        }
        H_ = hi;
        P_ = 0;
    } else {   // at the top of the blade: only grows a bit longer ...
        H_ = bladeH_;
        const float capacity = heapArea(reg, bladeH_, Pmax, nullptr);
        if (capacity < carried_) {   // ... and the rest spills over the blade
            overflow = carried_ - capacity;
            carried_ = capacity;
            P_ = Pmax;
        } else {
            float lo = 0, hi = Pmax;
            for (int it = 0; it < 22; ++it) {
                const float mid = 0.5f * (lo + hi);
                (heapArea(reg, bladeH_, mid, nullptr) < carried_ ? lo : hi) = mid;
            }
            P_ = hi;
        }
    }
    heapArea(reg, H_, P_, &offEnd);
}

float Snowplow::residualAt(long long column) const {
    // Thin uneven layer with occasional clods (2..5 px) the blade rode over.
    const float d = dpi_;
    const float base = vnoise(runSeed_ ^ 0x5151ull, static_cast<float>(column) / 4.0f);
    float r = kResidualAt96 * d * base * base;
    const long long group = column / 2;
    if (hash01(runSeed_ ^ 0xC10Dull, group) < 0.05f) r = std::max(r, (2.0f + 3.0f * hash01(runSeed_ ^ 0xC10Eull, group)) * d);
    return r;
}

void Snowplow::scrape(SurfaceRegistry& reg, float lo, float hi) {
    for (SurfaceId id : route_.surfaces) {
        Surface* s = reg.find(id);
        if (!s) continue;
        Heightmap& hm = s->hm;
        const float step = hm.step();
        const float l = static_cast<float>(s->left);
        const int i0 = std::max(0, static_cast<int>(std::ceil((lo - l) / step)));
        const int i1 = std::min(hm.columns() - 1, static_cast<int>(std::floor((hi - l) / step)));
        for (int i = i0; i <= i1; ++i) {
            const long long col = static_cast<long long>(std::floor((l + static_cast<float>(i) * step) / step_));
            const float got = hm.lowerTo(i, residualAt(col));
            carried_ += got;
            collected_ += got;
        }
    }
}

void Snowplow::emitSpill(float dt, ParticleSystem* fx) {
    if (!fx) {
        spillQueue_ = 0;
        return;
    }
    const float d = dpi_;
    const float unit = 20.0f * d * d;   // area represented by one spill particle
    spillBudget_ = std::min(spillBudget_ + 55.0f * dt, 6.0f);
    spillQueue_ = std::min(spillQueue_, 60.0f * unit);   // the rest goes unseen
    const float top = route_.top;
    const float worldFront = frontLocal();
    const float fdir = static_cast<float>(dir_);
    while (spillQueue_ >= unit && spillBudget_ >= 1.0f) {
        spillQueue_ -= unit;
        spillBudget_ -= 1.0f;
        if (rng_.uniform() < 0.55f) {   // tumbles back over the top of the blade
            fx->addDust(worldFront - fdir * rng_.range(0.0f, 10.0f) * d, top - bladeH_ - rng_.range(0.0f, 4.0f) * d,
                        -fdir * rng_.range(20.0f, 90.0f) * d + fdir * 0.5f * speed_, -rng_.range(40.0f, 140.0f) * d,
                        rng_.range(0.9f, 2.0f) * d, rng_.range(0.5f, 1.0f));
        } else {                        // falls off the front of the taskbar, fading
            Particle* p = fx->addDust(worldFront + fdir * rng_.range(-1.5f, 0.5f) * halfW_, top + 1.0f,
                                      fdir * rng_.range(-20.0f, 60.0f) * d, rng_.range(20.0f, 80.0f) * d,
                                      rng_.range(1.5f, 3.5f) * d, rng_.range(0.8f, 1.4f));
            if (p) p->fall *= 5.0f;
        }
    }
}

void Snowplow::emitFallAt(float ex, float area, float bigness, ParticleSystem* fx) {
    if (!fx || area <= 0) return;
    const float d = dpi_;
    const float top = route_.top;
    const float fdir = static_cast<float>(dir_);
    const float radius = (1.5f + 3.0f * std::clamp(bigness, 0.0f, 1.0f)) * d;   // large clumps first
    const int count = std::clamp(static_cast<int>(area / (radius * radius * 2.0f)), 1, 6);
    for (int i = 0; i < count; ++i)
        fx->addLoose(ex + fdir * rng_.range(1.0f, 8.0f) * d, top - rng_.uniform() * std::max(H_, 4.0f * d),
                     fdir * rng_.range(40.0f, 140.0f) * d, rng_.range(-40.0f, 30.0f) * d,
                     radius * rng_.range(0.7f, 1.1f));
    if (rng_.uniform() < 0.5f)
        fx->addDust(ex, top - rng_.uniform() * std::max(H_, 4.0f * d), fdir * rng_.range(20.0f, 120.0f) * d,
                    -rng_.range(30.0f, 120.0f) * d, rng_.range(0.8f, 1.6f) * d, rng_.range(0.5f, 0.9f));
}

void Snowplow::emitEndFall(float area, float bigness, ParticleSystem* fx) {
    emitFallAt(dir_ > 0 ? route_.right : route_.left, area, bigness, fx);
}

void Snowplow::step(float dt, SurfaceRegistry& reg, ObjectSystem& objects, ParticleSystem* fx) {
    if (!active() || dt <= 0) return;
    segs_.clear();
    for (SurfaceId id : route_.surfaces) {   // a taskbar of the route went away
        const Surface* s = reg.find(id);
        if (!s || !usableTaskbar(*s)) {
            abortDump(fx);
            return;
        }
        segs_.push_back(s);
    }
    ++renderVersion_;
    const float d = dpi_;
    const float fdir = static_cast<float>(dir_);
    bounce_ += 14.0f * dt;
    if (dipT_ > 0) dipT_ -= dt;
    if (phase_ != Phase::Dumping) x_ += fdir * speed_ * speedFactor() * dt;
    const float front = frontLocal();

    // Scrape everything under the body and up to the blade.
    if (dir_ > 0) scrape(reg, x_ - halfW_, front);
    else scrape(reg, front, x_ + halfW_);

    // Snowballs / snowmen in the way: the blade shoves the base (it squashes and crumbles into
    // the heap), the balls above topple forward and fly / roll on (ObjectSystem::stepKnock).
    std::vector<Pile>& piles = objects.piles();
    for (std::size_t k = 0; k < piles.size(); ++k) {
        Pile& p = piles[k];
        if (!onSurface(p.surface)) continue;
        const Surface* s = reg.find(p.surface);
        if (!s) continue;
        const float sl = static_cast<float>(s->left);
        const float lead = (sl + p.localX - front) * fdir;
        if (lead < p.r[0] && lead > -2.0f * halfW_) {
            if (p.knock == KnockBy::None) {   // first contact: bump, slow down
                objects.knock(k, *s, dir_, speed_, KnockBy::Plow, fx);
                dipT_ = kPlowImpactSeconds;
                bounce_ += 1.5f;
                ++impacts_;
            }
            Pile& q = objects.piles()[k];
            const float target = front + fdir * q.r[0] - sl;
            q.knock = KnockBy::Plow;
            q.knockPush += std::fabs(target - q.localX);
            q.localX = std::clamp(target, 0.0f, s->width());
            q.sink = 0;
        }
    }
    {   // crumbled snowmen go into the windrow; a little spills over the blade
        const float m = objects.takeCrumbled(KnockBy::Plow);
        if (m > 0) {
            const float spill = m * kPlowObjectSpill;
            carried_ += m - spill;
            spilled_ += spill;
            spillQueue_ += spill;
            collected_ += m;
            objectsIn_ += m;
        }
    }
    {   // decorations lying in the path are plowed under
        std::vector<Debris>& deb = objects.debrisMut();
        for (std::size_t k = 0; k < deb.size();) {
            const Debris& db = deb[k];
            if (db.fade >= 0 && (db.x - front) * fdir < 2.0f * d && (db.x - front) * fdir > -halfW_ &&
                db.y > route_.top - 80.0f * d && db.y < route_.top + 4.0f * d) {
                if (fx) fx->addDust(db.x, db.y, fdir * 60.0f * d, -80.0f * d, 1.2f * d, 0.4f);
                deb.erase(deb.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                ++k;
            }
        }
    }

    float overflow = 0, offEnd = 0;
    solve(reg, overflow, offEnd);
    spilled_ += overflow;
    spillQueue_ += overflow;

    const float farEnd = dir_ > 0 ? route_.right : route_.left;
    if (phase_ == Phase::Driving) {
        // The part sticking out past the end is shoved off progressively.
        if (offEnd > 0) {
            const float shove = std::min(offEnd, std::max(carried_ * dt * 1.5f, 30.0f * d * d * dt));
            carried_ -= shove;
            dumped_ += shove;
            emitEndFall(shove, std::min(1.0f, carried_ / (400.0f * d * d)), fx);
        }
        // Small clumps roll down the face and settle in the drift ahead.
        if (H_ > 6.0f * d) {
            rollAcc_ += dt * 1.2f;
            if (rollAcc_ >= 1.0f) {
                rollAcc_ -= 1.0f;
                const float a = std::min(carried_ * 0.03f, 12.0f * d * d);
                const float at = front + fdir * heapLength() * rng_.range(0.85f, 1.05f);
                for (SurfaceId id : route_.surfaces) {
                    Surface* s = reg.find(id);
                    if (!s || a <= 0.5f || at < static_cast<float>(s->left) || at >= static_cast<float>(s->right)) continue;
                    const float local = at - static_cast<float>(s->left);
                    const float got = s->hm.deposit(local, a);
                    carried_ -= got;
                    deposited_ += got;
                    if (fx && got > 0)
                        fx->addDust(at, static_cast<float>(s->top) - s->hm.heightAtLocalX(local),
                                    fdir * rng_.range(10.0f, 40.0f) * d, -rng_.range(10.0f, 40.0f) * d, 1.2f * d, 0.4f);
                    break;
                }
            }
        }
        if ((front - farEnd) * fdir >= 0.0f) {   // reached the end
            phase_ = Phase::Dumping;
            dumpTimer_ = 0;
            dumpTotal_ = carried_;
            dumpRate_ = carried_ / kDumpSeconds;
        }
    } else if (phase_ == Phase::Dumping) {
        dumpTimer_ += dt;
        const float release = std::min(carried_, std::max(dumpRate_ * dt, 1.0f));
        carried_ -= release;
        dumped_ += release;
        emitEndFall(release, dumpTotal_ > 0 ? carried_ / dumpTotal_ : 0.0f, fx);
        if (carried_ <= 0.01f) {
            carried_ = 0;
            H_ = P_ = 0;
            if (dumpTimer_ >= kDumpPauseSeconds) phase_ = Phase::Leaving;
        }
    } else {   // Leaving: whatever is still scraped up falls off too
        if (carried_ > 0) {
            emitEndFall(carried_, 0.0f, fx);
            dumped_ += carried_;
            carried_ = 0;
            H_ = P_ = 0;
        }
        const bool gone = dir_ > 0 ? x_ - halfW_ > route_.right + 2.0f : x_ + halfW_ < route_.left - 2.0f;
        if (gone) {
            lastExit_ = x_;
            phase_ = Phase::Off;
            ++runs_;
        }
    }
    emitSpill(dt, fx);
}

void Snowplow::appendSprite(const SurfaceRegistry& /*reg*/, std::vector<SpriteInstance>& out) const {
    if (!active()) return;
    SpriteInstance sp;
    sp.x = x_;
    sp.y = route_.top - halfH_ - std::fabs(std::sin(bounce_)) * 1.5f * dpi_;
    sp.vx = phase_ == Phase::Dumping ? 0.0f : static_cast<float>(dir_) * speed_ * speedFactor();
    sp.halfW = halfW_;
    sp.halfH = halfH_;
    sp.rotation = 0.02f * std::sin(0.5f * bounce_);
    sp.shape = Shape::Snowplow;
    sp.variant = dir_ > 0 ? std::uint8_t{0} : std::uint8_t{1};
    out.push_back(sp);
}

void Snowplow::applyHump(const Surface& s, std::vector<float>& heights) const {
    if (!onSurface(s.id) || H_ <= 0) return;
    const float step = s.hm.step();
    const int n = static_cast<int>(heights.size());
    const float front = frontLocal() - static_cast<float>(s.left);   // in this surface's local x
    const float wobble = 1.0f + 0.02f * std::sin(1.3f * bounce_);   // the heap shakes with the bounce
    const float reach = P_ + 2.2f * H_ / kReposeSlope + 6.0f * dpi_;
    // Columns of this surface within [0, reach] ahead of the blade (the heap may cross a seam).
    const float lo = dir_ > 0 ? front : front - reach, hi = dir_ > 0 ? front + reach : front;
    const int i0 = std::max(0, static_cast<int>(std::ceil(lo / step)));
    const int i1 = std::min(n - 1, static_cast<int>(std::floor(hi / step)));
    for (int i = i0; i <= i1; ++i) {
        const float d = (static_cast<float>(i) * step - front) * static_cast<float>(dir_);
        float& h = heights[static_cast<std::size_t>(i)];
        h = std::max(h, profile(d, H_, P_) * wobble);   // union with the drift ahead
    }
}

}  // namespace snow
