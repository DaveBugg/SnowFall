// Cheap animation frames: a static layer is rendered once with Direct2D into a DIB (Layer); each
// frame copies it into a scratch DIB, blends small anti-aliased sprites (flakes, bulbs, sparks)
// in plain C++ and blits the result once. Avoids the fixed cost of a Direct2D DC render target
// per frame. Coordinates in physical pixels. UI thread only.
#pragma once
#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "d2d_paint.h"

namespace snow::ui {

class Layer {
public:
    Layer() = default;
    ~Layer();
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    bool valid(const std::wstring& key, int w, int h, UINT dpi) const;
    // (Re)renders with Direct2D when key/size/dpi changed. fn draws in DIPs.
    void render(const std::wstring& key, int w, int h, UINT dpi, const PaintFn& fn);
    // Copies `base` (same size) and draws fn over it (e.g. drifts over a static background).
    void renderFrom(const Layer& base, const std::wstring& key, UINT dpi, const PaintFn& fn);
    void reset();
    const std::uint32_t* bits() const { return bits_; }
    int width() const { return w_; }
    int height() const { return h_; }
    HDC dc() const { return dc_; }

private:
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    std::uint32_t* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    UINT dpi_ = 0;
    std::wstring key_;
};

class Compositor {
public:
    static Compositor& instance();

    // Starts a frame of size w x h from the layer region at (sx, sy).
    bool begin(const Layer& base, int w, int h, int sx = 0, int sy = 0);
    // Blits the frame to `target` at (x, y).
    void end(HDC target, int x, int y);

    int width() const { return w_; }
    int height() const { return h_; }
    void setClip(int l, int t, int r, int b);   // sprites are clipped to this rect (frame px)
    void resetClip();

    // Sprites. alpha 0..1.
    void dot(float x, float y, float r, COLORREF c, float alpha);                       // soft disc
    void ellipse(float x, float y, float rx, float ry, COLORREF c, float alpha);
    void glow(float x, float y, float r, COLORREF c, float alpha);                      // radial falloff
    void streak(float x0, float y0, float x1, float y1, float width, COLORREF c, float alpha);
    void rect(float l, float t, float r, float b, COLORREF c, float alpha);

private:
    struct Sprite {
        int w = 0, h = 0, ox = 0, oy = 0;   // origin offset: top-left = floor(pos) - o
        std::vector<std::uint8_t> cov;
    };
    const Sprite& sprite(std::uint64_t key, int kind, float a, float b, float c, float fx, float fy);
    void blend(const Sprite& s, int x, int y, COLORREF c, float alpha);
    bool ensure(int w, int h);

    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    std::uint32_t* bits_ = nullptr;
    int cap_w_ = 0, cap_h_ = 0;
    int w_ = 0, h_ = 0;
    int cl_ = 0, ct_ = 0, cr_ = 0, cb_ = 0;
    std::map<std::uint64_t, Sprite> sprites_;
};

}  // namespace snow::ui
