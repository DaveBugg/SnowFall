// snow_art: writes the packaging artwork (installer .ico, MSIX logo PNGs) from the app's own
// procedural snowflake badge (snow::TrayIcon::createAppIcon, platform/windows/app/ui/app_icon.cpp), so every
// package shows exactly the same original art as the tray icon. No files are read; nothing is
// downloaded.
//
//   snow_art --ico  <out.ico>        multi-size icon (16..256, PNG-compressed entries)
//   snow_art --msix <assets-dir>     StoreLogo.png, Square44x44Logo.png, Square150x150Logo.png,
//                                    Wide310x150Logo.png, SplashScreen.png
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "app/ui/tray_icon.h"

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // BGRA, straight alpha, top-down
};

Image iconPixels(int size) {
    Image img;
    HICON icon = snow::TrayIcon::createAppIcon(size);
    if (!icon) return img;
    ICONINFO ii{};
    if (GetIconInfo(icon, &ii)) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        img.w = img.h = size;
        img.px.assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
        HDC dc = GetDC(nullptr);
        if (!GetDIBits(dc, ii.hbmColor, 0, static_cast<UINT>(size), img.px.data(), &bi, DIB_RGB_COLORS)) img = {};
        ReleaseDC(nullptr, dc);
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
    }
    DestroyIcon(icon);
    return img;
}

// "Over" compositing of straight-alpha `src` onto straight-alpha `dst` at (ox, oy).
void blit(Image& dst, const Image& src, int ox, int oy) {
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const int dx = ox + x, dy = oy + y;
            if (dx < 0 || dy < 0 || dx >= dst.w || dy >= dst.h) continue;
            const uint32_t s = src.px[static_cast<size_t>(y) * src.w + x];
            uint32_t& d = dst.px[static_cast<size_t>(dy) * dst.w + dx];
            const float sa = static_cast<float>(s >> 24) / 255.0f, da = static_cast<float>(d >> 24) / 255.0f;
            const float oa = sa + da * (1 - sa);
            if (oa <= 0) continue;
            auto ch = [&](int shift) {
                const float sc = static_cast<float>((s >> shift) & 0xFF), dc = static_cast<float>((d >> shift) & 0xFF);
                return static_cast<uint32_t>(std::lround((sc * sa + dc * da * (1 - sa)) / oa)) & 0xFF;
            };
            d = (static_cast<uint32_t>(std::lround(oa * 255)) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }
    }
}

// Soft pale-blue dot (falling snow) with analytic anti-aliasing.
void dot(Image& img, float cx, float cy, float r, float alpha) {
    for (int y = static_cast<int>(cy - r - 2); y <= static_cast<int>(cy + r + 2); ++y) {
        for (int x = static_cast<int>(cx - r - 2); x <= static_cast<int>(cx + r + 2); ++x) {
            if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
            const float d = std::hypot(static_cast<float>(x) + 0.5f - cx, static_cast<float>(y) + 0.5f - cy);
            const float cov = std::fmin(1.0f, std::fmax(0.0f, r + 0.5f - d)) * alpha;
            if (cov <= 0) continue;
            Image one;
            one.w = one.h = 1;
            one.px = {(static_cast<uint32_t>(std::lround(cov * 255)) << 24) | 0x96C8FAu};   // pale ice blue: visible on light and dark tiles
            blit(img, one, x, y);
        }
    }
}

Image canvas(int w, int h) {
    Image img;
    img.w = w;
    img.h = h;
    img.px.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    return img;
}

Image badgeOn(int w, int h, int badge) {
    Image img = canvas(w, h);
    const Image b = iconPixels(badge);
    blit(img, b, (w - badge) / 2, (h - badge) / 2);
    return img;
}

// Deterministic "falling snow" around the badge for wide/splash images.
void sprinkle(Image& img, int count, unsigned seed) {
    for (int i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float x = static_cast<float>(seed % 10000u) / 10000.0f * static_cast<float>(img.w);
        seed = seed * 1664525u + 1013904223u;
        const float y = static_cast<float>(seed % 10000u) / 10000.0f * static_cast<float>(img.h);
        seed = seed * 1664525u + 1013904223u;
        const float r = 1.0f + static_cast<float>(seed % 100u) / 100.0f * 2.2f * static_cast<float>(img.h) / 150.0f;
        const float cx = static_cast<float>(img.w) / 2, cy = static_cast<float>(img.h) / 2;
        if (std::hypot(x - cx, y - cy) < static_cast<float>(img.h) * 0.42f) continue;   // keep the badge clean
        dot(img, x, y, r, 0.85f);
    }
}

