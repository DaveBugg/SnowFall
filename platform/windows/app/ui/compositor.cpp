#include "compositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace snow::ui {
namespace {

HBITMAP makeDib(int w, int h, std::uint32_t** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    *bits = static_cast<std::uint32_t*>(p);
    return b;
}

int q(float v, float step) { return static_cast<int>(std::lround(v / step)); }

}  // namespace

// ---- Layer --------------------------------------------------------------------------------------
Layer::~Layer() { reset(); }

void Layer::reset() {
    if (dc_) {
        SelectObject(dc_, old_);
        DeleteObject(bmp_);
        DeleteDC(dc_);
    }
    dc_ = nullptr;
    bmp_ = nullptr;
    bits_ = nullptr;
    w_ = h_ = 0;
    key_.clear();
}

bool Layer::valid(const std::wstring& key, int w, int h, UINT dpi) const {
    return dc_ && w == w_ && h == h_ && dpi == dpi_ && key == key_;
}

void Layer::render(const std::wstring& key, int w, int h, UINT dpi, const PaintFn& fn) {
    if (valid(key, w, h, dpi) || w <= 0 || h <= 0) return;
    if (!dc_ || w != w_ || h != h_) {
        reset();
        dc_ = CreateCompatibleDC(nullptr);
        bmp_ = makeDib(w, h, &bits_);
        if (!bmp_) {
            DeleteDC(dc_);
            dc_ = nullptr;
            return;
        }
        old_ = SelectObject(dc_, bmp_);
        w_ = w;
        h_ = h;
    }
    dpi_ = dpi;
    key_ = key;
    RECT rc{0, 0, w, h};
    Painter::instance().paint(dc_, rc, dpi, fn);
    GdiFlush();
}

void Layer::renderFrom(const Layer& base, const std::wstring& key, UINT dpi, const PaintFn& fn) {
    const int w = base.width(), h = base.height();
    if (!base.bits() || w <= 0 || h <= 0) return;
    if (!dc_ || w != w_ || h != h_) {
        reset();
        dc_ = CreateCompatibleDC(nullptr);
        bmp_ = makeDib(w, h, &bits_);
        if (!bmp_) {
            DeleteDC(dc_);
            dc_ = nullptr;
            return;
        }
        old_ = SelectObject(dc_, bmp_);
        w_ = w;
        h_ = h;
    }
    dpi_ = dpi;
    key_ = key;
    std::memcpy(bits_, base.bits(), static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(std::uint32_t));
    RECT rc{0, 0, w, h};
    Painter::instance().paintOver(dc_, rc, dpi, fn);
    GdiFlush();
}

// ---- Compositor ---------------------------------------------------------------------------------
Compositor& Compositor::instance() {
    static Compositor c;
    return c;
}

bool Compositor::ensure(int w, int h) {
    if (dc_ && w <= cap_w_ && h <= cap_h_) return true;
    const int nw = std::max(w, cap_w_), nh = std::max(h, cap_h_);
    if (dc_) {
        SelectObject(dc_, old_);
        DeleteObject(bmp_);
    } else {
        dc_ = CreateCompatibleDC(nullptr);
    }
    bmp_ = makeDib(nw, nh, &bits_);
    if (!bmp_) return false;
    old_ = SelectObject(dc_, bmp_);
    cap_w_ = nw;
    cap_h_ = nh;
    return true;
}

bool Compositor::begin(const Layer& base, int w, int h, int sx, int sy) {
    if (w <= 0 || h <= 0 || !base.bits() || !ensure(w, h)) return false;
    w_ = w;
    h_ = h;
    resetClip();
    const int copyW = std::max(0, std::min(w, base.width() - sx));
    for (int y = 0; y < h; ++y) {
        std::uint32_t* dst = bits_ + static_cast<size_t>(y) * static_cast<size_t>(cap_w_);
        const int by = y + sy;
        if (by < 0 || by >= base.height() || copyW <= 0) continue;
        std::memcpy(dst, base.bits() + static_cast<size_t>(by) * static_cast<size_t>(base.width()) + sx,
                    static_cast<size_t>(copyW) * sizeof(std::uint32_t));
    }
    return true;
}

void Compositor::end(HDC target, int x, int y) {
    if (!dc_ || !target) return;
    GdiFlush();
    BitBlt(target, x, y, w_, h_, dc_, 0, 0, SRCCOPY);
}

void Compositor::setClip(int l, int t, int r, int b) {
    cl_ = std::max(0, l);
    ct_ = std::max(0, t);
    cr_ = std::min(w_, r);
    cb_ = std::min(h_, b);
}

