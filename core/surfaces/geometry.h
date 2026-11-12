// Integer rectangle helpers used for occlusion / clip computation. Pure, allocation-free
// apart from the caller-provided output vectors.
#pragma once
#include <span>
#include <vector>

#include "core/types.h"

namespace snow {

// Appends (a minus hole) to `out` as up to four non-overlapping rects.
void subtractRect(const RectI& a, const RectI& hole, std::vector<RectI>& out);

// out = base minus every rect in `holes`; the result rects never overlap each other.
// `scratch` is working storage (lets callers avoid per-call allocations).
void rectMinusRects(const RectI& base, std::span<const RectI> holes, std::vector<RectI>& out,
                    std::vector<RectI>& scratch);

// out = (base ∩ each of `within`) minus `holes`. `within` rects are assumed not to overlap
// each other (monitor rects), so the result does not overlap either.
void clipRegion(const RectI& base, std::span<const RectI> within, std::span<const RectI> holes,
                std::vector<RectI>& out, std::vector<RectI>& scratch, std::vector<RectI>& scratch2);

bool pointInAny(std::span<const RectI> rects, int x, int y);
long long totalArea(std::span<const RectI> rects);

struct IntervalI {
    int a = 0, b = 0;   // half-open [a, b)
    bool operator==(const IntervalI&) const = default;
};
// Parts of the horizontal segment [l, r) at row y that are not covered by any occluder,
// sorted left to right.
void visibleIntervals(int l, int r, int y, std::span<const RectI> occluders,
                      std::vector<IntervalI>& out);

}  // namespace snow
