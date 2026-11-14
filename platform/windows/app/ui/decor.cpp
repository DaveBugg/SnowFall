#include "decor.h"

#include <algorithm>
#include <cmath>

namespace snow::ui {
namespace {

constexpr float kPi = 3.14159265f;

D2D1_POINT_2F pt(float x, float y) { return D2D1::Point2F(x, y); }

// Fills a closed polygon.
void polygon(Painter& p, const D2D1_POINT_2F* pts, int n, D2D1_COLOR_F c) {
    ID2D1PathGeometry* g = p.path();
    if (!g) return;
    ID2D1GeometrySink* s = nullptr;
    if (SUCCEEDED(g->Open(&s))) {
        s->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < n; ++i) s->AddLine(pts[i]);
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        s->Release();
        p.fillGeometry(g, c);
    }
    g->Release();
}

// Rolling hill filled from `top` (wave) down to the bottom of `r`.
void hill(Painter& p, const D2D1_RECT_F& r, float top, float amp, float phase, D2D1_COLOR_F c) {
    ID2D1PathGeometry* g = p.path();
    if (!g) return;
    ID2D1GeometrySink* s = nullptr;
    if (SUCCEEDED(g->Open(&s))) {
        const float w = r.right - r.left;
        s->BeginFigure(pt(r.left, r.bottom), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(pt(r.left, top + amp * std::sin(phase)));
        s->AddBezier(D2D1::BezierSegment(pt(r.left + w * 0.3f, top - amp), pt(r.left + w * 0.55f, top + amp * 1.4f),
                                         pt(r.right, top + amp * std::cos(phase))));
        s->AddLine(pt(r.right, r.bottom));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        s->Release();
        p.fillGeometry(g, c);
    }
    g->Release();
}

void firTree(Painter& p, float cx, float baseY, float h, D2D1_COLOR_F body, D2D1_COLOR_F snowCap) {
    for (int i = 0; i < 3; ++i) {
        const float t = static_cast<float>(i);
        const float top = baseY - h + t * h * 0.25f;
        const float bottom = baseY - h * 0.45f + t * h * 0.22f;
        const float half = h * (0.22f + 0.1f * t);
        D2D1_POINT_2F tri[3] = {pt(cx, top), pt(cx + half, bottom), pt(cx - half, bottom)};
        polygon(p, tri, 3, body);
        D2D1_POINT_2F cap[3] = {pt(cx, top), pt(cx + half * 0.35f, top + (bottom - top) * 0.35f),
                                pt(cx - half * 0.35f, top + (bottom - top) * 0.35f)};
        polygon(p, cap, 3, snowCap);
    }
    p.fill(D2D1::RectF(cx - h * 0.04f, baseY - h * 0.1f, cx + h * 0.04f, baseY), body);
}

const COLORREF kLightColours[8] = {RGB(255, 205, 80), RGB(255, 90, 80), RGB(90, 220, 120), RGB(90, 160, 255),
                                   RGB(190, 120, 255), RGB(255, 255, 255), RGB(255, 150, 60), RGB(90, 230, 230)};

}  // namespace

// ---- FlakeField ---------------------------------------------------------------------------------
unsigned FlakeField::rnd() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_;
}

float FlakeField::frand(float a, float b) { return a + (b - a) * static_cast<float>(rnd() % 10000) / 10000.0f; }

void FlakeField::reset(unsigned seed, int count, float minR, float maxR, float minSpeed, float maxSpeed, float minAlpha) {
    seed_ = seed ? seed : 1;
    flakes_.clear();
    for (int i = 0; i < count; ++i) {
        DecoFlake f{};
        f.x = frand(0, 1);
        f.y = frand(0, 1);
        f.r = frand(minR, maxR);
        const float k = (f.r - minR) / std::max(0.01f, maxR - minR);   // bigger = closer = faster
        f.speed = minSpeed + (maxSpeed - minSpeed) * (0.35f * frand(0, 1) + 0.65f * k);
        f.phase = frand(0, 2 * kPi);
        f.sway = frand(0.004f, 0.02f);
        f.alpha = frand(minAlpha, 1.0f);
        flakes_.push_back(f);
    }
}

void FlakeField::step(float dt, float wind) {
    time_ += dt;
    for (DecoFlake& f : flakes_) {
        f.y += f.speed * dt;
        f.x += wind * dt * (0.6f + f.speed);
        if (f.y > 1.05f) {
            f.y -= 1.1f;
            f.x = frand(0, 1);
        }
        if (f.x > 1.05f) f.x -= 1.1f;
        if (f.x < -0.05f) f.x += 1.1f;
    }
}

void FlakeField::draw(Compositor& c, float l, float t, float w, float h, float s, COLORREF color, float streak, float wind) const {
    for (const DecoFlake& f : flakes_) {
        const float x = l + (f.x + f.sway * std::sin(time_ * 1.3f + f.phase)) * w;
        const float y = t + f.y * h;
        if (streak > 0) {
            const float len = streak * f.speed * h * 0.12f;
            c.streak(x - wind * len * 2.0f, y - len, x, y, f.r * 0.9f * s, color, f.alpha * 0.8f);
        } else {
            if (f.r > 1.6f) c.dot(x, y, f.r * 1.8f * s, color, f.alpha * 0.12f);
            c.dot(x, y, f.r * s, color, f.alpha);
        }
    }
}

