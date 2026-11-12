#include "rendering/d3d11/drift_mesh.h"

#include <algorithm>
#include <cmath>

namespace snow {
namespace {

// Appends triangle strip `src` to `dst`, joined with degenerate triangles.
void appendStrip(std::vector<DriftVertex>& dst, const std::vector<DriftVertex>& src) {
    if (src.empty()) return;
    if (!dst.empty()) {
        dst.push_back(dst.back());
        dst.push_back(src.front());
    }
    dst.insert(dst.end(), src.begin(), src.end());
}

}  // namespace

void DriftMeshCache::clear() {
    entries_.clear();
    order_.clear();
    all_.clear();
}

bool DriftMeshCache::build(const std::vector<DriftStrip>& strips) {
    ++pass_;
    bool changed = false;
    scratchOrder_.clear();
    for (const DriftStrip& s : strips) {
        if (s.heights.size() < 2 || s.clip.empty() || !(s.opacity > 0.0f)) continue;
        bool any = false;
        for (float h : s.heights) {
            if (h > 0.0f) {
                any = true;
                break;
            }
        }
        if (!any) continue;

        Entry& e = entries_[s.id];
        const bool fresh = e.usedAt == 0;
        const bool duplicate = e.usedAt == pass_;  // same id twice in one snapshot
        if (fresh || duplicate || e.version != s.version || e.x0 != s.x0 || e.baseY != s.baseY ||
            e.step != s.step || e.dpi != s.dpiScale || e.opacity != s.opacity || e.wetness != s.wetness || e.count != s.heights.size() || e.clip != s.clip) {
            e.version = s.version;
            e.x0 = s.x0;
            e.baseY = s.baseY;
            e.step = s.step;
            e.dpi = s.dpiScale;
            e.opacity = s.opacity;
            e.wetness = s.wetness;
            e.count = s.heights.size();
            e.clip = s.clip;
            tessellate(s, e.verts);
            ++rebuilt_;
            changed = true;
        }
        e.usedAt = pass_;
        if (!duplicate) scratchOrder_.push_back(s.id);
    }
    if (scratchOrder_ != order_) changed = true;
    if (!changed) return false;

    order_ = scratchOrder_;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.usedAt != pass_) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    all_.clear();
    for (SurfaceId id : order_) appendStrip(all_, entries_[id].verts);
    return true;
}

void DriftMeshCache::tessellate(const DriftStrip& s, std::vector<DriftVertex>& out) {
    out.clear();
    const int n = static_cast<int>(s.heights.size());
    const float step = std::max(s.step, 0.25f);
    const float dpi = std::max(s.dpiScale, 0.5f);
    // Catmull-Rom subdivision down to <= 2 px between samples; the pixel shader interpolates
    // linearly between samples, which is visually smooth at that spacing.
    const int sub = std::clamp(static_cast<int>(std::ceil(step / 2.0f)), 1, 4);
    const int fn = (n - 1) * sub + 1;
    fine_.resize(static_cast<std::size_t>(fn));
    float maxH = 0;
    for (int i = 0; i + 1 < n; ++i) {
        const float p0 = s.heights[static_cast<std::size_t>(std::max(i - 1, 0))];
        const float p1 = s.heights[static_cast<std::size_t>(i)];
        const float p2 = s.heights[static_cast<std::size_t>(i + 1)];
        const float p3 = s.heights[static_cast<std::size_t>(std::min(i + 2, n - 1))];
        for (int k = 0; k < sub; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(sub);
            const float t2 = t * t, t3 = t2 * t;
            float v = 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                              (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
            if (!(v > 0.0f)) v = 0.0f;  // also clears NaN
            fine_[static_cast<std::size_t>(i * sub + k)] = v;
            maxH = std::max(maxH, v);
        }
    }
    {
        float v = s.heights[static_cast<std::size_t>(n - 1)];
        if (!(v > 0.0f)) v = 0.0f;
        fine_[static_cast<std::size_t>(fn - 1)] = v;
        maxH = std::max(maxH, v);
    }

    const float fstep = step / static_cast<float>(sub);
    const float x0 = s.x0;
    const float xEnd = x0 + static_cast<float>(fn - 1) * fstep;
    const float padTop = 1.5f * dpi + 1.0f;      // feather + 1 px
    const float bottomExt = 1.5f * dpi + 1.0f;   // the snow hugs the edge slightly below baseY
    const float hullTop = s.baseY - maxH - padTop;
    const float hullBottom = s.baseY + bottomExt;

    auto heightAt = [&](float x) {
        float f = (x - x0) / fstep;
        f = std::clamp(f, 0.0f, static_cast<float>(fn - 1));
        const int j = std::min(static_cast<int>(f), fn - 2);
        const float t = f - static_cast<float>(j);
        return fine_[static_cast<std::size_t>(j)] * (1.0f - t) + fine_[static_cast<std::size_t>(j + 1)] * t;
    };

    for (const RectI& c : s.clip) {
        if (c.empty()) continue;
        const float xa = std::max(static_cast<float>(c.l), x0);
        const float xb = std::min(static_cast<float>(c.r), xEnd);
        if (!(xb > xa)) continue;
        const float ct = static_cast<float>(c.t), cb = static_cast<float>(c.b);
        if (cb <= hullTop || ct >= hullBottom) continue;

        // Emits the (top, bottom) vertex pair of one column; `join` starts a new strip piece.
        auto column = [&](float x, float h, bool join) {
            DriftVertex top{};
            top.x = x;
            top.topY = s.baseY - h;
            top.h = h;
            top.baseY = s.baseY;
            top.lx = x - x0;
            top.dpi = dpi;
            top.opacity = std::clamp(s.opacity, 0.0f, 1.0f);
            top.wetness = std::clamp(s.wetness, 0.0f, 1.0f);
            top.y = std::clamp(s.baseY - h - padTop, ct, cb);
            DriftVertex bottom = top;
            bottom.y = std::clamp(hullBottom, ct, cb);
            if (join && !out.empty()) {
                out.push_back(out.back());
                out.push_back(top);
            }
            out.push_back(top);
            out.push_back(bottom);
        };

        float px = xa, ph = heightAt(xa);
        bool open = false;
        auto segment = [&](float x, float h) {
            if (std::max(ph, h) > 0.02f) {   // skip bare stretches entirely
                if (!open) {
                    column(px, ph, true);
                    open = true;
                }
                column(x, h, false);
            } else {
                open = false;
            }
            px = x;
            ph = h;
        };
        const int j0 = static_cast<int>(std::floor((xa - x0) / fstep)) + 1;
        const int j1 = static_cast<int>(std::ceil((xb - x0) / fstep)) - 1;
        for (int j = j0; j <= j1; ++j) {
            const float x = x0 + static_cast<float>(j) * fstep;
            if (x <= xa || x >= xb) continue;
            segment(x, fine_[static_cast<std::size_t>(std::clamp(j, 0, fn - 1))]);
        }
        segment(xb, heightAt(xb));
    }
}

}  // namespace snow