void Compositor::resetClip() {
    cl_ = ct_ = 0;
    cr_ = w_;
    cb_ = h_;
}

// kind: 0 dot(r), 1 ellipse(rx, ry), 2 glow(r), 3 streak(dx, dy, width), 4 rect(w, h)
const Compositor::Sprite& Compositor::sprite(std::uint64_t key, int kind, float a, float b, float c, float fx, float fy) {
    auto it = sprites_.find(key);
    if (it != sprites_.end()) return it->second;
    if (sprites_.size() > 1500) sprites_.clear();
    Sprite s;
    float ext = 1.0f;
    float halfX = 0, halfY = 0;
    switch (kind) {
        case 0: halfX = halfY = a; break;
        case 1: halfX = a; halfY = b; break;
        case 2: halfX = halfY = a; break;
        case 3: halfX = std::fabs(a) / 2 + c; halfY = std::fabs(b) / 2 + c; break;
        default: halfX = a / 2; halfY = b / 2; break;
    }
    s.ox = static_cast<int>(std::ceil(halfX + ext));
    s.oy = static_cast<int>(std::ceil(halfY + ext));
    s.w = 2 * s.ox + 2;
    s.h = 2 * s.oy + 2;
    s.cov.assign(static_cast<size_t>(s.w) * static_cast<size_t>(s.h), 0);
    // Sprite centre inside the bitmap: (ox + fx, oy + fy) where fx/fy = sub-pixel phase.
    const float cx = static_cast<float>(s.ox) + fx, cy = static_cast<float>(s.oy) + fy;
    const int ss = 4;   // 4x4 supersampling
    for (int y = 0; y < s.h; ++y) {
        for (int x = 0; x < s.w; ++x) {
            float acc = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / ss - cx;
                    const float py = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / ss - cy;
                    float v = 0;
                    switch (kind) {
                        case 0: v = (px * px + py * py <= a * a) ? 1.0f : 0.0f; break;
                        case 1: v = (a > 0 && b > 0 && (px * px) / (a * a) + (py * py) / (b * b) <= 1.0f) ? 1.0f : 0.0f; break;
                        case 2: {
                            const float d = std::sqrt(px * px + py * py) / std::max(a, 0.01f);
                            v = d < 1 ? (1 - d) * (1 - d) : 0.0f;
                            break;
                        }
                        case 3: {   // capsule from (-a/2,-b/2) to (a/2,b/2), radius c
                            const float vx = a, vy = b;
                            const float wx = px + a / 2, wy = py + b / 2;
                            const float len2 = vx * vx + vy * vy;
                            float t = len2 > 0 ? (wx * vx + wy * vy) / len2 : 0;
                            t = std::clamp(t, 0.0f, 1.0f);
                            const float dx = wx - t * vx, dy = wy - t * vy;
                            v = (dx * dx + dy * dy <= c * c) ? (0.35f + 0.65f * t) : 0.0f;   // fades towards the tail
                            break;
                        }
                        default: v = (std::fabs(px) <= a / 2 && std::fabs(py) <= b / 2) ? 1.0f : 0.0f; break;
                    }
                    acc += v;
                }
            }
            s.cov[static_cast<size_t>(y) * static_cast<size_t>(s.w) + static_cast<size_t>(x)] =
                static_cast<std::uint8_t>(std::lround(255.0f * acc / (ss * ss)));
        }
    }
    return sprites_.emplace(key, std::move(s)).first->second;
}

