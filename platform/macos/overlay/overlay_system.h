// OverlaySystem (macOS): one borderless, transparent, click-through NSWindow per screen with
// a CAMetalLayer, plus a render thread paced by CVDisplayLink that draws the latest
// FrameSnapshot into every overlay (same behaviour as the Windows OverlaySystem).
//
// Window setup: level = Dock level - 1 (above every application window, floating palette
// and modal panel; below the Dock, the menu bar, menus, popovers, Notification Center and
// Spotlight), ignoresMouseEvents, joins all Spaces (also fullscreen ones), never activates,
// excluded from the window cycle and Mission Control's window grid (stationary).
//
// Public methods: main thread. Objective-C++ header.
#pragma once
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/snapshot.h"
#include "render/cursor_layout.h"
#include "tracker/desktop_model.h"

namespace snow::mac {

struct RenderStats {
    float fps = 0;
    float frameMs = 0;          // CPU time to build + submit one frame (all overlays)
    int overlays = 0;
    std::uint64_t frames = 0;   // frames presented so far
    int gpuErrors = 0;          // command buffers that completed with an error
    bool failed = false;
};

class OverlaySystem {
public:
    OverlaySystem();
    ~OverlaySystem();
    OverlaySystem(const OverlaySystem&) = delete;
    OverlaySystem& operator=(const OverlaySystem&) = delete;

    // Creates the Metal device/pipelines and the render thread. false: no Metal / shader
    // failure (logged; `error` says what).
    bool initialize(const SnapshotExchange* source, std::string* error);

    // Exactly one overlay per screen (created / moved / destroyed as needed).
    void setScreens(const std::vector<ScreenInfo>& screens);

    void setVisible(bool visible);   // hidden overlays cost ~0 CPU/GPU
    bool visible() const;
    void setFpsCap(int fps);         // 30 or 60
    void setGlobalOpacity(float o);  // 0.1..1
    void setCursorDraw(CursorDraw mode);   // tool cursor: replaces the arrow / held by the arrow

    std::set<std::uint32_t> windowNumbers() const;   // for the tracker's ignore list
    RenderStats stats() const;
    std::string deviceName() const;

    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snow::mac
