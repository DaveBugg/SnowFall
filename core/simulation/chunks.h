// ChunkSystem: pieces of a snow cap that break off a window ("Осыпать" on a sharp move,
// shake to clear, a closed window dropping its snow). A substantial cap is fractured along
// x into short (8..40 px) and long (60..250 px) slabs, preferably at valleys of the lumpy
// cap and at the rounded corners. Each slab carries the drift area it was cut from; slabs
// lag behind the window's jerk, tumble, crack mid-air, and shatter on impact: part of their
// snow is deposited where they land, smaller pieces bounce, the rest becomes dust.
// Thin snow is shed as flakes instead.
//
// Mass bookkeeping (px^2 of drift area): shedChunkMass() == liveMass() + deposited() + dust()
// + lost() at all times (up to float rounding).
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

inline constexpr int kMaxChunks = 150;
inline constexpr float kChunkMinCapAt96 = 6.0f;      // average cap thickness that breaks into slabs
inline constexpr float kChunkThinAt96 = 2.0f;        // thinner columns are not part of any slab
inline constexpr float kChunkFractureDust = 0.08f;   // share of a slab that crumbles at its fracture lines
inline constexpr float kChunkGravityAt96 = 1300.0f;
inline constexpr float kChunkSmallHalfWAt96 = 6.0f;  // smaller chunks just deposit on impact

struct Chunk {
    std::uint32_t id = 0;
    float x = 0, y = 0, vx = 0, vy = 0;
    float rotation = 0, angVel = 0;
    float halfW = 4, halfH = 3;
    float mass = 0;            // drift area it is made of
    float crackTimer = -1;     // > 0: splits into 2..3 parts when it runs out
    float dpi = 1;
    SurfaceId exclude = 0;     // the surface it slid off (no collision with it)
    std::uint8_t variant = 0;
    bool soft = false;         // flung loose snow: lands as a mound (most of it deposits)
};

struct ShedResult {
    int chunks = 0;
    float chunkMass = 0;
    float flakeMass = 0;       // thin parts shed as flakes
    int piecesKept = 0;
};

class ChunkSystem {
public:
    explicit ChunkSystem(std::uint64_t seed) : rng_(seed ^ 0xC4C4u) {}

    // True when the cap is thick enough to break into slabs.
    static bool substantial(const Surface& s);
    // Fracture the cap of `s` and shed each piece with probability `fraction` (removed from
    // the heightmap). Window velocity (wvx, wvy) is partially inherited; (offX, offY) shifts
    // the spawn position (where the snow was before the move). fx == null: the snow simply
    // disappears (animations off).
    ShedResult shed(Surface& s, float fraction, float wvx, float wvy, float offX, float offY, ParticleSystem* fx);

    void step(float dt, SurfaceRegistry& reg, ParticleSystem* fx);
    // Shatter a body (e.g. a falling snowball) of c.mass drift area that hit surface
    // `surfaceIndex` at (hitX, hitY) like a slab: deposit part, smaller pieces, dust.
    void shatterAt(const Chunk& c, SurfaceRegistry& reg, int surfaceIndex, float hitX, float hitY, ParticleSystem* fx);
    // A free slab of `mass` drift area (e.g. a splat falling off a window). nullptr when the
    // cap is reached (its mass is then counted as dust).
    Chunk* spawnFree(float x, float y, float halfW, float halfH, float mass, float vx, float vy, float dpi,
                     SurfaceId exclude);
    void clear();

    const std::vector<Chunk>& chunks() const { return chunks_; }
    std::vector<Chunk>& chunks() { return chunks_; }
    std::size_t count() const { return chunks_.size(); }
    void appendSprites(std::vector<SpriteInstance>& out, float opacity) const;

    double liveMass() const;
    double shedChunkMass() const { return shed_; }
    double deposited() const { return deposited_; }
    double dust() const { return dust_; }
    double lost() const { return lost_; }
    int impacts() const { return impacts_; }
    int cracks() const { return cracks_; }

private:
    Chunk* add(float x, float y, float halfW, float halfH, float mass, float dpi);
    void crack(std::size_t index, ParticleSystem* fx);
    void impact(Chunk c, SurfaceRegistry& reg, int surfaceIndex, float hitX, float hitY, ParticleSystem* fx);
    void puff(float x, float y, float dpi, int n, ParticleSystem* fx);

    std::vector<Chunk> chunks_;
    std::vector<Chunk> pending_;   // created during step (children)
    Rng rng_;
    std::uint32_t nextId_ = 1;
    double shed_ = 0, deposited_ = 0, dust_ = 0, lost_ = 0;
    int impacts_ = 0, cracks_ = 0;
};

}  // namespace snow
