// Pure (Apple-header-free) part of the macOS window tracker: coordinate conversion, Dock
// geometry, classification of CGWindowList entries and the DesktopLayout builder with its
// window-id bookkeeping. Everything here is unit-tested from synthetic data in
// tests/mac_unit_tests.cpp; window_tracker.mm only feeds it with real CoreGraphics data.
//
// Coordinates: CG global display space, i.e. POINTS with the origin at the top-left corner
// of the primary (menu-bar) screen, y growing downwards — the same convention as the core.
// AppKit (NSScreen / NSWindow frames) uses a bottom-left origin and is converted with
// cocoaToGlobal() / globalToCocoa(). The simulation runs in points (dpiScale = 1); the
// renderer multiplies by the backing scale factor.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/layout.h"

namespace snow::mac {

// ---- coordinates --------------------------------------------------------------------
struct CocoaRect {
    double x = 0, y = 0, w = 0, h = 0;   // bottom-left origin
};
// primaryHeight = height of NSScreen.screens[0] (the screen with the menu bar).
RectI cocoaToGlobal(const CocoaRect& r, double primaryHeight);
CocoaRect globalToCocoa(const RectI& r, double primaryHeight);
// Rounds a fractional CG rect (x, y, w, h) to the integer half-open RectI.
RectI roundRect(double x, double y, double w, double h);

// ---- screens and the Dock --------------------------------------------------------------
struct ScreenInfo {
    std::uint32_t displayId = 0;
    RectI frame;              // full screen, global points
    RectI visible;            // minus menu bar and Dock (NSScreen.visibleFrame), global points
    float backingScale = 1;   // pixels per point
    bool primary = false;     // NSScreen.screens[0]
    bool operator==(const ScreenInfo&) const = default;
};

enum class DockEdge { None, Bottom, Left, Right };
// Which edge of `s` the Dock occupies (None: on another screen or auto-hidden).
DockEdge dockEdge(const ScreenInfo& s);
// The "taskbar" surface for a bottom Dock: `dockWidth` points wide (0 = unknown: whole
// screen width), centred, from visible.b to one point ABOVE the screen bottom, so the
// core keeps its screen-bottom surface left and right of the Dock. Empty when the Dock is
// not at the bottom of this screen.
RectI dockTaskbarRect(const ScreenInfo& s, int dockWidth, int dockCenterX = 0);

// ---- window list entries ---------------------------------------------------------------
struct RawWindow {
    std::uint32_t number = 0;   // kCGWindowNumber
    int pid = 0;                // kCGWindowOwnerPID
    int layer = 0;              // kCGWindowLayer
    RectI bounds;               // kCGWindowBounds (global points)
    float alpha = 1;            // kCGWindowAlpha
    bool onscreen = true;       // kCGWindowIsOnscreen
    std::string owner;          // kCGWindowOwnerName (process name; window TITLES are never read)
};

enum class Verdict { Skip, App, Occluder };
struct ClassifyContext {
    int ownPid = 0;
    int overlayLevel = 19;                  // our overlay window level: higher layers draw above it
    std::set<std::uint32_t> ignored;        // our overlay windows etc.
    std::vector<ScreenInfo> screens;
};
Verdict classify(const RawWindow& w, const ClassifyContext& ctx);

// Is `b` a fullscreen window (covers one screen's whole frame, within 1 pt)?
bool coversScreen(const RectI& b, const std::vector<ScreenInfo>& screens);

// ---- layout builder ---------------------------------------------------------------------
struct TrackerFrame {
    std::vector<ScreenInfo> screens;        // screens[i].primary marks the menu-bar screen
    std::vector<RawWindow> windows;         // on-screen list, FRONT TO BACK
    int frontPid = 0;                       // frontmost application (fullscreen detection)
    int dockWidth = 0;                      // points; 0 = unknown
    int dockCenterX = 0;                    // global x of the Dock centre; 0 = screen centre
    float cornerRadius = 10;                // macOS window corner radius in points
};

class LayoutBuilder {
public:
    // Which of the given window numbers still exist (minimized / on another Space)?
    using ExistQuery = std::function<std::set<std::uint32_t>(const std::vector<std::uint32_t>&)>;
    // Lower-case executable/bundle names of a process (cached by the caller).
    using NameQuery = std::function<std::vector<std::string>(int pid)>;

    ClassifyContext context;
    std::vector<std::string> excluded;      // lower-case names (TrackerOptions)

    // Builds the layout for `f`. Returns true when it differs from the previous result
    // (`out` then holds the new layout with revision+1).
    bool build(const TrackerFrame& f, const ExistQuery& exists, const NameQuery& names, DesktopLayout& out,
               bool verifyHidden);

    const DesktopLayout& last() const { return last_; }
    WindowId idFor(int pid, std::uint32_t number);   // session-unique, stable while it lives
    void forgetAll();

private:
    static std::uint64_t key(int pid, std::uint32_t number) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pid)) << 32) | number;
    }
    bool isExcluded(int pid, const NameQuery& names);

    std::map<std::uint64_t, WindowId> ids_;
    std::map<std::uint64_t, std::pair<int, std::uint32_t>> appWindows_;   // key -> (pid, number), last on-screen App set
    std::set<std::uint64_t> hidden_;                                       // keys currently minimized / elsewhere
    WindowId nextId_ = 1;
    DesktopLayout last_;
    bool hasLast_ = false;
};

bool sameLayout(const DesktopLayout& a, const DesktopLayout& b);   // ignores revision

}  // namespace snow::mac
