// render_demo: standalone exerciser for snow::OverlaySystem.
//
//   render_demo [--seconds N] [--flakes N]   synthetic scene on all monitors, exits after N s (default 8)
//   render_demo --selftest                   automatic checks -> render_selftest.txt, exit code != 0 on failure
//   render_demo --gallery                    draws every sprite shape over an OWN test window and saves
//                                            that window's screen area to render_gallery.png
//   render_demo --gallery2                   round-6 shapes (hats, scarves, garland, fireworks, icicles,
//                                            splats, droplets, wet drifts) -> render_gallery2.png
//   render_demo --gallery3                   every snowman skin + all 20 accessories -> render_gallery3.png
//   render_demo --sparks N                   keep about N firework sparks alive (load test)
//
// Privacy rule: the only screen pixels ever read are those inside this program's own opaque
// test window (magenta in the self-test, dark/light panels in the gallery).
#include <windows.h>

#include <objidl.h>
#include <shellscalingapi.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#pragma warning(push, 0)
#include <gdiplus.h>
#pragma warning(pop)

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/skins.h"
#include "diagnostics/log.h"
#include "rendering/d3d11/overlay_system.h"
#include "rendering/d3d11/overlay_test_hooks.h"

#pragma comment(lib, "gdiplus.lib")

using namespace snow;

namespace {

const UINT kFailedMsg = WM_APP + 7;
const float kPi = 3.14159265f;

std::atomic<bool> g_failedMsgSeen{false};

// ---------------------------------------------------------------------------------------
// Small Win32 helpers
// ---------------------------------------------------------------------------------------
void pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount64() >= end) break;
        Sleep(5);
    }
}

std::wstring exeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L'\\') + 1);
}

BOOL CALLBACK monitorEnum(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    MonitorInfo m;
    m.id = static_cast<int>(out->size());
    m.bounds = RectI{mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
    m.work = RectI{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom};
    m.dpiScale = static_cast<float>(dx) / 96.0f;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    out->push_back(m);
    return TRUE;
}

std::vector<MonitorInfo> enumMonitors() {
    std::vector<MonitorInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, monitorEnum, reinterpret_cast<LPARAM>(&v));
    return v;
}

COLORREF g_bgColor = RGB(255, 0, 255);
bool g_bgSplit = false;   // gallery: left half dark, right half light
struct Label {
    RECT rc;                  // client coordinates of the own background window
    std::wstring text;
    COLORREF color;
};
std::vector<Label> g_labels;

