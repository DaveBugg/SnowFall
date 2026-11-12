// WindowTracker: observes monitors and top-level windows and publishes DesktopLayout.
// EnumWindows for sync, out-of-context SetWinEventHook for changes (coalesced), plus a
// low-frequency reconciliation timer. Never reads window text or pixels, never injects.
//
// All methods are called on the UI thread; the callback is invoked on the UI thread.
//
// PUBLIC API IS A FIXED CONTRACT used by the platform app layer. Keep state behind Impl.
#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/layout.h"

namespace snow {

struct TrackerOptions {
    // Executable names (case-insensitive, e.g. L"game.exe"): their windows get snowAllowed=false.
    std::vector<std::wstring> excludedProcesses;
};

class WindowTracker {
public:
    using LayoutCallback = std::function<void(std::shared_ptr<const DesktopLayout>)>;

    WindowTracker();
    ~WindowTracker();
    WindowTracker(const WindowTracker&) = delete;
    WindowTracker& operator=(const WindowTracker&) = delete;

    // Installs hooks/timers and immediately publishes the first layout through `cb`.
    bool start(HINSTANCE instance, LayoutCallback cb);
    void stop();   // removes all hooks/timers; idempotent

    void setOptions(const TrackerOptions& options);
    // Windows belonging to this app that must be ignored entirely (overlays, settings...).
    // Windows of the current process are ignored by default EXCEPT those listed in
    // setOwnAppWindows (e.g. the settings window, which should collect snow like any app).
    void setIgnoredWindows(const std::vector<HWND>& windows);
    void setOwnAppWindows(const std::vector<HWND>& windows);

    void requestResync();   // schedule a full re-enumeration (coalesced)
    std::shared_ptr<const DesktopLayout> current() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snow
