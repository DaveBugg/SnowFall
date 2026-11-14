// Decorative, purely cosmetic drawing for the themed windows: falling-snow fields, preset scene
// cards, header, garland, navigation glyphs, countdown card, festive fireworks scene.
// All original procedural artwork.
//
// Every animated element is split in two: a STATIC part drawn with Direct2D (in DIPs) into a cached
// Layer, and a DYNAMIC part (flakes, bulbs, sparks) blended per frame by the Compositor (in physical
// pixels; `s` = pixels per DIP).
#pragma once
#include <string>
#include <vector>

#include "compositor.h"
#include "core/settings.h"
#include "d2d_paint.h"
#include "theme.h"

namespace snow::ui {

struct DecoFlake {
    float x, y;        // normalised 0..1 within the area
    float r;           // radius in DIPs
    float speed;       // area heights per second
    float phase, sway; // horizontal wobble
    float alpha;
};

class FlakeField {
public:
    void reset(unsigned seed, int count, float minR, float maxR, float minSpeed, float maxSpeed, float minAlpha = 0.55f);
    void step(float dt, float wind);   // wind: area widths per second
    // Area in frame pixels; streak > 0 draws motion streaks (blizzard).
    void draw(Compositor& c, float l, float t, float w, float h, float s, COLORREF color, float streak = 0, float wind = 0) const;
    bool empty() const { return flakes_.empty(); }

private:
    unsigned rnd();
    float frand(float a, float b);
    std::vector<DecoFlake> flakes_;
    unsigned seed_ = 1;
    float time_ = 0;
};

// ---- preset scene cards -------------------------------------------------------------------------
struct SceneAnim {
    WeatherPreset preset = WeatherPreset::Normal;
    bool ready = false;
    float time = 0;
    FlakeField flakes;
    Layer layer;   // cached static card image
    void ensure(WeatherPreset p, unsigned seed);
    void step(float dt);
};

struct SceneCardState {
    bool selected = false, focused = false, enabled = true, hot = false, compact = false;
};
std::wstring sceneKey(const Palette& pal, Surface surface, const SceneAnim& anim, const std::wstring& title,
                      const std::wstring& subtitle, const SceneCardState& st);
void drawSceneStatic(Painter& p, D2D1_SIZE_F size, const Palette& pal, Surface surface, const SceneAnim& anim,
                     const std::wstring& title, const std::wstring& subtitle, const SceneCardState& st);
void drawSceneDynamic(Compositor& c, float s, const Palette& pal, const SceneAnim& anim, const SceneCardState& st);

// ---- header ---------------------------------------------------------------------------------------
struct HeaderAnim {
    FlakeField flakes;
    float time = 0;
    bool ready = false;
    Layer layer;
    void ensure(unsigned seed, int count);
    void step(float dt);
};
// chip: area of the theme-swatch container (drawn as a pill), empty = none.
struct HeaderChip {
    D2D1_RECT_F rect{};        // pill behind a group of header controls (empty = none)
    std::wstring caption;      // drawn left of the pill
    bool limitsText = true;    // title / subtitle end before it (false: a chip on the title line)
};
void drawHeaderStatic(Painter& p, D2D1_SIZE_F size, const Palette& pal, const std::wstring& title, const std::wstring& subtitle,
                      const HeaderChip* chips, int chipCount);
void drawHeaderDynamic(Compositor& c, float s, const Palette& pal, const HeaderAnim& anim, float heightDip);
// Garland: wire + unlit bulbs (static, D2D) and lit bulbs (dynamic). Coordinates in DIPs.
void drawGarlandStatic(Painter& p, float x0, float x1, float y, float sag);
void drawGarlandLights(Compositor& c, float s, float x0, float x1, float y, float sag, float time);
void drawLogo(Painter& p, const Palette& pal, float cx, float cy, float size);

// Navigation glyphs: 0 snow, 1 drifts, 2 tools, 3 performance, 4 festive, 5 system, 6 about.
void drawNavGlyph(Painter& p, int glyph, float cx, float cy, float size, D2D1_COLOR_F c);
// Tool glyphs (snowball, mitten, shovel).
void drawToolGlyph(Painter& p, Tool tool, float cx, float cy, float size, D2D1_COLOR_F c);

// ---- countdown --------------------------------------------------------------------------------------
void drawCountdownStatic(Painter& p, D2D1_SIZE_F size, const Palette& pal, const std::wstring& title, const std::wstring& big,
                         const std::wstring& caption, bool festive);
void drawCountdownDynamic(Compositor& c, float s, float widthDip, float heightDip, float time);

// ---- festive scene ----------------------------------------------------------------------------------
struct FireworksAnim {
    struct Burst {
        float x, y, t, hue;
        int colour;
    };
    std::vector<Burst> bursts;
    float time = 0, next = 0;
    unsigned seed = 7;
    void step(float dt);
};
void drawFestiveStatic(Painter& p, D2D1_SIZE_F size, const Palette& pal);
void drawFestiveDynamic(Compositor& c, float s, float widthDip, float heightDip, const FireworksAnim& anim);

}  // namespace snow::ui