LRESULT CALLBACK bgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        if (g_bgSplit) {
            RECT a = rc, b = rc;
            a.right = b.left = rc.right / 2;
            HBRUSH dark = CreateSolidBrush(RGB(24, 30, 48));
            HBRUSH light = CreateSolidBrush(RGB(250, 250, 250));
            FillRect(reinterpret_cast<HDC>(w), &a, dark);
            FillRect(reinterpret_cast<HDC>(w), &b, light);
            DeleteObject(dark);
            DeleteObject(light);
            if (!g_labels.empty()) {
                HDC dc = reinterpret_cast<HDC>(w);
                HFONT font = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                HGDIOBJ old = SelectObject(dc, font);
                SetBkMode(dc, TRANSPARENT);
                for (Label& lb : g_labels) {
                    SetTextColor(dc, lb.color);
                    DrawTextW(dc, lb.text.c_str(), -1, &lb.rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
                SelectObject(dc, old);
                DeleteObject(font);
            }
        } else {
            HBRUSH br = CreateSolidBrush(g_bgColor);
            FillRect(reinterpret_cast<HDC>(w), &rc, br);
            DeleteObject(br);
        }
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK notifyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kFailedMsg) {
        g_failedMsgSeen.store(true);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND createBgWindow(HINSTANCE inst, const RectI& r) {
    HWND bg = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SnowDemoBg", L"", WS_POPUP | WS_VISIBLE, r.l,
                              r.t, r.width(), r.height(), nullptr, nullptr, inst, nullptr);
    SetWindowPos(bg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return bg;
}

// ---------------------------------------------------------------------------------------
// Synthetic scene
// ---------------------------------------------------------------------------------------
struct Flake {
    float x, y, vx, vy, r, rot, rotSpeed, phase, opacity;
    std::uint8_t variant;
};

struct Scene {
    std::vector<MonitorInfo> monitors;
    MonitorInfo primary;
    RectI desktop;              // union of all monitors
    bool selftest = false;
    // selftest: 0 = off, 1 = dense flakes over the test window clipped to its left half,
    // 2 = same flakes with the single-empty-rect clip (everything covered -> nothing drawn)
    std::atomic<int> particleClipTest{0};
    bool gallery = false;
    int galleryPage = 1;
    int sparkTarget = 0;           // plain demo: keep about this many sparks alive (0 = occasional bursts)

    struct Spark {
        float x, y, vx, vy, life, maxLife, r;
        int color;
    };
    std::vector<Spark> sparks;
    double nextBurst = 1.0;
    RectI testRect;             // own background window (selftest / gallery)
    int flakeCount = 3000;

    std::vector<Flake> flakes;
    std::vector<float> groundHeights;   // drift 1 profile (plow rides on it)
    struct Chunk {
        float x, y, vx, vy, rot, spin, hw, hh;
        int variant;
    };
    std::vector<Chunk> chunks;
    double nextChunk = 0.5;
    std::mt19937 rng{20261112u};
    double time = 0;
    std::uint64_t frame = 0;

    float rnd(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

    void init() {
        desktop = monitors.front().bounds;
        primary = monitors.front();
        for (const MonitorInfo& m : monitors) {
            desktop.l = std::min(desktop.l, m.bounds.l);
            desktop.t = std::min(desktop.t, m.bounds.t);
            desktop.r = std::max(desktop.r, m.bounds.r);
            desktop.b = std::max(desktop.b, m.bounds.b);
            if (m.primary) primary = m;
        }
        flakes.resize(static_cast<std::size_t>(gallery ? 0 : flakeCount));
        for (Flake& f : flakes) {
            respawn(f);
            f.y = rnd(static_cast<float>(desktop.t), static_cast<float>(desktop.b));
        }
    }

    void respawn(Flake& f) {
        const float s = primary.dpiScale;
        const float kind = rnd(0, 1);
        if (kind < 0.70f) {
            f.r = rnd(1.2f, 3.0f) * s;
            f.variant = 0;
        } else if (kind < 0.92f) {
            f.r = rnd(4.0f, 8.0f) * s;
            f.variant = static_cast<std::uint8_t>(1 + static_cast<int>(rnd(0, 2.999f)));
        } else {
            f.r = rnd(8.0f, 14.0f) * s;
            f.variant = static_cast<std::uint8_t>(1 + static_cast<int>(rnd(0, 2.999f)));
        }
        f.x = rnd(static_cast<float>(desktop.l) - 50.0f, static_cast<float>(desktop.r) + 50.0f);
        f.y = static_cast<float>(desktop.t) - 20.0f;
        f.vy = (30.0f + f.r * 9.0f) * rnd(0.8f, 1.2f);
        f.vx = 12.0f;
        f.rot = rnd(0, 2 * kPi);
        f.rotSpeed = rnd(-1.2f, 1.2f);
        f.phase = rnd(0, 2 * kPi);
        f.opacity = rnd(0.6f, 1.0f);
    }

    static SpriteInstance sprite(Shape shape, float x, float y, float hw, float hh, float rot = 0, int variant = 0,
                                 float opacity = 1) {
        SpriteInstance s;
        s.x = x;
        s.y = y;
        s.halfW = hw;
        s.halfH = hh;
        s.rotation = rot;
        s.opacity = opacity;
        s.shape = shape;
        s.variant = static_cast<std::uint8_t>(variant);
        return s;
    }

    // Snowman with a hat and a scarf. Returns nothing; appends to `b`.
    static void snowmanDressed(SpriteBatch& b, float cx, float groundY, float k, int hatV, int scarfV) {
        snowman(b, cx, groundY, k);
        const float r1 = 40 * k, r2 = 30 * k, r3 = 22 * k;
        const float y1 = groundY - r1 * 0.9f, y2 = y1 - r1 - r2 * 0.75f, y3 = y2 - r2 - r3 * 0.75f;
        const float neck = y3 + r3 * 0.85f;
        const float sw = r3 * 1.25f, sh = r3 * 1.1f;
        b.sprites.push_back(sprite(Shape::Scarf, cx + sw * 0.04f, neck + 0.62f * sh, sw, sh, 0, scarfV));
        const float hw = r3 / 1.1f;
        const float hhTable[4] = {0.95f, 0.80f, 0.75f, 0.95f};
        const float sinkTable[4] = {0.30f, 0.30f, 0.50f, 0.42f};
        const float hh = hw * hhTable[hatV & 3];
        const float headTop = y3 - r3;
        b.sprites.push_back(sprite(Shape::Hat, cx, headTop + r3 * sinkTable[hatV & 3] - hh, hw, hh, 0, hatV));
    }

    static void snowman(SpriteBatch& b, float cx, float groundY, float k) { snowmanBase(b, cx, groundY, k, false, true); }

    // Head geometry of a snowman built by snowmanBase().
    struct Head {
        float cx, hy, r, k, y2, r2;
    };
    static Head headOf(float cx, float groundY, float k) {
        const float r1 = 40 * k, r2 = 30 * k, r3 = 22 * k;
        const float y1 = groundY - r1 * 0.9f, y2 = y1 - r1 - r2 * 0.75f, y3 = y2 - r2 - r3 * 0.75f;
        return Head{cx, y3, r3, k, y2, r2};
    }

    // Places one accessory the way the gallery recommends (sprite = art bounding box).
    static void placeAccessory(SpriteBatch& b, const Head& h, Accessory a) {
        const float k = h.k, cx = h.cx, hy = h.hy, r = h.r;
        const float headTop = hy - r, neck = hy + 0.85f * r;
        // GooglyEye: rotation = pupil direction (pi/2 = looking down), the ball itself stays upright.
        const float rot = a == Accessory::GooglyEye ? kPi * 0.5f : 0.0f;
        auto put = [&](float x, float y, float hw, float hh) {
            b.sprites.push_back(sprite(Shape::Accessory, x, y, hw, hh, rot, static_cast<int>(a)));
        };
        auto hatSlot = [&](float hw, float hh, float sink) { put(cx, headTop + r * sink - hh, hw, hh); };
        switch (a) {
            case Accessory::Sunglasses:
            case Accessory::RoundGlasses: put(cx, hy - 6 * k, 16 * k, 6.2f * k); break;
            case Accessory::EyePatch: put(cx + 8 * k, hy - 6 * k, 20 * k, 8.5f * k); break;
            case Accessory::Headphones:
            case Accessory::Earmuffs: put(cx, hy - 2 * k, 27 * k, 22.5f * k); break;
            case Accessory::BowTie: put(cx, neck + 3 * k, 11 * k, 6 * k); break;
            case Accessory::Crown: hatSlot(18 * k, 13 * k, 0.25f); break;
            case Accessory::TricornHat: hatSlot(30 * k, 15 * k, 0.35f); break;
            case Accessory::CowboyHat: hatSlot(32 * k, 17 * k, 0.35f); break;
            case Accessory::ChefHat: hatSlot(16 * k, 18 * k, 0.30f); break;
            case Accessory::CatEars: hatSlot(24 * k, 12 * k, 0.45f); break;
            case Accessory::Whiskers: put(cx + 2 * k, hy + 2 * k, 26 * k, 8 * k); break;
            case Accessory::Mustache: put(cx + 3 * k, hy + 8.5f * k, 14 * k, 6 * k); break;
            case Accessory::Beard: put(cx, hy + 12 * k, 22 * k, 20 * k); break;
            case Accessory::Pipe: put(cx + 17 * k, hy + 11 * k, 13 * k, 8 * k); break;
            case Accessory::GooglyEye:
                put(cx - 8 * k, hy - 6 * k, 5.5f * k, 5.5f * k);
                put(cx + 8 * k, hy - 6 * k, 5.5f * k, 5.5f * k);
                break;
            case Accessory::Flower: put(cx - 0.75f * r, headTop + 0.15f * r, 8 * k, 8 * k); break;
            case Accessory::Broom: {
                const float hx = cx + h.r2 + 20 * k + 26 * k * std::cos(-0.45f);
                const float hyy = h.y2 - 8 * k + 26 * k * std::sin(-0.45f);
                put(hx, hyy + 0.14f * 30 * k, 8 * k, 30 * k);
                break;
            }
            case Accessory::Bandana: put(cx, neck + 8.2f * k, 17 * k, 12 * k); break;
            case Accessory::Monocle: put(cx + 8 * k, hy - 6 * k, 9.6f * k, 12 * k); break;
            default: break;
        }
    }

    // Draw layer of a skin part: lower draws first.
    static int partLayer(const SkinPart& sp) {
        if (sp.kind == SkinPart::Kind::Scarf) return 0;
        if (sp.kind == SkinPart::Kind::Hat) return 5;
        switch (static_cast<Accessory>(sp.variant)) {
            case Accessory::BowTie: case Accessory::Bandana: return 1;
            case Accessory::Beard: case Accessory::Mustache: case Accessory::Whiskers: case Accessory::Pipe: return 2;
            case Accessory::Sunglasses: case Accessory::RoundGlasses: case Accessory::EyePatch: case Accessory::Monocle: return 3;
            case Accessory::Headphones: case Accessory::Earmuffs: return 4;
            case Accessory::Crown: case Accessory::TricornHat: case Accessory::CowboyHat: case Accessory::ChefHat:
            case Accessory::CatEars: return 5;
            default: return 6;
        }
    }

    static void addHat(SpriteBatch& b, const Head& h, int hatV) {
        const float hw = h.r / 1.1f;
        const float hhTable[4] = {0.95f, 0.80f, 0.75f, 0.95f};
        const float sinkTable[4] = {0.30f, 0.30f, 0.50f, 0.42f};
        const float hh = hw * hhTable[hatV & 3];
        b.sprites.push_back(sprite(Shape::Hat, h.cx, h.hy - h.r + h.r * sinkTable[hatV & 3] - hh, hw, hh, 0, hatV));
    }
    static void addScarf(SpriteBatch& b, const Head& h, int scarfV) {
        const float neck = h.hy + h.r * 0.85f;
        const float sw = h.r * 1.25f, sh = h.r * 1.1f;
        b.sprites.push_back(sprite(Shape::Scarf, h.cx + sw * 0.04f, neck + 0.62f * sh, sw, sh, 0, scarfV));
    }

    // Full snowman dressed in one of the named skins.
    static void snowmanSkin(SpriteBatch& b, float cx, float groundY, float k, const SnowmanSkin& skin) {
        snowmanBase(b, cx, groundY, k, skin.googlyEyes, false);
        const Head h = headOf(cx, groundY, k);
        for (int layer = 0; layer <= 6; ++layer) {
            if (layer == 3) {   // the carrot nose goes over beards / moustaches, under glasses
                b.sprites.push_back(sprite(Shape::Carrot, cx + 11 * k, h.hy + 3 * k, 11 * k, 3.6f * k, 0.12f));
            }
            for (const SkinPart& sp : skin.parts) {
                if (sp.kind == SkinPart::Kind::Accessory && sp.variant == kNoPart) continue;
                if (partLayer(sp) != layer) continue;
                if (sp.kind == SkinPart::Kind::Hat) addHat(b, h, sp.variant);
                else if (sp.kind == SkinPart::Kind::Scarf) addScarf(b, h, sp.variant);
                else placeAccessory(b, h, static_cast<Accessory>(sp.variant));
            }
        }
    }

    static void snowmanBase(SpriteBatch& b, float cx, float groundY, float k, bool googly, bool carrot) {
        const float r1 = 40 * k, r2 = 30 * k, r3 = 22 * k;
        const float y1 = groundY - r1 * 0.9f, y2 = y1 - r1 - r2 * 0.75f, y3 = y2 - r2 - r3 * 0.75f;
        b.sprites.push_back(sprite(Shape::Stick, cx - r2 - 20 * k, y2 - 8 * k, 26 * k, 9 * k, kPi + 0.45f));
        b.sprites.push_back(sprite(Shape::Stick, cx + r2 + 20 * k, y2 - 8 * k, 26 * k, 9 * k, -0.45f));
        b.sprites.push_back(sprite(Shape::Snowball, cx, y1, r1, r1));
        b.sprites.push_back(sprite(Shape::Snowball, cx, y2, r2, r2));
        b.sprites.push_back(sprite(Shape::Snowball, cx, y3, r3, r3));
        for (int i = 0; i < 3; ++i) {
            b.sprites.push_back(sprite(Shape::Coal, cx, y2 - 14 * k + static_cast<float>(i) * 13 * k, 3.5f * k, 3.5f * k));
        }
        if (googly) {
            placeAccessory(b, Head{cx, y3, r3, k, y2, r2}, Accessory::GooglyEye);
        } else {
            b.sprites.push_back(sprite(Shape::Coal, cx - 8 * k, y3 - 6 * k, 3 * k, 3 * k));
            b.sprites.push_back(sprite(Shape::Coal, cx + 8 * k, y3 - 6 * k, 3 * k, 3 * k));
        }
        if (carrot) b.sprites.push_back(sprite(Shape::Carrot, cx + 11 * k, y3 + 3 * k, 11 * k, 3.6f * k, 0.12f));
    }

    std::shared_ptr<FrameSnapshot> step(float dt) {
        time += dt;
        ++frame;
        auto snap = std::make_shared<FrameSnapshot>();
        snap->frame = frame;
        const float T = static_cast<float>(time);
        const float s = primary.dpiScale;
        const float W = static_cast<float>(primary.bounds.width());
        const float H = static_cast<float>(primary.bounds.height());
        const float L = static_cast<float>(primary.bounds.l);
        const float Tp = static_cast<float>(primary.bounds.t);

        if (gallery) {
            if (galleryPage == 3) buildGallery3(*snap);
            else if (galleryPage == 2) buildGallery2(*snap);
            else buildGallery(*snap);
            snap->taken = std::chrono::steady_clock::now();
            return snap;
        }

        // ---- falling flakes --------------------------------------------------------
        RectI keepOut = testRect;
        keepOut.l -= 60;
        keepOut.t -= 60;
        keepOut.r += 60;
        keepOut.b += 60;
        snap->particles.sprites.reserve(flakes.size());
        for (Flake& f : flakes) {
            const float sway = std::sin(T * 1.3f + f.phase) * 18.0f;
            f.vx = 12.0f + sway;
            f.x += f.vx * dt;
            f.y += f.vy * dt;
            f.rot += f.rotSpeed * dt;
            if (f.y > static_cast<float>(desktop.b) + 20.0f) respawn(f);
            if (selftest && keepOut.contains(static_cast<int>(f.x), static_cast<int>(f.y))) continue;
            SpriteInstance sp = sprite(Shape::Flake, f.x, f.y, f.r, f.r, f.rot, f.variant, f.opacity);
            sp.vx = f.vx;
            sp.vy = f.vy;
            snap->particles.sprites.push_back(sp);
        }

        if (selftest && particleClipTest.load() != 0) {
            for (int gy = testRect.t + 4; gy < testRect.b; gy += 6) {
                for (int gx = testRect.l + 4; gx < testRect.r; gx += 6) {
                    snap->particles.sprites.push_back(
                        sprite(Shape::Flake, static_cast<float>(gx), static_cast<float>(gy), 5, 5, 0, 0));
                }
            }
            const int mid = (testRect.l + testRect.r) / 2;
            snap->particles.clip.push_back(particleClipTest.load() == 1 ? RectI{testRect.l, testRect.t, mid, testRect.b}
                                                                        : RectI{});
        }

        // ---- drifts ----------------------------------------------------------------
        // 1: along the bottom of the primary work area, slowly growing (version bumps).
        const float groundY = static_cast<float>(primary.work.b);
        {
            DriftStrip d;
            d.id = 1;
            d.step = 3;
            d.dpiScale = s;
            d.x0 = L;
            d.baseY = groundY;
            const std::uint32_t growth = static_cast<std::uint32_t>(frame / 10);
            d.version = growth;
            const int n = static_cast<int>(W / d.step) + 2;
            d.heights.resize(static_cast<std::size_t>(n));
            const float grow = std::min(1.0f, 0.55f + static_cast<float>(growth) * 0.01f);
            for (int i = 0; i < n; ++i) {
                const float x = static_cast<float>(i) * d.step / s;
                float h = 20 + 9 * std::sin(x * 0.021f) + 6 * std::sin(x * 0.057f + 1.3f) + 3 * std::sin(x * 0.13f + 0.4f);
                // irregular per-column caps, like the simulation's lumpy tops
                const std::uint32_t hsh = static_cast<std::uint32_t>(i) * 2654435761u;
                h += static_cast<float>((hsh >> 24) & 7u) * 0.9f - 3.0f;
                d.heights[static_cast<std::size_t>(i)] = std::max(0.0f, h) * grow * s;
            }
            d.clip.push_back(primary.bounds);
            groundHeights = d.heights;
            snap->drifts.push_back(std::move(d));
        }
        // 2: a "window top edge" with a hole cut out of the middle by its clip rects (static).
        {
            DriftStrip d;
            d.id = 2;
            d.step = 3;
            d.dpiScale = s;
            d.x0 = L + W * 0.50f;
            d.baseY = Tp + H * 0.55f;
            d.version = 1;
            const int n = static_cast<int>(W * 0.35f / d.step);
            d.heights.resize(static_cast<std::size_t>(std::max(n, 2)));
            for (int i = 0; i < n; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(n - 1);
                const float edge = std::min(1.0f, std::min(u, 1 - u) * 12.0f);
                d.heights[static_cast<std::size_t>(i)] =
                    (14 + 8 * std::sin(u * 17.0f) + 5 * std::sin(u * 41.0f + 2.0f)) * edge * s;
            }
            const int x0 = static_cast<int>(d.x0), x1 = x0 + static_cast<int>(W * 0.35f);
            const int y0 = static_cast<int>(d.baseY) - 200, y1 = static_cast<int>(d.baseY) + 20;
            const int holeL = x0 + (x1 - x0) * 4 / 10, holeR = x0 + (x1 - x0) * 6 / 10;
            d.clip.push_back(RectI{x0, y0, holeL, y1});
            d.clip.push_back(RectI{holeR, y0, x1, y1});
            if (!selftest) {
                // icicles growing under this edge (same occlusion, extended downwards)
                SpriteBatch ice;
                ice.clip.push_back(RectI{x0, y0, holeL, y1 + static_cast<int>(70 * s)});
                ice.clip.push_back(RectI{holeR, y0, x1, y1 + static_cast<int>(70 * s)});
                const float grow = std::fmod(T, 30.0f);
                for (int k = 0; k < 16; ++k) {
                    const std::uint32_t hsh = static_cast<std::uint32_t>(k + 1) * 2654435761u;
                    const float maxLen = (8.0f + static_cast<float>((hsh >> 20) & 31u)) * s;
                    const float len = std::min(maxLen, (2.0f + grow * 1.6f) * s);
                    const float hw = (1.6f + static_cast<float>((hsh >> 12) & 3u)) * s * std::min(1.0f, len / (10 * s) + 0.4f);
                    const float ix = d.x0 + (static_cast<float>(k) + 0.5f) * static_cast<float>(x1 - x0) / 16.0f;
                    ice.sprites.push_back(sprite(Shape::Icicle, ix, d.baseY + 1 + len * 0.5f, hw, len * 0.5f, 0,
                                                 static_cast<int>(hsh >> 28) & 3));
                    if (len >= maxLen && (k % 3) == 0) {   // a drop forming at the tip
                        const float ph = std::fmod(T * 0.7f + static_cast<float>(k), 1.0f);
                        ice.sprites.push_back(sprite(Shape::Droplet, ix, d.baseY + 1 + len + 3.5f * s * ph,
                                                     2.2f * s * (0.6f + 0.4f * ph), 3.5f * s * (0.5f + 0.5f * ph), 0, 1));
                    }
                }
                snap->objects.push_back(std::move(ice));
            }
            snap->drifts.push_back(std::move(d));
        }
        // 3: a small strip whose profile changes every frame, with bare (zero) stretches.
        {
            DriftStrip d;
            d.id = 3;
            d.step = 2;
            d.dpiScale = s;
            d.x0 = L + W * 0.12f;
            d.baseY = Tp + H * 0.72f;
            d.version = static_cast<std::uint32_t>(frame);
            d.opacity = 0.5f;   // half-transparent drift
            d.heights.resize(120);
            for (int i = 0; i < 120; ++i) {
                const float u = static_cast<float>(i) / 119.0f;
                const float h = 22 * std::sin(u * 9.0f + T * 1.5f) * std::sin(u * kPi);
                d.heights[static_cast<std::size_t>(i)] = std::max(0.0f, h) * s;
            }
            d.clip.push_back(primary.bounds);
            snap->drifts.push_back(std::move(d));
        }
        // selftest: a tall flat block of snow over the own magenta window.
        if (selftest) {
            DriftStrip d;
            d.id = 99;
            d.step = 3;
            d.dpiScale = s;
            d.x0 = static_cast<float>(testRect.l + 100);
            d.baseY = static_cast<float>(testRect.t + 280);
            d.version = 1;
            d.heights.assign(68, 110.0f);
            d.clip.push_back(testRect);
            snap->drifts.push_back(std::move(d));
            DriftStrip half = snap->drifts.back();
            half.id = 98;
            half.x0 = static_cast<float>(testRect.l + 320);
            half.heights.assign(24, 110.0f);
            half.opacity = 0.5f;
            snap->drifts.push_back(std::move(half));

            // a big snowball seen only through a 10x10 grid of 100 small clip rects
            SpriteBatch grid;
            for (int gy = 0; gy < 10; ++gy) {
                for (int gx = 0; gx < 10; ++gx) {
                    const int cx = testRect.l + 90 + gx * 12, cy = testRect.t + 20 + gy * 12;
                    grid.clip.push_back(RectI{cx, cy, cx + 8, cy + 8});
                }
            }
            grid.sprites.push_back(sprite(Shape::Snowball, static_cast<float>(testRect.l + 150),
                                          static_cast<float>(testRect.t + 80), 70, 70));
            snap->objects.push_back(std::move(grid));
        }

        // ---- fireworks ----------------------------------------------------------------
        if (!selftest) {
            const bool wantBurst = sparkTarget > 0 ? static_cast<int>(sparks.size()) + 150 <= sparkTarget
                                                   : time >= nextBurst;
            if (wantBurst) {
                nextBurst = time + rnd(1.8f, 3.2f);
                const float bx = L + W * rnd(0.15f, 0.85f), by = Tp + H * rnd(0.12f, 0.35f);
                const int burstCol = static_cast<int>(rnd(0, 7.999f));
                for (int k = 0; k < 150; ++k) {
                    const float a = rnd(0, 2 * kPi), v = rnd(60.0f, 320.0f) * s;
                    Spark sp{bx, by, std::cos(a) * v, std::sin(a) * v, 0, rnd(1.1f, 1.8f), rnd(1.3f, 2.1f) * s,
                             rnd(0, 1) < 0.25f ? 0 : burstCol};
                    sp.life = sp.maxLife;
                    sparks.push_back(sp);
                }
            }
            for (Spark& sp : sparks) {
                sp.vy += 110.0f * s * dt;
                const float drag = 1.0f - 0.9f * dt;
                sp.vx *= drag;
                sp.vy *= drag;
                sp.x += sp.vx * dt;
                sp.y += sp.vy * dt;
                sp.life -= dt;
            }
            std::erase_if(sparks, [](const Spark& sp) { return sp.life <= 0; });
            for (const Spark& sp : sparks) {
                const float f = sp.life / sp.maxLife;
                SpriteInstance si = sprite(Shape::Spark, sp.x, sp.y, sp.r * (0.6f + 0.4f * f), sp.r, 0, sp.color,
                                           std::min(1.0f, f * 1.6f));
                si.vx = sp.vx;
                si.vy = sp.vy;
                snap->particles.sprites.push_back(si);
            }
        }

        // ---- blinking garland across the top of the primary monitor -------------------
        if (!selftest) {
            const float gy = Tp + 46 * s, span = 80 * s, sag = 6 * s;
            const int n = static_cast<int>(W / span);
            for (int k = 0; k < n; ++k) {
                const float xa = L + static_cast<float>(k) * span + span * 0.5f;
                snap->particles.sprites.push_back(sprite(Shape::GarlandWire, xa + span * 0.5f, gy, span * 0.5f, sag));
                const bool on = ((static_cast<int>(T * 2.0f) + k) % 3) != 0;
                snap->particles.sprites.push_back(sprite(Shape::Bulb, xa + span * 0.5f, gy + sag + 7 * s, 4 * s, 7 * s,
                                                         0.08f * std::sin(T * 1.3f + static_cast<float>(k)),
                                                         (k % 8) + (on ? 8 : 0)));
            }
        }

        // ---- chunks breaking off the "window" drift (strip 2) and tumbling down -------
        if (!selftest) {
            if (time >= nextChunk) {
                nextChunk = time + 0.35;
                Chunk c{};
                c.x = L + W * 0.50f + rnd(0.0f, W * 0.35f);
                c.y = Tp + H * 0.55f - 10 * s;
                c.vx = rnd(-60.0f, 60.0f);
                c.vy = rnd(-40.0f, 10.0f);
                c.rot = 0;
                c.spin = rnd(-3.0f, 3.0f);
                const float kind = rnd(0, 1);
                c.hw = (kind < 0.3f ? rnd(40.0f, 120.0f) : (kind < 0.7f ? rnd(10.0f, 30.0f) : rnd(4.0f, 8.0f))) * s;
                c.hh = std::min(c.hw * 0.8f, rnd(4.0f, 14.0f)) * s;
                c.variant = static_cast<int>(rnd(0, 3.999f));
                chunks.push_back(c);
            }
            for (Chunk& c : chunks) {
                c.vy += 900.0f * dt;
                c.x += c.vx * dt;
                c.y += c.vy * dt;
                c.rot += c.spin * dt;
            }
            std::erase_if(chunks, [&](const Chunk& c) { return c.y > static_cast<float>(desktop.b) + 200.0f; });
            for (const Chunk& c : chunks) {
                SpriteInstance sp = sprite(Shape::SnowChunk, c.x, c.y, c.hw, c.hh, c.rot, c.variant);
                sp.vx = c.vx;
                sp.vy = c.vy;
                snap->particles.sprites.push_back(sp);
            }
        }

        // ---- objects ---------------------------------------------------------------
        {
            SpriteBatch b;   // unclipped
            const int outfit = static_cast<int>(T / 4.0f);
            snowmanDressed(b, L + W * 0.30f, groundY - 14 * s, s, outfit & 3, (outfit / 4) & 3);
            snap->objects.push_back(std::move(b));
            SpriteBatch roll;   // clipped to the work area: disappears behind the "taskbar"
            roll.clip.push_back(primary.work);
            const float rx = L + W * 0.62f + std::sin(T * 0.8f) * W * 0.12f;
            SpriteInstance ball = sprite(Shape::Snowball, rx, groundY - 30 * s + std::fabs(std::sin(T * 2.4f)) * -40 * s,
                                         18 * s, 18 * s);
            ball.vx = std::cos(T * 0.8f) * 0.8f * W * 0.12f;
            roll.sprites.push_back(ball);
            snap->objects.push_back(std::move(roll));

            // toy snowplow driving back and forth on top of the ground drift
            SpriteBatch plowBatch;
            plowBatch.clip.push_back(primary.bounds);
            const float px = L + W * (0.5f + 0.42f * std::sin(T * 0.22f));
            const float pvx = W * 0.42f * 0.22f * std::cos(T * 0.22f);
            float gh = 0;
            if (!groundHeights.empty()) {
                const std::size_t gi = std::min(groundHeights.size() - 1,
                                                 static_cast<std::size_t>(std::max(0.0f, (px - L) / 3.0f)));
                gh = groundHeights[gi];
            }
            SpriteInstance plow = sprite(Shape::Snowplow, px, groundY - gh * 0.8f - 30 * s, 46 * s, 30 * s, 0,
                                         pvx >= 0 ? 0 : 1);
            plow.vx = pvx;
            plowBatch.sprites.push_back(plow);
            snap->objects.push_back(std::move(plowBatch));
        }

        // ---- cursor ----------------------------------------------------------------
        {
            const float cx = L + W * 0.70f + std::cos(T * 0.9f) * W * 0.14f;
            const float cy = Tp + H * 0.30f + std::sin(T * 1.7f) * H * 0.11f;
            const bool mittenPhase = (static_cast<int>(T / 2.0f) % 2) == 0;
            if (mittenPhase) {
                snap->cursor.sprites.push_back(sprite(Shape::Ring, cx, cy, 28 * s, 28 * s, 0, 0, 0.9f));
                snap->cursor.sprites.push_back(sprite(Shape::Mitten, cx, cy, 16 * s, 16 * s, -0.25f, static_cast<int>(T / 4.0f) % 2));
            } else {
                snap->cursor.sprites.push_back(sprite(Shape::Ring, cx, cy, 60 * s, 60 * s, 0, 0, 0.9f));
                snap->cursor.sprites.push_back(sprite(Shape::Shovel, cx, cy, 20 * s, 20 * s, 0.35f));
            }
        }
        snap->taken = std::chrono::steady_clock::now();
        return snap;
    }

    // Every shape on the dark (left) and light (right) half of the own test window.
    // Page 3 layout (also used for the labels painted on the background window).
    static constexpr float kSkinK = 0.62f;
    static void skinCell(int i, float& cx, float& groundY) {   // relative to the half's origin
        cx = 60 + static_cast<float>(i % 4) * 110;
        groundY = 150 + static_cast<float>(i / 4) * 160;
    }
    void buildGallery3(FrameSnapshot& snap) {
        const float halfW = static_cast<float>(testRect.width()) / 2;
        for (int side = 0; side < 2; ++side) {
            const float ox = static_cast<float>(testRect.l) + static_cast<float>(side) * halfW;
            const float oy = static_cast<float>(testRect.t);
            SpriteBatch men;
            for (int i = 0; i < kSnowmanSkinCount; ++i) {
                float cx, gy;
                skinCell(i, cx, gy);
                snowmanSkin(men, ox + cx, oy + gy, kSkinK, kSnowmanSkins[i]);
            }
            snap.objects.push_back(std::move(men));
            // sheet: every accessory on a bare head (k = 0.85)
            SpriteBatch sheet;
            for (int a = 0; a < static_cast<int>(Accessory::Count); ++a) {
                const float k = 0.85f;
                const float cx = ox + 45 + static_cast<float>(a % 5) * 90;
                const float hy = oy + 505 + static_cast<float>(a / 5) * 95 + 50;
                const Accessory acc = static_cast<Accessory>(a);
                if (acc == Accessory::Broom) {
                    sheet.sprites.push_back(sprite(Shape::Accessory, cx, hy, 8 * k * 1.2f, 30 * k * 1.2f, 0, a));
                    continue;
                }
                const Head h{cx, hy, 22 * k, k, hy + 60 * k, 30 * k};
                sheet.sprites.push_back(sprite(Shape::Snowball, cx, hy, h.r, h.r));
                if (acc != Accessory::GooglyEye) {
                    sheet.sprites.push_back(sprite(Shape::Coal, cx - 8 * k, hy - 6 * k, 3 * k, 3 * k));
                    sheet.sprites.push_back(sprite(Shape::Coal, cx + 8 * k, hy - 6 * k, 3 * k, 3 * k));
                }
                const bool face = acc == Accessory::Beard || acc == Accessory::Mustache || acc == Accessory::Whiskers ||
                                  acc == Accessory::Pipe;
                if (face) placeAccessory(sheet, h, acc);
                sheet.sprites.push_back(sprite(Shape::Carrot, cx + 11 * k, hy + 3 * k, 11 * k, 3.6f * k, 0.12f));
                if (!face) placeAccessory(sheet, h, acc);
            }
            snap.objects.push_back(std::move(sheet));
        }
    }

    // Round-6 shapes on the dark (left) and light (right) half of the own test window.
    void buildGallery2(FrameSnapshot& snap) {
        const float halfW = static_cast<float>(testRect.width()) / 2;
        std::mt19937 g(7u);
        auto R = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); };
        for (int side = 0; side < 2; ++side) {
            const float ox = static_cast<float>(testRect.l) + static_cast<float>(side) * halfW;
            const float oy = static_cast<float>(testRect.t);
            // snowmen: all 16 hat x scarf combinations across both halves
            SpriteBatch men;
            for (int row = 0; row < 2; ++row) {
                for (int c = 0; c < 4; ++c) {
                    snowmanDressed(men, ox + 65 + static_cast<float>(c) * 105, oy + 180 + static_cast<float>(row) * 210,
                                   0.62f, c, (c + row + side * 2) & 3);
                }
            }
            snap.objects.push_back(std::move(men));

            // garland: wire segments between attachment points, bulbs at the low points
            SpriteBatch& pt = snap.particles;
            float px = ox + 20, py = oy + 432;
            for (int k = 0; k < 8; ++k) {
                const float nx = px + 51, ny = oy + 432 + 5 * std::sin(static_cast<float>(k + 1) * 1.7f);
                const float mx = (px + nx) * 0.5f, my = (py + ny) * 0.5f;
                const float hl = std::sqrt((nx - px) * (nx - px) + (ny - py) * (ny - py)) * 0.5f;
                pt.sprites.push_back(sprite(Shape::GarlandWire, mx, my, hl, 6, std::atan2(ny - py, nx - px)));
                pt.sprites.push_back(sprite(Shape::Bulb, mx, my + 6 + 7.5f, 4.5f, 7.5f, 0.1f * static_cast<float>(k % 3 - 1),
                                            k + ((k % 3) != 1 ? 8 : 0)));
                px = nx;
                py = ny;
            }

            // firework burst, frozen
            const float bx = ox + 115, by = oy + 585;
            const int mainCol = side == 0 ? 0 : 3;
            for (int k = 0; k < 160; ++k) {
                const float a = R(0, 2 * kPi), f = R(0.35f, 1.0f);
                const float v = 280.0f * f;
                SpriteInstance sp = sprite(Shape::Spark, bx + std::cos(a) * 72 * f, by + std::sin(a) * 72 * f + 10 * f * f,
                                           R(1.3f, 2.0f), 2, 0, k % 4 == 0 ? (side == 0 ? 1 : 2) : (k % 7 == 0 ? 5 : mainCol));
                sp.vx = std::cos(a) * v;
                sp.vy = std::sin(a) * v + 40;
                pt.sprites.push_back(sp);
            }
            for (int k = 0; k < 6; ++k) {
                pt.sprites.push_back(sprite(Shape::Spark, bx + R(-6, 6), by + R(-6, 6), 2.2f, 2, 0, 5));
            }

            // splats
            SpriteBatch spl;
            spl.sprites.push_back(sprite(Shape::Splat, ox + 335, oy + 590, 62, 52, 0, side * 2));
            spl.sprites.push_back(sprite(Shape::Splat, ox + 250, oy + 520, 26, 22, 0.4f, 1 + side * 2));
            snap.objects.push_back(std::move(spl));

            // three drifts (dry, damp, wet) with icicles and droplets underneath
            const float wets[3] = {0.0f, 0.35f, 0.75f};
            SpriteBatch ice;
            for (int k = 0; k < 3; ++k) {
                DriftStrip d;
                d.id = 600 + static_cast<SurfaceId>(side * 3 + k);
                d.step = 3;
                d.dpiScale = 1;
                d.x0 = ox + 20 + static_cast<float>(k) * 140;
                d.baseY = oy + 700;
                d.version = 1;
                d.wetness = wets[k];
                d.heights.resize(44);
                for (int i = 0; i < 44; ++i) {
                    const float u = static_cast<float>(i) / 43.0f;
                    const float edge = std::min(1.0f, std::min(u, 1 - u) * 8.0f);
                    d.heights[static_cast<std::size_t>(i)] =
                        (20 + 6 * std::sin(u * 11.0f + static_cast<float>(k)) + 3 * std::sin(u * 29.0f)) * edge;
                }
                d.clip.push_back(testRect);
                for (int j = 0; j < 7; ++j) {
                    const float ix = d.x0 + 10 + static_cast<float>(j) * 18 + R(-3, 3);
                    const float len = R(5, 40), hw = R(1.6f, 5.5f) * std::min(1.0f, len / 14 + 0.35f);
                    ice.sprites.push_back(sprite(Shape::Icicle, ix, d.baseY + 1 + len, hw, len, 0, j & 3));
                    if (k > 0 && (j % 2) == 0) {
                        ice.sprites.push_back(sprite(Shape::Droplet, ix, d.baseY + 1 + 2 * len + 3.2f, 2.6f, 4.2f, 0, 1));
                        ice.sprites.push_back(sprite(Shape::Droplet, ix, d.baseY + 1 + 2 * len + 26 + R(0, 20), 2.4f, 4.0f, 0, 0));
                    }
                }
                snap.drifts.push_back(std::move(d));
            }
            snap.objects.push_back(std::move(ice));
        }
    }

    void buildGallery(FrameSnapshot& snap) {
        const float halfW = static_cast<float>(testRect.width()) / 2;
        for (int side = 0; side < 2; ++side) {
            const float ox = static_cast<float>(testRect.l) + static_cast<float>(side) * halfW;
            const float oy = static_cast<float>(testRect.t);
            SpriteBatch& p = snap.particles;
            // row 1: flakes, variants 0..3 at radius 2, 4, 7, 10, 14
            const float radii[5] = {2, 4, 7, 10, 14};
            for (int v = 0; v < 4; ++v) {
                float x = ox + 20;
                for (int k = 0; k < 5; ++k) {
                    p.sprites.push_back(sprite(Shape::Flake, x + radii[k], oy + 24 + static_cast<float>(v) * 34, radii[k],
                                               radii[k], 0.2f * static_cast<float>(k), v));
                    x += radii[k] * 2 + 12;
                }
            }
            // snowman + loose parts
            SpriteBatch b;
            snowman(b, ox + 300, oy + 215, 1.0f);
            b.sprites.push_back(sprite(Shape::Snowball, ox + 400, oy + 60, 30, 30));
            b.sprites.push_back(sprite(Shape::Snowball, ox + 400, oy + 120, 8, 8));
            b.sprites.push_back(sprite(Shape::Coal, ox + 190, oy + 40, 12, 12));
            b.sprites.push_back(sprite(Shape::Carrot, ox + 200, oy + 80, 28, 9));
            b.sprites.push_back(sprite(Shape::Stick, ox + 200, oy + 120, 34, 12));
            snap.objects.push_back(std::move(b));
            // cursors
            snap.cursor.sprites.push_back(sprite(Shape::Ring, ox + 60, oy + 220, 36, 36));
            snap.cursor.sprites.push_back(sprite(Shape::Mitten, ox + 60, oy + 220, 20, 20, 0, 0));
            snap.cursor.sprites.push_back(sprite(Shape::Mitten, ox + 150, oy + 200, 14, 14, 0, 1));
            snap.cursor.sprites.push_back(sprite(Shape::Shovel, ox + 150, oy + 250, 20, 20));
            snap.cursor.sprites.push_back(sprite(Shape::Mitten, ox + 395, oy + 200, 40, 40, 0, 0));
            snap.cursor.sprites.push_back(sprite(Shape::Shovel, ox + 60, oy + 330, 40, 40));
            // snowplows: normal size both directions, then a double-size one parked on the drift
            SpriteBatch plows;
            plows.sprites.push_back(sprite(Shape::Snowplow, ox + 205, oy + 285, 46, 30, 0, 0));
            plows.sprites.push_back(sprite(Shape::Snowplow, ox + 340, oy + 285, 46, 30, 0, 1));
            plows.sprites.push_back(sprite(Shape::Snowplow, ox + 150, oy + 395, 23, 15, 0, 0));
            // double size, pushing a heap of snow through the deep drift below
            const float bigX = side == 0 ? ox + 230 : ox + 330;
            const float driftBase = oy + static_cast<float>(testRect.height()) - 150;
            plows.sprites.push_back(sprite(Shape::Snowplow, bigX, driftBase - 60, 92, 60, 0, side));
            snap.objects.push_back(std::move(plows));
            // snow chunks: long slabs, medium pieces, small lumps; all variants, some rotated
            SpriteBatch& pc = snap.particles;
            pc.sprites.push_back(sprite(Shape::SnowChunk, ox + 290, oy + 555, 150, 8, 0, 0));
            pc.sprites.push_back(sprite(Shape::SnowChunk, ox + 115, oy + 595, 80, 14, 0.12f, 1));
            pc.sprites.push_back(sprite(Shape::SnowChunk, ox + 395, oy + 345, 40, 22, -0.5f, 2));
            pc.sprites.push_back(sprite(Shape::SnowChunk, ox + 230, oy + 345, 30, 30, 0.3f, 3));
            pc.sprites.push_back(sprite(Shape::SnowChunk, ox + 330, oy + 595, 60, 4, 0.0f, 2));
            for (int k = 0; k < 8; ++k) {
                pc.sprites.push_back(sprite(Shape::SnowChunk, ox + 250 + static_cast<float>(k) * 24, oy + 622,
                                            k < 4 ? 6.0f : 10.0f, k < 4 ? 5.0f : 6.0f, static_cast<float>(k) * 0.7f, k & 3));
            }
            // deep drift along the bottom of this half: cleared path behind the big plow,
            // a heap piled against its blade, deep snow beyond
            DriftStrip d;
            d.id = 500 + static_cast<SurfaceId>(side);
            d.step = 3;
            d.dpiScale = 1;
            d.x0 = ox + 110;
            d.baseY = oy + static_cast<float>(testRect.height()) - 150;
            d.version = 1;
            const int n = static_cast<int>((halfW - 130) / 3);
            d.heights.resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                const float x = d.x0 + static_cast<float>(i) * 3;
                const float bladeFront = side == 0 ? ox + 230 + 1.42f * 60 : ox + 330 - 1.42f * 60;
                const float u = side == 0 ? x - bladeFront : bladeFront - x;   // ahead of the blade > 0
                float h;
                if (u < -8) {
                    h = 5 + 1.5f * std::sin(x * 0.15f);                           // ploughed path
                } else {
                    const float a = std::max(u, 0.0f);
                    h = 30 + 23 * std::exp(-(a / 18) * (a / 18)) + 3 * std::sin(x * 0.11f) + 1.5f * std::sin(x * 0.29f);
                }
                d.heights[static_cast<std::size_t>(i)] = std::max(0.0f, h);
            }
            d.clip.push_back(testRect);
            snap.drifts.push_back(std::move(d));
        }
    }
};

