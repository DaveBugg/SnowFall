#include "app/icon_render.h"

#include <algorithm>
#include <cmath>

namespace snow::mac {
namespace {

struct Seg {
    float ax, ay, bx, by;
};
struct Rgb {
    float r, g, b;
};

float segDistance(const Seg& s, float px, float py) {
    const float vx = s.bx - s.ax, vy = s.by - s.ay;
    const float wx = px - s.ax, wy = py - s.ay;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0 ? (wx * vx + wy * vy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = wx - t * vx, dy = wy - t * vy;
    return std::sqrt(dx * dx + dy * dy);
}

Rgb mix(const Rgb& a, const Rgb& b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

std::vector<Seg> buildFlake(bool small, float scale) {
    std::vector<Seg> segs;
    const float kPi = 3.14159265f;
    const float armLen = (small ? 0.68f : 0.66f) * scale;
    struct Branch {
        float at, len;
    };
    const Branch branchesBig[] = {{0.40f, 0.25f}, {0.68f, 0.16f}};
    const Branch branchesSmall[] = {{0.52f, 0.24f}};
    const Branch* branches = small ? branchesSmall : branchesBig;
    const int branchCount = small ? 1 : 2;
    for (int k = 0; k < 6; ++k) {
        const float a = -kPi / 2 + static_cast<float>(k) * kPi / 3;
        const float dx = std::cos(a), dy = std::sin(a);
        segs.push_back({0, 0, dx * armLen, dy * armLen});
        for (int b = 0; b < branchCount; ++b) {
            const float ox = dx * armLen * branches[b].at, oy = dy * armLen * branches[b].at;
            for (int side = -1; side <= 1; side += 2) {
                const float ba = a + static_cast<float>(side) * kPi / 3;
                const float len = branches[b].len * scale;
                segs.push_back({ox, oy, ox + std::cos(ba) * len, oy + std::sin(ba) * len});
            }
        }
    }
    return segs;
}

void store(std::vector<std::uint8_t>& px, int size, int x, int y, float r, float g, float b, float a) {
    // r, g, b in 0..255 straight; a in 0..1 -> premultiplied bytes.
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4;
    px[i + 0] = static_cast<std::uint8_t>(std::lround(std::clamp(r * a, 0.0f, 255.0f)));
    px[i + 1] = static_cast<std::uint8_t>(std::lround(std::clamp(g * a, 0.0f, 255.0f)));
    px[i + 2] = static_cast<std::uint8_t>(std::lround(std::clamp(b * a, 0.0f, 255.0f)));
    px[i + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(a * 255.0f, 0.0f, 255.0f)));
}

}  // namespace

std::vector<std::uint8_t> renderAppIconRGBA(int size, bool paused) {
    size = std::clamp(size, 8, 1024);
    const bool small = size <= 24;
    const int ss = 4;
    const std::vector<Seg> segs = buildFlake(small, 1.0f);
    const float halfWidth = small ? 0.088f : 0.052f;

    const Rgb badgeTop = paused ? Rgb{158, 167, 177} : Rgb{86, 180, 247};
    const Rgb badgeBottom = paused ? Rgb{96, 105, 116} : Rgb{24, 98, 198};
    const Rgb rim = paused ? Rgb{70, 77, 87} : Rgb{14, 66, 150};
    const Rgb flake = paused ? Rgb{236, 239, 243} : Rgb{255, 255, 255};
    const Rgb markFill = {38, 43, 52};
    const Rgb markBar = {255, 255, 255};
    // macOS icons leave a margin around the artwork (the badge spans ~80% of the canvas).
    const float margin = small ? 1.0f : 0.80f;
    const float badgeR = 0.97f, rimR = small ? 0.86f : 0.90f;
    const float markCx = 0.50f, markCy = 0.50f, markR = 0.46f;

    std::vector<std::uint8_t> px(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    const float inv = 2.0f / static_cast<float>(size * ss);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float accR = 0, accG = 0, accB = 0, accA = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const float u = ((static_cast<float>(x * ss + sx) + 0.5f) * inv - 1.0f) / margin;
                    const float v = ((static_cast<float>(y * ss + sy) + 0.5f) * inv - 1.0f) / margin;
                    const float d = std::sqrt(u * u + v * v);
                    bool hit = false;
                    Rgb c{};
                    if (d <= badgeR) {
                        hit = true;
                        c = d > rimR ? rim : mix(badgeTop, badgeBottom, (v + 1.0f) * 0.5f);
                        bool onFlake = d < halfWidth * 1.9f;
                        if (!onFlake && d < 0.80f) {
                            for (const Seg& s : segs) {
                                if (segDistance(s, u, v) <= halfWidth) {
                                    onFlake = true;
                                    break;
                                }
                            }
                        }
                        if (onFlake) c = flake;
                    }
                    if (paused) {
                        const float mx = u - markCx, my = v - markCy;
                        if (std::sqrt(mx * mx + my * my) <= markR) {
                            hit = true;
                            c = markFill;
                            const float ax = std::fabs(mx);
                            if (ax >= 0.07f && ax <= 0.20f && std::fabs(my) <= 0.24f) c = markBar;
                        }
                    }
                    if (hit) {
                        accR += c.r;
                        accG += c.g;
                        accB += c.b;
                        accA += 1.0f;
                    }
                }
            }
            if (accA > 0) store(px, size, x, y, accR / accA, accG / accA, accB / accA, accA / static_cast<float>(ss * ss));
        }
    }
    return px;
}

std::vector<std::uint8_t> renderMenuBarGlyphRGBA(int size, bool paused) {
    size = std::clamp(size, 8, 256);
    const int ss = 4;
    const bool small = size <= 24;
    const std::vector<Seg> segs = buildFlake(true, 1.35f);
    const float halfWidth = small ? 0.10f : 0.085f;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    const float inv = 2.0f / static_cast<float>(size * ss);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const float u = (static_cast<float>(x * ss + sx) + 0.5f) * inv - 1.0f;
                    const float v = (static_cast<float>(y * ss + sy) + 0.5f) * inv - 1.0f;
                    bool on = std::sqrt(u * u + v * v) < halfWidth * 1.9f;
                    for (std::size_t k = 0; !on && k < segs.size(); ++k) on = segDistance(segs[k], u, v) <= halfWidth;
                    if (paused) {
                        // Cut-out pause mark in the lower right corner.
                        const float mx = u - 0.55f, my = v - 0.55f;
                        const float md = std::sqrt(mx * mx + my * my);
                        if (md <= 0.45f) {
                            const float ax = std::fabs(mx);
                            on = md > 0.40f ? false : (ax >= 0.08f && ax <= 0.22f && std::fabs(my) <= 0.24f);
                        }
                    }
                    if (on) ++hits;
                }
            }
            if (hits) store(px, size, x, y, 0, 0, 0, static_cast<float>(hits) / static_cast<float>(ss * ss));
        }
    }
    return px;
}

}  // namespace snow::mac
