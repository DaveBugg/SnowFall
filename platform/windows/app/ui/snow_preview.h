// Live preview: a miniature desktop (two fake windows + taskbar) running a real snow::World
// with the settings being edited. Drawn with Direct2D from World::buildSnapshot().
#pragma once
#include <memory>

#include "core/layout.h"
#include "core/settings.h"
#include "compositor.h"
#include "core/snapshot.h"
#include "d2d_paint.h"
#include "theme.h"

namespace snow {
class World;
}

namespace snow::ui {

class SnowPreview {
public:
    static constexpr int kDesktopW = 400;   // virtual desktop of the preview, "physical px"
    static constexpr int kDesktopH = 250;
    static constexpr float kSpeedUp = 4.0f;        // simulated seconds per real second
    static constexpr float kWarmUpSeconds = 360.0f; // pre-simulated on open / reset

    SnowPreview();
    ~SnowPreview();

    void apply(const Settings& s);   // live settings (cheap; call on every edit)
    void reset();                    // fresh world + warm-up (kWarmUpSeconds)
    void advance(float seconds);     // simulate synchronously (fixed steps)
    void tick(float realDt);         // animation tick (time-accelerated)
    void drawBackground(Painter& p, const D2D1_RECT_F& area, const Palette& pal);   // static: wallpaper, windows, taskbar
    void drawDrifts(Painter& p, const D2D1_RECT_F& area);                           // drifts + desktop objects (D2D)
    std::uint64_t driftKey();                                                       // changes when drifts/objects change
    // Falling particles, blended by the compositor. left/top in frame px, k = frame px per desktop px.
    void drawParticles(Compositor& c, float left, float top, float k);
    double snowMass() const;
    size_t particles() const;

private:
    void ensureWorld();
    void snapshot();
    std::unique_ptr<World> world_;
    DesktopLayout layout_;
    FrameSnapshot snap_;
    Settings settings_;
    bool snapDirty_ = true;
    float carry_ = 0;
};

}  // namespace snow::ui
