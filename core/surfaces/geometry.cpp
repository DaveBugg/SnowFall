#include "surfaces/geometry.h"

#include <algorithm>

namespace snow {

void subtractRect(const RectI& a, const RectI& hole, std::vector<RectI>& out) {
    if (a.empty()) return;
    if (hole.empty() || !a.intersects(hole)) {
        out.push_back(a);
        return;
    }
    if (hole.t > a.t) out.push_back(RectI{a.l, a.t, a.r, hole.t});
    if (hole.b < a.b) out.push_back(RectI{a.l, hole.b, a.r, a.b});
    const int t = std::max(a.t, hole.t);
    const int b = std::min(a.b, hole.b);
    if (hole.l > a.l) out.push_back(RectI{a.l, t, hole.l, b});
    if (hole.r < a.r) out.push_back(RectI{hole.r, t, a.r, b});
}

static void subtractAll(std::span<const RectI> holes, std::vector<RectI>& io,
                        std::vector<RectI>& scratch) {
    for (const RectI& h : holes) {
        if (io.empty()) break;
        scratch.clear();
        for (const RectI& r : io) subtractRect(r, h, scratch);
        io.swap(scratch);
    }
}

void rectMinusRects(const RectI& base, std::span<const RectI> holes, std::vector<RectI>& out,
                    std::vector<RectI>& scratch) {
    out.clear();
    if (base.empty()) return;
    out.push_back(base);
    subtractAll(holes, out, scratch);
}

void clipRegion(const RectI& base, std::span<const RectI> within, std::span<const RectI> holes,
                std::vector<RectI>& out, std::vector<RectI>& scratch, std::vector<RectI>& scratch2) {
    out.clear();
    for (const RectI& w : within) {
        const RectI part = base.intersect(w);
        if (part.empty()) continue;
        scratch2.clear();
        scratch2.push_back(part);
        subtractAll(holes, scratch2, scratch);
        out.insert(out.end(), scratch2.begin(), scratch2.end());
    }
}

bool pointInAny(std::span<const RectI> rects, int x, int y) {
    for (const RectI& r : rects)
        if (r.contains(x, y)) return true;
    return false;
}

long long totalArea(std::span<const RectI> rects) {
    long long a = 0;
    for (const RectI& r : rects)
        if (!r.empty()) a += static_cast<long long>(r.width()) * r.height();
    return a;
}

void visibleIntervals(int l, int r, int y, std::span<const RectI> occluders,
                      std::vector<IntervalI>& out) {
    out.clear();
    if (r <= l) return;
    out.push_back(IntervalI{l, r});
    for (const RectI& o : occluders) {
        if (o.empty() || y < o.t || y >= o.b) continue;
        const std::size_t n = out.size();
        for (std::size_t i = 0; i < n; ++i) {
            IntervalI v = out[i];
            if (o.r <= v.a || o.l >= v.b) continue;
            if (o.l > v.a && o.r < v.b) {   // split
                out[i] = IntervalI{v.a, o.l};
                out.push_back(IntervalI{o.r, v.b});
            } else if (o.l > v.a) {
                out[i] = IntervalI{v.a, o.l};
            } else if (o.r < v.b) {
                out[i] = IntervalI{o.r, v.b};
            } else {
                out[i] = IntervalI{0, 0};
            }
        }
        out.erase(std::remove_if(out.begin(), out.end(), [](const IntervalI& v) { return v.b <= v.a; }),
                  out.end());
    }
    std::sort(out.begin(), out.end(), [](const IntervalI& p, const IntervalI& q) { return p.a < q.a; });
}

}  // namespace snow
