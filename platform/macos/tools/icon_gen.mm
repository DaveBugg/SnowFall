// Build-time tool: renders the procedural app icon (app/icon_render.cpp) into an .iconset
// directory; CMake then runs `iconutil -c icns` on it to produce AppIcon.icns.
//   snow_icon_gen <out.iconset>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "app/icon_render.h"

static bool writePng(const std::string& path, int size) {
    const std::vector<std::uint8_t> px = snow::mac::renderAppIconRGBA(size);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, px.data(), static_cast<CFIndex>(px.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate(static_cast<size_t>(size), static_cast<size_t>(size), 8, 32, static_cast<size_t>(size) * 4, cs,
                                   static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast), provider, nullptr, false,
                                   kCGRenderingIntentDefault);
    bool ok = false;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    if (img) {
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
        if (dst) {
            CGImageDestinationAddImage(dst, img, nullptr);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CGImageRelease(img);
    }
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(cs);
    return ok;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) {
            std::fprintf(stderr, "usage: snow_icon_gen <out.iconset>\n");
            return 2;
        }
        const std::string dir = argv[1];
        ::mkdir(dir.c_str(), 0755);
        struct Entry {
            int pt, scale;
        };
        const Entry entries[] = {{16, 1}, {16, 2}, {32, 1}, {32, 2}, {128, 1}, {128, 2}, {256, 1}, {256, 2}, {512, 1}, {512, 2}};
        for (const Entry& e : entries) {
            char name[64];
            std::snprintf(name, sizeof name, "/icon_%dx%d%s.png", e.pt, e.pt, e.scale == 2 ? "@2x" : "");
            if (!writePng(dir + name, e.pt * e.scale)) {
                std::fprintf(stderr, "snow_icon_gen: cannot write %s\n", name);
                return 1;
            }
        }
    }
    return 0;
}
