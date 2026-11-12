// Direct2D/DirectWrite helper shared by every themed window of the UI module (UI thread only).
// paint() renders into an off-screen DIB through an ID2D1DCRenderTarget and blits the result,
// so it can draw into any HDC (WM_PAINT, NM_CUSTOMDRAW, WM_DRAWITEM) without flicker.
// Drawing coordinates are DIPs relative to the painted rectangle.
#pragma once
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <map>
#include <string>

namespace snow::ui {

inline D2D1_COLOR_F col(COLORREF c, float a = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a);
}
COLORREF mixColor(COLORREF a, COLORREF b, float t);

struct TextStyle {
    float size = 12.0f;                         // DIPs
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    bool display = false;                       // Segoe UI Variable Display (headings)
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    bool wrap = false;                          // false: single line with ellipsis
};

using PaintFn = std::function<void(ID2D1RenderTarget*, D2D1_SIZE_F)>;

class Painter {
public:
    static Painter& instance();

    bool paint(HDC dc, const RECT& rc, UINT dpi, const PaintFn& fn);
    // Like paint() but draws over the existing pixels of `dc` instead of an undefined background.
    bool paintOver(HDC dc, const RECT& rc, UINT dpi, const PaintFn& fn);

    // Font families: body = the system message font, display = Segoe UI Variable Display when
    // installed (else body). bodySize = message font em size in DIPs.
    void setBodyFont(const std::wstring& family, float sizeDip);
    float bodySize() const { return bodySize_; }
    const std::wstring& bodyFamily() const { return bodyFamily_; }
    const std::wstring& displayFamily() const { return displayFamily_; }

    // ---- drawing helpers (valid inside paint()) ----
    ID2D1RenderTarget* rt() const { return rt_; }
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c);
    void fill(const D2D1_RECT_F& r, D2D1_COLOR_F c);
    void fillRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c);
    void strokeRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width);
    void fillEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c);
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width);
    void vgradient(const D2D1_RECT_F& r, D2D1_COLOR_F top, D2D1_COLOR_F bottom, float radius = 0);
    void glow(float cx, float cy, float radius, D2D1_COLOR_F c);   // soft radial spot
    void text(const std::wstring& s, const D2D1_RECT_F& r, const TextStyle& st, D2D1_COLOR_F c);
    float measure(const std::wstring& s, const TextStyle& st, float maxWidth = 4000.0f, float* height = nullptr);
    ID2D1PathGeometry* path();   // new empty geometry (caller Releases)
    void fillGeometry(ID2D1Geometry* g, D2D1_COLOR_F c);
    void strokeGeometry(ID2D1Geometry* g, D2D1_COLOR_F c, float width);

    ID2D1Factory* factory() const { return factory_; }

    // Static-layer cache: renders `draw` once into an offscreen bitmap keyed by `key` (re-rendered
    // when the key, size or DPI changes) and draws that bitmap at (0,0). Valid inside paint().
    void cached(const std::wstring& key, D2D1_SIZE_F size, const std::function<void()>& draw);
    void clearCache();

private:
    Painter();
    IDWriteTextFormat* format(const TextStyle& st);
    bool ensureTarget();
    bool ensureDib(int w, int h);

    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID2D1DCRenderTarget* dcRt_ = nullptr;
    ID2D1RenderTarget* rt_ = nullptr;          // current target (DC target or a cache bitmap target)
    struct CacheEntry {
        ID2D1Bitmap* bmp = nullptr;
        float w = 0, h = 0, dpi = 0;
    };
    std::map<std::wstring, CacheEntry> cache_;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1StrokeStyle* roundCap_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    bool painting_ = false;
    bool keep_ = false;
    std::wstring bodyFamily_ = L"Segoe UI";
    std::wstring displayFamily_ = L"Segoe UI";
    float bodySize_ = 12.0f;
    std::map<std::wstring, IDWriteTextFormat*> formats_;
};

}  // namespace snow::ui
