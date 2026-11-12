// NSImage / CGImage wrappers around the procedural artwork of app/icon_render.h.
#pragma once
#import <AppKit/AppKit.h>

#include <cstdint>
#include <vector>

namespace snow::mac {

// Premultiplied RGBA bytes (size x size) -> CGImage (+1 reference, caller releases).
CGImageRef makeCGImage(const std::vector<std::uint8_t>& rgba, int size);
// Application icon (badge), multi-resolution.
NSImage* makeAppIconImage();
// Monochrome template image for the status item (18 pt, 1x + 2x + 3x representations).
NSImage* makeMenuBarImage(bool paused);

}  // namespace snow::mac