IWICImagingFactory* g_wic = nullptr;

bool encodePng(const Image& img, IStream* stream) {
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(static_cast<UINT>(img.w), static_cast<UINT>(img.h)));
    if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(frame->WritePixels(static_cast<UINT>(img.h), static_cast<UINT>(img.w) * 4,
                                          static_cast<UINT>(img.px.size() * 4),
                                          reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.px.data())))) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    return ok;
}

std::vector<uint8_t> pngBytes(const Image& img) {
    std::vector<uint8_t> out;
    IStream* s = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &s))) return out;
    if (encodePng(img, s)) {
        HGLOBAL h = nullptr;
        STATSTG st{};
        if (SUCCEEDED(GetHGlobalFromStream(s, &h)) && SUCCEEDED(s->Stat(&st, STATFLAG_NONAME))) {
            const auto* p = static_cast<const uint8_t*>(GlobalLock(h));
            if (p) {
                out.assign(p, p + st.cbSize.QuadPart);
                GlobalUnlock(h);
            }
        }
    }
    s->Release();
    return out;
}

bool writeFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

bool writePng(const std::wstring& path, const Image& img) {
    const std::vector<uint8_t> bytes = pngBytes(img);
    const bool ok = !bytes.empty() && writeFile(path, bytes);
    wprintf(L"%ls %ls (%dx%d)\n", ok ? L"wrote" : L"FAILED", path.c_str(), img.w, img.h);
    return ok;
}

bool writeIco(const std::wstring& path) {
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    std::vector<std::vector<uint8_t>> pngs;
    for (int s : sizes) pngs.push_back(pngBytes(iconPixels(s)));
    std::vector<uint8_t> out;
    auto put16 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto put32 = [&](uint32_t v) {
        put16(v & 0xFFFF);
        put16(v >> 16);
    };
    const uint32_t n = static_cast<uint32_t>(pngs.size());
    put16(0);
    put16(1);
    put16(n);
    uint32_t offset = 6 + 16 * n;
    for (uint32_t i = 0; i < n; ++i) {
        if (pngs[i].empty()) return false;
        const int s = sizes[i];
        out.push_back(static_cast<uint8_t>(s >= 256 ? 0 : s));
        out.push_back(static_cast<uint8_t>(s >= 256 ? 0 : s));
        out.push_back(0);
        out.push_back(0);
        put16(1);
        put16(32);
        put32(static_cast<uint32_t>(pngs[i].size()));
        put32(offset);
        offset += static_cast<uint32_t>(pngs[i].size());
    }
    for (const auto& p : pngs) out.insert(out.end(), p.begin(), p.end());
    const bool ok = writeFile(path, out);
    wprintf(L"%ls %ls (%u sizes)\n", ok ? L"wrote" : L"FAILED", path.c_str(), n);
    return ok;
}

bool writeMsixAssets(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    bool ok = true;
    ok &= writePng(dir + L"\\StoreLogo.png", badgeOn(50, 50, 46));
    ok &= writePng(dir + L"\\Square44x44Logo.png", badgeOn(44, 44, 44));
    ok &= writePng(dir + L"\\Square150x150Logo.png", badgeOn(150, 150, 100));
    Image wide = badgeOn(310, 150, 100);
    sprinkle(wide, 70, 2026u);
    ok &= writePng(dir + L"\\Wide310x150Logo.png", wide);
    Image splash = badgeOn(620, 300, 200);
    sprinkle(splash, 140, 1231u);
    ok &= writePng(dir + L"\\SplashScreen.png", splash);
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        wprintf(L"usage: snow_art --ico <out.ico> | --msix <assets-dir>\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) {
        wprintf(L"WIC unavailable\n");
        return 1;
    }
    const std::wstring mode = argv[1];
    bool ok = false;
    if (mode == L"--ico") ok = writeIco(argv[2]);
    else if (mode == L"--msix") ok = writeMsixAssets(argv[2]);
    else wprintf(L"unknown mode\n");
    g_wic->Release();
    CoUninitialize();
    return ok ? 0 : 1;
}