// Publishes snapshots at 60 Hz from its own thread (stands in for the simulation thread).
class Generator {
public:
    Generator(Scene& scene, SnapshotExchange& ex) : scene_(scene), ex_(ex) {
        thread_ = std::thread([this] { run(); });
    }
    ~Generator() { stop(); }
    void stop() {
        quit_.store(true);
        if (thread_.joinable()) thread_.join();
    }
    std::atomic<bool> publishNull{false};
    // CPU time consumed so far by the generator thread itself.
    double cpuSeconds() {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(static_cast<HANDLE>(thread_.native_handle()), &c, &e, &k, &u)) return 0;
        ULARGE_INTEGER a, b;
        a.LowPart = k.dwLowDateTime;
        a.HighPart = k.dwHighDateTime;
        b.LowPart = u.dwLowDateTime;
        b.HighPart = u.dwHighDateTime;
        return static_cast<double>(a.QuadPart + b.QuadPart) * 1e-7;
    }

private:
    void run() {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        while (!quit_.load()) {
            if (publishNull.load()) {
                ex_.publish(nullptr);
            } else {
                ex_.publish(scene_.step(1.0f / 60.0f));
            }
            next += std::chrono::microseconds(16667);
            const auto now = clock::now();
            if (next < now) next = now;
            const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(next - now).count();
            LARGE_INTEGER due;
            due.QuadPart = -wait * 10;
            if (timer && due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 100);
            } else {
                Sleep(static_cast<DWORD>(wait / 1000));
            }
        }
        if (timer) CloseHandle(timer);
    }
    Scene& scene_;
    SnapshotExchange& ex_;
    std::atomic<bool> quit_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------------------
