#include "interaction/icicles.h"

#include <algorithm>
#include <cmath>

#include "simulation/particles.h"
#include "surfaces/surface_registry.h"

namespace snow {

namespace {
constexpr float kGravityAt96 = 1400.0f;
}

const IcicleSet* IcicleSystem::setFor(SurfaceId id) const {
    for (const IcicleSet& s : sets_)
        if (s.surface == id) return &s;
    return nullptr;
}
IcicleSet* IcicleSystem::setFor(SurfaceId id) {
    for (IcicleSet& s : sets_)
        if (s.surface == id) return &s;
    return nullptr;
}

int IcicleSystem::attachedCount() const {
    int n = 0;
    for (const IcicleSet& s : sets_) n += static_cast<int>(s.list.size());
    return n;
}

void IcicleSystem::seed(IcicleSet& set, const Surface& s) {
    const float d = s.dpiScale;
    const float margin = std::max(s.cornerRadius, 6.0f * d);
    float x = margin + rng_.range(0.0f, 20.0f) * d;
    while (x < s.width() - margin) {
        Icicle ic;
        ic.localX = x;
        ic.len = 1.0f * d;
        ic.maxLen = rng_.range(6.0f, 40.0f) * d;
        ic.width = rng_.range(2.0f, 6.0f) * d;
        ic.rate = ic.maxLen / (kIcicleGrowSeconds * rng_.range(0.75f, 1.3f));
        ic.dripRate = rng_.range(0.1f, 0.25f);
        ic.variant = static_cast<std::uint8_t>(rng_.below(4));
        set.list.push_back(ic);
        x += rng_.range(25.0f, 90.0f) * d;   // irregular spacing
    }
    set.seeded = true;
}

void IcicleSystem::step(float dt, SurfaceRegistry& reg, ParticleSystem* fx, bool enabled, bool thaw) {
    thaw_ = thaw;
    if (dt <= 0) return;
    const float speed = thaw ? kIcicleThawFactor : 1.0f;
    for (const Surface& s : reg.surfaces()) {
        if (s.kind != SurfaceKind::Window || !s.active) continue;
        IcicleSet* set = setFor(s.id);
        const float full = s.width() * s.hm.maxHeight();
        const float fill = full > 0 ? static_cast<float>(s.hm.mass()) / full : 0.0f;
        if (!set) {
            if (!enabled || fill < kIcicleCapShare) continue;
            sets_.emplace_back();
            set = &sets_.back();
            set->surface = s.id;
        }
        if (fill >= kIcicleCapShare) set->capTime += dt * speed;
        else set->capTime = std::max(0.0f, set->capTime - 0.5f * dt);
        if (enabled && !set->seeded && set->capTime >= kIcicleCapSeconds) seed(*set, s);
        const bool grow = enabled && (fill >= kIcicleCapShare || thaw);
        const float d = s.dpiScale;
        for (std::size_t k = 0; k < set->list.size();) {
            Icicle& ic = set->list[k];
            if (ic.localX > s.width()) {   // window got narrower
                set->list.erase(set->list.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
            if (grow) ic.len = std::min(ic.maxLen, ic.len + ic.rate * speed * dt);
            if (thaw && fx && ic.len > 4.0f * d) {   // melt water drips off the tip
                ic.drip += ic.dripRate * dt;
                if (ic.drip >= 1.0f) {
                    ic.drip = 0;
                    fx->addDroplet(static_cast<float>(s.left) + ic.localX, static_cast<float>(s.bottom) + ic.len + 2.0f * d,
                                   0.0f, 20.0f * d, d);
                }
            } else {
                ic.drip = 0;
            }
            ++k;
        }
    }
    // Falling icicles and shards.
    const RectI scene = reg.sceneBounds();
    for (std::size_t k = 0; k < falling_.size();) {
        FallingIce& f = falling_[k];
        // Lowest point before and after the move (it spins: use the rotated extent both times).
        const float tip0 = f.y + f.halfH * std::fabs(std::cos(f.rotation)) + f.halfW * std::fabs(std::sin(f.rotation));
        f.vy += kGravityAt96 * f.dpi * dt;
        f.vx *= std::exp(-0.2f * dt);
        f.x += f.vx * dt;
        f.y += f.vy * dt;
        f.rotation += f.angVel * dt;
        bool remove = false;
        if (f.life >= 0) {
            f.life -= dt;
            remove = f.life <= 0;
        } else {
            const float reach = f.halfH * std::fabs(std::cos(f.rotation)) + f.halfW * std::fabs(std::sin(f.rotation));
            SurfaceHit hit;
            if (reg.sweep(f.x, tip0, f.x, f.y + reach, true, hit, f.exclude)) {
                const FallingIce copy = f;
                falling_[k] = falling_.back();
                falling_.pop_back();
                shatter(copy, hit.x, hit.y, fx);
                continue;
            }
        }
        if (scene.empty() || f.y > static_cast<float>(scene.b) + 60.0f) remove = true;
        if (remove) {
            falling_[k] = falling_.back();
            falling_.pop_back();
        } else {
            ++k;
        }
    }
}

void IcicleSystem::shatter(const FallingIce& f, float hitX, float hitY, ParticleSystem* fx) {
    ++shattered_;
    const float d = f.dpi;
    const int n = 3 + static_cast<int>(rng_.below(4));
    for (int i = 0; i < n && falling_.size() < static_cast<std::size_t>(kMaxFallingIce); ++i) {
        FallingIce s;
        s.x = hitX + rng_.range(-4.0f, 4.0f) * d;
        s.y = hitY - 2.0f * d;
        s.vx = rng_.range(-120.0f, 120.0f) * d;
        s.vy = -rng_.range(60.0f, 200.0f) * d;
        s.rotation = rng_.range(0.0f, 6.28f);
        s.angVel = rng_.range(-12.0f, 12.0f);
        s.halfW = rng_.range(0.8f, 1.6f) * d;
        s.halfH = rng_.range(1.5f, 4.0f) * d;
        s.dpi = d;
        s.life = rng_.range(0.5f, 0.9f);
        s.variant = static_cast<std::uint8_t>(rng_.below(4));
        falling_.push_back(s);
    }
    if (fx)
        for (int i = 0; i < 4; ++i)
            fx->addDust(hitX, hitY - 1.0f, rng_.range(-80.0f, 80.0f) * d, -rng_.range(40.0f, 140.0f) * d,
                        rng_.range(0.7f, 1.4f) * d, rng_.range(0.3f, 0.6f));
}

int IcicleSystem::hitSegment(const SurfaceRegistry& reg, float x0, float y0, float x1, float y1, float radius, float vx,
                             float vy, float minSpeed, ParticleSystem* fx, bool* bumped) {
    if (bumped) *bumped = false;
    if (sets_.empty()) return 0;
    hits_.clear();
    const float dx = x1 - x0, dy = y1 - y0;
    for (std::size_t si = 0; si < sets_.size(); ++si) {
        const IcicleSet& set = sets_[si];
        if (set.list.empty()) continue;
        const Surface* s = reg.find(set.surface);
        if (!s || !s->active) continue;
        const float pad = kIcicleHitPadAt96 * s->dpiScale + radius;
        const float top = static_cast<float>(s->bottom) - pad;
        if (std::max(y0, y1) < top) continue;
        for (const Icicle& ic : set.list) {
            const float cx = static_cast<float>(s->left) + ic.localX;
            const float bx0 = cx - 0.5f * ic.width - pad, bx1 = cx + 0.5f * ic.width + pad;
            const float by0 = top, by1 = static_cast<float>(s->bottom) + ic.len + pad;
            // Slab test of the segment against the box.
            float t0 = 0, t1 = 1;
            bool miss = false;
            for (int axis = 0; axis < 2 && !miss; ++axis) {
                const float o = axis == 0 ? x0 : y0, dd = axis == 0 ? dx : dy;
                const float lo = axis == 0 ? bx0 : by0, hi = axis == 0 ? bx1 : by1;
                if (std::fabs(dd) < 1e-6f) {
                    miss = o < lo || o > hi;
                } else {
                    float ta = (lo - o) / dd, tb = (hi - o) / dd;
                    if (ta > tb) std::swap(ta, tb);
                    t0 = std::max(t0, ta);
                    t1 = std::min(t1, tb);
                    miss = t0 > t1;
                }
            }
            if (!miss) hits_.push_back(Hit{t0, si, ic.localX});
        }
    }
    if (hits_.empty()) return 0;
    if (bumped) *bumped = true;
    if (std::sqrt(vx * vx + vy * vy) < minSpeed) return 0;   // too slow: a bump, nothing breaks
    std::sort(hits_.begin(), hits_.end(), [](const Hit& a, const Hit& b) { return a.t < b.t; });
    int broken = 0;
    for (const Hit& h : hits_) {
        IcicleSet& set = sets_[h.set];
        const Surface* s = reg.find(set.surface);
        auto it = std::find_if(set.list.begin(), set.list.end(), [&](const Icicle& ic) { return ic.localX == h.localX; });
        if (!s || it == set.list.end()) continue;
        const float d = s->dpiScale;
        const float hx = x0 + dx * h.t, hy = y0 + dy * h.t;
        const float root = static_cast<float>(s->bottom);
        const float frac = it->len > 0 ? (hy - radius - root) / it->len : 0.0f;   // where its upper edge strikes
        // Low hit: only the part below the contact snaps; the stub stays (and regrows).
        // (the stub ends above the ball, so the same pass cannot hit it again)
        const float stub = frac > kIcicleLowHit ? std::clamp(hy - radius - kIcicleHitPadAt96 * d - 1.0f - root, 1.5f * d, it->len - 1.5f * d) : 0.0f;
        const float pieceLen = it->len - stub;
        if (falling_.size() < static_cast<std::size_t>(kMaxFallingIce) && pieceLen > 1.0f * d) {
            FallingIce f;
            f.x = static_cast<float>(s->left) + it->localX;
            f.y = root + stub + 0.5f * pieceLen;
            f.vx = 0.35f * vx + rng_.range(-20.0f, 20.0f) * d;
            f.vy = 0.3f * vy - rng_.range(0.0f, 60.0f) * d;
            f.angVel = (vx >= 0 ? 1.0f : -1.0f) * rng_.range(4.0f, 9.0f);
            f.halfW = 0.5f * it->width * (stub > 0 ? 0.7f : 1.0f);
            f.halfH = 0.5f * pieceLen;
            f.dpi = d;
            f.variant = it->variant;
            f.exclude = s->id;
            falling_.push_back(f);
        }
        if (fx)   // a bright little burst of ice glitter
            for (int i = 0; i < 6; ++i)
                fx->addDust(hx, hy, rng_.range(-90.0f, 90.0f) * d + 0.2f * vx, -rng_.range(30.0f, 120.0f) * d,
                            rng_.range(0.6f, 1.2f) * d, rng_.range(0.25f, 0.5f));
        if (stub > 0) {
            it->len = stub;
            ++hitStubs_;
        } else {
            set.list.erase(it);
        }
        ++broken;
        ++hitBroken_;
    }
    return broken;
}

int IcicleSystem::snapOff(const Surface& s, float fraction, float wvx, float wvy, float offX, float offY) {
    IcicleSet* set = setFor(s.id);
    if (!set) return 0;
    int n = 0;
    const float d = s.dpiScale;
    const float vmax = 600.0f * d;
    for (std::size_t k = 0; k < set->list.size();) {
        const Icicle& ic = set->list[k];
        if (!(rng_.uniform() < fraction)) {
            ++k;
            continue;
        }
        if (falling_.size() < static_cast<std::size_t>(kMaxFallingIce) && ic.len > 1.5f * d) {
            FallingIce f;
            const float lag = rng_.range(0.15f, 0.3f);
            f.x = static_cast<float>(s.left) + ic.localX + offX;
            f.y = static_cast<float>(s.bottom) + 0.5f * ic.len + offY;
            f.vx = std::clamp(wvx * lag, -vmax, vmax) + rng_.range(-15.0f, 15.0f) * d;
            f.vy = std::clamp(wvy * lag, -vmax, vmax);
            f.angVel = rng_.range(-3.0f, 3.0f);
            f.halfW = 0.5f * ic.width;
            f.halfH = 0.5f * ic.len;
            f.dpi = d;
            f.variant = ic.variant;
            f.exclude = s.id;
            falling_.push_back(f);
        }
        set->list.erase(set->list.begin() + static_cast<std::ptrdiff_t>(k));
        ++n;
    }
    if (set->list.empty()) {   // they will grow again after a while
        set->seeded = false;
        set->capTime = 0;
    }
    return n;
}

void IcicleSystem::onSurfacesRemoved(const std::vector<Surface>& removed, bool drop) {
    for (const Surface& s : removed) {
        if (drop && s.active) snapOff(s, 1.0f, 0, 0, 0, 0);
        for (std::size_t k = 0; k < sets_.size();) {
            if (sets_[k].surface == s.id) sets_.erase(sets_.begin() + static_cast<std::ptrdiff_t>(k));
            else ++k;
        }
    }
}

void IcicleSystem::appendBatches(const SurfaceRegistry& reg, std::vector<SpriteBatch>& out) const {
    for (const IcicleSet& set : sets_) {
        if (set.list.empty()) continue;
        const Surface* s = reg.find(set.surface);
        if (!s || !s->active || s->iceClip.empty()) continue;
        out.emplace_back();
        SpriteBatch& b = out.back();
        b.clip = s->iceClip;
        const float d = s->dpiScale;
        for (const Icicle& ic : set.list) {
            SpriteInstance sp;
            sp.x = static_cast<float>(s->left) + ic.localX;
            sp.y = static_cast<float>(s->bottom) + 0.5f * ic.len;
            sp.halfW = 0.5f * ic.width;
            sp.halfH = 0.5f * ic.len;
            sp.shape = Shape::Icicle;
            sp.variant = ic.variant;
            sp.opacity = 0.9f;
            b.sprites.push_back(sp);
            if (thaw_ && ic.drip > 0.3f) {   // a drop forming at the tip
                SpriteInstance dr;
                dr.x = sp.x;
                dr.y = static_cast<float>(s->bottom) + ic.len + 1.5f * d * ic.drip;
                dr.halfW = 1.3f * d * ic.drip;
                dr.halfH = 2.0f * d * ic.drip;
                dr.shape = Shape::Droplet;
                dr.variant = 1;
                dr.opacity = 0.9f;
                b.sprites.push_back(dr);
            }
        }
    }
}

void IcicleSystem::appendFalling(std::vector<SpriteInstance>& out, float opacity) const {
    for (const FallingIce& f : falling_) {
        SpriteInstance sp;
        sp.x = f.x;
        sp.y = f.y;
        sp.vx = f.vx;
        sp.vy = f.vy;
        sp.halfW = f.halfW;
        sp.halfH = f.halfH;
        sp.rotation = f.rotation;
        sp.shape = Shape::Icicle;
        sp.variant = f.variant;
        sp.opacity = opacity * (f.life >= 0 ? std::clamp(f.life / 0.5f, 0.0f, 1.0f) : 0.9f);
        out.push_back(sp);
    }
}

}  // namespace snow
