// Toy snowplow that clears taskbar drifts (accumulation.snowplow). It drives along a ROUTE:
// one taskbar surface, or several taskbars side by side at the same height on adjacent
// monitors, driven across as one continuous line (accumulation.snowplowAllTaskbars).
// All plow coordinates are WORLD x (virtual-desktop px).
//
// Snow model (2D profile):
//  * Every column the plow body passes is scraped down to a thin, uneven residual layer
//    (0..kResidualAt96 px with noise, plus occasional 2..5 px clods); the scraped snow is
//    carried by the plow (carried()).
//  * The carried snow forms a HEAP (windrow) against the blade: crest at the blade face
//    (height H <= blade height), an optional short plateau P, a rounded crest and a slope at
//    the angle of repose (kReposeSlope) ending in a soft toe. The heap is UNIONED with the
//    undisturbed drift ahead: drawn height = max(drift, heap). H / P are solved every step
//    so that the area the heap adds above the drift equals carried() exactly — the heap gets
//    taller and longer as the plow collects snow; its shape comes from volume + repose slope.
//  * Once H reaches the blade top and the plateau its maximum, the excess spills over the
//    blade (dust tumbling back over the top, clumps falling off the taskbar front edge).
//  * The face is lumpy (noise fixed relative to the blade), small clumps occasionally roll
//    forward and settle in the drift ahead (real deposits), and it wobbles with the bounce.
//  * The part of the heap that sticks out past the end of the route is shoved off as
//    falling clumps; at the end the plow pauses while the rest of the heap falls off
//    (large clumps first, ~kDumpSeconds), then drives off-screen.
// The heap itself is not stored in the heightmap (relaxation would smear it backwards);
// it is added to the drift's heights in the snapshot (applyHump).
#pragma once
#include <cstdint>
#include <vector>

#include "core/snapshot.h"
#include "core/types.h"
#include "simulation/rng.h"

namespace snow {

class SurfaceRegistry;
class ObjectSystem;
class ParticleSystem;
struct Surface;

inline constexpr float kPlowHalfWAt96 = 46.0f;
inline constexpr float kPlowHalfHAt96 = 30.0f;
inline constexpr float kPlowSpeedAt96 = 150.0f;      // px/s
inline constexpr float kPlowPushBreakAt96 = 40.0f;   // (unused since the knock-over; see kCrushPushAt96)
inline constexpr float kPlowImpactDip = 0.3f;        // speed drop when the blade hits a snowman ...
inline constexpr float kPlowImpactSeconds = 0.25f;   // ... for this long
inline constexpr float kPlowObjectSpill = 0.12f;     // share of a crumbled snowman spilling over the blade
inline constexpr float kBladeHeightShare = 0.88f;    // heap limit = share * sprite height (52.8 px at 96 DPI)
inline constexpr float kReposeSlope = 0.7f;          // ~35 degrees
inline constexpr float kPlateauMaxShare = 1.5f;      // max plateau length = share * blade height
inline constexpr float kResidualAt96 = 1.4f;         // max thin residual layer behind the blade
inline constexpr float kDumpSeconds = 0.7f;          // heap falling off the end
inline constexpr float kDumpPauseSeconds = 0.9f;     // plow waits at the end before leaving
inline constexpr float kRouteJoinYAt96 = 2.0f;       // taskbars this close in height (and touching) join
inline constexpr float kRouteJoinGap = 2.0f;         // ... with at most this horizontal gap

// One plow route: taskbar surfaces left to right forming a continuous line.
struct PlowRoute {
    std::uint64_t key = 0;               // identity (from the surface ids)
    std::vector<SurfaceId> surfaces;     // left to right
    std::vector<RectI> monitors;         // monitors the route runs over (where the plow is drawn)
    float left = 0, right = 0;           // world x span
    float top = 0;                       // the edge's y (first surface)
    float dpi = 1;                       // the largest dpi along the route (plow size / speed)
    bool primary = false;                // contains the primary monitor's taskbar
    bool sameAs(const PlowRoute& o) const {
        return key == o.key && left == o.left && right == o.right && top == o.top && dpi == o.dpi;
    }
};
// Routes for the current surfaces: every taskbar (all = true) or only the primary one.
std::vector<PlowRoute> buildPlowRoutes(const SurfaceRegistry& reg, bool all);

class Snowplow {
public:
    enum class Phase : std::uint8_t { Off, Driving, Dumping, Leaving };

    // The taskbar surface of the primary monitor, or nullptr.
    static const Surface* primaryTaskbar(const SurfaceRegistry& reg);