// ---- scenes -------------------------------------------------------------------------------------
void SceneAnim::ensure(WeatherPreset p, unsigned seed) {
    if (ready && p == preset) return;
    preset = p;
    ready = true;
    switch (p) {
        case WeatherPreset::Light: flakes.reset(seed, 16, 0.7f, 1.7f, 0.08f, 0.16f); break;
        case WeatherPreset::Normal: flakes.reset(seed, 42, 0.7f, 2.0f, 0.16f, 0.32f); break;
        case WeatherPreset::Blizzard: flakes.reset(seed, 90, 0.5f, 1.4f, 0.5f, 0.9f); break;
        case WeatherPreset::Thaw: flakes.reset(seed, 12, 1.3f, 2.4f, 0.32f, 0.48f, 0.5f); break;
    }
}

void SceneAnim::step(float dt) {
    time += dt;
    const float wind = preset == WeatherPreset::Blizzard ? 0.55f : preset == WeatherPreset::Normal ? 0.04f : 0.0f;
    flakes.step(dt, wind);
}

std::wstring sceneKey(const Palette& pal, Surface surface, const SceneAnim& anim, const std::wstring& title,
                      const std::wstring& subtitle, const SceneCardState& st) {
    wchar_t key[320];
    swprintf_s(key, L"card|%d|%d|%d|%ls|%ls|%d%d%d%d%d", static_cast<int>(pal.theme), static_cast<int>(surface),
               static_cast<int>(anim.preset), title.c_str(), subtitle.c_str(), st.selected ? 1 : 0, st.focused ? 1 : 0,
               st.hot ? 1 : 0, st.enabled ? 1 : 0, st.compact ? 1 : 0);
    return key;
}

void drawSceneDynamic(Compositor& c, float s, const Palette& pal, const SceneAnim& anim, const SceneCardState& st) {
    if (!st.enabled) return;
    const float inset = 3.0f, textH = st.compact ? 22.0f : 38.0f;
    const float wd = static_cast<float>(c.width()) / s, hd = static_cast<float>(c.height()) / s;
    // scene rect in DIPs (inside the card border)
    const float L = inset + 2.5f, T = inset + 2.5f, R = wd - inset - 2.5f, B = hd - inset - textH;
    c.setClip(static_cast<int>(L * s), static_cast<int>(T * s), static_cast<int>(R * s), static_cast<int>(B * s));
    const float sw = R - L, sh = B - T;
    switch (anim.preset) {
        case WeatherPreset::Blizzard: anim.flakes.draw(c, L * s, T * s, sw * s, sh * s, s, pal.snow, 1.0f, 0.55f); break;
        case WeatherPreset::Thaw: {
            const float sceneTop = inset, sceneW = wd - 2 * inset, sceneH = hd - inset - textH - inset;
            const float eave = sceneTop + sceneH * 0.14f;
            for (int i = 0; i < 5; ++i) {
                const float ix = inset + sceneW * (0.12f + 0.19f * static_cast<float>(i));
                const float len = sceneH * (0.12f + 0.06f * static_cast<float>((i * 7) % 3));
                const float period = 1.4f + 0.3f * static_cast<float>(i % 3);
                const float ph = std::fmod(anim.time + static_cast<float>(i) * 0.53f, period) / period;
                const float dy = eave + len + ph * ph * (B - eave - len);
                c.ellipse(ix * s, dy * s, 1.4f * s, 2.2f * s, RGB(200, 230, 255), 0.9f * (1.0f - ph * 0.4f));
            }
            anim.flakes.draw(c, L * s, T * s, sw * s, sh * s, s, mixColor(pal.snow, RGB(200, 225, 245), 0.3f));
            break;
        }
        default: anim.flakes.draw(c, L * s, T * s, sw * s, sh * s, s, pal.snow); break;
    }
    c.resetClip();
}

