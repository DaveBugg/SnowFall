// Shared plain types. No <windows.h> here: everything in core/ must stay portable
// so the simulation can be unit-tested without Win32.
#pragma once
#include <algorithm>
#include <cstdint>

namespace snow {

// Session-unique identifiers. Never derived from an HWND value and never reused
// during a process lifetime (HWND values can be recycled by the OS).
using WindowId = std::uint64_t;
using SurfaceId = std::uint64_t;

struct Vec2 {
    float x = 0, y = 0;
};

// Integer rectangle in PHYSICAL pixels of the virtual desktop (may be negative).
// Half-open: [l, r) x [t, b).
struct RectI {
    int l = 0, t = 0, r = 0, b = 0;
    int width() const { return r - l; }
    int height() const { return b - t; }
    bool empty() const { return r <= l || b <= t; }
    bool contains(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
    bool intersects(const RectI& o) const { return l < o.r && o.l < r && t < o.b && o.t < b; }
    RectI intersect(const RectI& o) const {
        RectI x{std::max(l, o.l), std::max(t, o.t), std::min(r, o.r), std::min(b, o.b)};
        if (x.empty()) return RectI{};
        return x;
    }
    bool operator==(const RectI&) const = default;
};

}  // namespace snow
