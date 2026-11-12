// ParticleSystem: falling flakes, loose flakes shed by drifts, and short-lived dust.
// Flat array, swap-remove, no per-particle allocations; capacity is reserved up front.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/layout.h"
#include "core/types.h"
#include "simulation/rng.h"
#include "simulation/weather.h"

namespace snow {

class SurfaceRegistry;

struct Particle {
    enum : std::uint8_t {
        kCollidesWindows = 1,   // lands on window tops too (otherwise taskbar / screen bottom only)
        kDust = 2,              // cosmetic, fades out over `life`, never deposits
        kLoose = 4,             // shed by a drift / object; not part of the ambient budget
        kDroplet = 8,           // melt-water drop: falls fast, splashes (no deposit)
    };
    float x = 0, y = 0;
    float vx = 0, vy = 0;       // px/s
    float radius = 1;           // physical px
    float opacity = 1;
    float mass = 1;             // relative density (heavier flakes react less to wind)
    float fall = 1;             // terminal fall speed factor (includes DPI scale)
    float drift = 1;            // wind / turbulence response factor (includes DPI scale)
    float swayPhase = 0, swayFreq = 1, swayAmp = 0;
    float turbPhase = 0, turbFreq = 1;
    float rotation = 0, angVel = 0;
    float life = -1;            // dust: seconds left; < 0 = unlimited
    float maxLife = 1;
    std::uint32_t id = 0;
    std::uint8_t variant = 0;   // sprite style, 0..kFlakeVariants-1
    std::uint8_t flags = 0;
    std::uint8_t home = 255;    // sky per monitor: index of its home monitor (255 = whole desktop)
};

class ParticleSystem {
public:
    // Fraction of ambient flakes that interact with window surfaces.
    inline static constexpr float kWindowColliderShare = 0.55f;
    // Deposited drift area per flake = kDepositArea * radius^2 * preset growth.
    inline static constexpr float kDepositArea = 0.6f;
    inline static constexpr std::size_t kHardCapacity = 13500;   // ambient cap + loose/dust headroom

    explicit ParticleSystem(std::uint64_t seed);

    void setScene(const RectI& bounds, const std::vector<MonitorInfo>& monitors);
    void setTargetCount(int n) { target_ = n; }
    int targetCount() const { return target_; }
    void setEmission(bool on) { emission_ = on; }
    // Each monitor has its own sky: flakes start at their monitor's top / upwind edge and never
    // leave it (they fade out over kEdgeFadePx at its sides / bottom and are recycled).
    void setSkyPerMonitor(bool on) { perMonitor_ = on; }
    bool skyPerMonitor() const { return perMonitor_; }
    inline static constexpr float kEdgeFadePx = 6.0f;
    // Opacity factor of a flake near its home monitor's edges (1 = fully visible).
    float edgeFade(const Particle& p) const;
    int skyCount(std::size_t monitor) const { return monitor < skies_.size() ? skies_[monitor].count : 0; }
    int skyTarget(std::size_t monitor) const;
    void clear();

    // Advance all particles. `surfaces` may be null (no collisions / deposition).
    void step(float dt, const Weather& weather, SurfaceRegistry* surfaces);

    Particle* spawnAmbient(const Weather& weather, int sky = -1);
    // A real flake (deposits when it lands) that is not counted against the ambient budget.
    Particle* addLoose(float x, float y, float vx, float vy, float radius);
    Particle* addDust(float x, float y, float vx, float vy, float radius, float life);
    Particle* addDroplet(float x, float y, float vx, float vy, float dpiScale);

    // Umbrella: flakes are deflected around a disc of `radius` at (x, y) (never enter it).
    void setUmbrella(float x, float y, float radius, bool on) {
        ux_ = x;
        uy_ = y;
        uR_ = radius;
        uOn_ = on && radius > 0;
    }
    // Short local gust (fast pointer swipe): velocity (vx, vy) imparted within `radius`.
    void setGust(float x, float y, float vx, float vy, float radius) {
        gx_ = x;
        gy_ = y;
        gvx_ = vx;
        gvy_ = vy;
        gR_ = radius;
        gT_ = kGustSeconds;
    }
    inline static constexpr float kGustSeconds = 0.4f;
    // Rectangles flakes flow around (rounded corners `radius`, steering starts `band` px out).
    void setObstacles(const std::vector<RectI>& rects, float radius, float band) {
        obstacles_.clear();
        for (const RectI& r : rects) {
            Obstacle o;
            o.cx = 0.5f * static_cast<float>(r.l + r.r);
            o.cy = 0.5f * static_cast<float>(r.t + r.b);
            o.r = std::min(radius, 0.5f * static_cast<float>(std::min(r.width(), r.height())));
            o.hx = 0.5f * static_cast<float>(r.width()) - o.r;
            o.hy = 0.5f * static_cast<float>(r.height()) - o.r;
            o.band = band;
            obstacles_.push_back(o);
        }
    }
    int splashes() const { return splashes_; }
    // `count` particles scattered in a disc, flying outwards.
    void burst(float cx, float cy, float radius, int count, float speed, bool dust, float dpiScale);

    const std::vector<Particle>& particles() const { return p_; }
    std::vector<Particle>& particles() { return p_; }
    int ambientCount() const { return ambient_; }
    Rng& rng() { return rng_; }
    // Totals since construction (diagnostics / tests).
    std::uint64_t landedCount() const { return landed_; }
    double depositedArea() const { return deposited_; }

private:
    Particle* add();
    void removeAt(std::size_t i);
    float scaleAtX(float x) const;

    struct Span {
        float l, r, scale;
    };
    std::vector<Particle> p_;
    std::vector<Span> spans_;
    Rng rng_;
    RectI scene_{};
    int target_ = 0;
    int ambient_ = 0;
    bool emission_ = true;
    float spawnAcc_ = 0;
    struct Sky {
        RectI r;
        float scale = 1, weight = 0, acc = 0;
        int count = 0;
    };
    std::vector<Sky> skies_;
    bool perMonitor_ = false;
    struct Obstacle {
        float cx, cy, hx, hy, r, band;
    };
    // Signed distance to an obstacle's rounded rect and the outward normal.
    static float obstacleDist(const Obstacle& o, float x, float y, float& nx, float& ny);
    std::vector<Obstacle> obstacles_;
    float ux_ = 0, uy_ = 0, uR_ = 0;
    bool uOn_ = false;
    float gx_ = 0, gy_ = 0, gvx_ = 0, gvy_ = 0, gR_ = 0, gT_ = 0;
    int splashes_ = 0;
    float meanScale_ = 1;
    std::uint32_t nextId_ = 1;
    std::uint64_t landed_ = 0;
    double deposited_ = 0;
};

}  // namespace snow
