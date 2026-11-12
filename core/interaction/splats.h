// Snowballs thrown hard "at the screen" splat on the application window under their throw:
// a Shape::Splat stuck to the window pane (local coordinates, moves with it), sticks for
// 1..3 s, then slides down leaving a fading trail and falls off the window's bottom edge
// as a snow chunk. Removed when the window is minimised or closed; a jerk knocks it off.
// Mass: the ball's snow -> splat (-> trail dust) -> chunk.
#pragma once
#include <cstdint>
#include <vector>

#include "core/snapshot.h"
#include "core/types.h"
#include "simulation/rng.h"

namespace snow {

class SurfaceRegistry;
class ParticleSystem;
class ChunkSystem;
struct Ball;

inline constexpr float kSplatThrowSpeedAt96 = 900.0f;   // release speed that splats
inline constexpr float kSplatLookAhead = 0.08f;         // s: where the ball would be -> impact point
inline constexpr float kSplatJerkSpeedAt96 = 1500.0f;   // window speed that knocks splats off

struct Splat {
    WindowId window = 0;
    float lx = 0, ly = 0;            // centre relative to the window's top-left corner
    float halfW = 10, halfH = 8, rotation = 0;
    float stick = 2;                 // s before it starts sliding
    float vy = 0;                    // slide speed
    float slid = 0;                  // px since the last trail bit
    float mass = 0;                  // drift area it carries (main splat only)
    float life = -1, maxLife = 1;    // trail bits: fading; main splat: -1
    float dpi = 1;
    int lastL = 0, lastT = 0;        // window position at the last layout
    std::uint8_t variant = 0;
};

class SplatSystem {
public:
    explicit SplatSystem(std::uint64_t seed) : rng_(seed ^ 0x5B1A7ull) {}

    // Released ball with velocity (vx, vy): splat it on the window under its throw?
    bool tryThrow(const SurfaceRegistry& reg, const Ball& b, float vx, float vy, ParticleSystem* fx);
    void step(float dt, const SurfaceRegistry& reg, ChunkSystem* chunks, ParticleSystem* fx);
    // New layout: drop splats of hidden / closed windows, knock them off on a jerk.
    void onLayout(const SurfaceRegistry& reg, float elapsed, ChunkSystem* chunks);
    void clear();

    // One batch per window carrying splats, clipped to its visible region (on the glass).
    void appendBatches(const SurfaceRegistry& reg, std::vector<SpriteBatch>& out) const;

    const std::vector<Splat>& splats() const { return splats_; }
    int mainCount() const;
    double splatted() const { return splatted_; }   // mass that went into splats
    double dropped() const { return dropped_; }     // mass that fell off as chunks
    double dust() const { return dust_; }           // trail bits / burst / removed with the window
    double liveMass() const;

private:
    void fallOff(Splat& s, float wx, float wy, float vx, float vy, ChunkSystem* chunks);

    std::vector<Splat> splats_;
    Rng rng_;
    double splatted_ = 0, dropped_ = 0, dust_ = 0;
    mutable std::vector<RectI> clip_;
};

}  // namespace snow
