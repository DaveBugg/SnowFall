// Icicles hanging from the BOTTOM edge of application windows that carried a substantial
// snow cap for a while (accumulation.icicles). Stored per window surface in local x, so they
// move with the window, hide with it and go when it closes. A sharp move / shake / close
// snaps them off: they fall with part of the window's velocity, spin, and shatter on impact
// into tiny ice shards and white dust (foreground particles).
#pragma once
#include <cstdint>
#include <vector>

#include "core/snapshot.h"
#include "core/types.h"
#include "simulation/rng.h"

namespace snow {

class SurfaceRegistry;
class ParticleSystem;
struct Surface;

inline constexpr float kIcicleCapShare = 0.4f;     // cap fill (mass / full cap) that grows icicles
inline constexpr float kIcicleCapSeconds = 150.0f; // ... for this long before the first appear
inline constexpr float kIcicleThawFactor = 5.0f;   // thaw: everything about them is 5x faster
inline constexpr float kIcicleGrowSeconds = 320.0f;  // typical time to full length
inline constexpr int kMaxFallingIce = 400;
inline constexpr float kIcicleHitPadAt96 = 3.0f;      // hit box = drawn shape + this
inline constexpr float kIcicleBreakSpeedAt96 = 250.0f;  // slower: the ball just bumps off
inline constexpr float kIcicleLowHit = 0.55f;          // hit below this share of the length: only the tip snaps

struct Icicle {
    float localX = 0;      // from the window's left edge
    float len = 0, maxLen = 10, width = 3, rate = 0.03f;
    float drip = 0;        // thaw: 0..1 a drop forming at the tip
    float dripRate = 0.15f;
    std::uint8_t variant = 0;
};

struct IcicleSet {
    SurfaceId surface = 0;
    float capTime = 0;     // seconds of substantial cap (thaw counts 5x)
    bool seeded = false;
    std::vector<Icicle> list;
};

struct FallingIce {
    float x = 0, y = 0, vx = 0, vy = 0, rotation = 0, angVel = 0;
    float halfW = 1, halfH = 3, dpi = 1;
    float life = -1;       // shards: seconds left (fade); icicles: -1
    std::uint8_t variant = 0;
    SurfaceId exclude = 0;
};

class IcicleSystem {
public:
    explicit IcicleSystem(std::uint64_t seed) : rng_(seed ^ 0x1C1C1Eull) {}

    void step(float dt, SurfaceRegistry& reg, ParticleSystem* fx, bool enabled, bool thaw);
    // Snap icicles of `s` off with probability `fraction` each.
    int snapOff(const Surface& s, float fraction, float wvx, float wvy, float offX, float offY);
    void onSurfacesRemoved(const std::vector<Surface>& removed, bool drop);
    // A body of `radius` moving from (x0,y0) to (x1,y1) with velocity (vx,vy): icicles whose
    // hit box the path crosses snap off, in path order (low hits leave a stub). Slower than
    // minSpeed nothing breaks; *bumped tells whether it touched one. Returns the number broken.
    int hitSegment(const SurfaceRegistry& reg, float x0, float y0, float x1, float y1, float radius, float vx, float vy,
                   float minSpeed, ParticleSystem* fx, bool* bumped = nullptr);
    int hitBroken() const { return hitBroken_; }
    int hitStubs() const { return hitStubs_; }
    void clear() {
        sets_.clear();
        falling_.clear();
    }

    // One batch per window with icicles: clip = below the window minus windows in front of it.
    void appendBatches(const SurfaceRegistry& reg, std::vector<SpriteBatch>& out) const;
    void appendFalling(std::vector<SpriteInstance>& out, float opacity) const;

    const std::vector<IcicleSet>& sets() const { return sets_; }
    std::vector<IcicleSet>& sets() { return sets_; }
    const IcicleSet* setFor(SurfaceId id) const;
    IcicleSet* setFor(SurfaceId id);
    const std::vector<FallingIce>& falling() const { return falling_; }
    int attachedCount() const;
    int shattered() const { return shattered_; }

private:
    void seed(IcicleSet& set, const Surface& s);
    void shatter(const FallingIce& f, float hitX, float hitY, ParticleSystem* fx);

    std::vector<IcicleSet> sets_;
    std::vector<FallingIce> falling_;
    Rng rng_;
    bool thaw_ = false;
    int shattered_ = 0;
    int hitBroken_ = 0, hitStubs_ = 0;
    struct Hit {
        float t;
        std::size_t set;
        float localX;
    };
    std::vector<Hit> hits_;
};

}  // namespace snow
