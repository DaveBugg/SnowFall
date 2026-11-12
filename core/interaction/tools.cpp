#include "interaction/tools.h"

#include <algorithm>
#include <cmath>

#include "simulation/particles.h"
#include "surfaces/surface_registry.h"

namespace snow {

namespace {
// Column index range of a surface covered by [xa, xb] (world px). Returns false if empty.
bool columnRange(const Surface& s, float xa, float xb, int& i0, int& i1) {
    const int n = s.hm.columns();
    if (n == 0) return false;
    const float step = s.hm.step();
    i0 = std::max(0, static_cast<int>(std::ceil((xa - static_cast<float>(s.left)) / step)));
    i1 = std::min(n - 1, static_cast<int>(std::floor((xb - static_cast<float>(s.left)) / step)));
    return i0 <= i1;
}
}  // namespace

float brushDisc(SurfaceRegistry& reg, float x, float y, float radius, float maxArea, Vec2* contact) {
    float removed = 0;
    if (radius <= 0 || maxArea <= 0) return 0;
    for (Surface& s : reg.surfaces()) {
        if (!s.active) continue;
        const float top = static_cast<float>(s.top);
        if (y - radius >= top || y + radius <= top - s.hm.peak()) continue;
        int i0, i1;
        if (!columnRange(s, x - radius, x + radius, i0, i1)) continue;
        for (int i = i0; i <= i1; ++i) {
            const float h = s.hm.height(i);
            if (h <= 0 || s.hm.blocked(i)) continue;
            const float dx = s.worldX(i) - x;
            const float half = std::sqrt(std::max(0.0f, radius * radius - dx * dx));
            const float discBottom = y + half, discTop = y - half;
            if (discBottom <= top - h || discTop >= top) continue;
            float newH = std::max(0.0f, top - discBottom);
            const float budget = maxArea - removed;
            const float want = (h - newH) * s.hm.step();
            if (want > budget) newH = h - budget / s.hm.step();
            if (contact) *contact = Vec2{s.worldX(i), top - h};
            removed += s.hm.lowerTo(i, newH);
            if (removed >= maxArea) return removed;
        }
    }
    return removed;
}

float shovelBand(SurfaceRegistry& reg, float x, float y, float halfWidth, float reach, Vec2* contact) {
    float removed = 0;
    for (Surface& s : reg.surfaces()) {
        if (!s.active) continue;
        const float top = static_cast<float>(s.top);
        if (y > top + reach || y < top - s.hm.peak() - reach) continue;
        int i0, i1;
        if (!columnRange(s, x - halfWidth, x + halfWidth, i0, i1)) continue;
        for (int i = i0; i <= i1; ++i) {
            const float h = s.hm.height(i);
            if (h <= 0 || s.hm.blocked(i)) continue;
            if (y < top - h - reach) continue;
            if (contact) *contact = Vec2{s.worldX(i), top - h};
            removed += s.hm.lowerTo(i, 0.0f);
        }
    }
    return removed;
}

float scoopBand(SurfaceRegistry& reg, float x, float y, float halfWidth, float reach, float room, Vec2* contact) {
    float taken = 0;
    for (Surface& s : reg.surfaces()) {
        if (!s.active || room - taken <= 0) continue;
        const float top = static_cast<float>(s.top);
        if (y > top + reach || y < top - s.hm.peak() - reach) continue;
        int i0, i1;
        if (!columnRange(s, x - halfWidth, x + halfWidth, i0, i1)) continue;
        const int c = std::clamp(s.hm.columnAt(x - static_cast<float>(s.left)), i0, i1);
        // Nearest columns first, alternating sides.
        for (int k = 0; k <= std::max(c - i0, i1 - c) && room - taken > 0; ++k) {
            for (int side = 0; side < 2 && room - taken > 0; ++side) {
                if (k == 0 && side == 1) break;
                const int i = side == 0 ? c + k : c - k;
                if (i < i0 || i > i1) continue;
                const float h = s.hm.height(i);
                if (h <= 0 || s.hm.blocked(i) || y < top - h - reach) continue;
                const float step = s.hm.step();
                const float take = std::min(h, (room - taken) / step);
                if (contact) *contact = Vec2{s.worldX(i), top - h};
                taken += s.hm.lowerTo(i, h - take);
            }
        }
    }
    return taken;
}

float scoopStroke(SurfaceRegistry& reg, float x0, float y0, float x1, float y1, float width, float& load,
                  float capacity, ParticleSystem* dust, float dpiScale) {
    const float half = width * 0.5f;
    const float dist = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const int n = std::clamp(static_cast<int>(std::ceil(dist / std::max(half * 0.5f, 1.0f))), 1, 512);
    float total = 0;
    for (int k = 1; k <= n; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(n);
        const float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
        if (load < capacity) {
            const float got = scoopBand(reg, x, y, half, half, capacity - load, nullptr);
            load += got;
            total += got;
        }
        if (load >= capacity - 1e-3f) {   // full: the rest is pushed aside like before
            Vec2 contact{};
            const float got = shovelBand(reg, x, y, half, half, &contact);
            if (got > 0 && dust) emitDust(*dust, contact.x, contact.y, got, dpiScale);
            total += got;
        }
    }
    return total;
}

void emitDust(ParticleSystem& particles, float x, float y, float area, float dpiScale) {
    if (area <= 0.5f) return;
    const int count = std::min(6, 1 + static_cast<int>(area / (40.0f * dpiScale * dpiScale)));
    Rng& rng = particles.rng();
    for (int i = 0; i < count; ++i) {
        const float vx = rng.range(-110.0f, 110.0f) * dpiScale;
        const float vy = rng.range(-190.0f, -50.0f) * dpiScale;
        particles.addDust(x + rng.range(-6.0f, 6.0f) * dpiScale, y - rng.range(0.0f, 4.0f) * dpiScale, vx, vy,
                          rng.range(0.8f, 1.8f) * dpiScale, rng.range(0.5f, 1.1f));
    }
}

Vec2 toolHotspotLocal(Tool tool, float dpiScale) {
    switch (tool) {
        case Tool::Mitten: return Vec2{0.0f, -kCursorHotspotShare * kMittenCursorAt96 * dpiScale};
        case Tool::Shovel: return Vec2{0.0f, kCursorHotspotShare * kShovelCursorAt96 * dpiScale};
        default: return Vec2{0.0f, 0.0f};
    }
}

SpriteInstance toolCursorSprite(Tool tool, float px, float py, float dpiScale) {
    SpriteInstance c;
    float half = kSnowballCursorAt96 * dpiScale, rot = 0;
    c.shape = Shape::Snowball;
    c.opacity = 0.85f;
    if (tool == Tool::Mitten) {
        half = kMittenCursorAt96 * dpiScale;
        rot = kMittenCursorRotation;
        c.shape = Shape::Mitten;
        c.opacity = 1.0f;
    } else if (tool == Tool::Shovel) {
        half = kShovelCursorAt96 * dpiScale;
        rot = kShovelCursorRotation;
        c.shape = Shape::Shovel;
        c.opacity = 1.0f;
    }
    const Vec2 h = toolHotspotLocal(tool, dpiScale);
    const float cs = std::cos(rot), sn = std::sin(rot);
    c.x = px - (h.x * cs - h.y * sn);
    c.y = py - (h.x * sn + h.y * cs);
    c.halfW = c.halfH = half;
    c.rotation = rot;
    return c;
}

namespace {
template <typename Apply>
float stroke(float x0, float y0, float x1, float y1, float spacing, ParticleSystem* dust, float dpiScale,
             Apply&& apply) {
    const float dist = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const int n = std::clamp(static_cast<int>(std::ceil(dist / std::max(spacing, 1.0f))), 1, 512);
    float total = 0;
    for (int k = 1; k <= n; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(n);
        Vec2 contact{};
        const float got = apply(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, &contact);
        if (got > 0 && dust) emitDust(*dust, contact.x, contact.y, got, dpiScale);
        total += got;
    }
    return total;
}
}  // namespace

float mittenStroke(SurfaceRegistry& reg, float x0, float y0, float x1, float y1, float radius,
                   ParticleSystem* dust, float dpiScale) {
    return stroke(x0, y0, x1, y1, radius * 0.5f, dust, dpiScale, [&](float x, float y, Vec2* c) {
        return brushDisc(reg, x, y, radius, 1e30f, c);
    });
}

float shovelStroke(SurfaceRegistry& reg, float x0, float y0, float x1, float y1, float width,
                   ParticleSystem* dust, float dpiScale) {
    const float half = width * 0.5f;
    return stroke(x0, y0, x1, y1, half * 0.5f, dust, dpiScale, [&](float x, float y, Vec2* c) {
        return shovelBand(reg, x, y, half, half, c);
    });
}

}  // namespace snow