void drawSceneStatic(Painter& p, D2D1_SIZE_F size, const Palette& pal, Surface surface, const SceneAnim& anim,
                     const std::wstring& title, const std::wstring& subtitle, const SceneCardState& st) {
    p.fill(D2D1::RectF(0, 0, size.width, size.height), col(pal.surface(surface)));
    const float inset = 3.0f, radius = 9.0f;
    const D2D1_RECT_F card = D2D1::RectF(inset, inset, size.width - inset, size.height - inset);
    const float textH = st.compact ? 22.0f : 38.0f;
    p.fillRound(card, radius, col(pal.card));

    // Scene, clipped to the rounded top part.
    const D2D1_RECT_F scene = D2D1::RectF(card.left, card.top, card.right, card.bottom - textH);
    ID2D1RoundedRectangleGeometry* clipGeom = nullptr;
    p.factory()->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(card.left, card.top, card.right, card.bottom - textH + radius), radius, radius), &clipGeom);
    ID2D1Layer* layer = nullptr;
    p.rt()->CreateLayer(&layer);
    if (layer && clipGeom) {
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(scene, clipGeom);
        p.rt()->PushLayer(lp, layer);
    } else {
        p.rt()->PushAxisAlignedClip(scene, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    const WeatherPreset preset = anim.preset;
    COLORREF skyTop = pal.skyTop, skyBottom = pal.skyBottom;
    if (preset == WeatherPreset::Blizzard) {
        skyTop = mixColor(skyTop, RGB(150, 165, 185), 0.25f);
        skyBottom = mixColor(skyBottom, RGB(210, 220, 232), 0.35f);
    } else if (preset == WeatherPreset::Thaw) {
        skyTop = mixColor(skyTop, RGB(120, 135, 150), 0.35f);
        skyBottom = mixColor(skyBottom, RGB(170, 180, 190), 0.35f);
    }
    p.vgradient(scene, col(skyTop), col(skyBottom));
    const float sw = scene.right - scene.left, sh = scene.bottom - scene.top;
    const D2D1_COLOR_F ground = col(pal.ground);
    const D2D1_COLOR_F dark = col(mixColor(pal.skyTop, RGB(8, 30, 28), 0.45f));

    switch (preset) {
        case WeatherPreset::Light: {
            const float mx = scene.right - sw * 0.2f, my = scene.top + sh * 0.26f, mr = sh * 0.12f;
            p.glow(mx, my, mr * 3.2f, col(RGB(255, 246, 214), 0.35f));
            p.fillEllipse(mx, my, mr, mr, col(RGB(255, 248, 225)));
            p.fillEllipse(mx + mr * 0.45f, my - mr * 0.25f, mr * 0.85f, mr * 0.85f, col(skyTop));
            for (int i = 0; i < 5; ++i) {
                const float sx = scene.left + sw * (0.1f + 0.15f * static_cast<float>(i)), sy = scene.top + sh * (0.12f + 0.09f * static_cast<float>(i % 3));
                p.fillEllipse(sx, sy, 0.8f, 0.8f, col(RGB(255, 255, 255), 0.5f + 0.1f * static_cast<float>(i % 3)));
            }
            hill(p, scene, scene.bottom - sh * 0.24f, sh * 0.05f, 0.6f, ground);
            // little house with a lit window
            const float hx = scene.left + sw * 0.28f, hy = scene.bottom - sh * 0.24f;
            p.fill(D2D1::RectF(hx - sh * 0.12f, hy - sh * 0.14f, hx + sh * 0.12f, hy + 2), dark);
            D2D1_POINT_2F roof[3] = {pt(hx - sh * 0.16f, hy - sh * 0.13f), pt(hx, hy - sh * 0.3f), pt(hx + sh * 0.16f, hy - sh * 0.13f)};
            polygon(p, roof, 3, ground);
            p.glow(hx, hy - sh * 0.06f, sh * 0.12f, col(RGB(255, 200, 90), 0.5f));
            p.fill(D2D1::RectF(hx - sh * 0.035f, hy - sh * 0.09f, hx + sh * 0.035f, hy - sh * 0.025f), col(RGB(255, 210, 110)));
            break;
        }
        case WeatherPreset::Normal: {
            hill(p, scene, scene.bottom - sh * 0.26f, sh * 0.06f, 1.3f, ground);
            firTree(p, scene.left + sw * 0.22f, scene.bottom - sh * 0.2f, sh * 0.5f, dark, ground);
            firTree(p, scene.left + sw * 0.4f, scene.bottom - sh * 0.17f, sh * 0.36f, dark, ground);
            firTree(p, scene.left + sw * 0.8f, scene.bottom - sh * 0.22f, sh * 0.44f, dark, ground);
            break;
        }
        case WeatherPreset::Blizzard: {
            hill(p, scene, scene.bottom - sh * 0.3f, sh * 0.09f, 2.4f, ground);
            firTree(p, scene.left + sw * 0.7f, scene.bottom - sh * 0.24f, sh * 0.42f, col(mixColor(pal.skyTop, RGB(40, 60, 70), 0.5f), 0.75f), ground);
            p.fill(scene, col(RGB(255, 255, 255), 0.10f));
            break;
        }
        case WeatherPreset::Thaw: {
            // roof eave with icicles and drops
            const float eave = scene.top + sh * 0.14f;
            p.fill(D2D1::RectF(scene.left, scene.top, scene.right, eave), dark);
            p.fill(D2D1::RectF(scene.left, eave - 3, scene.right, eave), ground);
            for (int i = 0; i < 5; ++i) {
                const float ix = scene.left + sw * (0.12f + 0.19f * static_cast<float>(i));
                const float len = sh * (0.12f + 0.06f * static_cast<float>((i * 7) % 3));
                D2D1_POINT_2F ice[3] = {pt(ix - 3, eave), pt(ix + 3, eave), pt(ix, eave + len)};
                polygon(p, ice, 3, col(RGB(220, 240, 255), 0.85f));
            }
            hill(p, scene, scene.bottom - sh * 0.18f, sh * 0.03f, 0.2f, col(mixColor(pal.ground, RGB(160, 180, 195), 0.35f)));
            p.fillEllipse(scene.left + sw * 0.55f, scene.bottom - sh * 0.08f, sw * 0.22f, sh * 0.04f, col(RGB(140, 185, 225), 0.7f));
            break;
        }
    }
    if (layer && clipGeom) p.rt()->PopLayer();
    else p.rt()->PopAxisAlignedClip();
    if (layer) layer->Release();
    if (clipGeom) clipGeom->Release();

    // Text area.
    const D2D1_COLOR_F textC = col(st.enabled ? pal.text : mixColor(pal.text, pal.card, 0.5f));
    if (st.compact) {
        TextStyle ts{p.bodySize() * 0.95f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
        ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        p.text(title, D2D1::RectF(card.left + 4, card.bottom - textH, card.right - 4, card.bottom - 1), ts, textC);
    } else {
        TextStyle ts{p.bodySize(), DWRITE_FONT_WEIGHT_SEMI_BOLD};
        p.text(title, D2D1::RectF(card.left + 10, card.bottom - textH + 2, card.right - 8, card.bottom - textH + 20), ts, textC);
        TextStyle ss{p.bodySize() * 0.88f};
        p.text(subtitle, D2D1::RectF(card.left + 10, card.bottom - 19, card.right - 8, card.bottom - 3), ss,
               col(pal.textSecondary));
    }

    // Border / selection / focus.
    if (st.selected) {
        p.strokeRound(card, radius, col(pal.accent), 2.0f);
        const float bx = card.right - 13, by = card.top + 13;
        p.fillEllipse(bx, by, 8, 8, col(pal.accent));
        p.line(bx - 3.5f, by, bx - 1, by + 3, col(pal.accentText), 1.6f);
        p.line(bx - 1, by + 3, bx + 4, by - 3, col(pal.accentText), 1.6f);
    } else {
        p.strokeRound(card, radius, col(st.hot ? pal.accent : pal.cardBorder, st.hot ? 0.7f : 1.0f), 1.0f);
    }
    if (st.focused) p.strokeRound(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), radius + 2, col(pal.focus), 1.5f);
    if (!st.enabled) p.fillRound(card, radius, col(pal.surface(surface), 0.45f));
}

// ---- header -------------------------------------------------------------------------------------
void HeaderAnim::ensure(unsigned seed, int count) {
    if (ready) return;
    ready = true;
    flakes.reset(seed, count, 0.6f, 1.8f, 0.10f, 0.30f, 0.35f);
}

void HeaderAnim::step(float dt) {
    time += dt;
    flakes.step(dt, 0.015f);
}

void drawLogo(Painter& p, const Palette& pal, float cx, float cy, float size) {
    const float r = size / 2;
    if (pal.theme == UiTheme::Globe) {
        p.fillEllipse(cx, cy - r * 0.12f, r * 0.86f, r * 0.86f, col(RGB(255, 255, 255), 0.22f));
        p.strokeRound(D2D1::RectF(cx - r * 0.86f, cy - r * 0.98f, cx + r * 0.86f, cy + r * 0.74f), r * 0.86f, col(RGB(255, 255, 255), 0.9f), 1.4f);
        D2D1_POINT_2F base[4] = {pt(cx - r * 0.7f, cy + r * 0.62f), pt(cx + r * 0.7f, cy + r * 0.62f), pt(cx + r * 0.9f, cy + r),
                                 pt(cx - r * 0.9f, cy + r)};
        polygon(p, base, 4, col(pal.headerChip));
        drawNavGlyph(p, 0, cx, cy - r * 0.12f, size * 0.55f, col(RGB(255, 255, 255)));
        return;
    }
    p.glow(cx, cy, r * 1.4f, col(pal.headerText, 0.18f));
    drawNavGlyph(p, 0, cx, cy, size, col(pal.headerText));
}

namespace {
template <class F>
void forEachBulb(float x0, float x1, float y, float sag, F&& fn) {
    const float span = 96.0f;
    const int spans = std::max(1, static_cast<int>((x1 - x0) / span + 0.5f));
    const float sw = (x1 - x0) / static_cast<float>(spans);
    int bulb = 0;
    for (int s = 0; s < spans; ++s) {
        const float a = x0 + sw * static_cast<float>(s);
        for (int k = 1; k <= 3; ++k) {
            const float u = static_cast<float>(k) / 4.0f;
            fn(bulb, a + sw * u, y + 2 * sag * 2 * u * (1 - u) + 2, kLightColours[bulb % 5 == 4 ? 3 : bulb % 4]);
            ++bulb;
        }
    }
}
}  // namespace

void drawGarlandStatic(Painter& p, float x0, float x1, float y, float sag) {
    const float span = 96.0f;
    const int spans = std::max(1, static_cast<int>((x1 - x0) / span + 0.5f));
    const float sw = (x1 - x0) / static_cast<float>(spans);
    for (int s = 0; s < spans; ++s) {
        const float a = x0 + sw * static_cast<float>(s);
        ID2D1PathGeometry* g = p.path();
        if (!g) continue;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(g->Open(&sink))) {
            sink->BeginFigure(pt(a, y), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(pt(a + sw / 2, y + 2 * sag), pt(a + sw, y)));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            p.strokeGeometry(g, col(RGB(30, 60, 36)), 1.5f);
        }
        g->Release();
    }
    forEachBulb(x0, x1, y, sag, [&](int, float bx, float by, COLORREF c) {
        p.fill(D2D1::RectF(bx - 1.6f, by - 1, bx + 1.6f, by + 1.5f), col(RGB(70, 80, 70)));
        p.fillEllipse(bx, by + 4.5f, 2.8f, 3.8f, col(mixColor(c, RGB(40, 30, 30), 0.6f)));
    });
}

