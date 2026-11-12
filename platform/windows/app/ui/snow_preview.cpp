#include "snow_preview.h"

#include <algorithm>
#include <cmath>

#include "simulation/world.h"

namespace snow::ui {
namespace {

constexpr float kStep = 1.0f / 30.0f;
constexpr int kMaxStepsPerTick = 12;
constexpr int kTaskbarH = 18;

RectI winA() { return RectI{28, 84, 208, 196}; }
RectI winB() { return RectI{184, 46, 370, 152}; }

D2D1_RECT_F rf(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

}  // namespace

SnowPreview::SnowPreview() {
    MonitorInfo m;
    m.id = 0;
    m.bounds = RectI{0, 0, kDesktopW, kDesktopH};
    m.work = RectI{0, 0, kDesktopW, kDesktopH - kTaskbarH};
    m.dpiScale = 1.0f;
    m.primary = true;
    layout_.revision = 1;
    layout_.monitors.push_back(m);
    WindowInfo front;   // front to back
    front.id = 1;
    front.bounds = winB();
    front.cornerRadiusPx = 6;
    WindowInfo back;
    back.id = 2;
    back.bounds = winA();
    back.cornerRadiusPx = 6;
    WindowInfo bar;
    bar.id = 3;
    bar.kind = WindowKind::Taskbar;
    bar.bounds = RectI{0, kDesktopH - kTaskbarH, kDesktopW, kDesktopH};
    layout_.windows = {bar, front, back};
}

SnowPreview::~SnowPreview() = default;

void SnowPreview::ensureWorld() {
    if (world_) return;
    world_ = std::make_unique<World>(0x5EED1234ull);
    world_->setLayout(layout_);
    RuntimeTuning tuning;
    tuning.animationsEnabled = settings_.performance.animations;
    world_->applySettings(settings_, tuning);
    world_->setEmissionEnabled(true);
}

void SnowPreview::apply(const Settings& s) {
    settings_ = s;
    // The preview ignores app exclusions / fullscreen pause; everything else is live.
    if (!world_) return;
    RuntimeTuning tuning;
    tuning.animationsEnabled = s.performance.animations;
    world_->applySettings(settings_, tuning);
    snapDirty_ = true;
}

void SnowPreview::reset() {
    world_.reset();
    ensureWorld();
    // Pre-simulate a few minutes (a few dozen ms of CPU) so the miniature already shows the
    // drifts the current settings produce; afterwards it runs time-accelerated.
    advance(kWarmUpSeconds);
}

void SnowPreview::advance(float seconds) {
    ensureWorld();
    const int steps = std::max(0, static_cast<int>(seconds / kStep + 0.5f));
    for (int i = 0; i < steps; ++i) world_->step(kStep);
    snapDirty_ = true;
}

void SnowPreview::tick(float realDt) {
    ensureWorld();
    carry_ += std::min(realDt, 0.25f) * kSpeedUp;
    int guard = 0;
    while (carry_ >= kStep && guard++ < kMaxStepsPerTick) {
        world_->step(kStep);
        carry_ -= kStep;
    }
    if (guard >= kMaxStepsPerTick) carry_ = 0;
    snapDirty_ = true;
}

double SnowPreview::snowMass() const { return world_ ? world_->totalSnowMass() : 0.0; }
size_t SnowPreview::particles() const { return world_ ? world_->particleCount() : 0; }

void SnowPreview::snapshot() {
    ensureWorld();
    if (!snapDirty_) return;
    world_->buildSnapshot(snap_);
    snapDirty_ = false;
}

void SnowPreview::drawBackground(Painter& p, const D2D1_RECT_F& area, const Palette& pal) {
    const float k = (area.right - area.left) / static_cast<float>(kDesktopW);
    auto X = [&](float x) { return area.left + x * k; };
    auto Y = [&](float y) { return area.top + y * k; };
    auto R = [&](const RectI& r) { return rf(X(static_cast<float>(r.l)), Y(static_cast<float>(r.t)), X(static_cast<float>(r.r)), Y(static_cast<float>(r.b))); };
    ID2D1RenderTarget* rt = p.rt();
    rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // Wallpaper + fake windows (back to front) + taskbar.
    p.vgradient(area, col(pal.desktopTop), col(pal.desktopBottom));
    p.glow(X(320), Y(44), 90 * k, col(RGB(255, 255, 255), 0.10f));
    auto fakeWindow = [&](const RectI& r) {
        const D2D1_RECT_F w = R(r);
        p.fillRound(w, 4 * k + 1, col(pal.fakeWindow));
        p.fill(rf(w.left, w.top + 3, w.right, w.top + 14 * k), col(pal.fakeTitle));
        p.fillRound(rf(w.left, w.top, w.right, w.top + 14 * k), 4 * k + 1, col(pal.fakeTitle));
        const D2D1_COLOR_F line = col(mixColor(pal.fakeWindow, pal.fakeTitle, 0.9f));
        for (int i = 0; i < 4; ++i) {
            const float y = w.top + (26.0f + 14.0f * static_cast<float>(i)) * k;
            if (y + 10 * k > w.bottom) break;
            p.fillRound(rf(w.left + 10 * k, y, w.left + (r.width() * (0.75f - 0.12f * static_cast<float>(i % 3))) * k, y + 5 * k), 2 * k, line);
        }
        p.fillEllipse(w.right - 9 * k, w.top + 7 * k, 2.5f * k, 2.5f * k, col(RGB(255, 255, 255), 0.5f));
    };
    fakeWindow(winA());
    fakeWindow(winB());
    const D2D1_RECT_F bar = rf(area.left, Y(static_cast<float>(kDesktopH - kTaskbarH)), area.right, area.bottom);
    p.fill(bar, col(pal.fakeTaskbar));
    for (int i = 0; i < 5; ++i) {
        const float cx = (area.left + area.right) / 2 + (static_cast<float>(i) - 2) * 17 * k;
        p.fillRound(rf(cx - 5 * k, bar.top + 4 * k, cx + 5 * k, bar.bottom - 4 * k), 1.5f * k, col(RGB(255, 255, 255), i == 2 ? 0.45f : 0.22f));
    }

    rt->PopAxisAlignedClip();
}

void SnowPreview::drawDrifts(Painter& p, const D2D1_RECT_F& area) {
    snapshot();
    const float k = (area.right - area.left) / static_cast<float>(kDesktopW);
    auto X = [&](float x) { return area.left + x * k; };
    auto Y = [&](float y) { return area.top + y * k; };
    auto R = [&](const RectI& r) { return rf(X(static_cast<float>(r.l)), Y(static_cast<float>(r.t)), X(static_cast<float>(r.r)), Y(static_cast<float>(r.b))); };
    ID2D1RenderTarget* rt = p.rt();
    rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // Desktop-layer objects (snowmen, snowballs, plow) behind windows.
    for (const SpriteBatch& b : snap_.objects) {
        for (const RectI& c : b.clip) {
            rt->PushAxisAlignedClip(R(c), D2D1_ANTIALIAS_MODE_ALIASED);
            for (const SpriteInstance& s : b.sprites) {
                const float x = X(s.x), y = Y(s.y), hw = std::max(0.8f, s.halfW * k), hh = std::max(0.8f, s.halfH * k);
                switch (s.shape) {
                    case Shape::Snowball:
                        p.fillEllipse(x, y, hw, hh, col(RGB(244, 249, 255), s.opacity));
                        p.fillEllipse(x + hw * 0.25f, y + hh * 0.3f, hw * 0.7f, hh * 0.6f, col(RGB(200, 220, 240), 0.35f * s.opacity));
                        break;
                    case Shape::Coal: p.fillEllipse(x, y, hw, hh, col(RGB(30, 30, 34), s.opacity)); break;
                    case Shape::Carrot: p.fillEllipse(x, y, hw, hh * 0.6f, col(RGB(240, 130, 40), s.opacity)); break;
                    case Shape::Hat: p.fill(rf(x - hw, y - hh, x + hw, y + hh), col(RGB(40, 40, 46), s.opacity)); break;
                    case Shape::Scarf: p.fill(rf(x - hw, y - hh * 0.5f, x + hw, y + hh * 0.5f), col(RGB(200, 50, 50), s.opacity)); break;
                    case Shape::Snowplow: p.fillRound(rf(x - hw, y - hh, x + hw, y + hh), 1.5f, col(RGB(240, 170, 40), s.opacity)); break;
                    case Shape::Stick: p.line(x - hw, y, x + hw, y, col(RGB(110, 70, 40), s.opacity), std::max(1.0f, hh)); break;
                    default: p.fillEllipse(x, y, hw, hh, col(RGB(244, 249, 255), s.opacity * 0.8f)); break;
                }
            }
            rt->PopAxisAlignedClip();
        }
    }

    // Drifts: smooth white profiles, clipped to their visible parts.
    for (const DriftStrip& d : snap_.drifts) {
        if (d.heights.size() < 2 || d.clip.empty()) continue;
        ID2D1PathGeometry* g = p.path();
        if (!g) continue;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(g->Open(&sink))) {
            const size_t n = d.heights.size();
            sink->BeginFigure(D2D1::Point2F(X(d.x0), Y(d.baseY) + 0.5f), D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 0; i < n; ++i) {
                const float x = d.x0 + static_cast<float>(i) * d.step;
                sink->AddLine(D2D1::Point2F(X(x), Y(d.baseY - d.heights[i])));
            }
            sink->AddLine(D2D1::Point2F(X(d.x0 + static_cast<float>(n - 1) * d.step), Y(d.baseY) + 0.5f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            for (const RectI& c : d.clip) {
                rt->PushAxisAlignedClip(R(RectI{c.l, c.t - 200, c.r, c.b}), D2D1_ANTIALIAS_MODE_ALIASED);
                p.fillGeometry(g, col(RGB(250, 253, 255), d.opacity));
                p.strokeGeometry(g, col(RGB(190, 215, 240), 0.55f * d.opacity), 0.8f);
                rt->PopAxisAlignedClip();
            }
        }
        g->Release();
    }

    rt->PopAxisAlignedClip();
}

std::uint64_t SnowPreview::driftKey() {
    snapshot();
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (const DriftStrip& d : snap_.drifts) {
        mix(d.id);
        mix(d.version);
        mix(static_cast<std::uint64_t>(d.opacity * 1000));
        for (const RectI& c : d.clip) mix(static_cast<std::uint64_t>((c.l * 73856093) ^ (c.r * 19349663) ^ (c.t * 83492791)));
    }
    for (const SpriteBatch& b : snap_.objects)
        for (const SpriteInstance& o : b.sprites) mix(static_cast<std::uint64_t>(static_cast<int>(o.x * 4)) * 31 + static_cast<std::uint64_t>(static_cast<int>(o.y * 4)));
    return h;
}

void SnowPreview::drawParticles(Compositor& c, float left, float top, float k) {
    snapshot();
    for (const SpriteInstance& s : snap_.particles.sprites) {
        const float x = left + s.x * k, y = top + s.y * k;
        const float r = std::max(0.6f, s.halfW * k);
        switch (s.shape) {
            case Shape::Flake: c.dot(x, y, r, RGB(255, 255, 255), s.opacity); break;
            case Shape::SnowChunk: c.ellipse(x, y, std::max(0.8f, s.halfW * k), std::max(0.8f, s.halfH * k), RGB(246, 250, 255), s.opacity); break;
            case Shape::Droplet: c.ellipse(x, y, std::max(0.6f, r * 0.7f), std::max(0.9f, s.halfH * k), RGB(170, 210, 245), s.opacity); break;
            case Shape::Icicle: c.rect(x - r * 0.5f, y - s.halfH * k, x + r * 0.5f, y + s.halfH * k, RGB(215, 238, 255), 0.8f * s.opacity); break;
            case Shape::Spark: c.dot(x, y, r, RGB(255, 220, 140), s.opacity); break;
            default: c.dot(x, y, r, RGB(255, 255, 255), s.opacity * 0.9f); break;
        }
    }
}

}  // namespace snow::ui
