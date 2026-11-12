#include "simulation/chunks.h"

#include <algorithm>
#include <cmath>

#include "simulation/particles.h"
#include "surfaces/surface_registry.h"

namespace snow {

namespace {
constexpr float kLagMin = 0.15f, kLagMax = 0.3f;   // share of the window velocity a slab keeps
constexpr float kMaxInheritAt96 = 600.0f;          // px/s cap of the inherited horizontal speed

float extentY(const Chunk& c) {
    return c.halfH * std::fabs(std::cos(c.rotation)) + c.halfW * std::fabs(std::sin(c.rotation));
}
}  // namespace

bool ChunkSystem::substantial(const Surface& s) {
    const float thin = kChunkThinAt96 * s.dpiScale;
    double sum = 0;
    int n = 0;
    for (int i = 0; i < s.hm.columns(); ++i) {
        const float h = s.hm.height(i);
        if (h < thin) continue;
        sum += h;
        ++n;
    }
    return n >= 4 && sum / n >= kChunkMinCapAt96 * s.dpiScale;
}

void ChunkSystem::clear() {
    for (const Chunk& c : chunks_) lost_ += c.mass;
    chunks_.clear();
}

double ChunkSystem::liveMass() const {
    double m = 0;
    for (const Chunk& c : chunks_) m += c.mass;
    for (const Chunk& c : pending_) m += c.mass;
    return m;
}

Chunk* ChunkSystem::add(float x, float y, float halfW, float halfH, float mass, float dpi) {
    if (chunks_.size() + pending_.size() >= static_cast<std::size_t>(kMaxChunks)) return nullptr;
    pending_.emplace_back();
    Chunk& c = pending_.back();
    c.id = nextId_++;
    c.x = x;
    c.y = y;
    c.halfW = halfW;
    c.halfH = halfH;
    c.mass = mass;
    c.dpi = dpi;
    c.variant = static_cast<std::uint8_t>(rng_.below(4));
    return &c;
}

void ChunkSystem::puff(float x, float y, float dpi, int n, ParticleSystem* fx) {
    if (!fx) return;
    for (int i = 0; i < n; ++i)
        fx->addDust(x + rng_.range(-4.0f, 4.0f) * dpi, y - rng_.range(0.0f, 3.0f) * dpi, rng_.range(-70.0f, 70.0f) * dpi,
                    -rng_.range(30.0f, 140.0f) * dpi, rng_.range(0.8f, 1.8f) * dpi, rng_.range(0.4f, 0.9f));
}

ShedResult ChunkSystem::shed(Surface& s, float fraction, float wvx, float wvy, float offX, float offY,
                             ParticleSystem* fx) {
    ShedResult res;
    Heightmap& hm = s.hm;
    const int n = hm.columns();
    if (n == 0) return res;
    const float d = s.dpiScale;
    const float step = hm.step();
    const float thin = kChunkThinAt96 * d, thick = kChunkMinCapAt96 * d;
    const float maxH = hm.maxHeight();
    const float maxInherit = kMaxInheritAt96 * d;
    int i = 0;
    while (i < n) {
        if (hm.height(i) < thin) {
            ++i;
            continue;
        }
        int runEnd = i;
        while (runEnd < n && hm.height(runEnd) >= thin) ++runEnd;
        int a = i;
        while (a < runEnd) {
            // Next piece: a random mix of short and long slabs (longer when the cap is thick).
            float local = 0;
            const int probe = std::min(runEnd, a + 20);
            for (int k = a; k < probe; ++k) local += hm.height(k);
            local /= static_cast<float>(std::max(1, probe - a));
            const float thickF = std::clamp(local / (18.0f * d), 0.5f, 1.3f);
            const bool longPiece = rng_.uniform() < 0.5f;
            const float L = (longPiece ? rng_.range(60.0f, 250.0f) * thickF : rng_.range(8.0f, 40.0f)) * d;
            const int lenCols = std::max(2, static_cast<int>(L / step));
            const int target = a + lenCols;
            int b = runEnd;
            if (target < runEnd - 1) {
                // Break at a valley of the lumpy cap or where a rounded corner thins it.
                const int w = std::max(1, static_cast<int>(static_cast<float>(lenCols) * 0.3f));
                const int lo = std::max(a + 2, target - w), hi = std::min(runEnd - 1, target + w);
                float bestScore = 1e30f;
                b = std::clamp(target, lo, hi);
                for (int j = lo; j <= hi; ++j) {
                    float score = hm.height(j) + 0.02f * static_cast<float>(std::abs(j - target));
                    if (hm.cap(j) < 0.6f * maxH) score -= 1000.0f;
                    if (score < bestScore) {
                        bestScore = score;
                        b = j;
                    }
                }
            }
            float area = 0;
            for (int k = a; k < b; ++k) area += hm.height(k) * step;
            const int pa = a;
            a = b;
            if (!(rng_.uniform() < fraction)) {
                ++res.piecesKept;
                continue;
            }
            for (int k = pa; k < b; ++k) hm.lowerTo(k, 0.0f);
            if (!fx || area <= 0) continue;   // animations off: it simply goes
            const float len = static_cast<float>(b - pa) * step;
            const float t = area / len;
            const float x0 = static_cast<float>(s.left) + static_cast<float>(pa) * step + offX;
            const float ty = static_cast<float>(s.top) + offY;
            if (t >= thick && chunks_.size() + pending_.size() < static_cast<std::size_t>(kMaxChunks)) {
                shed_ += area;
                dust_ += area * kChunkFractureDust;
                const float m = area * (1.0f - kChunkFractureDust);
                const int parts = std::max(1, static_cast<int>(std::ceil(len / (280.0f * d))));
                const float plen = len / static_cast<float>(parts);
                for (int p = 0; p < parts; ++p) {
                    const float halfW = std::clamp(0.5f * plen, 4.0f * d, 150.0f * d);
                    const float halfH = std::clamp(0.5f * t, 3.0f * d, 30.0f * d);
                    Chunk* c = add(x0 + plen * (static_cast<float>(p) + 0.5f), ty - 0.5f * t, halfW, halfH,
                                   m / static_cast<float>(parts), d);
                    if (!c) {   // no room: its snow is dust
                        dust_ += m / static_cast<float>(parts);
                        continue;
                    }
                    // Snow lags behind the jerk: in the window's frame it slides the other way.
                    const float lag = rng_.range(kLagMin, kLagMax);
                    c->vx = std::clamp(wvx * lag, -maxInherit, maxInherit) + rng_.range(-15.0f, 15.0f) * d;
                    c->vy = std::clamp(wvy * lag, -maxInherit, maxInherit) - rng_.range(0.0f, 50.0f) * d;
                    const float rel = c->vx - wvx;
                    c->angVel = rng_.range(-0.6f, 0.6f) + (rel >= 0 ? 1.0f : -1.0f) * rng_.range(0.8f, 2.2f) *
                                                             std::clamp(40.0f * d / halfW, 0.3f, 1.5f);
                    c->exclude = s.id;
                    if (halfW > 30.0f * d && rng_.chance(0.75f)) c->crackTimer = rng_.range(0.2f, 0.5f);
                    ++res.chunks;
                }
                res.chunkMass += area;
                puff(x0, ty - 0.5f * t, d, 2, fx);   // crumbs at the fracture lines
                puff(x0 + len, ty - 0.5f * t, d, 2, fx);
            } else {   // thin snow: flakes
                const int count = std::clamp(static_cast<int>(area / (10.0f * d * d)), 1, 20);
                for (int k = 0; k < count; ++k)
                    fx->addLoose(x0 + rng_.uniform() * len, ty - rng_.uniform() * t,
                                 std::clamp(wvx * rng_.range(kLagMin, kLagMax), -maxInherit, maxInherit),
                                 rng_.range(0.0f, 40.0f) * d, rng_.range(1.2f, 2.6f) * d);
                res.flakeMass += area;
            }
        }
        i = runEnd;
    }
    chunks_.insert(chunks_.end(), pending_.begin(), pending_.end());
    pending_.clear();
    return res;
}

void ChunkSystem::crack(std::size_t index, ParticleSystem* fx) {
    Chunk c = chunks_[index];
    ++cracks_;
    const int parts = c.halfW > 80.0f * c.dpi ? 3 : 2;
    const float dust = c.mass * 0.03f;
    dust_ += dust;
    const float m = (c.mass - dust) / static_cast<float>(parts);
    const float ax = std::cos(c.rotation), ay = std::sin(c.rotation);
    const float partHalf = std::max(4.0f * c.dpi, c.halfW / static_cast<float>(parts));
    for (int p = 0; p < parts; ++p) {
        const float off = (-1.0f + (2.0f * static_cast<float>(p) + 1.0f) / static_cast<float>(parts)) * c.halfW;
        Chunk part = c;
        part.halfW = partHalf;
        part.mass = m;
        part.x = c.x + ax * off;
        part.y = c.y + ay * off;
        part.vx += ax * (off > 0 ? 1.0f : -1.0f) * rng_.range(10.0f, 40.0f) * c.dpi;
        part.angVel += rng_.range(-1.5f, 1.5f);
        part.crackTimer = (partHalf > 60.0f * c.dpi && rng_.chance(0.5f)) ? rng_.range(0.15f, 0.35f) : -1.0f;
        if (p == 0) {
            chunks_[index] = part;
        } else {
            part.id = nextId_++;
            part.variant = static_cast<std::uint8_t>(rng_.below(4));
            pending_.push_back(part);   // the cap only limits new fractures, never existing mass
        }
    }
    puff(c.x, c.y, c.dpi, 2, fx);
}

void ChunkSystem::impact(Chunk c, SurfaceRegistry& reg, int surfaceIndex, float hitX, float hitY, ParticleSystem* fx) {
    ++impacts_;
    Surface& s = reg.surfaces()[static_cast<std::size_t>(surfaceIndex)];
    const float d = c.dpi;
    // Spread a deposit over the slab's footprint.
    auto depositSpread = [&](float area) {
        const int parts = std::clamp(static_cast<int>(1.6f * c.halfW / (3.0f * s.hm.step())), 1, 10);
        float got = 0;
        for (int k = 0; k < parts; ++k) {
            const float px = hitX + (-0.8f + 1.6f * (static_cast<float>(k) + 0.5f) / static_cast<float>(parts)) * c.halfW;
            if (px < static_cast<float>(s.left) || px >= static_cast<float>(s.right)) continue;
            SurfaceHit h;
            h.surfaceIndex = surfaceIndex;
            h.x = px;
            h.y = hitY;
            h.column = s.hm.columnAt(px - static_cast<float>(s.left));
            got += reg.deposit(h, area / static_cast<float>(parts));
        }
        return got;
    };
    if (c.soft) {   // flung loose snow: piles up where it lands
        const float got = depositSpread(c.mass * 0.92f);
        deposited_ += got;
        dust_ += c.mass - got;
        puff(hitX, hitY, d, 3, fx);
        return;
    }
    if (c.halfW < kChunkSmallHalfWAt96 * d) {   // small: just settles, with a puff
        const float got = depositSpread(c.mass);
        deposited_ += got;
        dust_ += c.mass - got;
        puff(hitX, hitY, d, 3, fx);
        return;
    }
    // Shatter: part of it stays where it hit, a few smaller pieces bounce off, the rest is dust.
    const float got = depositSpread(c.mass * rng_.range(0.4f, 0.6f));
    deposited_ += got;
    float rest = c.mass - got;
    const int kids = 3 + static_cast<int>(rng_.below(6));
    const float childTotal = std::min(rest, c.mass * 0.35f);
    float weights[8]{}, wsum = 0;
    for (int k = 0; k < kids; ++k) wsum += (weights[k] = rng_.range(0.6f, 1.4f));
    const float speed = std::min(1.0f, std::fabs(c.vy) / (400.0f * d));
    for (int k = 0; k < kids; ++k) {
        const float m = childTotal * weights[k] / wsum;
        const float halfW = std::clamp(c.halfW * rng_.range(0.15f, 0.4f), 3.0f * d, std::max(3.0f * d, c.halfW / 2.5f));
        const float halfH = std::clamp(m / (4.0f * halfW), 3.0f * d, 15.0f * d);
        Chunk* kid = add(hitX + rng_.range(-0.8f, 0.8f) * c.halfW, hitY - halfH - 1.0f, halfW, halfH, m, d);
        if (!kid) {
            dust_ += m;
        } else {
            kid->vx = c.vx * 0.25f + rng_.range(-90.0f, 90.0f) * d;
            kid->vy = -rng_.range(40.0f, 170.0f) * d * speed;
            kid->rotation = c.rotation;
            kid->angVel = rng_.range(-4.0f, 4.0f);
            kid->exclude = c.exclude;
        }
        rest -= m;
    }
    dust_ += rest;
    puff(hitX, hitY, d, 4 + static_cast<int>(rng_.below(5)), fx);
}

Chunk* ChunkSystem::spawnFree(float x, float y, float halfW, float halfH, float mass, float vx, float vy, float dpi,
                             SurfaceId exclude) {
    shed_ += mass;
    if (chunks_.size() >= static_cast<std::size_t>(kMaxChunks)) {
        dust_ += mass;
        return nullptr;
    }
    chunks_.emplace_back();
    Chunk& c = chunks_.back();
    c.id = nextId_++;
    c.x = x;
    c.y = y;
    c.vx = vx;
    c.vy = vy;
    c.halfW = std::clamp(halfW, 4.0f * dpi, 150.0f * dpi);
    c.halfH = std::clamp(halfH, 3.0f * dpi, 30.0f * dpi);
    c.mass = mass;
    c.dpi = dpi;
    c.exclude = exclude;
    c.variant = static_cast<std::uint8_t>(rng_.below(4));
    return &c;
}

void ChunkSystem::shatterAt(const Chunk& c, SurfaceRegistry& reg, int surfaceIndex, float hitX, float hitY,
                            ParticleSystem* fx) {
    shed_ += c.mass;
    Chunk body = c;
    body.halfW = std::max(body.halfW, (kChunkSmallHalfWAt96 + 1.0f) * body.dpi);   // always shatters
    impact(body, reg, surfaceIndex, hitX, hitY, fx);
    chunks_.insert(chunks_.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void ChunkSystem::step(float dt, SurfaceRegistry& reg, ParticleSystem* fx) {
    if (chunks_.empty() || dt <= 0) return;
    const RectI scene = reg.sceneBounds();
    for (std::size_t i = 0; i < chunks_.size();) {
        Chunk& c = chunks_[i];
        const float x0 = c.x, yb0 = c.y + extentY(c);
        c.vy += kChunkGravityAt96 * c.dpi * dt;
        c.vx *= std::exp(-0.35f * dt);
        c.vy *= std::exp(-0.08f * dt);
        c.x += c.vx * dt;
        c.y += c.vy * dt;
        c.rotation += c.angVel * dt;
        if (c.crackTimer > 0) {
            c.crackTimer -= dt;
            if (c.crackTimer <= 0) crack(i, fx);
        }
        Chunk& cc = chunks_[i];
        const float yb1 = cc.y + extentY(cc);
        SurfaceHit hit;
        if (yb1 > yb0 && reg.sweep(x0, yb0, cc.x, yb1, true, hit, cc.exclude)) {
            const Chunk copy = cc;
            chunks_[i] = chunks_.back();
            chunks_.pop_back();
            impact(copy, reg, hit.surfaceIndex, hit.x, hit.y, fx);
            continue;
        }
        if (scene.empty() || cc.y - extentY(cc) > static_cast<float>(scene.b) + 50.0f ||
            cc.x < static_cast<float>(scene.l) - 400.0f || cc.x > static_cast<float>(scene.r) + 400.0f) {
            lost_ += cc.mass;
            chunks_[i] = chunks_.back();
            chunks_.pop_back();
            continue;
        }
        ++i;
    }
    chunks_.insert(chunks_.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void ChunkSystem::appendSprites(std::vector<SpriteInstance>& out, float opacity) const {
    for (const Chunk& c : chunks_) {
        SpriteInstance sp;
        sp.x = c.x;
        sp.y = c.y;
        sp.vx = c.vx;
        sp.vy = c.vy;
        sp.halfW = c.halfW;
        sp.halfH = c.halfH;
        sp.rotation = c.rotation;
        sp.opacity = opacity;
        sp.shape = Shape::SnowChunk;
        sp.variant = c.variant;
        out.push_back(sp);
    }
}

}  // namespace snow