struct Report {
    std::string text;
    int failures = 0;
    void line(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        text += buf;
        text += "\n";
        std::printf("%s\n", buf);
        std::fflush(stdout);
    }
    void check(bool ok, const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if (!ok) ++failures;
        line("[%s] %s", ok ? "PASS" : "FAIL", buf);
    }
    void save(const std::wstring& path) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
        }
    }
};

double processCpuSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER a, b;
    a.LowPart = k.dwLowDateTime;
    a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime;
    b.HighPart = u.dwHighDateTime;
    return static_cast<double>(a.QuadPart + b.QuadPart) * 1e-7;
}

struct PixelProbe {
    RectI rect;   // own magenta window
    COLORREF empty1 = 0, empty2 = 0, snow = 0, half = 0, gridIn = 0, gridGap = 0;
    HWND hit = nullptr;
    // Reads ONLY pixels that lie inside the own test window.
    void read() {
        HDC dc = GetDC(nullptr);
        empty1 = GetPixel(dc, rect.l + 20, rect.t + 20);
        empty2 = GetPixel(dc, rect.l + 380, rect.t + 150);
        snow = GetPixel(dc, rect.l + 250, rect.t + 230);
        half = GetPixel(dc, rect.l + 355, rect.t + 230);
        gridIn = GetPixel(dc, rect.l + 90 + 5 * 12 + 4, rect.t + 20 + 5 * 12 + 4);
        gridGap = GetPixel(dc, rect.l + 90 + 5 * 12 + 10, rect.t + 20 + 5 * 12 + 4);
        ReleaseDC(nullptr, dc);
        hit = WindowFromPoint(POINT{rect.l + 250, rect.t + 230});
    }
    bool emptyIsMagenta() const { return empty1 == RGB(255, 0, 255) && empty2 == RGB(255, 0, 255); }
    bool snowIsNearWhite() const {
        return GetRValue(snow) >= 185 && GetGValue(snow) >= 185 && GetBValue(snow) >= 185;
    }
    bool snowIsMagenta() const { return snow == RGB(255, 0, 255); }
};

