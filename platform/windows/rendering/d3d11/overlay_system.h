// OverlaySystem: one transparent click-through overlay window per monitor plus the render
// thread (D3D11 + DirectComposition, premultiplied alpha). Proven path: see poc/overlay_poc.cpp.
//
// All public methods are called on the UI thread (the thread that pumps messages); the
// overlay HWNDs are created there. Rendering runs on an internal thread.
//
// PUBLIC API IS A FIXED CONTRACT used by the platform app layer. Keep state behind Impl.
#pragma once
#include <windows.h>

#include <memory>
#include <vector>

#include "core/layout.h"
#include "core/snapshot.h"

namespace snow {

struct RenderStats {
    float fps = 0;
    float frameMs = 0;          // average CPU time to build + submit one frame
    int overlays = 0;
    int deviceResets = 0;       // successful device re-creations so far
    bool failed = false;        // true: rendering disabled after unrecoverable device loss
};

class OverlaySystem {
public:
    OverlaySystem();
    ~OverlaySystem();
    OverlaySystem(const OverlaySystem&) = delete;
    OverlaySystem& operator=(const OverlaySystem&) = delete;

    // `source` must outlive this object. `notifyHwnd`/`failedMsg`: when rendering gives up
    // after repeated device loss, PostMessage(notifyHwnd, failedMsg, 0, 0) is sent once.
    bool initialize(HINSTANCE instance, const SnapshotExchange* source, HWND notifyHwnd, UINT failedMsg);

    // Create/move/destroy overlays so there is exactly one per monitor. Safe to call often.
    void setMonitors(const std::vector<MonitorInfo>& monitors);

    // Hide (and stop presenting) or show all overlays. Hidden overlays cost ~0 CPU/GPU.
    void setVisible(bool visible);
    bool visible() const;

    void setFpsCap(int fps);            // 30 or 60 (vsync-aligned)
    void setGlobalOpacity(float o);     // 0.1..1, multiplies everything drawn

    // Re-assert HWND_TOPMOST without activating (called by the app on Z-order changes).
    void raise();

    std::vector<HWND> overlayWindows() const;
    RenderStats stats() const;

    // Stops the render thread, releases all GPU resources and destroys the overlay windows.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snow
