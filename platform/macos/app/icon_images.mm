#include "app/icon_images.h"

#include "app/icon_render.h"

namespace snow::mac {

CGImageRef makeCGImage(const std::vector<std::uint8_t>& rgba, int size) {
    if (size <= 0 || rgba.size() < static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4) return nullptr;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, rgba.data(), static_cast<CFIndex>(rgba.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate(static_cast<size_t>(size), static_cast<size_t>(size), 8, 32,
                                   static_cast<size_t>(size) * 4, cs,
                                   static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrderDefault,
                                   provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(cs);
    return img;
}

NSImage* makeAppIconImage() {
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(512, 512)];
    for (int px : {32, 64, 128, 256, 512, 1024}) {
        CGImageRef cg = makeCGImage(renderAppIconRGBA(px), px);
        if (!cg) continue;
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
        rep.size = NSMakeSize(px / 2.0, px / 2.0);
        [image addRepresentation:rep];
        CGImageRelease(cg);
    }
    return image;
}

NSImage* makeMenuBarImage(bool paused) {
    const double pt = 18;
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(pt, pt)];
    for (int scale : {1, 2, 3}) {
        const int px = static_cast<int>(pt) * scale;
        CGImageRef cg = makeCGImage(renderMenuBarGlyphRGBA(px, paused), px);
        if (!cg) continue;
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
        rep.size = NSMakeSize(pt, pt);
        [image addRepresentation:rep];
        CGImageRelease(cg);
    }
    image.template = YES;   // AppKit tints it for light / dark menu bars
    image.accessibilityDescription = @"SnowFall";
    return image;
}

}  // namespace snow::mac
