#include "surfaces/surface_registry.h"

#include <algorithm>
#include <cmath>

#include "surfaces/geometry.h"

namespace snow {

namespace {
constexpr int kTopTolerancePx = 2;   // "touching the monitor top" tolerance
}

int SurfaceRegistry::stepFor(float dpiScale) {
    return std::max(2, static_cast<int>(std::lround(kColumnStepAt96 * dpiScale)));
}

Surface* SurfaceRegistry::find(SurfaceId id) {
    for (Surface& s : surfaces_)
        if (s.id == id) return &s;
    return nullptr;
}
const Surface* SurfaceRegistry::find(SurfaceId id) const {
    for (const Surface& s : surfaces_)
        if (s.id == id) return &s;
    return nullptr;
}
const Surface* SurfaceRegistry::findByWindow(WindowId id) const {
    for (const Surface& s : surfaces_)
        if (s.kind != SurfaceKind::Screen && s.window == id) return &s;
    return nullptr;
}
int SurfaceRegistry::activeCount() const {
    int n = 0;
    for (const Surface& s : surfaces_) n += s.active ? 1 : 0;
    return n;
}

double SurfaceRegistry::normalizedMonitorArea() const {
    double a = 0;
    for (const MonitorInfo& m : monitors_) {
        const double sc = m.dpiScale > 0.1f ? m.dpiScale : 1.0;
        a += static_cast<double>(m.bounds.width()) * m.bounds.height() / (sc * sc);
    }
    return a;
}

bool SurfaceRegistry::pointOnMonitor(int x, int y) const {
    for (const RectI& r : monitorRects_)
        if (r.contains(x, y)) return true;
    return false;
}

float SurfaceRegistry::dpiScaleAt(float x, float y) const {
    float best = 1.0f;
    float bestD = -1;
    for (const MonitorInfo& m : monitors_) {
        const float cx = std::clamp(x, static_cast<float>(m.bounds.l), static_cast<float>(m.bounds.r));
        const float cy = std::clamp(y, static_cast<float>(m.bounds.t), static_cast<float>(m.bounds.b));
        const float d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = m.dpiScale;
        }
    }
    return best > 0.1f ? best : 1.0f;
}

int SurfaceRegistry::monitorForTopEdge(int l, int r, int t) const {
    int best = -1, bestOverlap = 0;
    for (std::size_t i = 0; i < monitors_.size(); ++i) {
        const RectI& b = monitors_[i].bounds;
        if (t <= b.t || t > b.b) continue;   // the strip above the edge must be on this monitor
        const int overlap = std::min(r, b.r) - std::max(l, b.l);
        if (overlap > bestOverlap) {
            bestOverlap = overlap;
            best = static_cast<int>(i);
        }
    }
    return best;
}

Surface& SurfaceRegistry::create(SurfaceKind kind, WindowId window) {
    surfaces_.emplace_back();
    Surface& s = surfaces_.back();
    s.id = nextId_++;
    s.kind = kind;
    s.window = window;
    s.capSeed = seedRng_.next();
    return s;
}

void SurfaceRegistry::place(Surface& s, bool fresh, int l, int r, int t, float dpi, float radius,
                            float maxHeight, bool eligible, float invElapsed, LayoutEvents& events) {
    if (dpi < 0.1f) dpi = 1.0f;
    const int step = stepFor(dpi);
    const int cols = std::max(0, r - l) / step + 1;
    if (fresh) {
        s.hm.reset(cols, static_cast<float>(step));
    } else {
        const int dx = l - s.left, dy = t - s.top;
        if (std::fabs(dpi - s.dpiScale) > 0.01f) {
            const float ratio = dpi / s.dpiScale;
            s.hm.resample(cols, static_cast<float>(step), ratio);
            events.rescaled.push_back({s.id, ratio});
        } else if (cols != s.hm.columns()) {
            s.hm.resizeColumns(cols);
            events.resized.push_back(s.id);
        } else if (r - l != s.right - s.left) {
            events.resized.push_back(s.id);
        }
        if ((dx != 0 || dy != 0) && s.active && eligible) {
            const float dist = std::sqrt(static_cast<float>(dx) * static_cast<float>(dx) +
                                         static_cast<float>(dy) * static_cast<float>(dy));
            events.moved.push_back({s.id, dx, dy, dist * invElapsed});
        }
    }
    s.left = l;
    s.right = r;
    s.top = t;
    s.dpiScale = dpi;
    s.cornerRadius = radius;
    s.hm.setLimits(maxHeight, radius, static_cast<float>(r - l), naturalCaps_, s.capSeed, dpi);
    s.active = eligible;
    s.seen = true;
}

