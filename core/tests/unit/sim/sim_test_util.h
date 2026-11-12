// Shared helpers for the simulation unit tests.
#pragma once
#include <algorithm>
#include <cmath>

#include "core/layout.h"
#include "core/settings.h"
#include "core/snapshot.h"
#include "simulation/world.h"
#include "simulation/world_internal.h"
#include "snow_test.h"

namespace simtest {

using namespace snow;

inline constexpr WindowId kTaskbarId = 1;
inline constexpr int kTaskbarTop = 1032;

inline MonitorInfo monitor(int id, RectI bounds, float dpi = 1.0f, bool primary = false) {
    MonitorInfo m;
    m.id = id;
    m.bounds = bounds;
    m.work = bounds;
    m.dpiScale = dpi;
    m.primary = primary;
    return m;
}

inline WindowInfo window(WindowId id, RectI bounds, WindowKind kind = WindowKind::App, float dpi = 1.0f) {
    WindowInfo w;
    w.id = id;
    w.bounds = bounds;
    w.kind = kind;
    w.dpiScale = dpi;
    return w;
}

// One 1920x1080 monitor with a bottom taskbar (window id 1).
inline DesktopLayout basicLayout() {
    DesktopLayout l;
    l.revision = 1;
    l.monitors.push_back(monitor(0, RectI{0, 0, 1920, 1080}, 1.0f, true));
    l.windows.push_back(window(kTaskbarId, RectI{0, kTaskbarTop, 1920, 1080}, WindowKind::Taskbar));
    return l;
}

inline Surface* surfaceOf(World& w, WindowId id) {
    auto in = WorldTestAccess::get(w);
    const Surface* s = in.surfaces.findByWindow(id);
    return s ? in.surfaces.find(s->id) : nullptr;
}

// Fill every column of a surface up to `h` (respecting its caps).
inline void fill(World& w, Surface& s, float h) {
    for (int i = 0; i < s.hm.columns(); ++i) s.hm.setHeight(i, std::min(h, s.hm.cap(i)));
    WorldTestAccess::get(w).surfaces.rebuildColliders();
}

inline const DriftStrip* driftOf(const FrameSnapshot& snap, SurfaceId id) {
    for (const DriftStrip& d : snap.drifts)
        if (d.id == id) return &d;
    return nullptr;
}

inline void input(World& w, InputEvent::Type type, float x = 0, float y = 0) {
    InputEvent e;
    e.type = type;
    e.x = x;
    e.y = y;
    w.handleInput(e);
}

inline void stepN(World& w, int n, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < n; ++i) w.step(dt);
}

}  // namespace simtest
