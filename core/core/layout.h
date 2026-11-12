// DesktopLayout: the immutable description of monitors and top-level windows that
// WindowTracker (UI thread) hands to the simulation. All coordinates are physical
// pixels in virtual-desktop space.
#pragma once
#include <cstdint>
#include <vector>

#include "core/types.h"

namespace snow {

enum class WindowKind : std::uint8_t {
    App,       // normal application window: may host a snow surface on its top edge AND occludes
    Taskbar,   // Shell_TrayWnd / Shell_SecondaryTrayWnd: hosts a "taskbar" surface AND occludes
    Occluder,  // popup/menu/tooltip/tool window etc.: never hosts snow, but hides snow beneath it
};

struct WindowInfo {
    WindowId id = 0;         // session-unique, stable while the window lives
    RectI bounds;            // DWM extended frame bounds (visible frame), physical px
    WindowKind kind = WindowKind::App;
    float dpiScale = 1.0f;   // DPI of the window's monitor / 96
    float cornerRadiusPx = 0;  // approximate top-corner radius in physical px (0 = square)
    bool maximized = false;
    bool snowAllowed = true;   // false: user-excluded process; treat as Occluder for snow purposes
};

struct MonitorInfo {
    int id = 0;        // index stable within one layout revision
    RectI bounds;      // full monitor rect
    RectI work;        // work area (excludes docked taskbar)
    float dpiScale = 1.0f;
    bool primary = false;
    bool operator==(const MonitorInfo&) const = default;
};

struct DesktopLayout {
    std::uint64_t revision = 0;
    std::vector<MonitorInfo> monitors;
    // Visible windows in Z-order, FRONT TO BACK (index 0 is the topmost window).
    // Desktop/wallpaper windows and this application's own windows are not listed.
    std::vector<WindowInfo> windows;
    // Windows that still exist but are currently not shown (minimized / cloaked / on
    // another virtual desktop). Their snow must be kept but not drawn. A window id that
    // appears in neither list has been closed.
    std::vector<WindowId> hiddenWindows;
    // True when the foreground window is a fullscreen app/game/video (auto-pause trigger).
    bool fullscreenAppActive = false;
};

}  // namespace snow
