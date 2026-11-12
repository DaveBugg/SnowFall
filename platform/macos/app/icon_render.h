// Procedural SnowFall artwork, ported from platform/windows/app/ui/app_icon.cpp (same
// geometry: six-armed snowflake with side branches on a round blue badge). Pure C++.
#pragma once
#include <cstdint>
#include <vector>

namespace snow::mac {

// size x size pixels, row-major from the top, RGBA bytes with PREMULTIPLIED alpha (ready for
// a CGBitmapContext with kCGImageAlphaPremultipliedLast). 4x4 supersampled.
std::vector<std::uint8_t> renderAppIconRGBA(int size, bool paused = false);

// Menu-bar glyph: the bare snowflake (no badge) as an alpha mask, for a template NSImage
// (AppKit tints it for light/dark menu bars). `paused` adds a small pause mark.
std::vector<std::uint8_t> renderMenuBarGlyphRGBA(int size, bool paused = false);

}  // namespace snow::mac
