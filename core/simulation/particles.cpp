#include "simulation/particles.h"

#include <algorithm>
#include <cmath>

#include "core/snapshot.h"
#include "surfaces/surface_registry.h"

namespace snow {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.2831853f;
constexpr float kSceneMargin = 48.0f;     // px outside the monitors before a flake is recycled
constexpr float kVelocityResponse = 3.0f; // 1/s: how fast a flake adopts the air velocity
constexpr float kRefillGain = 0.5f;       // 1/s: extra spawn rate per missing flake

// Parabolic sine approximation for x in [-pi, pi] (max error ~0.001). Deterministic and
// far cheaper than std::sin; accuracy is irrelevant for sway.
inline float fastSin(float x) {
    float y = 1.27323954f * x - 0.405284735f * x * std::fabs(x);
    return 0.225f * (y * std::fabs(y) - y) + y;
}
inline float wrapPi(float x) { return x > kPi ? x - kTwoPi : x; }
}  // namespace

ParticleSystem::ParticleSystem(std::uint64_t seed) : rng_(seed) { p_.reserve(kHardCapacity); }

void ParticleSystem::setScene(const RectI& bounds, const std::vector<MonitorInfo>& monitors) {
    scene_ = bounds;
    spans_.clear();
    for (const MonitorInfo& m : monitors) {
        if (m.bounds.empty()) continue;
        spans_.push_back({static_cast<float>(m.bounds.l), static_cast<float>(m.bounds.r),
                          m.dpiScale > 0.1f ? m.dpiScale : 1.0f});
    }
    float sum = 0, width = 0;
    for (const Span& sp : spans_) {
        sum += sp.scale * (sp.r - sp.l);
        width += sp.r - sp.l;
    }
    meanScale_ = width > 0 ? sum / width : 1.0f;
    // Skies: the budget is split by DPI-normalised area. Keep counts of surviving flakes.
    std::vector<int> counts(monitors.size(), 0);
    for (Particle& p : p_) {
        if (p.home != 255 && p.home < monitors.size() && p.home < skies_.size() && skies_[p.home].r == monitors[p.home].bounds)
            ++counts[p.home];
        else
            p.home = 255;   // monitors changed: it falls on as a free flake
    }
    skies_.clear();
    double total = 0;
    for (const MonitorInfo& m : monitors) {
        const double sc = m.dpiScale > 0.1f ? m.dpiScale : 1.0;
        total += m.bounds.empty() ? 0.0 : static_cast<double>(m.bounds.width()) * m.bounds.height() / (sc * sc);
    }
    for (std::size_t i = 0; i < monitors.size() && i < 255; ++i) {
        const MonitorInfo& m = monitors[i];
        Sky sk;
        sk.r = m.bounds;
        sk.scale = m.dpiScale > 0.1f ? m.dpiScale : 1.0f;
        const double sc = sk.scale;
        sk.weight = (total > 0 && !m.bounds.empty())
                        ? static_cast<float>(static_cast<double>(m.bounds.width()) * m.bounds.height() / (sc * sc) / total)
                        : 0.0f;
        sk.count = counts[i];
        skies_.push_back(sk);
    }
}

int ParticleSystem::skyTarget(std::size_t monitor) const {
    if (monitor >= skies_.size()) return 0;
    return static_cast<int>(std::lround(static_cast<double>(target_) * skies_[monitor].weight));
}

float ParticleSystem::edgeFade(const Particle& p) const {
    if (!perMonitor_ || p.home >= skies_.size()) return 1.0f;
    const RectI& r = skies_[p.home].r;
    const float d = std::min({p.x - static_cast<float>(r.l), static_cast<float>(r.r) - p.x, static_cast<float>(r.b) - p.y});
    return std::clamp(d / kEdgeFadePx, 0.05f, 1.0f);
}

void ParticleSystem::clear() {
    p_.clear();
    ambient_ = 0;
    spawnAcc_ = 0;
}

float ParticleSystem::scaleAtX(float x) const {
    for (const Span& s : spans_)
        if (x >= s.l && x < s.r) return s.scale;
    return spans_.empty() ? 1.0f : spans_.front().scale;
}

Particle* ParticleSystem::add() {
    if (p_.size() >= kHardCapacity) return nullptr;
    p_.emplace_back();
    Particle& p = p_.back();
    p.id = nextId_++;
    return &p;
}

