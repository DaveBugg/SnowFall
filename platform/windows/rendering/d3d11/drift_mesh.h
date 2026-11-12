// CPU-side tessellation of DriftStrip height maps into a covering triangle strip, with a
// per-surface cache so unchanged drifts are not rebuilt (spec 5.3: re-mesh dirty strips only).
// No Win32/D3D dependencies.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/snapshot.h"

namespace snow {

struct DriftVertex {
    float x, y;      // virtual-desktop px
    float topY;      // y of the snow surface at this x
    float h;         // snow height at this x
    float baseY;     // y of the supporting edge
    float lx;        // x relative to the strip origin (pattern anchor)
    float dpi;
    float opacity;   // strip opacity 0..1
    float wetness;   // 0..1 thaw look
};

class DriftMeshCache {
public:
    // Rebuilds what changed. Returns true when vertices() differs from the previous call.
    bool build(const std::vector<DriftStrip>& strips);
    const std::vector<DriftVertex>& vertices() const { return all_; }  // one triangle strip
    void clear();
    std::uint64_t rebuiltStrips() const { return rebuilt_; }

private:
    struct Entry {
        std::uint32_t version = 0;
        float x0 = 0, baseY = 0, step = 0, dpi = 0, opacity = 1, wetness = 0;
        std::size_t count = 0;
        std::vector<RectI> clip;
        std::vector<DriftVertex> verts;
        std::uint64_t usedAt = 0;
    };
    void tessellate(const DriftStrip& s, std::vector<DriftVertex>& out);

    std::unordered_map<SurfaceId, Entry> entries_;
    std::vector<SurfaceId> order_, scratchOrder_;
    std::vector<float> fine_;
    std::vector<DriftVertex> all_;
    std::uint64_t pass_ = 0;
    std::uint64_t rebuilt_ = 0;
};

}  // namespace snow
