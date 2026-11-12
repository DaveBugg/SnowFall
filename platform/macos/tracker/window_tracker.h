// WindowTracker (macOS): publishes DesktopLayout from CGWindowListCopyWindowInfo.
//
// * Window list polled on a background serial queue: ~30 Hz after a change, 60 Hz while
//   the left mouse button is down (window drags), 10 Hz when nothing changed for 2 s.
//   Only bounds / layer / owner PID / alpha / process name are read — never window titles
//   (kCGWindowName), so no Screen Recording permission is involved.
// * Screens (NSScreen), the frontmost app and the Dock geometry are read on the main thread
//   (on screen-parameter notifications and every 2 s) and handed to the poller.
// * The Dock is reported as a WindowKind::Taskbar window when it sits at the bottom of a
//   screen. Its width comes from the Accessibility API when the app is trusted (exact),
//   otherwise from an estimate based on the Dock preferences.
// * Minimized / Cmd+H-hidden / other-Space windows are reported in hiddenWindows.
// * A layout is published (callback on the main thread) only when it changed.
//
// Public methods: main thread. Objective-C++ header.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/layout.h"
#include "tracker/desktop_model.h"

namespace snow::mac {

struct TrackerOptions {
    std::vector<std::string> excludedProcesses;   // app / executable names, case-insensitive
};

struct TrackerStats {
    double pollsPerSecond = 0;
    double avgPollMs = 0;
    int windowsSeen = 0;          // raw on-screen entries in the last poll
    int dockWidth = 0;            // points, 0 = unknown
    bool dockFromAccessibility = false;
};

class WindowTracker {
public:
    using LayoutCallback = std::function<void(std::shared_ptr<const DesktopLayout>)>;

    WindowTracker();
    ~WindowTracker();
    WindowTracker(const WindowTracker&) = delete;
    WindowTracker& operator=(const WindowTracker&) = delete;

    // Reads the screens, polls once synchronously (the first layout is delivered through
    // `cb` before start() returns) and starts the background poller.
    bool start(LayoutCallback cb);
    void stop();

    void setOptions(const TrackerOptions& options);
    void setIgnoredWindows(const std::set<std::uint32_t>& windowNumbers);
    void refreshScreens();    // re-read NSScreen / Dock geometry now
    void requestResync();     // poll as soon as possible and republish

    std::vector<ScreenInfo> screens() const;
    std::shared_ptr<const DesktopLayout> current() const;
    TrackerStats stats() const;

    // Synchronous one-shot capture (self-test, works without start()).
    std::shared_ptr<const DesktopLayout> captureOnce();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Executable names of apps that currently own on-screen application windows (for the
// exclusions picker), sorted, no duplicates, never window titles.
std::vector<std::string> runningAppNamesWithWindows();

}  // namespace snow::mac