void drawGarlandLights(Compositor& c, float s, float x0, float x1, float y, float sag, float time) {
    forEachBulb(x0, x1, y, sag, [&](int bulb, float bx, float by, COLORREF col) {
        if (std::sin(time * 2.2f + static_cast<float>(bulb) * 1.7f) <= -0.35f) return;
        c.glow(bx * s, (by + 4) * s, 9 * s, col, 0.55f);
        c.ellipse(bx * s, (by + 4.5f) * s, 2.8f * s, 3.8f * s, col, 1.0f);
        c.ellipse((bx - 0.8f) * s, (by + 3.4f) * s, 0.9f * s, 1.2f * s, RGB(255, 255, 255), 0.7f);
    });
}

void drawHeaderStatic(Painter& p, D2D1_SIZE_F size, const Palette& pal, const std::wstring& title, const std::wstring& subtitle,
                      const HeaderChip* chips, int chipCount) {
    float textRight = size.width - 8;
    for (int i = 0; i < chipCount; ++i) {
        if (chips[i].rect.right <= chips[i].rect.left || !chips[i].limitsText) continue;
        TextStyle cs{p.bodySize() * 0.92f};
        const float cw = p.measure(chips[i].caption, cs);
        textRight = std::min(textRight, chips[i].rect.left - cw - 18);
    }
    const D2D1_RECT_F chip = D2D1::RectF(textRight, 0, textRight, 0);   // right bound of title / subtitle
    const D2D1_RECT_F all = D2D1::RectF(0, 0, size.width, size.height);
    p.vgradient(all, col(pal.headerTop), col(pal.headerBottom));
    if (pal.theme == UiTheme::Night) {
        for (int i = 0; i < 26; ++i) {
            const float sx = std::fmod(static_cast<float>(i) * 97.3f, size.width);
            const float sy = 4 + std::fmod(static_cast<float>(i) * 37.1f, size.height * 0.6f);
            p.fillEllipse(sx, sy, 0.7f, 0.7f, col(RGB(255, 255, 255), 0.3f + 0.1f * static_cast<float>(i % 4)));
        }
        const float mx = size.width * 0.62f, my = size.height * 0.42f;
        p.glow(mx, my, 40, col(RGB(210, 230, 255), 0.18f));
        p.fillEllipse(mx, my, 9, 9, col(RGB(240, 246, 255), 0.92f));
        p.fillEllipse(mx + 4, my - 2.5f, 8, 8, col(mixColor(pal.headerTop, pal.headerBottom, 0.4f)));
    } else if (pal.theme == UiTheme::Globe) {
        p.glow(size.width * 0.5f, size.height * 1.4f, size.height * 2.2f, col(RGB(255, 255, 255), 0.22f));
        hill(p, all, size.height - 8, 3, 0.8f, col(RGB(255, 255, 255), 0.35f));
    }
    if (pal.garland) drawGarlandStatic(p, -20, size.width + 20, size.height - 15, 5);
    drawLogo(p, pal, 34, size.height / 2 - (pal.garland ? 6 : 0), 30);
    TextStyle ts{p.bodySize() * 1.75f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    ts.display = true;
    ts.valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    const float ty = pal.garland ? 5.0f : 9.0f;
    p.text(title, D2D1::RectF(62, ty, chip.left, ty + 28), ts, col(pal.headerText));
    TextStyle ss{p.bodySize()};
    ss.valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    p.text(subtitle, D2D1::RectF(63, ty + 29, chip.left, ty + 47), ss, col(pal.headerTextSecondary));
    for (int i = 0; i < chipCount; ++i) {
        const D2D1_RECT_F& c = chips[i].rect;
        if (c.right <= c.left) continue;
        const float r = (c.bottom - c.top) / 2;
        p.fillRound(c, r, col(pal.headerChip));
        p.strokeRound(c, r, col(pal.headerText, 0.25f), 1.0f);
        if (chips[i].caption.empty()) continue;
        TextStyle cs{p.bodySize() * 0.92f};
        cs.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
        p.text(chips[i].caption, D2D1::RectF(c.left - 160, c.top, c.left - 8, c.bottom), cs, col(pal.headerTextSecondary));
    }
}

void drawToolGlyph(Painter& p, Tool tool, float cx, float cy, float size, D2D1_COLOR_F c) {
    const float r = size / 2, w = std::max(1.3f, size / 11.0f);
    switch (tool) {
        case Tool::Snowball: {
            p.rt()->DrawEllipse(D2D1::Ellipse(pt(cx, cy), r * 0.8f, r * 0.8f), p.brush(c), w);
            D2D1_COLOR_F soft = c;
            soft.a *= 0.35f;
            p.fillEllipse(cx + r * 0.18f, cy + r * 0.22f, r * 0.45f, r * 0.38f, soft);
            p.fillEllipse(cx - r * 0.3f, cy - r * 0.3f, w * 0.9f, w * 0.9f, c);
            break;
        }
        case Tool::Mitten: {
            // cuff
            p.rt()->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - r * 0.42f, cy + r * 0.52f, cx + r * 0.5f, cy + r * 0.9f), w, w),
                                         p.brush(c), w);
            // palm + thumb
            ID2D1PathGeometry* g = p.path();
            if (g) {
                ID2D1GeometrySink* s = nullptr;
                if (SUCCEEDED(g->Open(&s))) {
                    s->BeginFigure(pt(cx - r * 0.38f, cy + r * 0.5f), D2D1_FIGURE_BEGIN_HOLLOW);
                    s->AddLine(pt(cx - r * 0.42f, cy - r * 0.35f));
                    s->AddBezier(D2D1::BezierSegment(pt(cx - r * 0.42f, cy - r * 0.95f), pt(cx + r * 0.45f, cy - r * 0.95f), pt(cx + r * 0.45f, cy - r * 0.35f)));
                    s->AddLine(pt(cx + r * 0.45f, cy - r * 0.05f));
                    s->AddBezier(D2D1::BezierSegment(pt(cx + r * 0.65f, cy - r * 0.3f), pt(cx + r * 0.95f, cy - r * 0.1f), pt(cx + r * 0.75f, cy + r * 0.15f)));
                    s->AddLine(pt(cx + r * 0.46f, cy + r * 0.5f));
                    s->EndFigure(D2D1_FIGURE_END_OPEN);
                    s->Close();
                    s->Release();
                    p.strokeGeometry(g, c, w);
                }
                g->Release();
            }
            break;
        }
        default:
            drawNavGlyph(p, 2, cx, cy, size * 0.9f, c);
            break;
    }
}