void Compositor::blend(const Sprite& s, int x0, int y0, COLORREF c, float alpha) {
    const int a = std::clamp(static_cast<int>(alpha * 256.0f), 0, 256);
    if (a <= 0) return;
    const int cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
    const int ys = std::max(y0, ct_), ye = std::min(y0 + s.h, cb_);
    const int xs = std::max(x0, cl_), xe = std::min(x0 + s.w, cr_);
    for (int y = ys; y < ye; ++y) {
        const std::uint8_t* src = s.cov.data() + static_cast<size_t>(y - y0) * static_cast<size_t>(s.w) - x0;
        std::uint32_t* dst = bits_ + static_cast<size_t>(y) * static_cast<size_t>(cap_w_);
        for (int x = xs; x < xe; ++x) {
            const int k = (src[x] * a) >> 8;   // 0..255
            if (!k) continue;
            const std::uint32_t d = dst[x];
            const int dr = static_cast<int>((d >> 16) & 0xFF), dg = static_cast<int>((d >> 8) & 0xFF), db = static_cast<int>(d & 0xFF);
            const int r = dr + (((cr - dr) * k) >> 8), g = dg + (((cg - dg) * k) >> 8), b = db + (((cb - db) * k) >> 8);
            dst[x] = (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
        }
    }
}

namespace {
std::uint64_t mk(int kind, int a, int b, int c, int fx, int fy) {
    return (static_cast<std::uint64_t>(kind) << 60) ^ (static_cast<std::uint64_t>(a & 0xFFF) << 40) ^
           (static_cast<std::uint64_t>(b & 0xFFF) << 24) ^ (static_cast<std::uint64_t>(c & 0xFFF) << 8) ^
           (static_cast<std::uint64_t>(fx & 3) << 2) ^ static_cast<std::uint64_t>(fy & 3);
}
// Integer anchor + quantised sub-pixel phase (quarters).
void split(float v, int& i, int& phase, float& frac) {
    const float f = std::floor(v);
    i = static_cast<int>(f);
    phase = std::clamp(static_cast<int>((v - f) * 4.0f), 0, 3);
    frac = (static_cast<float>(phase) + 0.5f) / 4.0f;
}
}  // namespace

void Compositor::dot(float x, float y, float r, COLORREF c, float alpha) {
    r = std::max(0.5f, r);
    int ix, iy, px, py;
    float fx, fy;
    split(x, ix, px, fx);
    split(y, iy, py, fy);
    const int qr = q(r, 0.25f);
    const Sprite& s = sprite(mk(0, qr, 0, 0, px, py), 0, static_cast<float>(qr) * 0.25f, 0, 0, fx, fy);
    blend(s, ix - s.ox, iy - s.oy, c, alpha);
}

void Compositor::ellipse(float x, float y, float rx, float ry, COLORREF c, float alpha) {
    int ix, iy, px, py;
    float fx, fy;
    split(x, ix, px, fx);
    split(y, iy, py, fy);
    const int a = q(std::max(0.5f, rx), 0.25f), b = q(std::max(0.5f, ry), 0.25f);
    const Sprite& s = sprite(mk(1, a, b, 0, px, py), 1, static_cast<float>(a) * 0.25f, static_cast<float>(b) * 0.25f, 0, fx, fy);
    blend(s, ix - s.ox, iy - s.oy, c, alpha);
}

void Compositor::glow(float x, float y, float r, COLORREF c, float alpha) {
    const int qr = std::max(1, q(r, 1.0f));
    const Sprite& s = sprite(mk(2, qr, 0, 0, 0, 0), 2, static_cast<float>(qr), 0, 0, 0.5f, 0.5f);
    blend(s, static_cast<int>(std::floor(x)) - s.ox, static_cast<int>(std::floor(y)) - s.oy, c, alpha);
}

void Compositor::streak(float x0, float y0, float x1, float y1, float width, COLORREF c, float alpha) {
    const float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    const int dx = q(x1 - x0, 1.0f), dy = q(y1 - y0, 1.0f);
    const int w = q(std::max(0.3f, width / 2), 0.25f);
    int ix, iy, px, py;
    float fx, fy;
    split(mx, ix, px, fx);
    split(my, iy, py, fy);
    const Sprite& s = sprite(mk(3, dx + 2048, dy + 2048, w, px, py), 3, static_cast<float>(dx), static_cast<float>(dy),
                             static_cast<float>(w) * 0.25f, fx, fy);
    blend(s, ix - s.ox, iy - s.oy, c, alpha);
}

void Compositor::rect(float l, float t, float r, float b, COLORREF c, float alpha) {
    const int x0 = std::max(cl_, static_cast<int>(std::lround(l))), x1 = std::min(cr_, static_cast<int>(std::lround(r)));
    const int y0 = std::max(ct_, static_cast<int>(std::lround(t))), y1 = std::min(cb_, static_cast<int>(std::lround(b)));
    const int k = std::clamp(static_cast<int>(alpha * 255.0f), 0, 255);
    const int cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
    for (int y = y0; y < y1; ++y) {
        std::uint32_t* dst = bits_ + static_cast<size_t>(y) * static_cast<size_t>(cap_w_);
        for (int x = x0; x < x1; ++x) {
            const std::uint32_t d = dst[x];
            const int dr = static_cast<int>((d >> 16) & 0xFF), dg = static_cast<int>((d >> 8) & 0xFF), db = static_cast<int>(d & 0xFF);
            dst[x] = (static_cast<std::uint32_t>(dr + (((cr - dr) * k) >> 8)) << 16) |
                     (static_cast<std::uint32_t>(dg + (((cg - dg) * k) >> 8)) << 8) |
                     static_cast<std::uint32_t>(db + (((cb - db) * k) >> 8));
        }
    }
}

}  // namespace snow::ui