// Counts overlay-class windows owned by THIS process (the real app may be running too).
int ownOverlayClassWindows() {
    struct Ctx { DWORD pid; int n; } ctx{GetCurrentProcessId(), 0};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            wchar_t cls[64] = {};
            if (pid == c->pid && GetClassNameW(h, cls, 64) && !wcscmp(cls, L"SnowFallOverlay")) ++c->n;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.n;
}

bool anyOverlayForeground(const OverlaySystem& sys) {
    const HWND fg = GetForegroundWindow();
    for (HWND h : sys.overlayWindows()) {
        if (h == fg) return true;
    }
    return false;
}

void pixelChecks(Report& rep, PixelProbe& probe, HWND bg, const OverlaySystem& sys, const char* stage) {
    probe.read();
    rep.check(probe.emptyIsMagenta(), "%s: empty area stays exactly magenta (got %06lx, %06lx; COLORREF bbggrr)", stage,
              static_cast<unsigned long>(probe.empty1), static_cast<unsigned long>(probe.empty2));
    rep.check(probe.snowIsNearWhite(), "%s: area under the drift is near-white (got r=%d g=%d b=%d)", stage,
              GetRValue(probe.snow), GetGValue(probe.snow), GetBValue(probe.snow));
    rep.check(probe.hit == bg, "%s: WindowFromPoint over the overlay returns the test window (click-through)", stage);
    rep.check(!anyOverlayForeground(sys), "%s: no overlay is the foreground window", stage);
}