void drawHeaderDynamic(Compositor& c, float s, const Palette& pal, const HeaderAnim& anim, float heightDip) {
    anim.flakes.draw(c, 0, 0, static_cast<float>(c.width()), static_cast<float>(c.height()), s, pal.snow);
    if (pal.garland) drawGarlandLights(c, s, -20, static_cast<float>(c.width()) / s + 20, heightDip - 15, 5, anim.time);
}

// ---- glyphs -------------------------------------------------------------------------------------
void drawNavGlyph(Painter& p, int glyph, float cx, float cy, float size, D2D1_COLOR_F c) {
    const float r = size / 2, w = std::max(1.2f, size / 12.0f);
    switch (glyph) {
        case 0: {   // snowflake
            for (int k = 0; k < 3; ++k) {
                const float a = kPi / 2 + static_cast<float>(k) * kPi / 3;
                const float dx = std::cos(a) * r, dy = std::sin(a) * r;
                p.line(cx - dx, cy - dy, cx + dx, cy + dy, c, w);
            }
            for (int k = 0; k < 6; ++k) {
                const float a = -kPi / 2 + static_cast<float>(k) * kPi / 3;
                const float bx = cx + std::cos(a) * r * 0.62f, by = cy + std::sin(a) * r * 0.62f;
                for (int s = -1; s <= 1; s += 2) {
                    const float ba = a + static_cast<float>(s) * kPi * 0.78f;
                    p.line(bx, by, bx + std::cos(ba) * r * 0.3f, by + std::sin(ba) * r * 0.3f, c, w);
                }
            }
            break;
        }
        case 1: {   // drift: wavy hill + base
            ID2D1PathGeometry* g = p.path();
            if (g) {
                ID2D1GeometrySink* s = nullptr;
                if (SUCCEEDED(g->Open(&s))) {
                    s->BeginFigure(pt(cx - r, cy + r * 0.45f), D2D1_FIGURE_BEGIN_HOLLOW);
                    s->AddBezier(D2D1::BezierSegment(pt(cx - r * 0.5f, cy - r * 0.6f), pt(cx, cy + r * 0.2f), pt(cx + r * 0.25f, cy - r * 0.15f)));
                    s->AddBezier(D2D1::BezierSegment(pt(cx + r * 0.5f, cy - r * 0.45f), pt(cx + r * 0.85f, cy), pt(cx + r, cy + r * 0.45f)));
                    s->EndFigure(D2D1_FIGURE_END_OPEN);
                    s->Close();
                    s->Release();
                    p.strokeGeometry(g, c, w);
                }
                g->Release();
            }
            p.line(cx - r, cy + r * 0.75f, cx + r, cy + r * 0.75f, c, w);
            break;
        }
        case 2: {   // shovel
            p.line(cx - r * 0.75f, cy - r * 0.75f, cx + r * 0.2f, cy + r * 0.2f, c, w);
            p.line(cx - r * 0.95f, cy - r * 0.55f, cx - r * 0.55f, cy - r * 0.95f, c, w);
            D2D1_POINT_2F blade[4] = {pt(cx + r * 0.05f, cy + r * 0.45f), pt(cx + r * 0.45f, cy + r * 0.05f),
                                      pt(cx + r, cy + r * 0.6f), pt(cx + r * 0.6f, cy + r)};
            ID2D1PathGeometry* g = p.path();
            if (g) {
                ID2D1GeometrySink* s = nullptr;
                if (SUCCEEDED(g->Open(&s))) {
                    s->BeginFigure(blade[0], D2D1_FIGURE_BEGIN_HOLLOW);
                    for (int i = 1; i < 4; ++i) s->AddLine(blade[i]);
                    s->EndFigure(D2D1_FIGURE_END_CLOSED);
                    s->Close();
                    s->Release();
                    p.strokeGeometry(g, c, w);
                }
                g->Release();
            }
            break;
        }
        case 3: {   // gauge
            ID2D1PathGeometry* g = p.path();
            if (g) {
                ID2D1GeometrySink* s = nullptr;
                if (SUCCEEDED(g->Open(&s))) {
                    s->BeginFigure(pt(cx - r, cy + r * 0.35f), D2D1_FIGURE_BEGIN_HOLLOW);
                    s->AddArc(D2D1::ArcSegment(pt(cx + r, cy + r * 0.35f), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                               D2D1_ARC_SIZE_SMALL));
                    s->EndFigure(D2D1_FIGURE_END_OPEN);
                    s->Close();
                    s->Release();
                    p.strokeGeometry(g, c, w);
                }
                g->Release();
            }
            p.line(cx, cy + r * 0.35f, cx + r * 0.5f, cy - r * 0.25f, c, w);
            p.fillEllipse(cx, cy + r * 0.35f, w * 1.2f, w * 1.2f, c);
            break;
        }
        case 4: {   // star
            D2D1_POINT_2F star[10];
            for (int i = 0; i < 10; ++i) {
                const float a = -kPi / 2 + static_cast<float>(i) * kPi / 5;
                const float rr = (i % 2) ? r * 0.42f : r;
                star[i] = pt(cx + std::cos(a) * rr, cy + std::sin(a) * rr * 1.0f + r * 0.08f);
            }
            ID2D1PathGeometry* g = p.path();
            if (g) {
                ID2D1GeometrySink* s = nullptr;
                if (SUCCEEDED(g->Open(&s))) {
                    s->BeginFigure(star[0], D2D1_FIGURE_BEGIN_HOLLOW);
                    for (int i = 1; i < 10; ++i) s->AddLine(star[i]);
                    s->EndFigure(D2D1_FIGURE_END_CLOSED);
                    s->Close();
                    s->Release();
                    p.strokeGeometry(g, c, w);
                }
                g->Release();
            }
            break;
        }
        case 5: {   // gear
            for (int k = 0; k < 8; ++k) {
                const float a = static_cast<float>(k) * kPi / 4;
                p.line(cx + std::cos(a) * r * 0.62f, cy + std::sin(a) * r * 0.62f, cx + std::cos(a) * r, cy + std::sin(a) * r, c, w * 1.4f);
            }
            p.rt()->DrawEllipse(D2D1::Ellipse(pt(cx, cy), r * 0.62f, r * 0.62f), p.brush(c), w);
            p.rt()->DrawEllipse(D2D1::Ellipse(pt(cx, cy), r * 0.22f, r * 0.22f), p.brush(c), w);
            break;
        }
        default: {  // info
            p.rt()->DrawEllipse(D2D1::Ellipse(pt(cx, cy), r * 0.92f, r * 0.92f), p.brush(c), w);
            p.fillEllipse(cx, cy - r * 0.42f, w * 0.8f, w * 0.8f, c);
            p.line(cx, cy - r * 0.08f, cx, cy + r * 0.5f, c, w);
            break;
        }
    }
}