void SurfaceRegistry::gatherOccluders(Surface& s, const DesktopLayout& layout, std::size_t frontCount) const {
    s.occluders.clear();
    const int pad = static_cast<int>(100.0f * s.dpiScale);
    const int tall = static_cast<int>(std::max(s.hm.maxHeight(), s.hm.peak()) + kObjectHeadroomAt96 * s.dpiScale);
    const RectI strip{s.left - pad, s.top - tall, s.right + pad, s.top + 2};
    for (std::size_t i = 0; i < frontCount && i < layout.windows.size(); ++i) {
        const RectI& b = layout.windows[i].bounds;
        if (!b.empty() && b.intersects(strip)) s.occluders.push_back(b);
    }
}

void SurfaceRegistry::update(const DesktopLayout& layout, const AccumulationSettings& acc,
                             float elapsedSeconds, LayoutEvents& events) {
    events.clear();
    monitors_ = layout.monitors;
    monitorRects_.clear();
    scene_ = RectI{};
    for (const MonitorInfo& m : monitors_) {
        if (m.bounds.empty()) continue;
        monitorRects_.push_back(m.bounds);
        if (scene_.empty()) {
            scene_ = m.bounds;
        } else {
            scene_.l = std::min(scene_.l, m.bounds.l);
            scene_.t = std::min(scene_.t, m.bounds.t);
            scene_.r = std::max(scene_.r, m.bounds.r);
            scene_.b = std::max(scene_.b, m.bounds.b);
        }
    }
    naturalCaps_ = acc.naturalCaps;
    primary_ = RectI{};
    primaryDpi_ = 1;
    for (const MonitorInfo& m : monitors_) {
        if (m.bounds.empty()) continue;
        if (primary_.empty() || m.primary) {
            primary_ = m.bounds;
            primaryDpi_ = m.dpiScale > 0.1f ? m.dpiScale : 1.0f;
            if (m.primary) break;
        }
    }
    windowRects_.clear();
    for (const WindowInfo& w : layout.windows)
        if (!w.bounds.empty()) windowRects_.push_back(w.bounds);
    windows_ = layout.windows;
    excludedRects_.clear();
    for (const WindowInfo& w : layout.windows)
        if (w.kind == WindowKind::App && !w.snowAllowed && !w.bounds.empty() && w.bounds.intersects(scene_))
            excludedRects_.push_back(w.bounds);
    rebuildDesktopClip();

    for (Surface& s : surfaces_) s.seen = false;
    const float invElapsed = 1.0f / std::max(elapsedSeconds, 1.0f / 240.0f);
    const float maxH96 = static_cast<float>(std::clamp(acc.maxHeightPxAt96Dpi, 0, 1000));

    // Window / taskbar surfaces. layout.windows is front-to-back, so everything with a
    // lower index than a window is in front of it and occludes its drift.
    for (std::size_t wi = 0; wi < layout.windows.size(); ++wi) {
        const WindowInfo& w = layout.windows[wi];
        SurfaceKind kind;
        if (w.kind == WindowKind::App && w.snowAllowed) kind = SurfaceKind::Window;
        else if (w.kind == WindowKind::Taskbar) kind = SurfaceKind::Taskbar;
        else continue;
        const float dpi = w.dpiScale > 0.1f ? w.dpiScale : 1.0f;
        const RectI& b = w.bounds;
        const int mon = b.empty() ? -1 : monitorForTopEdge(b.l, b.r, b.t);
        bool eligible = mon >= 0;
        float maxH = maxH96 * dpi;
        if (eligible) {
            const int room = b.t - monitors_[static_cast<std::size_t>(mon)].bounds.t;
            if (kind == SurfaceKind::Window) {
                eligible = acc.onWindows && static_cast<float>(b.width()) >= 24.0f * dpi;
                if (acc.skipMaximized && (w.maximized || room <= kTopTolerancePx)) eligible = false;
            } else {
                eligible = acc.onTaskbar && b.width() > b.height() && room > kTopTolerancePx;
            }
            maxH = std::min(maxH, static_cast<float>(std::max(room, 0)));
        }
        Surface* s = nullptr;
        for (Surface& e : surfaces_)
            if (e.kind != SurfaceKind::Screen && e.window == w.id) s = &e;
        bool fresh = false;
        if (!s) {
            if (!eligible) continue;
            s = &create(kind, w.id);
            fresh = true;
        }
        s->kind = kind;
        if (b.empty()) {   // bogus bounds: keep the snow, show nothing
            s->active = false;
            s->seen = true;
            continue;
        }
        place(*s, fresh, b.l, b.r, b.t, dpi, kind == SurfaceKind::Window ? w.cornerRadiusPx : 0.0f, maxH,
              eligible, invElapsed, events);
        gatherOccluders(*s, layout, wi);
        s->bottom = b.b;
        s->floorY = b.b;
        s->iceClip.clear();
        s->deskClip.clear();
        if (kind == SurfaceKind::Window) {
            const RectI strip{b.l, b.b, b.r, b.b + static_cast<int>(56.0f * dpi)};
            vholes_.clear();
            for (std::size_t k = 0; k < wi; ++k)
                if (layout.windows[k].bounds.intersects(strip)) vholes_.push_back(layout.windows[k].bounds);
            clipRegion(strip, monitorRects_, vholes_, s->iceClip, scratch_, scratch2_);
        } else if (mon >= 0) {
            const RectI monRect = monitors_[static_cast<std::size_t>(mon)].bounds;
            clipRegion(scene_, std::span<const RectI>(&monRect, 1), windowRects_, s->deskClip, scratch_, scratch2_);
        }
    }
    // Hidden (minimized / cloaked) windows keep their state.
    for (WindowId id : layout.hiddenWindows) {
        for (Surface& s : surfaces_) {
            if (s.kind != SurfaceKind::Screen && s.window == id && !s.seen) {
                s.seen = true;
                // The taskbar is reported hidden while the Start menu / Search is open even
                // though it stays on screen: keep its drift where it was, visible.
                if (s.kind != SurfaceKind::Taskbar) s.active = false;
            }
        }
    }
    // Screen-bottom surfaces: one per monitor, behind every window.
    for (const MonitorInfo& m : monitors_) {
        if (m.bounds.empty()) continue;
        const float dpi = m.dpiScale > 0.1f ? m.dpiScale : 1.0f;
        bool eligible = acc.onScreenBottom;
        const RectI bottomRow{m.bounds.l, m.bounds.b - 1, m.bounds.r, m.bounds.b};
        for (const WindowInfo& w : layout.windows)
            if (w.kind == WindowKind::Taskbar && w.bounds.intersects(bottomRow)) eligible = false;
        // A taskbar reported hidden (Start menu open) is still there.
        for (const Surface& e : surfaces_)
            if (e.kind == SurfaceKind::Taskbar && e.seen && e.active && e.left < m.bounds.r && e.right > m.bounds.l &&
                e.top > m.bounds.t && e.top <= m.bounds.b)
                eligible = false;
        Surface* s = nullptr;
        for (Surface& e : surfaces_)
            if (e.kind == SurfaceKind::Screen && e.originX == m.bounds.l && e.originY == m.bounds.t) s = &e;
        bool fresh = false;
        if (!s) {
            if (!eligible) continue;
            s = &create(SurfaceKind::Screen, 0);
            s->originX = m.bounds.l;
            s->originY = m.bounds.t;
            fresh = true;
        }
        const float maxH = std::min(maxH96 * dpi, static_cast<float>(m.bounds.height()));
        place(*s, fresh, m.bounds.l, m.bounds.r, m.bounds.b, dpi, 0.0f, maxH, eligible, invElapsed, events);
        gatherOccluders(*s, layout, layout.windows.size());
        s->floorY = m.bounds.b;
    }
    // Anything not seen belongs to a closed window / removed monitor.
    for (std::size_t i = 0; i < surfaces_.size();) {
        if (!surfaces_[i].seen) {
            events.removed.push_back(std::move(surfaces_[i]));
            surfaces_.erase(surfaces_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (Surface& s : surfaces_) {
        if (!s.active) {
            s.clip.clear();
            continue;
        }
        computeClip(s);
        refreshBlocked(s);
    }
    rebuildColliders();
}

void SurfaceRegistry::computeClip(Surface& s) {
    const int h = static_cast<int>(std::ceil(std::max(s.hm.maxHeight(), s.hm.peak()))) + 2;
    const RectI strip{s.left, s.top - h, s.right, s.top};
    clipRegion(strip, monitorRects_, s.occluders, s.clip, scratch_, scratch2_);
}

void SurfaceRegistry::refreshBlocked(Surface& s) const {
    const int n = s.hm.columns();
    const float step = s.hm.step();
    for (int i = 0; i < n; ++i) {
        const int x = s.left + static_cast<int>(static_cast<float>(i) * step);
        const int y = s.top - static_cast<int>(std::ceil(s.hm.height(i))) - 1;
        const bool open = x < s.right && pointOnMonitor(x, y) && !pointInAny(s.occluders, x, y);
        s.hm.setBlocked(i, !open);
    }
}

void SurfaceRegistry::refreshBlockedAll() {
    for (Surface& s : surfaces_)
        if (s.active && !s.occluders.empty()) refreshBlocked(s);
}

void SurfaceRegistry::rebuildColliders() {
    colliders_.clear();
    groundColliders_ = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < surfaces_.size(); ++i) {
            const Surface& s = surfaces_[i];
            if (!s.active) continue;
            const bool isWindow = s.kind == SurfaceKind::Window;
            if (isWindow != (pass == 1)) continue;
            Collider c;
            c.x0 = static_cast<float>(s.left);
            c.x1 = static_cast<float>(s.right);
            c.baseY = static_cast<float>(s.top);
            c.yTop = c.baseY - std::max(s.hm.maxHeight(), s.hm.peak()) - 1.0f;
            c.index = static_cast<int>(i);
            c.floorY = s.floorY;
            colliders_.push_back(c);
        }
        if (pass == 0) groundColliders_ = colliders_.size();
    }
}

bool SurfaceRegistry::sweep(float x0, float y0, float x1, float y1, bool includeWindows, SurfaceHit& hit,
                            SurfaceId exclude, bool passUpperGround) const {
    const std::size_t n = includeWindows ? colliders_.size() : groundColliders_;
    bool found = false;
    float bestY = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const Collider& c = colliders_[k];
        if (y1 < c.yTop || y0 > c.baseY) continue;
        float xh = x1;
        if (y1 > c.baseY) {   // passed the edge within this step: use the crossing point
            const float t = (c.baseY - y0) / (y1 - y0);
            xh = x0 + t * (x1 - x0);
        }
        if (xh < c.x0 || xh >= c.x1) continue;
        if (passUpperGround && k < groundColliders_ && pointOnMonitor(static_cast<int>(xh), c.floorY + 1)) continue;
        const Surface& s = surfaces_[static_cast<std::size_t>(c.index)];
        if (exclude != 0 && s.id == exclude) continue;
        const int col = s.hm.columnAt(xh - c.x0);
        const float topY = c.baseY - s.hm.height(col);
        if (y1 < topY) continue;
        if (s.hm.blocked(col)) continue;   // occluded: the flake keeps falling in front
        if (!found || topY < bestY) {
            found = true;
            bestY = topY;
            hit.surfaceIndex = c.index;
            hit.column = col;
            hit.x = xh;
            hit.y = topY;
        }
    }
    return found;
}

