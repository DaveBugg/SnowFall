#include "render/cursor_layout.h"

#include <cmath>

#include "interaction/tools.h"

namespace snow::mac {
namespace {

// Hotspot offset of a tool sprite in its local (unrotated) frame, relative to its centre, for
// the core's Windows layout (core/interaction/tools.cpp: toolHotspotLocal).
bool coreHotspotLocal(const SpriteInstance& s, float& hx, float& hy) {
    hx = 0;
    if (s.shape == Shape::Mitten) {
        hy = -kCursorHotspotShare * s.halfH;
        return true;
    }
    if (s.shape == Shape::Shovel) {
        hy = kCursorHotspotShare * s.halfH;
        return true;
    }
    if (s.shape == Shape::Snowball) {
        hy = 0;
        return true;
    }
    return false;
}

void rotate(float x, float y, float rot, float& ox, float& oy) {
    const float c = std::cos(rot), s = std::sin(rot);
    ox = x * c - y * s;
    oy = x * s + y * c;
}

}  // namespace

bool cursorHotspot(const std::vector<SpriteInstance>& batch, float& px, float& py) {
    for (const SpriteInstance& s : batch) {
        if (s.shape == Shape::Ring) {   // hover ring / held-ball ring: centred on the pointer
            px = s.x;
            py = s.y;
            return true;
        }
    }
    for (const SpriteInstance& s : batch) {
        float hx, hy;
        if (!coreHotspotLocal(s, hx, hy)) continue;
        float rx, ry;
        rotate(hx, hy, s.rotation, rx, ry);
        px = s.x + rx;
        py = s.y + ry;
        return true;
    }
    return false;
}

std::vector<SpriteInstance> layoutCursorHold(const std::vector<SpriteInstance>& batch, float dpi) {
    float px = 0, py = 0;
    if (!cursorHotspot(batch, px, py)) return batch;
    bool hasRing = false, hasBall = false;
    for (const SpriteInstance& s : batch) {
        hasRing = hasRing || s.shape == Shape::Ring;
        hasBall = hasBall || s.shape == Shape::Snowball;
    }
    if (hasRing && hasBall) return batch;   // held-ball marker: already reads as "ball on the pointer"

    std::vector<SpriteInstance> out;
    out.reserve(batch.size() + 1);
    for (const SpriteInstance& s : batch)
        if (s.shape == Shape::Ring) out.push_back(s);   // rings first (drawn below the tool)
    for (const SpriteInstance& s : batch) {
        if (s.shape == Shape::Ring) continue;
        SpriteInstance t = s;
        if (s.shape == Shape::Mitten) {
            // Tip inside the fingertip area instead of on its edge.
            float rx, ry;
            rotate(0.0f, -kHoldMittenHotspotShare * s.halfH, s.rotation, rx, ry);
            t.x = px - rx;
            t.y = py - ry;
        } else if (s.shape == Shape::Snowball) {
            SpriteInstance ring;
            ring.x = px;
            ring.y = py;
            ring.halfW = ring.halfH = s.halfW + kHoldBallRingGapAt96 * dpi;
            ring.opacity = kHoldRingOpacity;
            ring.shape = Shape::Ring;
            out.insert(out.begin(), ring);
            t.x = px;
            t.y = py;
        }
        // Shovel (blade edge on the tip, handle up-right) and the scooped heap: as laid out.
        out.push_back(t);
    }
    return out;
}

}  // namespace snow::mac