void ParticleSystem::removeAt(std::size_t i) {
    if (!(p_[i].flags & (Particle::kDust | Particle::kLoose | Particle::kDroplet))) {
        --ambient_;
        if (p_[i].home < skies_.size()) --skies_[p_[i].home].count;
    }
    p_[i] = p_.back();
    p_.pop_back();
}

Particle* ParticleSystem::spawnAmbient(const Weather& weather, int sky) {
    if (scene_.empty() || ambient_ >= kMaxParticles) return nullptr;
    if (sky >= static_cast<int>(skies_.size()) || (sky >= 0 && skies_[static_cast<std::size_t>(sky)].r.empty())) return nullptr;
    Particle* pp = add();
    if (!pp) return nullptr;
    Particle& p = *pp;
    ++ambient_;
    const WeatherParams& wp = weather.params();
    const RectI area = sky >= 0 ? skies_[static_cast<std::size_t>(sky)].r : scene_;
    const float W = static_cast<float>(area.width()), H = static_cast<float>(area.height());
    const float L = static_cast<float>(area.l), T = static_cast<float>(area.t);

    // Share of flakes entering through the upwind side instead of the top edge equals the
    // share of the air flux through that side.
    const float fluxTop = std::max(wp.fallSpeed, 1.0f) * W;
    const float fluxSide = std::fabs(wp.wind) * H;
    if (rng_.uniform() * (fluxTop + fluxSide) < fluxSide) {
        // Own sky: enter just inside the upwind edge (faded in there), not from the neighbour.
        const float in = sky >= 0 ? 0.5f : -8.0f;
        p.x = wp.wind > 0 ? L - in : L + W + in;
        p.y = T + rng_.uniform() * H;
    } else {
        p.x = sky >= 0 ? L + rng_.uniform() * W : L - 24.0f + rng_.uniform() * (W + 48.0f);
        p.y = T - 8.0f;
    }
    if (sky >= 0) {
        p.home = static_cast<std::uint8_t>(sky);
        ++skies_[static_cast<std::size_t>(sky)].count;
    }
    const float scale = scaleAtX(p.x);
    const float u = rng_.uniform();
    const float rel = u * u;   // skewed towards small flakes
    const bool windows = rng_.uniform() < kWindowColliderShare;
    float radius96 = 1.1f + 2.3f * rel;
    float fall = 0.7f + 0.6f * rel;
    if (!windows) {   // foreground layer: a little bigger and faster (parallax)
        radius96 *= 1.2f;
        fall *= 1.15f;
    }
    p.flags = windows ? Particle::kCollidesWindows : std::uint8_t{0};
    p.radius = radius96 * wp.sizeScale * scale;
    p.fall = fall * scale;
    p.mass = 0.8f + 0.4f * rel;
    p.drift = scale * (1.3f - 0.5f * rel);
    p.opacity = 0.55f + 0.45f * rng_.uniform();
    p.swayPhase = rng_.range(-kPi, kPi);
    p.swayFreq = rng_.range(0.6f, 1.8f);
    p.swayAmp = rng_.range(6.0f, 20.0f) * scale * wp.swayScale;
    p.turbPhase = rng_.range(-kPi, kPi);
    p.turbFreq = rng_.range(1.5f, 4.5f);
    p.rotation = rng_.range(0.0f, kTwoPi);
    p.angVel = rng_.range(-1.5f, 1.5f);
    p.variant = (radius96 < 1.7f || rng_.uniform() < 0.2f)
                    ? std::uint8_t{0}
                    : static_cast<std::uint8_t>(1 + rng_.below(static_cast<std::uint32_t>(kFlakeVariants - 1)));
    if (wp.thaw) {   // wet flakes: heavier, straighter, a little transparent
        p.opacity *= 0.75f;
        p.drift *= 0.5f;
        p.swayAmp *= 0.3f;
        p.variant = 0;
    }
    p.vx = weather.windAt(p.y) * p.drift;
    p.vy = wp.fallSpeed * p.fall;
    return pp;
}