    explicit Snowplow(std::uint64_t seed = 0x9100Bull) : rng_(seed) {}

    bool active() const { return phase_ != Phase::Off; }
    Phase phase() const { return phase_; }
    // Start a run along `route` now (no-op if running). dir: +1 enters at the left end and
    // drives right, -1 the opposite.
    bool start(const PlowRoute& route, int dir);
    // Compatibility: a run along the primary taskbar alone.
    bool start(const SurfaceRegistry& reg, int dir);
    void abort() {
        if (active()) ++renderVersion_;
        phase_ = Phase::Off;
        carried_ = 0;
        H_ = P_ = 0;
    }
    // The route changed under a running plow: drop the heap where it is and stop.
    void abortDump(ParticleSystem* fx);
    void step(float dt, SurfaceRegistry& reg, ObjectSystem& objects, ParticleSystem* fx);

    const PlowRoute& route() const { return route_; }
    SurfaceId surface() const { return route_.surfaces.empty() ? 0 : route_.surfaces.front(); }
    bool onSurface(SurfaceId id) const;
    int dir() const { return dir_; }
    float centerLocal() const { return x_; }   // world x of the sprite centre
    float frontLocal() const { return x_ + static_cast<float>(dir_) * halfW_; }   // world x of the blade
    int completedRuns() const { return runs_; }
    float lastExitLocal() const { return lastExit_; }   // world x where the last run left
    // Bumps every step while active (the heap moves), so drift versions can include it.
    std::uint32_t renderVersion() const { return renderVersion_; }

    // Heap state / accounting of the current run (px, px^2).
    float bladeHeight() const { return bladeH_; }
    float heapHeight() const { return H_; }
    float heapPlateau() const { return P_; }
    float heapLength() const;            // distance from the blade face to the toe
    float carried() const { return carried_; }
    float collected() const { return collected_; }        // scraped off the drift
    float spilled() const { return spilled_; }            // over the blade
    float dumped() const { return dumped_; }              // off the end of the route
    float depositedAhead() const { return deposited_; }   // rolled-off clumps that settled ahead
    float fromObjects() const { return objectsIn_; }      // snowmen / balls crumbled into the heap
    float speedFactor() const { return dipT_ > 0 ? 1.0f - kPlowImpactDip : 1.0f; }
    int impacts() const { return impacts_; }

    void appendSprite(const SurfaceRegistry& reg, std::vector<SpriteInstance>& out) const;
    // Unions the heap with `heights` (a copy of the surface's columns): h = max(h, heap).
    void applyHump(const Surface& s, std::vector<float>& heights) const;

private:
    float profile(float d, float H, float P) const;   // heap height at distance d ahead of the face
    // Drift height at world x along the route; inRoute = false past its ends.
    float heightAt(const SurfaceRegistry& reg, float x, bool& inRoute) const;
    // Area the heap (H, P) adds above the drift; `offEnd` receives the part past the route end.
    float heapArea(const SurfaceRegistry& reg, float H, float P, float* offEnd) const;
    void solve(const SurfaceRegistry& reg, float& overflow, float& offEnd);
    float residualAt(long long column) const;
    void scrape(SurfaceRegistry& reg, float lo, float hi);
    void emitSpill(float dt, ParticleSystem* fx);
    void emitEndFall(float area, float bigness, ParticleSystem* fx);
    void emitFallAt(float x, float area, float bigness, ParticleSystem* fx);

    Phase phase_ = Phase::Off;
    PlowRoute route_;
    std::vector<const Surface*> segs_;   // route surfaces, resolved each step
    int dir_ = 1;
    float x_ = 0;            // sprite centre, world px
    float step_ = 3;         // sampling step along the route
    float halfW_ = 46, halfH_ = 30, speed_ = 150, dpi_ = 1, bladeH_ = 52.8f;
    float H_ = 0, P_ = 0;
    float carried_ = 0, collected_ = 0, spilled_ = 0, dumped_ = 0, deposited_ = 0;
    float spillQueue_ = 0, spillBudget_ = 0, rollAcc_ = 0;
    float dumpTimer_ = 0, dumpRate_ = 0, dumpTotal_ = 0;
    float bounce_ = 0;
    float dipT_ = 0, objectsIn_ = 0;
    int impacts_ = 0;
    std::uint64_t runSeed_ = 0;
    int runs_ = 0;
    float lastExit_ = 0;
    std::uint32_t renderVersion_ = 0;
    Rng rng_;
};

}  // namespace snow
