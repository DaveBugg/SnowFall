// MetalRenderer: draws a FrameSnapshot into one or more render targets exactly like the
// Windows OverlaySystem (platform/windows/rendering/d3d11/overlay_system.cpp): drift strips
// from the shared DriftMeshCache, then the objects batches (each clipped by its rectangles
// via the scissor), the particles (clipped when the snapshot says so) and the tool cursor.
// The shaders are the Windows HLSL translated to MSL at start-up (render/msl_translate.h).
//
// Units: the snapshot is in POINTS (global, top-left origin); a target covers `bounds`
// points and has widthPx x heightPx pixels, scale = pixels per point.
//
// Not thread-safe: one renderer per thread (the overlay render thread, the preview view on
// the main thread, the self-test). Objective-C++ header.
#pragma once
#import <Metal/Metal.h>

#include <memory>
#include <string>

#include "core/snapshot.h"

namespace snow::mac {

struct RenderTarget {
    RectI bounds;              // virtual-desktop area shown, points
    int widthPx = 0, heightPx = 0;
    float scale = 1.0f;        // pixels per point
};

class MetalRenderer {
public:
    MetalRenderer();
    ~MetalRenderer();
    MetalRenderer(const MetalRenderer&) = delete;
    MetalRenderer& operator=(const MetalRenderer&) = delete;

    // Compiles the shaders and builds the pipelines. false + `error` on failure.
    bool initialize(id<MTLDevice> device, std::string* error);
    bool ready() const;
    id<MTLDevice> device() const;
    id<MTLCommandQueue> queue() const;

    // Frame protocol: beginFrame() (waits for a free buffer slot, at most 3 frames in
    // flight) -> prepare(snapshot) -> encode() per target -> endFrame(commandBuffer) BEFORE
    // committing it (nil when nothing was submitted).
    void beginFrame();
    void prepare(const std::shared_ptr<const FrameSnapshot>& snap);
    void encode(id<MTLRenderCommandEncoder> enc, const RenderTarget& target, float dt, float opacity);
    void endFrame(id<MTLCommandBuffer> cb);

    // macOS cannot hide the system cursor of a background app: the tool sprite is drawn
    // this many points to the lower right of the pointer (the brush ring stays centred).
    void setCursorBadgeOffset(float dx, float dy);

    // Forget uploaded geometry (e.g. after the preview world was reset).
    void reset();

    static MTLPixelFormat pixelFormat() { return MTLPixelFormatBGRA8Unorm; }

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The GPU to render with: the low-power one on dual-GPU Intel Macs, else the default device.
// nil when Metal is unavailable.
id<MTLDevice> pickMetalDevice();

}  // namespace snow::mac