Particle* ParticleSystem::addLoose(float x, float y, float vx, float vy, float radius) {
    Particle* pp = add();
    if (!pp) return nullptr;
    Particle& p = *pp;
    const float scale = scaleAtX(x);
    p.flags = Particle::kLoose;
    p.x = x;
    p.y = y;
    p.vx = vx;
    p.vy = vy;
    p.radius = radius;
    p.fall = rng_.range(1.1f, 1.6f) * scale;
    p.drift = scale;
    p.opacity = rng_.range(0.7f, 1.0f);
    p.swayPhase = rng_.range(-kPi, kPi);
    p.swayFreq = rng_.range(0.6f, 1.8f);
    p.swayAmp = rng_.range(4.0f, 12.0f) * scale;
    p.turbPhase = rng_.range(-kPi, kPi);
    p.turbFreq = rng_.range(1.5f, 4.5f);
    p.angVel = rng_.range(-2.0f, 2.0f);
    return pp;
}

Particle* ParticleSystem::addDust(float x, float y, float vx, float vy, float radius, float life) {
    Particle* pp = addLoose(x, y, vx, vy, radius);
    if (!pp) return nullptr;
    pp->flags = Particle::kDust;
    pp->life = pp->maxLife = std::max(life, 0.05f);
    pp->fall *= 0.5f;
    return pp;
}

float ParticleSystem::obstacleDist(const Obstacle& o, float x, float y, float& nx, float& ny) {
    const float dx = x - o.cx, dy = y - o.cy;
    const float sx = dx >= 0 ? 1.0f : -1.0f, sy = dy >= 0 ? 1.0f : -1.0f;
    const float qx = std::fabs(dx) - o.hx, qy = std::fabs(dy) - o.hy;
    if (qx > 0 && qy > 0) {
        const float L = std::sqrt(qx * qx + qy * qy);
        nx = qx / L * sx;
        ny = qy / L * sy;
        return L - o.r;
    }
    if (qx > qy) {
        nx = sx;
        ny = 0;
        return qx - o.r;
    }
    nx = 0;
    ny = sy;
    return qy - o.r;
}

Particle* ParticleSystem::addDroplet(float x, float y, float vx, float vy, float dpiScale) {
    Particle* pp = add();
    if (!pp) return nullptr;
    Particle& p = *pp;
    p.flags = Particle::kDroplet;
    p.x = x;
    p.y = y;
    p.vx = vx;
    p.vy = vy;
    p.radius = 1.6f * dpiScale;
    p.fall = dpiScale;   // droplets: gravity scale
    p.opacity = 0.9f;
    return pp;
}

void ParticleSystem::burst(float cx, float cy, float radius, int count, float speed, bool dust, float dpiScale) {
    for (int i = 0; i < count; ++i) {
        const float ang = rng_.range(0.0f, kTwoPi);
        const float rr = radius * std::sqrt(rng_.uniform());
        const float dx = std::cos(ang), dy = std::sin(ang);
        const float v = speed * rng_.range(0.3f, 1.0f);
        const float size = rng_.range(1.0f, 2.4f) * dpiScale;
        const float px = cx + dx * rr, py = cy + dy * rr;
        const float vx = dx * v, vy = dy * v - 0.4f * speed;
        if (dust) {
            if (!addDust(px, py, vx, vy, size, rng_.range(0.5f, 1.1f))) return;
        } else if (!addLoose(px, py, vx, vy, size)) {
            return;
        }
    }
}