// ---- countdown ----------------------------------------------------------------------------------
void drawCountdownStatic(Painter& p, D2D1_SIZE_F size, const Palette& pal, const std::wstring& title, const std::wstring& big,
                         const std::wstring& caption, bool festive) {
    const float tx = size.width - 52, base = size.height - 16, th = std::min(size.height - 34, 96.0f);
    p.fill(D2D1::RectF(0, 0, size.width, size.height), col(pal.bg));
    const D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f);
    p.vgradient(card, col(pal.card), col(mixColor(pal.card, pal.accent, pal.dark ? 0.10f : 0.06f)), 10);
    p.strokeRound(card, 10, col(pal.cardBorder), 1.0f);
    const D2D1_COLOR_F treeC = col(pal.theme == UiTheme::Postcard ? RGB(46, 94, 60) : pal.dark ? RGB(40, 110, 90) : RGB(46, 120, 96));
    firTree(p, tx, base, th, treeC, col(RGB(255, 255, 255), 0.85f));
    drawNavGlyph(p, 4, tx, base - th - 3, 9, col(RGB(255, 214, 102)));
    TextStyle ts{p.bodySize() * 0.95f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    ts.valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    p.text(title, D2D1::RectF(16, 14, size.width - 96, 34), ts, col(pal.textSecondary));
    TextStyle bs{festive ? p.bodySize() * 2.0f : p.bodySize() * 3.6f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    bs.display = true;
    bs.wrap = festive;
    bs.valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    const float bigTop = 34, bigBottom = size.height - 44;
    p.text(big, D2D1::RectF(14, bigTop, size.width - 96, bigBottom), bs, col(pal.heading));
    TextStyle cs{p.bodySize()};
    cs.wrap = true;
    cs.valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    p.text(caption, D2D1::RectF(16, bigBottom + 2, size.width - 96, size.height - 6), cs, col(pal.text));
}

void drawCountdownDynamic(Compositor& c, float s, float widthDip, float heightDip, float time) {
    const float tx = widthDip - 52, base = heightDip - 16, th = std::min(heightDip - 34, 96.0f);
    for (int i = 0; i < 9; ++i) {
        const float u = 0.25f + 0.08f * static_cast<float>(i);
        const float lx = tx + ((i % 2) ? 1.0f : -1.0f) * th * 0.22f * (u - 0.15f) * 1.4f;
        const float ly = base - th + th * u;
        const bool on = std::sin(time * 2.4f + static_cast<float>(i) * 1.3f) > -0.2f;
        const COLORREF col = kLightColours[i % 5];
        if (on) c.glow(lx * s, ly * s, 5 * s, col, 0.6f);
        c.dot(lx * s, ly * s, 1.6f * s, on ? col : mixColor(col, RGB(0, 0, 0), 0.5f), 1.0f);
    }
    c.glow(tx * s, (base - th - 3) * s, 9 * s, RGB(255, 220, 120), 0.35f + 0.2f * std::sin(time * 3));
}

// ---- fireworks ----------------------------------------------------------------------------------
void FireworksAnim::step(float dt) {
    time += dt;
    for (auto it = bursts.begin(); it != bursts.end();) {
        if (time - it->t > 2.6f) it = bursts.erase(it);
        else ++it;
    }
    if (time >= next) {
        seed = seed * 1103515245u + 12345u;
        const float a = static_cast<float>((seed >> 8) % 1000) / 1000.0f;
        seed = seed * 1103515245u + 12345u;
        const float b = static_cast<float>((seed >> 8) % 1000) / 1000.0f;
        bursts.push_back({0.15f + 0.7f * a, 0.18f + 0.3f * b, time, 0, static_cast<int>((seed >> 4) % 8)});
        next = time + 0.7f + 0.8f * b;
    }
}

void drawFestiveStatic(Painter& p, D2D1_SIZE_F size, const Palette& pal) {
    const D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f);
    const COLORREF top = pal.dark ? pal.skyTop : mixColor(pal.skyTop, RGB(10, 14, 40), 0.45f);
    p.fill(D2D1::RectF(0, 0, size.width, size.height), col(pal.page));
    p.vgradient(card, col(top), col(mixColor(pal.skyBottom, RGB(20, 20, 50), 0.4f)), 10);
    for (int i = 0; i < 40; ++i) {
        const float sx = 8 + std::fmod(static_cast<float>(i) * 131.7f, size.width - 16);
        const float sy = 8 + std::fmod(static_cast<float>(i) * 53.3f, size.height * 0.55f);
        p.fillEllipse(sx, sy, 0.8f, 0.8f, col(RGB(255, 255, 255), 0.25f + 0.12f * static_cast<float>(i % 4)));
    }
    const float mx = size.width - 60, my = 46;
    p.glow(mx, my, 44, col(RGB(255, 240, 210), 0.16f));
    p.fillEllipse(mx, my, 14, 14, col(RGB(255, 246, 225)));
    p.fillEllipse(mx + 6, my - 4, 12.5f, 12.5f, col(top));
    drawGarlandStatic(p, 4, size.width - 4, 6, 7);
    const D2D1_COLOR_F city = col(mixColor(top, RGB(0, 0, 0), 0.45f));
    float x = 1;
    int i = 0;
    while (x < size.width - 2) {
        const float w = 18.0f + static_cast<float>((i * 37) % 23);
        const float h = 20.0f + static_cast<float>((i * 53) % 34);
        p.fill(D2D1::RectF(x, size.height - h, std::min(x + w - 2, size.width - 1.5f), size.height - 1), city);
        for (int wy = 0; wy < 3; ++wy) {
            if (((i + wy) % 3) == 0 && x + 9 < size.width - 2) {
                p.fill(D2D1::RectF(x + 5, size.height - h + 6 + static_cast<float>(wy) * 9, x + 9,
                                   size.height - h + 10 + static_cast<float>(wy) * 9),
                       col(RGB(255, 210, 120), 0.7f));
            }
        }
        x += w;
        ++i;
    }
    p.fillRound(D2D1::RectF(1, size.height - 9, size.width - 1, size.height - 1), 6, col(pal.ground, 0.95f));
    p.strokeRound(card, 10, col(pal.cardBorder), 1.0f);
}

