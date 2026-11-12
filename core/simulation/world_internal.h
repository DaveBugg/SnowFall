// Access to the subsystems behind World::Impl for unit tests and diagnostics. Not part of
// the public contract used by the platform app layer.
#pragma once
#include "core/settings.h"
#include "interaction/snow_objects.h"
#include "interaction/icicles.h"
#include "interaction/snowplow.h"
#include "interaction/splats.h"
#include "simulation/festive.h"
#include "simulation/chunks.h"
#include "simulation/particles.h"
#include "simulation/weather.h"
#include "simulation/world.h"
#include "surfaces/surface_registry.h"

namespace snow {

// Tuning constants of the glue code in world.cpp.
inline constexpr float kShakeSpeedAt96 = 1200.0f;      // surface speed (px/s) that sheds snow ("Осыпать")
inline constexpr float kTeleportJumpAt96 = 600.0f;     // a single jump larger than this is a snap, not a shake
inline constexpr int kMaxDropFlakes = 120;             // flakes spawned when a window with snow closes
inline constexpr double kPlowMaxRunSeconds = 90.0;    // watchdog: abort a snowplow run that takes longer
// Shake to clear.
inline constexpr float kShakeMinLegAt96 = 25.0f;       // every leg of a shake moves at least this far
inline constexpr float kShakeShedFraction = 0.95f;     // share of the cap pieces thrown off
inline constexpr int kShakeMaxFlakes = 160;
// Falling snowmen.
inline constexpr float kSnowmanEvalPeriod = 3.0f;      // s between heavy-snow evaluations
inline constexpr float kHeavyColumnLevel = 0.8f;       // a column is "full" at this share of its cap
inline constexpr float kHeavyColumnShare = 0.6f;       // ... and the primary monitor is heavy at this share
inline constexpr int kHeavyMinColumns = 40;
inline constexpr float kSnowmanWaitJitterMin = 0.6f;   // wait = interval x uniform(0.6, 1.4) ...
inline constexpr float kSnowmanWaitJitterMax = 1.4f;
inline constexpr float kSnowmanMinWait = 60.0f;        // ... but never less than a minute
inline constexpr float kLandingSnowAt96 = 8.0f;        // without the heavy-snow rule: a drift run with
inline constexpr float kLandingWidthAt96 = 20.0f;      // >= 8 px of snow over >= 20 px to land on
inline constexpr int kMaxAutoSnowmen = 3;              // (default of accumulation.snowmenMaxAlive)
// Snowplow.
inline constexpr float kPlowEvalPeriod = 1.0f;         // s between fullness evaluations
inline constexpr float kPlowFullAverage = 0.5f;        // mean fill (h / cap) that counts as full
inline constexpr float kPlowFullColumnLevel = 0.6f;    // or: this share of columns ...
inline constexpr float kPlowFullColumnShare = 0.7f;    // ... at >= kPlowFullColumnLevel of their cap
inline constexpr float kPlowEmptyAverage = 0.15f;      // below this the full-time resets
// Umbrella cursor.
inline constexpr float kUmbrellaAt96 = 75.0f;          // dome radius (normal mode)
inline constexpr float kUmbrellaInteractAt96 = 45.0f;  // ... in interaction mode
inline constexpr float kSwipeSpeedAt96 = 1500.0f;      // pointer speed that whips up a gust
inline constexpr float kSwipeGustMaxAt96 = 900.0f;
inline constexpr float kSwipeRadiusAt96 = 140.0f;
// Tool gestures.
inline constexpr float kSwipeBrushSpeedAt96 = 600.0f;  // mitten: faster than this = brush (swipe)
inline constexpr float kGestureTravelAt96 = 25.0f;     // mitten: travel that decides swipe vs. sculpt
inline constexpr float kSculptDecideSeconds = 0.15f;   // mitten: held this long without a swipe = sculpt
inline constexpr float kSculptLeaveAt96 = 70.0f;       // moving this far from the snowman ends sculpting
inline constexpr float kSculptRubFullAt96 = 300.0f;    // rubbing at this speed doubles the growth (max x3)
inline constexpr float kScoopDepthAt96 = 20.0f;        // shovel capacity = shovel width x this
inline constexpr float kFlingMinAt96 = 450.0f;         // shovel: slower release = snow pushed aside (old clear)
inline constexpr float kFlingMaxAt96 = 2500.0f;
// Excluded windows.
inline constexpr float kExcludedMarginAt96 = 6.0f;     // "around": the field starts this far above / beside
inline constexpr float kExcludedRadiusAt96 = 10.0f;
inline constexpr float kExcludedBandAt96 = 40.0f;
// Thaw.
inline constexpr float kMeltSecondsPerPx = 30.0f;      // 1 px * dpi per 30 s (faster where thin)
inline constexpr float kWetnessMax = 0.6f;
inline constexpr float kWetnessRiseSeconds = 120.0f;
inline constexpr float kWetnessFallSeconds = 45.0f;
inline constexpr float kDripRatePerWindow = 0.25f;     // drops/s falling off each snowy window edge

// One snowplow route with its own timer and run (accumulation.snowplowAllTaskbars).
inline constexpr float kPlowRouteOffsetMax = 60.0f;     // s: non-primary routes start out of sync
inline constexpr float kPlowStaggerMin = 1.5f;          // manual call: routes start this far apart ...
inline constexpr float kPlowStaggerMax = 4.0f;
struct PlowRun {
    PlowRoute route;
    Snowplow own;               // the plow of a non-primary route
    bool main = false;          // the primary route: uses World's main plow (WorldInternals::plow)
    float fullSeconds = 0;      // "mostly full" time
    float offset = 0;           // extra delay of this route
    float pending = -1;         // manual call: seconds until it starts (< 0: none)
    int nextDir = 1;
    double startedAt = 0;
};

struct AutoEvents {
    double time = 0;              // simulated seconds
    float snowmanEval = 0;
    float snowmanCountdown = -1;  // < 0: not armed
    float lastWait = 0;           // the last wait drawn (diagnostics / tests)
    int snowmenDropped = 0;
    bool heavySnow = false;
    float plowEval = 0;
    float plowFullSeconds = 0;    // accumulated "mostly full" time of the primary taskbar drift
    int plowNextDir = 1;
};

struct WorldInternals {
    SurfaceRegistry& surfaces;
    ParticleSystem& particles;
    ObjectSystem& objects;
    Weather& weather;
    Snowplow& plow;
    AutoEvents& events;
    std::vector<PlowRun>& plowRuns;
    ChunkSystem& chunks;
    IcicleSystem& icicles;
    SplatSystem& splats;
    Fireworks& fireworks;
    float wetness;
    float scoop;          // shovel load (px^2)
    int gesture;          // World::Impl::Gesture
    Tool tool;            // currently selected tool
    bool interaction;     // interaction mode active
};

struct WorldTestAccess {
    static WorldInternals get(World& w);
    // Drop an auto snowman now (ignores the heavy-snow condition, not the target rules).
    static bool dropSnowman(World& w);
    // Evaluate the heavy-snow condition now.
    static bool heavySnow(World& w);
    // Draw a falling-snowman wait (s) with the current settings.
    static float snowmanWait(World& w);
};

}  // namespace snow
