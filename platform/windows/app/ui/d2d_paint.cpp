#include "d2d_paint.h"

#include <algorithm>
#include <cwchar>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace snow::ui {

COLORREF mixColor(COLORREF a, COLORREF b, float t) {
    auto m = [t](int x, int y) { return static_cast<int>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

Painter& Painter::instance() {
    static Painter p;
    return p;
}

Painter::Painter() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite_));
    if (factory_) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                      D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        factory_->CreateStrokeStyle(sp, nullptr, 0, &roundCap_);
    }
    // Prefer Segoe UI Variable Display for headings (Windows 11).
    if (dwrite_) {
        IDWriteFontCollection* fonts = nullptr;
        if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts, FALSE)) && fonts) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Display", &index, &exists)) && exists) {
                displayFamily_ = L"Segoe UI Variable Display";
            }
            fonts->Release();
        }
    }
}

void Painter::setBodyFont(const std::wstring& family, float sizeDip) {
    if (!family.empty()) bodyFamily_ = family;
    if (sizeDip > 4) bodySize_ = sizeDip;
    if (displayFamily_ == L"Segoe UI" && !family.empty()) displayFamily_ = family;
}

bool Painter::ensureTarget() {
    if (dcRt_) return true;
    if (!factory_) return false;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    if (FAILED(factory_->CreateDCRenderTarget(&props, &dcRt_))) {
        dcRt_ = nullptr;
        return false;
    }
    dcRt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return true;
}

bool Painter::ensureDib(int w, int h) {
    if (memDC_ && w <= dibW_ && h <= dibH_) return true;
    const int nw = std::max(w, dibW_), nh = std::max(h, dibH_);
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteObject(dib_);
    } else {
        memDC_ = CreateCompatibleDC(nullptr);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = nw;
    bi.bmiHeader.biHeight = -nh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    dib_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_) {
        dibW_ = dibH_ = 0;
        return false;
    }
    oldBmp_ = SelectObject(memDC_, dib_);
    dibW_ = nw;
    dibH_ = nh;
    return true;
}

bool Painter::paintOver(HDC dc, const RECT& rc, UINT dpi, const PaintFn& fn) {
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || painting_) return false;
    if (!ensureTarget() || !ensureDib(w, h)) return false;
    BitBlt(memDC_, 0, 0, w, h, dc, rc.left, rc.top, SRCCOPY);
    keep_ = true;
    const bool ok = paint(dc, rc, dpi, fn);
    keep_ = false;
    return ok;
}

bool Painter::paint(HDC dc, const RECT& rc, UINT dpi, const PaintFn& fn) {
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || painting_) return false;
    if (!ensureTarget() || !ensureDib(w, h)) return false;
    if (!keep_) {   // deterministic background for callers that do not fill everything
        RECT clear{0, 0, w, h};
        FillRect(memDC_, &clear, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    RECT bind{0, 0, w, h};
    if (FAILED(dcRt_->BindDC(memDC_, &bind))) return false;
    rt_ = dcRt_;
    const float fdpi = static_cast<float>(dpi ? dpi : 96);
    rt_->SetDpi(fdpi, fdpi);
    painting_ = true;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    fn(rt_, D2D1::SizeF(static_cast<float>(w) * 96.0f / fdpi, static_cast<float>(h) * 96.0f / fdpi));
    const HRESULT hr = rt_->EndDraw();
    painting_ = false;
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        if (brush_) brush_->Release();
        brush_ = nullptr;
        clearCache();
        dcRt_->Release();
        dcRt_ = nullptr;
        rt_ = nullptr;
        return false;
    }
    BitBlt(dc, rc.left, rc.top, w, h, memDC_, 0, 0, SRCCOPY);
    return SUCCEEDED(hr);
}

void Painter::clearCache() {
    for (auto& e : cache_)
        if (e.second.bmp) e.second.bmp->Release();
    cache_.clear();
}

void Painter::cached(const std::wstring& key, D2D1_SIZE_F size, const std::function<void()>& draw) {
    float dx = 96, dy = 96;
    rt_->GetDpi(&dx, &dy);
    auto it = cache_.find(key);
    if (it == cache_.end() || it->second.w != size.width || it->second.h != size.height || it->second.dpi != dx) {
        if (it != cache_.end()) {
            if (it->second.bmp) it->second.bmp->Release();
            cache_.erase(it);
        }
        if (cache_.size() > 48) clearCache();
        ID2D1BitmapRenderTarget* brt = nullptr;
        if (FAILED(rt_->CreateCompatibleRenderTarget(size, &brt)) || !brt) {
            draw();
            return;
        }
        ID2D1RenderTarget* saved = rt_;
        ID2D1SolidColorBrush* savedBrush = brush_;
        rt_ = brt;
        brush_ = nullptr;
        brt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        brt->BeginDraw();
        brt->Clear(D2D1::ColorF(0, 0, 0, 1));
        draw();
        brt->EndDraw();
        if (brush_) brush_->Release();
        brush_ = savedBrush;
        rt_ = saved;
        CacheEntry e;
        brt->GetBitmap(&e.bmp);
        brt->Release();
        e.w = size.width;
        e.h = size.height;
        e.dpi = dx;
        it = cache_.emplace(key, e).first;
    }
    if (it->second.bmp) rt_->DrawBitmap(it->second.bmp, D2D1::RectF(0, 0, size.width, size.height), 1.0f,
                                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

ID2D1SolidColorBrush* Painter::brush(D2D1_COLOR_F c) {
    if (!brush_) rt_->CreateSolidColorBrush(c, &brush_);
    else brush_->SetColor(c);
    return brush_;
}

void Painter::fill(const D2D1_RECT_F& r, D2D1_COLOR_F c) { rt_->FillRectangle(r, brush(c)); }

void Painter::fillRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush(c));
}