void drawFestiveDynamic(Compositor& c, float s, float widthDip, float heightDip, const FireworksAnim& anim) {
    drawGarlandLights(c, s, 4, widthDip - 4, 6, 7, anim.time);
    c.setClip(static_cast<int>(3 * s), static_cast<int>(22 * s), static_cast<int>((widthDip - 3) * s), static_cast<int>((heightDip - 40) * s));
    for (const auto& b : anim.bursts) {
        const float age = anim.time - b.t;
        const float cx = b.x * widthDip, cy = b.y * heightDip;
        const COLORREF col = kLightColours[b.colour];
        if (age < 0.35f) {
            const float u = age / 0.35f;
            const float y = heightDip - (heightDip - cy) * u;
            c.streak(cx * s, (y + 10) * s, cx * s, y * s, 1.4f * s, RGB(255, 230, 180), 0.8f);
            continue;
        }
        const float e = age - 0.35f;
        const float alpha = std::max(0.0f, 1.0f - e / 2.2f);
        if (e < 0.25f) c.glow(cx * s, cy * s, 30 * s, col, 0.35f * (1 - e / 0.25f));
        for (int k = 0; k < 28; ++k) {
            const float an = static_cast<float>(k) * 2 * kPi / 28;
            const float v = 46.0f * (0.75f + 0.25f * static_cast<float>((k * 7) % 4) / 3.0f);
            const float x = cx + std::cos(an) * v * (1 - std::exp(-2.2f * e));
            const float y = cy + std::sin(an) * v * (1 - std::exp(-2.2f * e)) + 14 * e * e;
            const float e2 = std::max(0.0f, e - 0.12f);
            const float tx = cx + std::cos(an) * v * (1 - std::exp(-2.2f * e2));
            const float ty = cy + std::sin(an) * v * (1 - std::exp(-2.2f * e2)) + 14 * e2 * e2;
            c.streak(tx * s, ty * s, x * s, y * s, 1.2f * s, col, alpha * 0.55f);
            c.dot(x * s, y * s, 1.4f * s, mixColor(col, RGB(255, 255, 255), 0.4f), alpha);
        }
    }
    c.resetClip();
}

}  // namespace snow::ui