void ParticleSystem::step(float dt, const Weather& weather, SurfaceRegistry* surfaces) {
    const WeatherParams& wp = weather.params();
    if (perMonitor_ && !skies_.empty()) {
        if (!scene_.empty() && emission_) {
            for (std::size_t si = 0; si < skies_.size(); ++si) {
                Sky& sk = skies_[si];
                if (sk.r.empty()) continue;
                const int tgt = skyTarget(si);
                if (sk.count >= tgt) continue;
                const float W = static_cast<float>(sk.r.width()), H = static_cast<float>(sk.r.height());
                const float rate = static_cast<float>(tgt) * (std::max(wp.fallSpeed, 1.0f) * W + std::fabs(wp.wind) * H) / (W * H);
                const float base = rate * sk.scale;
                const float boost = std::min(base, kRefillGain * static_cast<float>(tgt - sk.count));
                sk.acc = std::min(sk.acc + (base + boost) * dt, 64.0f);
                while (sk.acc >= 1.0f && sk.count < tgt) {
                    sk.acc -= 1.0f;
                    if (!spawnAmbient(weather, static_cast<int>(si))) break;
                }
            }
        }
    } else if (!scene_.empty() && emission_ && ambient_ < target_) {
        const float W = static_cast<float>(scene_.width()), H = static_cast<float>(scene_.height());
        // Steady state: flakes leave through the bottom / downwind side at this rate.
        const float rate = static_cast<float>(target_) * (std::max(wp.fallSpeed, 1.0f) * W + std::fabs(wp.wind) * H) / (W * H);
        // Flakes that land on window tops live shorter than a full fall, so top the rate up
        // in proportion to the deficit (bounded, to avoid a dense "curtain" at start-up).
        const float base = rate * meanScale_;
        const float boost = std::min(base, kRefillGain * static_cast<float>(target_ - ambient_));
        spawnAcc_ = std::min(spawnAcc_ + (base + boost) * dt, 64.0f);
        while (spawnAcc_ >= 1.0f && ambient_ < target_) {
            spawnAcc_ -= 1.0f;
            if (!spawnAmbient(weather)) break;
        }
    }
    const float minX = static_cast<float>(scene_.l) - kSceneMargin;
    const float maxX = static_cast<float>(scene_.r) + kSceneMargin;
    const float minY = static_cast<float>(scene_.t) - 4.0f * kSceneMargin;
    const float maxY = static_cast<float>(scene_.b) + kSceneMargin;
    const float a = std::min(1.0f, dt * kVelocityResponse);
    const float turb = wp.turbulence;
    const float fallSpeed = wp.fallSpeed;
    const float depositK = kDepositArea * wp.growth;

    const float gustK = gT_ > 0 ? std::min(1.0f, dt * 8.0f) * (gT_ / kGustSeconds) : 0.0f;
    if (gT_ > 0) gT_ -= dt;
    const float uOuter = 1.35f * uR_;

    for (std::size_t i = 0; i < p_.size();) {
        Particle& p = p_[i];
        const float x0 = p.x, y0 = p.y;
        if (p.flags & Particle::kDroplet) {   // falls straight and fast, splashes on impact
            p.vy += 1400.0f * p.fall * dt;
            p.x = x0 + p.vx * dt;
            p.y = y0 + p.vy * dt;
            SurfaceHit hit;
            if (surfaces && surfaces->sweep(x0, y0, p.x, p.y, true, hit)) {
                const float d = p.fall;
                ++splashes_;
                const float hx = hit.x, hy = hit.y;
                removeAt(i);
                const int n = 2 + static_cast<int>(rng_.below(2));
                for (int k = 0; k < n; ++k)
                    addDust(hx, hy - 1.0f, rng_.range(-60.0f, 60.0f) * d, -rng_.range(40.0f, 110.0f) * d, 0.8f * d, 0.3f);
                continue;
            }
            if (p.y > maxY || p.x < minX || p.x > maxX) {
                removeAt(i);
                continue;
            }
            ++i;
            continue;
        }
        const float wind = weather.windAt(y0);
        const float st = fastSin(p.turbPhase);
        const float ct = fastSin(wrapPi(p.turbPhase + 0.5f * kPi));
        const float tvx = (wind + turb * st) * p.drift + p.swayAmp * fastSin(p.swayPhase);
        const float tvy = fallSpeed * p.fall + 0.35f * turb * ct * p.drift;
        p.vx += (tvx - p.vx) * a;
        p.vy += (tvy - p.vy) * a;
        if (gustK > 0) {   // swipe gust
            const float dx = x0 - gx_, dy = y0 - gy_;
            if (std::fabs(dx) < gR_ && std::fabs(dy) < gR_) {
                const float f = 1.0f - (dx * dx + dy * dy) / (gR_ * gR_);
                if (f > 0) {
                    p.vx += (gvx_ - p.vx) * f * gustK;
                    p.vy += (gvy_ - p.vy) * f * gustK;
                }
            }
        }
        const bool flake = !(p.flags & Particle::kDust);
        if (uOn_ && flake) {   // umbrella: slide around the dome instead of through it
            const float dx = x0 - ux_, dy = y0 - uy_;
            if (std::fabs(dx) < uOuter && std::fabs(dy) < uOuter) {
                const float d2 = dx * dx + dy * dy;
                if (d2 < uOuter * uOuter && d2 > 1e-6f) {
                    const float d = std::sqrt(d2);
                    const float nx = dx / d, ny = dy / d;
                    const float vn = p.vx * nx + p.vy * ny;
                    if (vn < 0) {
                        const float t = std::clamp((uOuter - d) / (uOuter - uR_), 0.0f, 1.0f);
                        const float w = t * t * (3.0f - 2.0f * t);
                        p.vx -= vn * nx * w;
                        p.vy -= vn * ny * w;
                        // Near the crown there is no tangential motion: nudge it off sideways.
                        p.vx += (dx >= 0 ? 1.0f : -1.0f) * w * std::max(0.0f, -ny) * 0.5f * std::fabs(vn);
                    }
                }
            }
        }
        if (flake) {   // excluded windows ("around"): flow around their rounded rects
            for (const Obstacle& o : obstacles_) {
                if (std::fabs(x0 - o.cx) > o.hx + o.r + o.band || std::fabs(y0 - o.cy) > o.hy + o.r + o.band) continue;
                float nx, ny;
                const float dist = obstacleDist(o, x0, y0, nx, ny);
                if (dist >= o.band) continue;
                const float vn = p.vx * nx + p.vy * ny;
                if (vn >= 0) continue;
                const float t = std::clamp(1.0f - dist / o.band, 0.0f, 1.0f);
                const float w = t * t * (3.0f - 2.0f * t);
                p.vx -= vn * nx * w;
                p.vy -= vn * ny * w;
                // On the roof there is no tangential motion: slide off towards the nearer side.
                if (ny < -0.5f) p.vx += (x0 >= o.cx ? 1.0f : -1.0f) * w * std::max(std::fabs(vn), fallSpeed * 0.8f);
            }
        }
        p.x = x0 + p.vx * dt;
        p.y = y0 + p.vy * dt;
        if (flake) {
            for (const Obstacle& o : obstacles_) {
                if (std::fabs(p.x - o.cx) > o.hx + o.r || std::fabs(p.y - o.cy) > o.hy + o.r) continue;
                float nx, ny;
                const float dist = obstacleDist(o, p.x, p.y, nx, ny);
                if (dist < 0) {   // never inside
                    p.x -= nx * dist;
                    p.y -= ny * dist;
                }
            }
        }
        if (uOn_ && flake) {
            const float dx = p.x - ux_, dy = p.y - uy_;
            const float d2 = dx * dx + dy * dy;
            if (d2 < uR_ * uR_) {   // never inside the dome
                const float d = std::sqrt(std::max(d2, 1e-6f));
                const float nx = d > 1e-3f ? dx / d : (x0 >= ux_ ? 1.0f : -1.0f), ny = d > 1e-3f ? dy / d : 0.0f;
                p.x = ux_ + nx * uR_;
                p.y = uy_ + ny * uR_;
            }
        }
        p.swayPhase = wrapPi(p.swayPhase + p.swayFreq * dt);
        p.turbPhase = wrapPi(p.turbPhase + p.turbFreq * dt);
        p.rotation += p.angVel * dt;
        if (p.rotation > kTwoPi) p.rotation -= kTwoPi;
        else if (p.rotation < 0) p.rotation += kTwoPi;

        if (p.flags & Particle::kDust) {
            p.life -= dt;
            if (p.life <= 0) {
                removeAt(i);
                continue;
            }
            ++i;
            continue;
        }
        if (surfaces && p.y > y0) {
            SurfaceHit hit;
            const bool windows = (p.flags & Particle::kCollidesWindows) != 0;
            // Single sky: foreground flakes only land on the lowest ground in their column.
            if (surfaces->sweep(x0, y0, p.x, p.y, windows, hit, 0, !perMonitor_ && !windows)) {
                deposited_ += surfaces->deposit(hit, depositK * p.radius * p.radius);
                ++landed_;
                removeAt(i);
                continue;
            }
        }
        // Only after the swept test: a very fast flake must not skip a surface by leaving
        // the scene within the same step.
        if (p.y > maxY || p.x < minX || p.x > maxX || p.y < minY) {
            removeAt(i);
            continue;
        }
        if (perMonitor_ && p.home < skies_.size()) {   // never leaves its own monitor (faded out by then)
            const RectI& r = skies_[p.home].r;
            if (p.y > static_cast<float>(r.b) + 1.0f || p.x < static_cast<float>(r.l) - 1.0f || p.x > static_cast<float>(r.r) + 1.0f) {
                removeAt(i);
                continue;
            }
        }
        ++i;
    }
}

}  // namespace snow
