// SurfaceRegistry: the set of snow-bearing edges (window tops, taskbar top, screen bottoms)
// derived from a DesktopLayout. Surfaces are keyed by WindowId (never by position); their
// heightmaps live in local coordinates so moving a window carries its drift unchanged.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/layout.h"
#include "core/settings.h"
#include "core/types.h"
#include "simulation/rng.h"
#include "surfaces/heightmap.h"

namespace snow {

enum class SurfaceKind : std::uint8_t { Window, Taskbar, Screen };

// Detects a window being shaken: >= kReversals direction reversals on one axis within
// kWindowSeconds, every leg (before and after a reversal) at least `minLeg` px long.
struct ShakeTracker {
    inline static constexpr int kReversals = 3;
    inline static constexpr double kWindowSeconds = 1.0;
    struct Axis {
        int dir = 0;
        float leg = 0;
        bool pending = false;     // a reversal whose previous leg qualified, waiting for this leg
        double pendingTime = 0;
        double times[4]{};        // recent confirmed reversals (ring)
        int count = 0;
    };
    Axis ax[2];
    // Feed one displacement; returns true when a shake is detected (and resets).
    bool feed(int dx, int dy, double now, float minLeg);
    void reset() { ax[0] = ax[1] = Axis{}; }
};

struct Surface {
    SurfaceId id = 0;
    SurfaceKind kind = SurfaceKind::Window;
    WindowId window = 0;            // 0 for Screen surfaces
    int originX = 0, originY = 0;   // Screen surfaces: origin of their monitor (identity key)
    int left = 0, right = 0;        // x extent in virtual-desktop px
    int top = 0;                    // y of the edge the snow lies on
    float dpiScale = 1;
    float cornerRadius = 0;
    bool active = false;            // shown in the current layout and eligible to hold snow
    bool seen = false;              // scratch flag of update()
    std::uint64_t capSeed = 0;      // random per-surface seed (natural cap profile)
    int bottom = 0;                 // window surfaces: y of the window's bottom edge
    int floorY = 0;                 // taskbar / screen surfaces: bottom of the thing they lie on
    std::vector<RectI> iceClip;     // window surfaces: area below the window minus windows in front
    std::vector<RectI> deskClip;    // taskbar surfaces: its monitor minus all windows (garland)
    ShakeTracker shake;
    Heightmap hm;
    std::vector<RectI> clip;        // visible part of the drift strip
    std::vector<RectI> occluders;   // rects of windows in front of this surface (near the edge)

    float width() const { return static_cast<float>(right - left); }
    float worldX(int column) const { return static_cast<float>(left) + static_cast<float>(column) * hm.step(); }
    float topAt(int column) const { return static_cast<float>(top) - hm.height(column); }
};

struct LayoutEvents {
    struct Moved {
        SurfaceId id = 0;
        int dx = 0, dy = 0;
        float speed = 0;   // px/s
    };
    struct Rescaled {
        SurfaceId id = 0;
        float ratio = 1;   // new dpiScale / old dpiScale
    };
    std::vector<Surface> removed;      // their window was closed (or stopped hosting snow)
    std::vector<Moved> moved;          // active surfaces that changed position
    std::vector<SurfaceId> resized;    // width changed (columns truncated / extended)
    std::vector<Rescaled> rescaled;    // DPI changed (heightmap resampled)
    void clear() {
        removed.clear();
        moved.clear();
        resized.clear();
        rescaled.clear();
    }
};

struct SurfaceHit {
    int surfaceIndex = -1;   // index into SurfaceRegistry::surfaces()
    int column = 0;
    float x = 0, y = 0;      // contact point on the snow top line
};

class SurfaceRegistry {
public:
    // Extra room above an edge (px at 96 DPI) inside which occluders are remembered, so that
    // snowballs / snowmen standing on the edge can be clipped too.
    inline static constexpr float kObjectHeadroomAt96 = 320.0f;
    inline static constexpr float kColumnStepAt96 = 3.0f;

    // Rebuild from a layout. `elapsedSeconds` = simulated time since the previous update
    // (used to turn displacements into speeds).
    void update(const DesktopLayout& layout, const AccumulationSettings& acc, float elapsedSeconds,
                LayoutEvents& events);

    std::vector<Surface>& surfaces() { return surfaces_; }
    const std::vector<Surface>& surfaces() const { return surfaces_; }
    Surface* find(SurfaceId id);
    const Surface* find(SurfaceId id) const;
    const Surface* findByWindow(WindowId id) const;
    int activeCount() const;

    void setSeed(std::uint64_t seed) { seedRng_.reseed(seed ^ 0xC0FFEE5EEDull); }

    // All listed windows of the current layout, front to back.
    const std::vector<WindowInfo>& windows() const { return windows_; }
    int windowIndex(WindowId id) const {
        for (std::size_t i = 0; i < windows_.size(); ++i)
            if (windows_[i].id == id) return static_cast<int>(i);
        return -1;
    }
    // Front-most window containing (x, y), or -1.
    int frontWindowAt(float x, float y) const {
        const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
        for (std::size_t i = 0; i < windows_.size(); ++i)
            if (windows_[i].bounds.contains(ix, iy)) return static_cast<int>(i);
        return -1;
    }
    // Visible application windows whose process is excluded from snow (snowAllowed == false).
    const std::vector<RectI>& excludedRects() const { return excludedRects_; }
    // Rects of all listed windows (the desktop layer is clipped by them).
    const std::vector<RectI>& windowRects() const { return windowRects_; }
    // Visible part of window `index` (its rect minus windows in front, on the monitors).
    void visibleRegion(int index, std::vector<RectI>& out) const;