int runSelftest(HINSTANCE inst, std::vector<MonitorInfo> monitors) {
    Report rep;
    Scene scene;
    scene.monitors = monitors;
    scene.selftest = true;
    scene.init();
    const RectI pb = scene.primary.bounds;
    scene.testRect = RectI{pb.l + 200, pb.t + 200, pb.l + 600, pb.t + 500};
    rep.line("render_demo --selftest");
    rep.line("monitors: %zu; primary %dx%d at (%d,%d), dpiScale %.2f", monitors.size(), pb.width(), pb.height(), pb.l,
             pb.t, static_cast<double>(scene.primary.dpiScale));

    HWND bg = createBgWindow(inst, scene.testRect);
    HWND notify = CreateWindowExW(0, L"SnowDemoNotify", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    pump(200);

    SnapshotExchange exchange;
    OverlaySystem sys;
    const bool initOk = sys.initialize(inst, &exchange, notify, kFailedMsg);
    rep.check(initOk, "initialize()");
    if (!initOk) {
        rep.save(exeDir() + L"render_selftest.txt");
        return 1;
    }
    Generator gen(scene, exchange);
    sys.setMonitors(monitors);
    sys.raise();
    PixelProbe probe;
    probe.rect = scene.testRect;

    // (1) transparency, drawing, click-through, focus
    pump(1300);
    rep.check(sys.overlayWindows().size() == monitors.size(), "one overlay per monitor (%zu)", sys.overlayWindows().size());
    for (HWND h : sys.overlayWindows()) {
        RECT rc{};
        GetWindowRect(h, &rc);
        bool match = false;
        for (const MonitorInfo& m : monitors) {
            if (rc.left == m.bounds.l && rc.top == m.bounds.t && rc.right == m.bounds.r && rc.bottom == m.bounds.b) match = true;
        }
        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        const LONG_PTR want = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        rep.check(match && IsWindowVisible(h) && (ex & want) == want,
                  "overlay covers its monitor exactly, is visible, has click-through/no-activate/toolwindow styles");
    }
    pixelChecks(rep, probe, bg, sys, "(1) initial");
    {
        // Over magenta (G = 0) the green channel is exactly colour*coverage, so the ratio of
        // the half-opacity drift to the full one is its relative coverage.
        const double gFull = GetGValue(probe.snow), gHalf = GetGValue(probe.half);
        const double ratio = gFull > 0 ? gHalf / gFull : 0;
        rep.check(ratio > 0.42 && ratio < 0.58, "DriftStrip.opacity 0.5 halves coverage: G %d vs %d, ratio %.3f",
                  static_cast<int>(gHalf), static_cast<int>(gFull), ratio);
        rep.check(GetGValue(probe.gridIn) > 180 && probe.gridGap == RGB(255, 0, 255),
                  "objects batch with 100 clip rects: inside a rect g=%d, gap stays magenta (%06lx)",
                  GetGValue(probe.gridIn), static_cast<unsigned long>(probe.gridGap));
    }
    {
        // particles.clip: flakes everywhere over the test window, clip = its left half only
        const RectI& t = scene.testRect;
        auto read3 = [&](COLORREF& left, COLORREF& right1, COLORREF& right2) {
            HDC dc = GetDC(nullptr);
            left = GetPixel(dc, t.l + 40, t.t + 200);
            right1 = GetPixel(dc, t.l + 300, t.t + 60);
            right2 = GetPixel(dc, t.l + 380, t.t + 150);
            ReleaseDC(nullptr, dc);
        };
        COLORREF lc, r1, r2;
        scene.particleClipTest.store(1);
        pump(400);
        read3(lc, r1, r2);
        rep.check(GetGValue(lc) > 60 && r1 == RGB(255, 0, 255) && r2 == RGB(255, 0, 255),
                  "particles.clip = left half: flakes on the left (g=%d), right half exactly magenta (%06lx, %06lx)",
                  GetGValue(lc), static_cast<unsigned long>(r1), static_cast<unsigned long>(r2));
        scene.particleClipTest.store(2);
        pump(400);
        read3(lc, r1, r2);
        rep.check(lc == RGB(255, 0, 255) && r1 == RGB(255, 0, 255) && r2 == RGB(255, 0, 255),
                  "particles.clip = {empty rect}: no flakes drawn (%06lx)", static_cast<unsigned long>(lc));
        scene.particleClipTest.store(0);
        pump(300);
    }

    // (2) frame rate at both caps
    pump(1200);
    RenderStats st = sys.stats();
    const float fps60 = st.fps, ms60 = st.frameMs;
    rep.check(fps60 >= 50 && fps60 <= 70, "(2) fps at cap 60 = %.1f (expected 50..70), frame CPU %.3f ms",
              static_cast<double>(fps60), static_cast<double>(ms60));
    sys.setFpsCap(30);
    pump(2400);
    st = sys.stats();
    rep.check(st.fps >= 25 && st.fps <= 35, "(2) fps at cap 30 = %.1f (expected 25..35), frame CPU %.3f ms",
              static_cast<double>(st.fps), static_cast<double>(st.frameMs));
    sys.setFpsCap(60);
    pump(300);

    // (3) forced device loss -> recovery
    const int resetsBefore = sys.stats().deviceResets;
    render_test::forceDeviceLoss(0);
    pump(1800);
    st = sys.stats();
    rep.check(st.deviceResets == resetsBefore + 1 && !st.failed, "(3) forced device loss recovered: deviceResets %d -> %d",
              resetsBefore, st.deviceResets);
    rep.check(st.fps >= 50 && st.fps <= 70, "(3) rendering continues after recovery, fps = %.1f", static_cast<double>(st.fps));
    pixelChecks(rep, probe, bg, sys, "(3) after recovery");

    // (4) monitor list changes
    const std::vector<HWND> firstWindows = sys.overlayWindows();
    sys.setMonitors({});
    bool gone = sys.overlayWindows().empty();
    for (HWND h : firstWindows) gone = gone && !IsWindow(h);
    rep.check(gone, "(4) setMonitors({}) destroys every overlay window");
    pump(400);
    probe.read();
    rep.check(probe.snowIsMagenta() && probe.emptyIsMagenta(), "(4) nothing is drawn with zero monitors");
    rep.check(sys.stats().fps == 0.0f || sys.stats().overlays == 0, "(4) stats: overlays=%d", sys.stats().overlays);

    sys.setMonitors(monitors);
    sys.raise();
    pump(900);
    rep.check(sys.overlayWindows().size() == monitors.size(), "(4) overlays are back after restoring the list");
    pixelChecks(rep, probe, bg, sys, "(4) restored");

    // shrink the primary overlay (ResizeBuffers path), then restore it
    std::vector<MonitorInfo> shrunk = monitors;
    for (MonitorInfo& m : shrunk) {
        if (m.bounds == pb) {
            m.bounds = RectI{pb.l, pb.t, pb.l + 800, pb.t + 600};
            m.work = m.bounds;
        }
    }
    const std::vector<HWND> beforeShrink = sys.overlayWindows();
    sys.setMonitors(shrunk);
    pump(700);
    bool reused = sys.overlayWindows() == beforeShrink;
    bool sized = false;
    for (HWND h : sys.overlayWindows()) {
        RECT rc{};
        GetWindowRect(h, &rc);
        if (rc.left == pb.l && rc.top == pb.t && rc.right - rc.left == 800 && rc.bottom - rc.top == 600) sized = true;
    }
    rep.check(reused && sized, "(4) changed bounds reuse the same HWND and move/resize it (800x600)");
    pixelChecks(rep, probe, bg, sys, "(4) resized overlay");
    sys.setMonitors(monitors);
    pump(700);
    pixelChecks(rep, probe, bg, sys, "(4) full-size again");

    // rapid reconfiguration (UI/render hand-off must neither deadlock nor leak windows)
    const ULONGLONG stressStart = GetTickCount64();
    for (int i = 0; i < 30; ++i) {
        sys.setMonitors(i % 3 == 0 ? std::vector<MonitorInfo>{} : (i % 3 == 1 ? shrunk : monitors));
        sys.setVisible(i % 5 != 0);
        pump(i % 4 == 0 ? 0 : 12);
    }
    sys.setVisible(true);
    sys.setMonitors(monitors);
    sys.raise();
    const ULONGLONG stressMs = GetTickCount64() - stressStart;
    pump(900);
    rep.check(sys.overlayWindows().size() == monitors.size() && stressMs < 5000,
              "(4) 30 rapid setMonitors/setVisible changes took %llu ms", static_cast<unsigned long long>(stressMs));
    pixelChecks(rep, probe, bg, sys, "(4) after rapid changes");

    // null snapshot -> cleared to transparent
    gen.publishNull.store(true);
    pump(500);
    probe.read();
    rep.check(probe.snowIsMagenta() && probe.emptyIsMagenta(), "null snapshot clears the overlay to transparent");
    gen.publishNull.store(false);
    pump(400);

    // (5) hidden -> not visible, idle
    sys.setVisible(false);
    pump(300);
    bool hidden = !sys.visible();
    for (HWND h : sys.overlayWindows()) hidden = hidden && !IsWindowVisible(h);
    rep.check(hidden, "(5) setVisible(false): overlay windows are not visible");
    // Snapshots keep arriving at 60 Hz; everything except the generator thread must be idle.
    const double cpu0 = processCpuSeconds(), gen0 = gen.cpuSeconds();
    pump(2000);
    const double procPct = (processCpuSeconds() - cpu0) / 2.0 * 100.0;
    const double genPct = (gen.cpuSeconds() - gen0) / 2.0 * 100.0;
    st = sys.stats();
    rep.check(procPct - genPct < 2.0 && st.fps == 0.0f,
              "(5) hidden: process CPU %.2f%% of one core, of which the demo's snapshot generator %.2f%%; fps %.1f",
              procPct, genPct, static_cast<double>(st.fps));
    probe.read();
    rep.check(probe.snowIsMagenta(), "(5) hidden: nothing is composited over the test window");
    sys.setVisible(true);
    sys.raise();
    pump(1300);
    pixelChecks(rep, probe, bg, sys, "(5) shown again");
    st = sys.stats();
    rep.check(st.fps >= 50 && st.fps <= 70, "(5) fps after show = %.1f", static_cast<double>(st.fps));

    // opacity
    sys.setGlobalOpacity(0.1f);
    pump(400);
    probe.read();
    rep.check(GetGValue(probe.snow) < 60 && GetRValue(probe.snow) > 240, "global opacity 0.1 makes the drift faint (g=%d)",
              GetGValue(probe.snow));
    sys.setGlobalOpacity(1.0f);
    pump(300);

    // (7) unrecoverable device loss -> failed + notification, overlays hidden
    render_test::forceDeviceLoss(3);
    for (int i = 0; i < 60 && !g_failedMsgSeen.load(); ++i) pump(100);
    pump(300);
    st = sys.stats();
    bool hiddenAfterFail = true;
    for (HWND h : sys.overlayWindows()) hiddenAfterFail = hiddenAfterFail && !IsWindowVisible(h);
    rep.check(st.failed && g_failedMsgSeen.load() && hiddenAfterFail && st.fps == 0.0f,
              "(7) 3 failed re-creations: stats().failed=%d, failedMsg received=%d, overlays hidden=%d", st.failed ? 1 : 0,
              g_failedMsgSeen.load() ? 1 : 0, hiddenAfterFail ? 1 : 0);
    probe.read();
    rep.check(probe.snowIsMagenta() && probe.emptyIsMagenta(), "(7) failed renderer leaves the screen untouched");

    // (6) shutdown
    const std::vector<HWND> lastWindows = sys.overlayWindows();
    gen.stop();
    sys.shutdown();
    bool allGone = sys.overlayWindows().empty();
    for (HWND h : lastWindows) allGone = allGone && !IsWindow(h);
    rep.check(allGone && ownOverlayClassWindows() == 0, "(6) shutdown leaves no overlay windows (none of this process)");
    sys.shutdown();   // must be harmless

    // re-initialise after shutdown: clean device, leak check on the D3D device reference count
    g_failedMsgSeen.store(false);
    Generator gen2(scene, exchange);
    const bool again = sys.initialize(inst, &exchange, notify, kFailedMsg);
    sys.setMonitors(monitors);
    sys.raise();
    pump(1200);
    rep.check(again && !sys.stats().failed, "initialize() works again after shutdown()");
    pixelChecks(rep, probe, bg, sys, "(6) second session");
    const std::vector<HWND> w2 = sys.overlayWindows();
    gen2.stop();
    sys.shutdown();
    allGone = true;
    for (HWND h : w2) allGone = allGone && !IsWindow(h);
    const unsigned long refs = render_test::lastDeviceRefCount();
    rep.check(allGone, "(6) second shutdown leaves no overlay windows");
    rep.check(refs == 0, "(6) D3D11 device reference count at final release = %lu (0 = no leaked GPU objects)", refs);

    DestroyWindow(bg);
    DestroyWindow(notify);
    rep.line("RESULT: %s (%d failure(s))", rep.failures == 0 ? "PASS" : "FAIL", rep.failures);
    rep.save(exeDir() + L"render_selftest.txt");
    return rep.failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------
// Gallery: screenshot of the OWN test window area only
// ---------------------------------------------------------------------------------------
// Saves an enlarged copy of a region (must lie inside the own test window) for close inspection.
bool saveZoomPng(const RectI& r, int factor, const std::wstring& path) {
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, r.width() * factor, r.height() * factor);
        HGDIOBJ old = SelectObject(mem, bmp);
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchBlt(mem, 0, 0, r.width() * factor, r.height() * factor, screen, r.l, r.t, r.width(), r.height(), SRCCOPY);
        SelectObject(mem, old);
        const CLSID pngClsid = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        Gdiplus::Bitmap image(bmp, nullptr);
        ok = image.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

bool saveRectPng(const RectI& r, const std::wstring& path) {
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, r.width(), r.height());
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, r.width(), r.height(), screen, r.l, r.t, SRCCOPY);
        SelectObject(mem, old);
        const CLSID pngClsid = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        Gdiplus::Bitmap image(bmp, nullptr);
        ok = image.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

int runGallery(HINSTANCE inst, std::vector<MonitorInfo> monitors, int page) {
    Scene scene;
    scene.monitors = monitors;
    scene.gallery = true;
    scene.galleryPage = page;
    scene.init();
    const RectI pb = scene.primary.bounds;
    scene.testRect = page >= 2 ? RectI{pb.l + 100, pb.t + 60, pb.l + 1000, pb.t + 940}
                               : RectI{pb.l + 100, pb.t + 100, pb.l + 1000, pb.t + 760};
    g_labels.clear();
    if (page == 3) {
        for (int side = 0; side < 2; ++side) {
            for (int i = 0; i < kSnowmanSkinCount; ++i) {
                float cx, gy;
                Scene::skinCell(i, cx, gy);
                const int x = static_cast<int>(cx) + side * scene.testRect.width() / 2, y = static_cast<int>(gy);
                g_labels.push_back(Label{RECT{x - 55, y + 2, x + 55, y + 18}, kSnowmanSkins[i].nameRu,
                                         side == 0 ? RGB(225, 230, 245) : RGB(40, 44, 60)});
            }
        }
    }
    g_bgSplit = true;
    HWND bg = createBgWindow(inst, scene.testRect);
    SnapshotExchange exchange;
    OverlaySystem sys;
    if (!sys.initialize(inst, &exchange, nullptr, 0)) return 1;
    Generator gen(scene, exchange);
    sys.setMonitors(monitors);
    sys.raise();
    pump(1200);
    const bool ok = saveRectPng(scene.testRect, exeDir() + (page == 3 ? L"render_gallery3.png" : page == 2 ? L"render_gallery2.png" : L"render_gallery.png"));
    std::printf("gallery %s\n", ok ? "saved" : "FAILED");
    if (page == 3) {   // close-ups of the accessory sheet (dark + light) and a few skins, for reviewing details
        const RectI& t = scene.testRect;
        const int hw = t.width() / 2;
        for (int half = 0; half < 2; ++half) {
            for (int part = 0; part < 2; ++part) {
                const RectI z{t.l + half * hw, t.t + 500 + part * 190, t.l + half * hw + hw, t.t + 500 + part * 190 + 190};
                wchar_t name[64];
                swprintf(name, 64, L"render_gallery3_zoom_%d%d.png", half, part);
                saveZoomPng(z, 2, exeDir() + name);
            }
        }
        saveZoomPng(RectI{t.l, t.t, t.l + hw, t.t + 250}, 2, exeDir() + L"render_gallery3_zoom_skins.png");
    }
    gen.stop();
    sys.shutdown();
    DestroyWindow(bg);
    return ok ? 0 : 1;
}

int runDemo(HINSTANCE inst, std::vector<MonitorInfo> monitors, int seconds, int flakes, int sparkTarget) {
    Scene scene;
    scene.monitors = monitors;
    scene.flakeCount = flakes;
    scene.sparkTarget = sparkTarget;
    scene.init();
    SnapshotExchange exchange;
    OverlaySystem sys;
    if (!sys.initialize(inst, &exchange, nullptr, 0)) {
        std::printf("initialize failed\n");
        return 1;
    }
    Generator gen(scene, exchange);
    sys.setMonitors(monitors);
    sys.raise();
    std::printf("render_demo: %zu monitor(s), %d flakes, %d sparks target, %d s\n", monitors.size(), flakes, sparkTarget,
                seconds);
    for (int i = 0; i < seconds; ++i) {
        pump(1000);
        const RenderStats st = sys.stats();
        std::printf("  t=%2d s  fps=%.1f  frame CPU=%.3f ms  overlays=%d  resets=%d%s\n", i + 1, static_cast<double>(st.fps),
                    static_cast<double>(st.frameMs), st.overlays, st.deviceResets, st.failed ? "  FAILED" : "");
        std::fflush(stdout);
    }
    gen.stop();
    sys.shutdown();
    std::printf("render_demo: clean exit, device refs at release = %lu\n", render_test::lastDeviceRefCount());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    int seconds = 8, flakes = 3000, sparkTarget = 0, galleryPage = 1;
    bool selftest = false, gallery = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--selftest")) selftest = true;
        else if (!std::strcmp(argv[i], "--gallery")) gallery = true;
        else if (!std::strcmp(argv[i], "--gallery2")) gallery = true, galleryPage = 2;
        else if (!std::strcmp(argv[i], "--gallery3")) gallery = true, galleryPage = 3;
        else if (!std::strcmp(argv[i], "--sparks") && i + 1 < argc) sparkTarget = std::clamp(std::atoi(argv[++i]), 0, 50000);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--flakes") && i + 1 < argc) flakes = std::clamp(std::atoi(argv[++i]), 0, 100000);
    }
    snow::log::init(exeDir() + L"render_demo.log");

    WNDCLASSW bc{};
    bc.lpfnWndProc = bgProc;
    bc.hInstance = inst;
    bc.lpszClassName = L"SnowDemoBg";
    bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&bc);
    WNDCLASSW nc{};
    nc.lpfnWndProc = notifyProc;
    nc.hInstance = inst;
    nc.lpszClassName = L"SnowDemoNotify";
    RegisterClassW(&nc);

    const std::vector<MonitorInfo> monitors = enumMonitors();
    if (monitors.empty()) {
        std::printf("no monitors\n");
        return 2;
    }
    int rc;
    if (selftest) rc = runSelftest(inst, monitors);
    else if (gallery) rc = runGallery(inst, monitors, galleryPage);
    else rc = runDemo(inst, monitors, seconds, flakes, sparkTarget);
    snow::log::shutdown();
    return rc;
}
