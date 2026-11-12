#include "tracker/desktop_model.h"

#include <algorithm>
#include <cmath>

namespace snow::mac {
namespace {

constexpr int kMinVisiblePt = 4;           // thinner windows are helpers/markers: skip
constexpr int kMaximizedTolerancePt = 2;   // window top "touching" the menu bar
constexpr float kScreenCoverShare = 0.9f;  // non-app layers covering a screen are system veils

// System processes whose windows never hold snow and must not occlude it either (the Dock
// and the menu bar have their own handling; the rest are transparent full-screen helpers).
bool systemOwner(const std::string& o) {
    static const char* kNames[] = {"Window Server", "Dock",        "SystemUIServer",  "Control Center",
                                   "ControlCenter", "Notification Center", "NotificationCenter",
                                   "Spotlight",     "WindowManager", "Wallpaper",     "loginwindow",
                                   "screencaptureui", "TextInputMenuAgent", "TextInputSwitcher",
                                   "CursorUIViewService", "AutoFill", "Universal Control"};
    for (const char* n : kNames)
        if (o == n) return true;
    return false;
}

long long area(const RectI& r) {
    return r.empty() ? 0 : static_cast<long long>(r.width()) * static_cast<long long>(r.height());
}

bool sameWindow(const WindowInfo& a, const WindowInfo& b) {
    return a.id == b.id && a.bounds == b.bounds && a.kind == b.kind && a.dpiScale == b.dpiScale &&
           a.cornerRadiusPx == b.cornerRadiusPx && a.maximized == b.maximized && a.snowAllowed == b.snowAllowed;
}

}  // namespace

RectI roundRect(double x, double y, double w, double h) {
    const int l = static_cast<int>(std::lround(x));
    const int t = static_cast<int>(std::lround(y));
    const int r = static_cast<int>(std::lround(x + w));
    const int b = static_cast<int>(std::lround(y + h));
    return RectI{l, t, r, b};
}

RectI cocoaToGlobal(const CocoaRect& r, double primaryHeight) {
    return roundRect(r.x, primaryHeight - (r.y + r.h), r.w, r.h);
}

CocoaRect globalToCocoa(const RectI& r, double primaryHeight) {
    CocoaRect c;
    c.x = r.l;
    c.w = r.width();
    c.h = r.height();
    c.y = primaryHeight - static_cast<double>(r.b);
    return c;
}

DockEdge dockEdge(const ScreenInfo& s) {
    if (s.frame.empty() || s.visible.empty()) return DockEdge::None;
    // A few points of slack: the visible frame is never exactly flush with a hidden Dock.
    if (s.frame.b - s.visible.b > 4) return DockEdge::Bottom;
    if (s.visible.l - s.frame.l > 4) return DockEdge::Left;
    if (s.frame.r - s.visible.r > 4) return DockEdge::Right;
    return DockEdge::None;
}

RectI dockTaskbarRect(const ScreenInfo& s, int dockWidth, int dockCenterX) {
    if (dockEdge(s) != DockEdge::Bottom) return RectI{};
    const int sw = s.frame.width();
    int w = dockWidth > 0 ? std::min(dockWidth, sw) : sw;
    int cx = dockCenterX != 0 ? dockCenterX : s.frame.l + sw / 2;
    int l = cx - w / 2;
    l = std::clamp(l, s.frame.l, s.frame.r - w);
    RectI r{l, s.visible.b, l + w, s.frame.b - 1};
    if (r.empty()) return RectI{};
    return r;
}

bool coversScreen(const RectI& b, const std::vector<ScreenInfo>& screens) {
    for (const ScreenInfo& s : screens) {
        const RectI& f = s.frame;
        if (std::abs(b.l - f.l) <= 1 && std::abs(b.t - f.t) <= 1 && std::abs(b.r - f.r) <= 1 &&
            std::abs(b.b - f.b) <= 1)
            return true;
    }
    return false;
}

Verdict classify(const RawWindow& w, const ClassifyContext& ctx) {
    if (ctx.ignored.count(w.number)) return Verdict::Skip;
    if (!(w.alpha > 0.01f)) return Verdict::Skip;
    if (w.bounds.width() < kMinVisiblePt || w.bounds.height() < kMinVisiblePt) return Verdict::Skip;
    if (w.layer < 0) return Verdict::Skip;   // desktop, wallpaper, desktop widgets
    if (systemOwner(w.owner)) return Verdict::Skip;
    if (w.layer == 0) {
        // Normal application windows. Our own normal windows (the settings window) collect
        // snow like any app; our overlays have a non-zero layer and are ignored anyway.
        return w.onscreen ? Verdict::App : Verdict::Skip;
    }
    if (w.pid == ctx.ownPid) return Verdict::Skip;            // our popovers / status item
    if (w.layer >= ctx.overlayLevel) return Verdict::Skip;    // drawn above the snow anyway
    // Floating palettes, modal panels, torn-off menus: never hold snow, but hide it.
    for (const ScreenInfo& s : ctx.screens) {
        const RectI x = w.bounds.intersect(s.frame);
        if (area(x) >= static_cast<long long>(kScreenCoverShare * static_cast<float>(area(s.frame))))
            return Verdict::Skip;   // transparent full-screen veil of some helper
    }
    return Verdict::Occluder;
}

bool sameLayout(const DesktopLayout& a, const DesktopLayout& b) {
    if (a.fullscreenAppActive != b.fullscreenAppActive || a.monitors != b.monitors ||
        a.hiddenWindows != b.hiddenWindows || a.windows.size() != b.windows.size())
        return false;
    for (std::size_t i = 0; i < a.windows.size(); ++i)
        if (!sameWindow(a.windows[i], b.windows[i])) return false;
    return true;
}

WindowId LayoutBuilder::idFor(int pid, std::uint32_t number) {
    const std::uint64_t k = key(pid, number);
    auto it = ids_.find(k);
    if (it != ids_.end()) return it->second;
    const WindowId id = nextId_++;
    ids_.emplace(k, id);
    return id;
}

void LayoutBuilder::forgetAll() {
    ids_.clear();
    appWindows_.clear();
    hidden_.clear();
    hasLast_ = false;
    last_ = DesktopLayout{};
}

bool LayoutBuilder::isExcluded(int pid, const NameQuery& names) {
    if (excluded.empty() || !names) return false;
    const std::vector<std::string> own = names(pid);
    for (const std::string& e : excluded) {
        std::string bare = e;
        for (const char* suffix : {".app", ".exe"}) {
            const std::size_t n = std::char_traits<char>::length(suffix);
            if (bare.size() > n && bare.compare(bare.size() - n, n, suffix) == 0) bare.resize(bare.size() - n);
        }
        for (const std::string& o : own)
            if (o == e || o == bare) return true;
    }
    return false;
}

bool LayoutBuilder::build(const TrackerFrame& f, const ExistQuery& exists, const NameQuery& names,
                          DesktopLayout& out, bool verifyHidden) {
    DesktopLayout L;
    context.screens = f.screens;

    // Monitors (primary first is not required by the core; ids are list indices).
    for (std::size_t i = 0; i < f.screens.size(); ++i) {
        const ScreenInfo& s = f.screens[i];
        MonitorInfo m;
        m.id = static_cast<int>(i);
        m.bounds = s.frame;
        m.work = s.visible;
        m.dpiScale = 1.0f;   // simulation in points
        m.primary = s.primary;
        L.monitors.push_back(m);
    }

    std::set<std::uint64_t> seen;
    // The Dock is above every application window: first in the front-to-back list.
    for (const ScreenInfo& s : f.screens) {
        const RectI dock = dockTaskbarRect(s, f.dockWidth, f.dockCenterX);
        if (dock.empty()) continue;
        WindowInfo w;
        w.id = idFor(-1, s.displayId);
        seen.insert(key(-1, s.displayId));
        w.bounds = dock;
        w.kind = WindowKind::Taskbar;
        L.windows.push_back(w);
    }

    std::map<std::uint64_t, std::pair<int, std::uint32_t>> apps;
    for (const RawWindow& r : f.windows) {
        const Verdict v = classify(r, context);
        if (v == Verdict::Skip) continue;
        const std::uint64_t k = key(r.pid, r.number);
        if (seen.count(k)) continue;   // duplicate entry
        seen.insert(k);
        WindowInfo w;
        w.id = idFor(r.pid, r.number);
        w.bounds = r.bounds;
        if (v == Verdict::Occluder) {
            w.kind = WindowKind::Occluder;
        } else {
            w.kind = WindowKind::App;
            apps.emplace(k, std::make_pair(r.pid, r.number));
            const bool full = coversScreen(r.bounds, f.screens);
            bool touchesTop = false;
            for (const ScreenInfo& s : f.screens) {
                if (r.bounds.l < s.frame.r && r.bounds.r > s.frame.l && r.bounds.t >= s.frame.t - 1 &&
                    r.bounds.t <= s.visible.t + kMaximizedTolerancePt) {
                    touchesTop = true;
                    break;
                }
            }
            w.maximized = full || touchesTop;
            w.cornerRadiusPx = full ? 0.0f : f.cornerRadius;
            w.snowAllowed = !isExcluded(r.pid, names);
            if (full && f.frontPid != 0 && r.pid == f.frontPid) L.fullscreenAppActive = true;
        }
        L.windows.push_back(w);
    }

    // Application windows that left the on-screen list: minimized, hidden (Cmd+H) or on
    // another Space keep their snow (hiddenWindows); closed ones are forgotten.
    std::vector<std::uint32_t> query;
    std::vector<std::uint64_t> queryKeys;
    std::set<std::uint64_t> stillHidden;
    for (const auto& [k, pn] : appWindows_) {
        if (apps.count(k) || hidden_.count(k)) continue;
        query.push_back(pn.second);
        queryKeys.push_back(k);
    }
    for (std::uint64_t k : hidden_) {
        if (apps.count(k)) continue;   // back on screen
        if (verifyHidden) {
            query.push_back(static_cast<std::uint32_t>(k & 0xFFFFFFFFu));
            queryKeys.push_back(k);
        } else {
            stillHidden.insert(k);
        }
    }
    if (!query.empty()) {
        const std::set<std::uint32_t> alive = exists ? exists(query) : std::set<std::uint32_t>{};
        for (std::size_t i = 0; i < query.size(); ++i)
            if (alive.count(query[i])) stillHidden.insert(queryKeys[i]);
    }
    hidden_ = stillHidden;
    appWindows_ = apps;
    for (std::uint64_t k : hidden_) {
        auto it = ids_.find(k);
        if (it != ids_.end()) L.hiddenWindows.push_back(it->second);
    }
    std::sort(L.hiddenWindows.begin(), L.hiddenWindows.end());
    // Forget ids of windows that are gone for good.
    for (auto it = ids_.begin(); it != ids_.end();) {
        if (!seen.count(it->first) && !hidden_.count(it->first)) {
            it = ids_.erase(it);
        } else {
            ++it;
        }
    }

    if (hasLast_ && sameLayout(L, last_)) return false;
    L.revision = last_.revision + 1;
    last_ = L;
    hasLast_ = true;
    out = L;
    return true;
}

}  // namespace snow::mac