    const std::vector<MonitorInfo>& monitors() const { return monitors_; }
    // Primary monitor (MonitorInfo.primary, else the first one); empty rect if none.
    RectI primaryRect() const { return primary_; }
    float primaryDpi() const { return primaryDpi_; }
    // Desktop layer (snowballs, snowmen, ...): the primary monitor, or every monitor with
    // interaction.objectsOnAllMonitors. desktopClip = those monitors minus ALL listed windows.
    void setObjectsOnAllMonitors(bool on) {
        if (on != objectsAll_) {
            objectsAll_ = on;
            rebuildDesktopClip();
        }
    }
    bool objectsOnAllMonitors() const { return objectsAll_; }
    const std::vector<RectI>& objectRects() const { return objectRects_; }
    const std::vector<RectI>& desktopClip() const { return desktopClip_; }
    // (x, y) inside an object monitor; `above` px above its top edge still count.
    bool inObjectArea(float x, float y, float above = 0) const {
        for (const RectI& r : objectRects_)
            if (x >= static_cast<float>(r.l) && x < static_cast<float>(r.r) && y >= static_cast<float>(r.t) - above &&
                y <= static_cast<float>(r.b))
                return true;
        return false;
    }
    // Object monitor containing (x, y), or nullptr.
    const RectI* objectRectAt(float x, float y) const {
        for (const RectI& r : objectRects_)
            if (x >= static_cast<float>(r.l) && x < static_cast<float>(r.r) && y >= static_cast<float>(r.t) &&
                y <= static_cast<float>(r.b))
                return &r;
        return nullptr;
    }
    // The surface's edge lies on an object monitor.
    bool edgeInObjectArea(const Surface& s) const {
        for (const RectI& r : objectRects_)
            if (s.top > r.t && s.top <= r.b && s.left < r.r && s.right > r.l) return true;
        return false;
    }
    const std::vector<RectI>& monitorRects() const { return monitorRects_; }
    RectI sceneBounds() const { return scene_; }          // bounding box of all monitors
    double normalizedMonitorArea() const;                 // sum of w*h / dpiScale^2
    bool pointOnMonitor(int x, int y) const;
    float dpiScaleAt(float x, float y) const;

    // Swept test of a point moving from (x0,y0) to (x1,y1) against the snow top lines of
    // active surfaces. Only downward crossings that start at or above the edge count.
    // Occluded (blocked) columns are transparent. Returns the highest hit.
    // passUpperGround: ignore taskbar / screen-bottom surfaces that have another monitor right
    // below them (single sky: foreground flakes keep falling into the lower monitor).
    bool sweep(float x0, float y0, float x1, float y1, bool includeWindows, SurfaceHit& hit,
               SurfaceId exclude = 0, bool passUpperGround = false) const;
    // Deposit `area` px^2 at a hit; returns the area actually added.
    float deposit(const SurfaceHit& hit, float area);

    // Recompute which columns are open (point just above the snow top visible on a monitor
    // and not under an occluder).
    void refreshBlocked(Surface& s) const;
    void refreshBlockedAll();
    void computeClip(Surface& s);
    void clearAllSnow();
    // Refresh the broad-phase data after heights were edited directly (tests / tools).
    void rebuildColliders();
    double totalMass() const;

    static int stepFor(float dpiScale);

private:
    struct Collider {
        float x0, x1, yTop, baseY;
        int index;
        int floorY;
    };
    Surface& create(SurfaceKind kind, WindowId window);
    int monitorForTopEdge(int l, int r, int t) const;
    void place(Surface& s, bool fresh, int l, int r, int t, float dpi, float radius, float maxHeight,
               bool eligible, float invElapsed, LayoutEvents& events);
    void gatherOccluders(Surface& s, const DesktopLayout& layout, std::size_t frontCount) const;

    std::vector<Surface> surfaces_;
    std::vector<MonitorInfo> monitors_;
    std::vector<RectI> monitorRects_;
    RectI scene_{};
    RectI primary_{};
    float primaryDpi_ = 1;
    std::vector<RectI> windowRects_;
    std::vector<WindowInfo> windows_;
    std::vector<RectI> excludedRects_;
    mutable std::vector<RectI> vscratch_, vscratch2_, vholes_;
    std::vector<RectI> desktopClip_;
    std::vector<RectI> objectRects_;
    bool objectsAll_ = false;
    void rebuildDesktopClip();
    Rng seedRng_{0x5EEDull};
    bool naturalCaps_ = false;
    std::vector<Collider> colliders_;     // non-window surfaces first
    std::size_t groundColliders_ = 0;     // number of leading non-window colliders
    std::vector<RectI> scratch_, scratch2_;
    SurfaceId nextId_ = 1;
};

}  // namespace snow
