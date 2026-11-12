// Procedural application icon: a six-armed snowflake on a round blue badge (original artwork).
// Rendered analytically with 4x4 supersampling into a 32-bit DIB, then wrapped in an HICON.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "tray_icon.h"

namespace snow {
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

Rgb mix(const Rgb& a, const Rgb& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

std::vector<Seg> buildFlake(bool small) {
    std::vector<Seg> segs;
    const float kPi = 3.14159265f;
    const float armLen = small ? 0.68f : 0.66f;
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
                segs.push_back({ox, oy, ox + std::cos(ba) * branches[b].len, oy + std::sin(ba) * branches[b].len});
            }
        }
    }
    return segs;
}

}  // namespace

HICON TrayIcon::createAppIcon(int size, bool paused) {
    size = std::clamp(size, 8, 512);
    const bool small = size <= 24;
    const int ss = 4;
    const std::vector<Seg> segs = buildFlake(small);
    const float halfWidth = small ? 0.088f : 0.052f;

    const Rgb badgeTop = paused ? Rgb{158, 167, 177} : Rgb{86, 180, 247};
    const Rgb badgeBottom = paused ? Rgb{96, 105, 116} : Rgb{24, 98, 198};
    const Rgb rim = paused ? Rgb{70, 77, 87} : Rgb{14, 66, 150};
    const Rgb flake = paused ? Rgb{236, 239, 243} : Rgb{255, 255, 255};
    const Rgb markFill = {38, 43, 52};
    const Rgb markBar = {255, 255, 255};

    const float badgeR = 0.97f;
    const float rimR = small ? 0.86f : 0.90f;
    const float markCx = 0.50f, markCy = 0.50f, markR = 0.46f;

    std::vector<uint32_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size));
    const float inv = 2.0f / static_cast<float>(size * ss);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float accR = 0, accG = 0, accB = 0, accA = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const float u = (static_cast<float>(x * ss + sx) + 0.5f) * inv - 1.0f;
                    const float v = (static_cast<float>(y * ss + sy) + 0.5f) * inv - 1.0f;
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
            uint32_t out = 0;
            if (accA > 0) {
                // Straight (non-premultiplied) alpha, as icons expect.
                const auto r = static_cast<uint32_t>(std::lround(accR / accA));
                const auto g = static_cast<uint32_t>(std::lround(accG / accA));
                const auto b = static_cast<uint32_t>(std::lround(accB / accA));
                const auto a = static_cast<uint32_t>(std::lround(accA * 255.0f / static_cast<float>(ss * ss)));
                out = (a << 24) | (r << 16) | (g << 8) | b;
            }
            pixels[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] = out;
        }
    }

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;   // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    memcpy(bits, pixels.data(), pixels.size() * sizeof(uint32_t));

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (!mask) {
        DeleteObject(color);
        return nullptr;
    }
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}  // namespace snow