float SurfaceRegistry::deposit(const SurfaceHit& hit, float area) {
    Surface& s = surfaces_[static_cast<std::size_t>(hit.surfaceIndex)];
    return s.hm.deposit(hit.x - static_cast<float>(s.left), area);
}

void SurfaceRegistry::clearAllSnow() {
    for (Surface& s : surfaces_) s.hm.clear();
}

namespace {
bool feedAxis(ShakeTracker::Axis& a, int d, double now, float minLeg) {
    if (d == 0) return false;
    const int s = d > 0 ? 1 : -1;
    const float ad = static_cast<float>(d > 0 ? d : -d);
    if (s != a.dir) {
        a.pending = a.dir != 0 && a.leg >= minLeg;
        a.pendingTime = now;
        a.dir = s;
        a.leg = ad;
    } else {
        a.leg += ad;
    }
    if (a.pending && a.leg >= minLeg) {
        a.pending = false;
        a.times[a.count % 4] = a.pendingTime;
        ++a.count;
    }
    int recent = 0;
    for (int k = 0; k < std::min(a.count, 4); ++k)
        if (now - a.times[k] <= ShakeTracker::kWindowSeconds) ++recent;
    return recent >= ShakeTracker::kReversals;
}
}  // namespace

bool ShakeTracker::feed(int dx, int dy, double now, float minLeg) {
    const bool hx = feedAxis(ax[0], dx, now, minLeg);
    const bool hy = feedAxis(ax[1], dy, now, minLeg);
    if (hx || hy) {
        reset();
        return true;
    }
    return false;
}

void SurfaceRegistry::visibleRegion(int index, std::vector<RectI>& out) const {
    out.clear();
    if (index < 0 || index >= static_cast<int>(windows_.size())) return;
    const RectI r = windows_[static_cast<std::size_t>(index)].bounds;
    vholes_.clear();
    for (int k = 0; k < index; ++k)
        if (windows_[static_cast<std::size_t>(k)].bounds.intersects(r)) vholes_.push_back(windows_[static_cast<std::size_t>(k)].bounds);
    clipRegion(r, monitorRects_, vholes_, out, vscratch_, vscratch2_);
}

void SurfaceRegistry::rebuildDesktopClip() {
    objectRects_.clear();
    if (objectsAll_) objectRects_ = monitorRects_;
    else if (!primary_.empty()) objectRects_.push_back(primary_);
    clipRegion(scene_, objectRects_, windowRects_, desktopClip_, scratch_, scratch2_);
}

double SurfaceRegistry::totalMass() const {
    double m = 0;
    for (const Surface& s : surfaces_) m += s.hm.mass();
    return m;
}

}  // namespace snow
