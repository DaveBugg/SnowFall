#include "interaction/splats.h"

#include <algorithm>
#include <cmath>

#include "interaction/snow_objects.h"
#include "simulation/chunks.h"
#include "simulation/particles.h"
#include "surfaces/surface_registry.h"

namespace snow {

namespace {
constexpr float kBurstShare = 0.1f;   // of the ball's snow flies off as dust on impact
}

int SplatSystem::mainCount() const {
    int n = 0;
    for (const Splat& s : splats_) n += s.life < 0 ? 1 : 0;
    return n;
}

double SplatSystem::liveMass() const {
    double m = 0;
    for (const Splat& s : splats_) m += s.mass;
    return m;
}

void SplatSystem::clear() {
    for (const Splat& s : splats_) dust_ += s.mass;
    splats_.clear();
}

bool SplatSystem::tryThrow(const SurfaceRegistry& reg, const Ball& b, float vx, float vy, ParticleSystem* fx) {
    const float speed = std::sqrt(vx * vx + vy * vy);
    if (speed <= kSplatThrowSpeedAt96 * b.dpi) return false;
    // Where would the ball be a moment later? Walk back along the throw to the first point
    // that lies on an application window.
    for (int k = 8; k >= 1; --k) {
        const float t = kSplatLookAhead * static_cast<float>(k) / 8.0f;
        const float px = b.x + vx * t, py = b.y + vy * t;
        const int idx = reg.frontWindowAt(px, py);
        if (idx < 0) continue;
        const WindowInfo& w = reg.windows()[static_cast<std::size_t>(idx)];
        if (w.kind != WindowKind::App) continue;
        Splat s;
        s.window = w.id;
        s.dpi = b.dpi;
        s.halfW = b.r * rng_.range(1.6f, 2.2f);
        s.halfH = s.halfW * rng_.range(0.7f, 0.95f);
        s.rotation = rng_.range(-0.6f, 0.6f);
        s.variant = static_cast<std::uint8_t>(rng_.below(4));
        s.stick = rng_.range(1.0f, 3.0f);
        s.lx = std::clamp(px - static_cast<float>(w.bounds.l), std::min(s.halfW, 0.5f * w.bounds.width()),
                          std::max(static_cast<float>(w.bounds.width()) - s.halfW, 0.5f * w.bounds.width()));
        s.ly = std::clamp(py - static_cast<float>(w.bounds.t), std::min(s.halfH, 0.5f * w.bounds.height()),
                          std::max(static_cast<float>(w.bounds.height()) - s.halfH, 0.5f * w.bounds.height()));
        s.lastL = w.bounds.l;
        s.lastT = w.bounds.t;
        s.mass = b.collected * (1.0f - kBurstShare);
        splatted_ += b.collected;
        dust_ += b.collected * kBurstShare;
        if (fx) {
            const float d = b.dpi;
            const float cx = static_cast<float>(w.bounds.l) + s.lx, cy = static_cast<float>(w.bounds.t) + s.ly;
            for (int i = 0; i < 10; ++i) {
                const float a = rng_.range(0.0f, 6.2831853f);
                const float v = rng_.range(60.0f, 260.0f) * d;
                Particle* p = fx->addDust(cx + std::cos(a) * s.halfW * 0.6f, cy + std::sin(a) * s.halfH * 0.6f, std::cos(a) * v,
                                          std::sin(a) * v - 60.0f * d, rng_.range(1.0f, 2.6f) * d, rng_.range(0.4f, 0.9f));
                if (p && i % 3 == 0) p->fall *= 4.0f;   // clumps drop
            }
        }
        splats_.push_back(s);
        return true;
    }
    return false;
}

void SplatSystem::fallOff(Splat& s, float wx, float wy, float vx, float vy, ChunkSystem* chunks) {
    if (chunks && s.mass > 0) {
        const float d = s.dpi;
        Chunk* c = chunks->spawnFree(wx, wy, std::max(4.0f * d, 0.8f * s.halfW), std::max(3.0f * d, 0.45f * s.halfH),
                                     s.mass, vx, vy, d, 0);
        if (c) c->angVel = rng_.range(-2.0f, 2.0f);
        dropped_ += s.mass;
    } else {
        dust_ += s.mass;
    }
    s.mass = 0;
}

void SplatSystem::step(float dt, const SurfaceRegistry& reg, ChunkSystem* chunks, ParticleSystem* /*fx*/) {
    if (splats_.empty() || dt <= 0) return;
    const std::size_t count = splats_.size();
    for (std::size_t k = 0; k < count && k < splats_.size();) {
        Splat& s = splats_[k];
        bool remove = false;
        if (s.life >= 0) {
            s.life -= dt;
            remove = s.life <= 0;
        } else {
            const int idx = reg.windowIndex(s.window);
            if (idx < 0) {
                ++k;
                continue;   // handled by onLayout
            }
            const RectI w = reg.windows()[static_cast<std::size_t>(idx)].bounds;
            if (s.stick > 0) {
                s.stick -= dt;
            } else {   // slides down the pane, a little faster every moment
                s.vy = std::min(s.vy + 25.0f * s.dpi * dt, 140.0f * s.dpi);
                const float dy = s.vy * dt;
                s.ly += dy;
                s.slid += dy;
                if (s.slid >= 9.0f * s.dpi && s.mass > 0) {   // trail of fading bits
                    s.slid = 0;
                    Splat bit = s;
                    bit.halfW = s.halfW * rng_.range(0.18f, 0.35f);
                    bit.halfH = bit.halfW * rng_.range(0.6f, 1.0f);
                    bit.ly = s.ly - s.halfH * 0.8f;
                    bit.lx = s.lx + rng_.range(-0.4f, 0.4f) * s.halfW;
                    bit.life = bit.maxLife = rng_.range(1.8f, 2.8f);
                    bit.rotation = rng_.range(-1.0f, 1.0f);
                    bit.variant = static_cast<std::uint8_t>(rng_.below(4));
                    const float m = s.mass * 0.015f;
                    s.mass -= m;
                    dust_ += m;
                    bit.mass = 0;
                    splats_.push_back(bit);
                }
                Splat& cur = splats_[k];   // push_back may have moved it
                if (cur.ly + 0.6f * cur.halfH >= static_cast<float>(w.height())) {   // off the bottom edge
                    fallOff(cur, static_cast<float>(w.l) + cur.lx, static_cast<float>(w.b) + 2.0f, 0.0f, cur.vy, chunks);
                    remove = true;
                }
            }
        }
        if (remove) splats_.erase(splats_.begin() + static_cast<std::ptrdiff_t>(k));
        else ++k;
    }
}

void SplatSystem::onLayout(const SurfaceRegistry& reg, float elapsed, ChunkSystem* chunks) {
    const float inv = 1.0f / std::max(elapsed, 1.0f / 240.0f);
    for (std::size_t k = 0; k < splats_.size();) {
        Splat& s = splats_[k];
        const int idx = reg.windowIndex(s.window);
        if (idx < 0) {   // minimised / closed: it goes with the window
            dust_ += s.mass;
            splats_.erase(splats_.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        const RectI w = reg.windows()[static_cast<std::size_t>(idx)].bounds;
        const int dx = w.l - s.lastL, dy = w.t - s.lastT;
        const float jump = static_cast<float>(std::max(std::abs(dx), std::abs(dy)));
        const float speed = std::sqrt(static_cast<float>(dx * dx + dy * dy)) * inv;
        const bool teleport = jump > 600.0f * s.dpi;
        if (!teleport && speed > kSplatJerkSpeedAt96 * s.dpi) {   // knocked off by a jerk
            if (s.life < 0) {
                const float vx = static_cast<float>(dx) * inv * 0.2f, vy = static_cast<float>(dy) * inv * 0.2f;
                fallOff(s, static_cast<float>(s.lastL) + s.lx, static_cast<float>(s.lastT) + s.ly, vx, vy, chunks);
            }
            splats_.erase(splats_.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        s.lastL = w.l;
        s.lastT = w.t;
        ++k;
    }
}

void SplatSystem::appendBatches(const SurfaceRegistry& reg, std::vector<SpriteBatch>& out) const {
    for (std::size_t k = 0; k < splats_.size(); ++k) {
        const WindowId win = splats_[k].window;
        bool first = true;
        for (std::size_t j = 0; j < k; ++j) first = first && splats_[j].window != win;
        if (!first) continue;   // batch already made for this window
        const int idx = reg.windowIndex(win);
        if (idx < 0) continue;
        reg.visibleRegion(idx, clip_);
        if (clip_.empty()) continue;
        const RectI w = reg.windows()[static_cast<std::size_t>(idx)].bounds;
        out.emplace_back();
        SpriteBatch& b = out.back();
        b.clip = clip_;
        for (std::size_t j = k; j < splats_.size(); ++j) {
            const Splat& s = splats_[j];
            if (s.window != win) continue;
            SpriteInstance sp;
            sp.x = static_cast<float>(w.l) + s.lx;
            sp.y = static_cast<float>(w.t) + s.ly;
            sp.vy = s.life < 0 && s.stick <= 0 ? s.vy : 0.0f;
            sp.halfW = s.halfW;
            sp.halfH = s.halfH;
            sp.rotation = s.rotation;
            sp.opacity = s.life < 0 ? 1.0f : 0.8f * std::clamp(s.life / s.maxLife, 0.0f, 1.0f);
            sp.shape = Shape::Splat;
            sp.variant = s.variant;
            b.sprites.push_back(sp);
        }
    }
}

}  // namespace snow