void Painter::strokeRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width) {
    const float h = width / 2;
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), radius, radius),
                              brush(c), width);
}

void Painter::fillEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c) {
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush(c));
}

void Painter::line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
    rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(c), width, roundCap_);
}

void Painter::vgradient(const D2D1_RECT_F& r, D2D1_COLOR_F top, D2D1_COLOR_F bottom, float radius) {
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, top}, {1.0f, bottom}};
    ID2D1GradientStopCollection* coll = nullptr;
    if (FAILED(rt_->CreateGradientStopCollection(stops, 2, &coll))) return;
    ID2D1LinearGradientBrush* lg = nullptr;
    rt_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom)), coll, &lg);
    if (lg) {
        if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), lg);
        else rt_->FillRectangle(r, lg);
        lg->Release();
    }
    coll->Release();
}

void Painter::glow(float cx, float cy, float radius, D2D1_COLOR_F c) {
    if (radius < 16.0f) {
        D2D1_COLOR_F k = c;
        k.a = c.a * 0.30f;
        fillEllipse(cx, cy, radius, radius, k);
        k.a = c.a * 0.45f;
        fillEllipse(cx, cy, radius * 0.55f, radius * 0.55f, k);
        return;
    }
    D2D1_COLOR_F clear = c;
    clear.a = 0;
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, c}, {1.0f, clear}};
    ID2D1GradientStopCollection* coll = nullptr;
    if (FAILED(rt_->CreateGradientStopCollection(stops, 2, &coll))) return;
    ID2D1RadialGradientBrush* rg = nullptr;
    rt_->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), radius, radius), coll, &rg);
    if (rg) {
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), rg);
        rg->Release();
    }
    coll->Release();
}

IDWriteTextFormat* Painter::format(const TextStyle& st) {
    if (!dwrite_) return nullptr;
    const std::wstring& family = st.display ? displayFamily_ : bodyFamily_;
    wchar_t key[160];
    swprintf_s(key, L"%ls|%d|%d|%d|%d|%d", family.c_str(), static_cast<int>(st.size * 100), static_cast<int>(st.weight),
               static_cast<int>(st.align), static_cast<int>(st.valign), st.wrap ? 1 : 0);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    if (FAILED(dwrite_->CreateTextFormat(family.c_str(), nullptr, st.weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, st.size, L"", &f))) {
        return nullptr;
    }
    f->SetTextAlignment(st.align);
    f->SetParagraphAlignment(st.valign);
    f->SetWordWrapping(st.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!st.wrap) {
        IDWriteInlineObject* ellipsis = nullptr;
        dwrite_->CreateEllipsisTrimmingSign(f, &ellipsis);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trimming, ellipsis);
        if (ellipsis) ellipsis->Release();
    }
    formats_[key] = f;
    return f;
}

void Painter::text(const std::wstring& s, const D2D1_RECT_F& r, const TextStyle& st, D2D1_COLOR_F c) {
    IDWriteTextFormat* f = format(st);
    if (!f || s.empty()) return;
    rt_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Painter::measure(const std::wstring& s, const TextStyle& st, float maxWidth, float* height) {
    IDWriteTextFormat* f = format(st);
    if (height) *height = 0;
    if (!f || s.empty()) return 0;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, maxWidth, 4000.0f, &layout))) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    if (height) *height = m.height;
    return m.widthIncludingTrailingWhitespace;
}

ID2D1PathGeometry* Painter::path() {
    ID2D1PathGeometry* g = nullptr;
    if (factory_) factory_->CreatePathGeometry(&g);
    return g;
}

void Painter::fillGeometry(ID2D1Geometry* g, D2D1_COLOR_F c) {
    if (g) rt_->FillGeometry(g, brush(c));
}

void Painter::strokeGeometry(ID2D1Geometry* g, D2D1_COLOR_F c, float width) {
    if (g) rt_->DrawGeometry(g, brush(c), width, roundCap_);
}

}  // namespace snow::ui
