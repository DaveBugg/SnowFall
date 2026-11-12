// Festive extras: the blinking light garland along taskbar top edges and the fireworks show.
#pragma once
#include <cstdint>
#include <vector>

#include "core/snapshot.h"
#include "core/types.h"
#include "simulation/rng.h"

namespace snow {

struct Surface;

// ---------------------------------------------------------------- garland
// Anchors every kGarlandAnchorMin..Max px (deterministic per surface), riding on the drift
// (anchor = drift top + sag + clearance), shallow parabolic swags between them, a bulb every
// kGarlandBulbSpacing px. Parts that end up inside the drift (lumps) are not drawn: the
// garland looks partially buried. Blink pattern changes every kGarlandPatternSeconds.
inline constexpr float kGarlandAnchorMinAt96 = 120.0f;
inline constexpr float kGarlandAnchorMaxAt96 = 160.0f;
inline constexpr float kGarlandBulbSpacingAt96 = 26.0f;
inline constexpr float kGarlandPatternSeconds = 20.0f;
inline constexpr int kGarlandPatterns = 4;   // 0 chase, 1 alternate, 2 random twinkle, 3 slow fade

int garlandPattern(float time);
// Bulb k on? (and its brightness 0..1 for the slow fade)
bool garlandBulbOn(int pattern, int k, float time, std::uint64_t seed, float* brightness = nullptr);
// Garland sprites (GarlandWire segments + Bulbs) for one taskbar surface. plowX: world x of
// a snowplow on this surface (bulbs near it swing), or NaN.
void buildGarland(const Surface& s, float time, float plowX, std::vector<SpriteInstance>& out);

// ---------------------------------------------------------------- fireworks
inline constexpr int kMaxSparks = 1500;
inline constexpr float kNewYearShowSeconds = 90.0f;
inline constexpr float kManualShowSeconds = 25.0f;

struct Spark {
    float x = 0, y = 0, vx = 0, vy = 0;
    float life = 1, maxLife = 1, size = 1.5f, drag = 1, gravity = 100;
    float crackleAt = -1;      // crackle stars: life at which they pop into micro sparks
    float dpi = 1;
    std::uint8_t color = 0;    // Shape::Spark variant
    bool smoke = false;        // dim rocket trail
};

struct Rocket {
    float x = 0, y = 0, vx = 0, vy = 0, burstY = 0, trailAcc = 0, dpi = 1;
    std::uint8_t color = 0, type = 0;   // type: 0 peony, 1 ring, 2 willow, 3 crackle
};

class Fireworks {
public:
    explicit Fireworks(std::uint64_t seed) : rng_(seed ^ 0xF12E3011ull) {}

    // Start (or extend) a show of `seconds` over `area` (primary monitor).
    void start(float seconds, const RectI& area, float dpi);
    void step(float dt);
    void clear() {
        sparks_.clear();
        rockets_.clear();
        showLeft_ = 0;
    }
    bool active() const { return showLeft_ > 0 || !sparks_.empty() || !rockets_.empty(); }
    float showLeft() const { return showLeft_; }
    void appendSprites(std::vector<SpriteInstance>& out) const;

    std::size_t sparkCount() const { return sparks_.size(); }
    int launched() const { return launched_; }
    int bursts() const { return bursts_; }
    int shows() const { return shows_; }

private:
    void launch();
    void burst(const Rocket& r);
    void addSpark(const Spark& s) {
        if (sparks_.size() < static_cast<std::size_t>(kMaxSparks)) sparks_.push_back(s);
    }

    std::vector<Spark> sparks_;
    std::vector<Rocket> rockets_;
    Rng rng_;
    RectI area_{};
    float dpi_ = 1;
    float showLeft_ = 0, launchAcc_ = 0, rate_ = 2;
    bool finale_ = false;
    std::uint8_t colors_[3]{0, 1, 2};
    int launched_ = 0, bursts_ = 0, shows_ = 0;
};

}  // namespace snow
