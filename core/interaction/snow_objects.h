// Snowballs and snowmen. They live in the DESKTOP LAYER: only on the primary monitor and
// behind every window (FrameSnapshot::objects clip = primary monitor minus all windows).
//  * Ball: a held (dragged) or free (flying / rolling) snowball in WORLD coordinates.
//  * Pile: 1..3 balls resting on a surface, stored in the surface's LOCAL coordinates so it
//    moves, hides and dies with its window. A pile of >= 2 balls is a snowman.
//  * FallingSnowman: a ready-made snowman dropping from above the primary monitor onto a
//    full drift (accumulation.fallingSnowmen); becomes a Pile when it lands.
#pragma once
#include <cstdint>
#include <vector>

#include <string>

#include "core/skins.h"
#include "core/snapshot.h"
#include "core/types.h"
#include "simulation/rng.h"
#include "surfaces/surface_registry.h"

namespace snow {

class ParticleSystem;
class ChunkSystem;
class SplatSystem;
class IcicleSystem;

// Mass conservation of snowballs: ball area (pi r^2) = kPackFactor * drift area (px^2)
// removed from heightmaps. > 1 means the 2D ball is "fluffier" than the drift it came from,
// a deliberate gameplay simplification so that thin drifts still give usable balls. The
// factor is constant, so snow never appears from nothing.
inline constexpr float kPackFactor = 1.5f;
inline constexpr float kBallMinDiameterAt96 = 12.0f;
inline constexpr float kBallMaxDiameterAt96 = 80.0f;
// A released ball that gathered less than this fraction of the minimal ball's area is not a
// ball at all: it puffs into dust.
inline constexpr float kBallMinFill = 0.5f;
// Stacking rule: new ball radius must be within [kStackMinRatio, kStackMaxRatio] of the ball below.
inline constexpr float kStackMinRatio = 0.45f;
inline constexpr float kStackMaxRatio = 1.02f;
inline constexpr float kStackOverlap = 0.85f;        // centre distance = overlap * (r1 + r2)
inline constexpr float kBallGravityAt96 = 1500.0f;   // px/s^2
inline constexpr float kBallBounceSpeedAt96 = 260.0f;  // landing faster than this bounces
inline constexpr float kBallRestitution = 0.28f;
inline constexpr float kBallRollFriction = 2.5f;     // 1/s exponential decay of rolling speed
inline constexpr float kBallRestSpeedAt96 = 8.0f;
inline constexpr float kBallMaxThrowAt96 = 3000.0f;
inline constexpr float kPileBreakSpeedAt96 = 2200.0f;     // ball impact speed that shatters a pile
inline constexpr float kSurfaceBreakSpeedAt96 = 5000.0f;  // surface speed that shatters its piles
// Falling snowmen.
inline constexpr float kFallGravityAt96 = 1100.0f;
inline constexpr float kFallMaxSpeedAt96 = 1300.0f;
inline constexpr float kTiltRecovery = 7.0f;          // 1/s: landed snowmen right themselves
inline constexpr float kCraterDepth = 0.65f;          // fraction of the snow height removed at the centre
inline constexpr float kCraterRimShare = 0.6f;        // share of the crater snow pushed into rims (rest: splash)
// Knock-over (snowplow / shovel / fast hit): the base is shoved, squashes and crumbles; the
// balls above topple forward as a stack, then fly / roll on as free (fragile) balls.
inline constexpr float kCrushPushAt96 = 18.0f;          // base crumbles after being shoved this far
inline constexpr float kToppleAngle = 0.6f;             // ~35 degrees before the stack separates
inline constexpr float kToppleSecondsAt150 = 0.25f;     // ... reached in this time at 150 px/s (faster if harder)
inline constexpr float kToppleShatterSpeedAt96 = 420.0f;   // toppled balls shatter landing faster than this
inline constexpr float kCrumblePieces = 0.25f;          // share of the base thrown as soft chunks ahead
inline constexpr float kCrumbleDust = 0.05f;            // share puffed into dust (the rest: plow heap / shovel)
enum class KnockBy : std::uint8_t { None, Plow, Shovel, Hit };

// Detached balls (window jerked / shaken / closed).
inline constexpr float kBallShatterSpeedAt96 = 900.0f;   // impact speed that shatters a ball (~270 px drop)
inline constexpr float kDetachLagMin = 0.15f, kDetachLagMax = 0.3f;   // share of the window velocity kept
inline constexpr float kDetachMaxSpeedAt96 = 600.0f;
inline constexpr float kDebrisFadeSeconds = 1.5f;     // coal / carrot / sticks fade after landing

// Random look of a snowman (rolled once when a pile becomes a snowman).
struct SnowmanLook {
    bool set = false;
    std::int8_t hat = -1;      // -1 none, else Shape::Hat variant 0..3 (3 = Santa)
    std::int8_t scarf = -1;    // -1 none, else Shape::Scarf variant 0..3
    std::uint8_t arms = 0;     // 0 out, 1 both up, 2 one waving
    bool smile = false;        // 5 small coals in an arc
    float nose = 0.5f;         // carrot half length / head radius
    std::int8_t skin = -1;     // index into kSnowmanSkins, -1 = random parts
    std::uint8_t hatAcc = kNoPart;   // Accessory in the hat slot (Crown, TricornHat, ...)
    std::uint8_t acc[4]{kNoPart, kNoPart, kNoPart, kNoPart};   // other accessories
    bool googly = false;       // googly eyes instead of coal
};

// Where an accessory sits on a snowman, relative to the head centre, for head radius rh.
// The sprite's halfW/halfH is the bounding box of the art; it is centred at (dx, dy).
// Broom is relative to the end of the +X arm; Flower to the hat side (no hat: see sprites).
struct AccessoryPlacement {
    float dx = 0, dy = 0, halfW = 1, halfH = 1, rotation = 0;
};
AccessoryPlacement accessoryPlacement(Accessory a, float rh);
bool isHatSlot(Accessory a);
struct Pile;
// Angle of the waving (+X) arm of a pile at this moment (kArmRestAngle when not waving).
float waveArmAngle(const Pile& p);
// Draw layer (renderer convention): 1 neck, 2 face, 3 eyewear, 4 headphones, 5 hats, 6 flower/broom.
int accessoryLayer(Accessory a);
// Skin index for a settings value ("random" or an id) -> -1 for random.
int skinIndex(const std::string& id);
// Stick arms: shoulders on the ball below the head at its upper third, mirrored. Angles are
// for the +X arm (y down: negative = raised); the -X arm is the mirror image.
inline constexpr float kArmRestAngle = -0.35f;      // ~20 deg above horizontal, pointing outwards
inline constexpr float kArmUpAngle = -0.9f;         // "both up" pose
inline constexpr float kWaveCenter = -0.524f;       // wave swings -5 .. -55 deg (centre -30, +-25)
inline constexpr float kWaveAmp = 0.436f;
inline constexpr float kWaveFlick = 0.061f;         // ~3.5 deg overshoot at the top of each swing
inline constexpr float kArmHeadClearance = 0.25f;   // arm tips stay this many head radii outside the head
inline constexpr float kHatChance = 0.85f;
inline constexpr float kScarfChance = 0.6f;
inline constexpr float kSmileChance = 0.5f;
inline constexpr float kMeltRadiusPerSecAt96 = 1.0f / 15.0f;   // thaw: snowmen shrink 1 px per 15 s
inline constexpr float kCollapseRadiusAt96 = 7.0f;             // bottom ball smaller: it collapses

// Mitten sculpting: press and hold on a drift -> base, middle, head grow out of the snow.
inline constexpr float kSculptBaseRadiusAt96 = 22.0f;   // base ball target radius
inline constexpr float kSculptMiddleShare = 0.72f;      // middle / base radius
inline constexpr float kSculptHeadShare = 0.5f;         // head / base radius
inline constexpr float kSculptRateAt96 = 500.0f;        // px^2 of drift gathered per second (x dpi^2)
inline constexpr float kSculptGatherAt96 = 40.0f;       // gathers from base radius + this on each side
inline constexpr float kSculptPatSeconds = 0.2f;        // a pat (dust puff) every ...

struct SculptState {
    bool active = false;
    std::uint32_t pile = 0;
    SurfaceId surface = 0;
    float localX = 0;
    float area[3]{};       // drift area gathered into each ball
    float target[3]{};     // radius at which each stage is complete
    float patAcc = 0;
    float dpi = 1;
    int completed = 0;     // fully sculpted snowmen (diagnostics)
};

struct Ball {
    std::uint32_t id = 0;
    float x = 0, y = 0, vx = 0, vy = 0;
    float r = 6;
    float rotation = 0;
    float collected = 0;       // drift area (px^2) this ball is made of
    float dpi = 1;
    bool held = false;
    SurfaceId rolling = 0;     // surface it is rolling on (0 = airborne)
    SurfaceId exclude = 0;     // surface it fell off (no collision with it)
    bool fragile = false;      // toppled off a snowman: shatters on a harder landing
};

// A snowman decoration (coal, carrot, stick) falling on its own after the snowman came apart.
struct Debris {
    float x = 0, y = 0, vx = 0, vy = 0;
    float rotation = 0, angVel = 0;
    float halfW = 1, halfH = 1;
    float dpi = 1;
    float fade = -1;           // >= 0: landed, seconds until it is gone
    Shape shape = Shape::Coal;
    SurfaceId exclude = 0;
    float gravity = 1;         // scarf: flutters down slower
    float sway = 0, swayPhase = 0, baseVx = 0;
    int bounces = 1;           // bounces once on the drift before lying still
    std::uint8_t variant = 0;
};

struct Pile {
    std::uint32_t id = 0;
    SurfaceId surface = 0;
    float localX = 0;          // centre line, relative to the surface's left edge
    float sink = 0;            // snow height it came to rest on (it never rises above that)
    float dpi = 1;
    float tilt = 0;            // radians, decays to 0 (falling snowmen right themselves)
    float pushed = 0;          // px pushed by the snowplow so far
    bool autoSpawned = false;  // dropped from the sky (counts toward kMaxAutoSnowmen)
    int n = 0;
    float r[3]{};              // bottom to top
    SnowmanLook look;
    float waveIn = 4.0f;       // waving arm: seconds until the next wave
    float waveT = 0;           // > 0: waving, seconds left
    float waveDur = 0;         // length of the current wave (2-3 swings)
    float wavePeriod = 1.6f;   // one up-down swing
    float lastCx = -1e30f;     // googly-eye jiggle when the snowman is moved
    float jiggle = 0, jigPhase = 0;
    bool bare = false;         // decorations already flew off
    KnockBy knock = KnockBy::None;   // being knocked over
    std::int8_t knockDir = 1;
    float knockSpeed = 0, knockT = 0, knockPush = 0, squash = 0;
};

struct FallingSnowman {
    std::uint32_t id = 0;
    float x = 0, y = 0;        // bottom centre, world px
    float vy = 0;
    float tilt = 0, swayPhase = 0, swayAmp = 0, startY = 0;
    float dpi = 1;
    SurfaceId target = 0;      // 0: lost its target, falls freely
    float targetLocalX = 0;
    int n = 0;
    float r[3]{};
    SnowmanLook look;
};

struct PileGeom {
    int n = 0;
    float cx = 0;
    float base = 0;            // y of the bottom of the lowest ball
    float cy[3]{};
    float r[3]{};
};
PileGeom pileGeom(const Pile& p, const Surface& s);
PileGeom stackGeom(float cx, float base, const float* radii, int n);

struct CraterResult {
    float removed = 0;   // px^2 taken out of the crater
    float rims = 0;      // px^2 pushed into the rims
};
// Crater under a landing snowman of bottom radius r0 at localX: removes snow under it, pushes
// kCraterRimShare of it into small rims on both sides (may exceed the cap a little).
CraterResult makeCrater(Surface& s, float localX, float r0);

class ObjectSystem {
public:
    static float radiusFor(float collectedArea, float dpiScale);

    bool holding() const { return heldIndex() >= 0; }
    const Ball* heldBall() const;
    // Grab a resting pile's top ball or a free ball under (x, y). Returns true on success.
    bool grabAt(float x, float y, const SurfaceRegistry& reg);
    // Start a new (empty) ball held at (x, y).
    void startBall(float x, float y, float dpiScale);
    // Drag the held ball along the segment, collecting snow from the drifts it touches.
    void moveHeld(float x0, float y0, float x1, float y1, SurfaceRegistry& reg);
    // Let go of the held ball with the given velocity: stack it, free it, or puff it.
    void release(float vx, float vy, const SurfaceRegistry& reg, ParticleSystem* particles);

    void step(float dt, SurfaceRegistry& reg, ParticleSystem* particles);

    // drop = accumulation.dropSnowOnClose: the balls fall off; otherwise they vanish.
    void onSurfacesRemoved(const std::vector<Surface>& removed, ParticleSystem* particles, bool drop);
    // The window under these piles was jerked / shaken: each pile comes off with probability
    // `fraction`; its balls become free balls and its decorations debris, all lagging behind
    // the window velocity. (offX, offY): where the surface was before the move.
    int detachPilesOn(const Surface& s, float fraction, float wvx, float wvy, float offX, float offY);
    void setChunks(ChunkSystem* chunks) { chunks_ = chunks; }
    void setSplats(SplatSystem* splats) { splats_ = splats; }
    void setIcicles(IcicleSystem* icicles) { icicles_ = icicles; }
    void setSeed(std::uint64_t seed) { rng_.reseed(seed ^ 0x0B1EC7ull); }
    // Late December / early January: Santa hats are much more likely.
    void setFestiveSeason(bool on) { festiveSeason_ = on; }
    // interaction.snowmanSkin: -1 = random (a random skin ~70 %, random parts ~30 %).
    void setSkin(int skinIndex) { skin_ = skinIndex; }
    static void applySkin(SnowmanLook& look, int skinIndex);
    // Thaw: snowmen lose kMeltRadiusPerSecAt96 * melt px/s and finally collapse into the drift.
    void setMelt(float melt) { melt_ = melt; }
    SnowmanLook rollLook();
    void ensureLook(Pile& p) {
        if (p.n >= 2 && !p.look.set) p.look = rollLook();
    }
    int collapsed() const { return collapsed_; }
    const std::vector<Debris>& debris() const { return debris_; }
    int shattered() const { return shattered_; }
    void onSurfaceMoved(const LayoutEvents::Moved& ev, bool teleport, const SurfaceRegistry& reg,
                        ParticleSystem* particles);
    void onSurfaceResized(const Surface& s);
    void onSurfaceRescaled(SurfaceId id, float ratio);
    // Break every pile standing on the surface (shake to clear).
    void breakPilesOn(SurfaceId id, const SurfaceRegistry& reg, ParticleSystem* particles);
    void breakPile(std::size_t index, const SurfaceRegistry& reg, ParticleSystem* particles);
    // Start knocking pile `index` over in direction dir (+-1) at `speed` px/s. The plow pushes
    // the base itself (knockPush); otherwise the base slides on by its own momentum.
    void knock(std::size_t index, const Surface& s, int dir, float speed, KnockBy by, ParticleSystem* particles);
    // Snow of crumbled bases waiting for the plow heap / the shovel load (taken = reset).
    float takeCrumbled(KnockBy by) {
        const float m = pending_[static_cast<int>(by)];
        pending_[static_cast<int>(by)] = 0;
        return m;
    }
    double crumbleDust() const { return crumbleDust_; }
    double lostBalls() const { return lostBalls_; }
    int crumbled() const { return crumbled_; }
    std::vector<Debris>& debrisMut() { return debris_; }
    void clear();

    // Falling snowmen.
    void dropSnowman(const Surface& target, float localX, int n, const float* radii, float startBottomY,
                     float dpi, Rng& rng);
    int autoSnowmenAlive() const;   // falling + landed auto snowmen
    const std::vector<FallingSnowman>& falling() const { return falling_; }
    std::uint32_t lastLandedPile() const { return lastLanded_; }
    CraterResult lastCrater() const { return lastCrater_; }

    const std::vector<Ball>& balls() const { return balls_; }
    const std::vector<Pile>& piles() const { return piles_; }
    std::vector<Ball>& balls() { return balls_; }
    std::vector<Pile>& piles() { return piles_; }
    int snowballCount() const;
    int snowmanCount() const;   // landed snowmen (incl. auto); falling ones are not counted

    // Appends the sprites of piles, falling snowmen and balls (desktop layer, no clip).
    void appendSprites(const SurfaceRegistry& reg, std::vector<SpriteInstance>& out) const;
    // Hit rects of grabbable objects, restricted to their visible (desktop-layer) parts.
    void appendHitRects(const SurfaceRegistry& reg, std::vector<RectI>& out) const;

    // Mitten sculpting. sculptStart: (x, y) on a drift in the desktop layer -> true.
    bool sculptStart(SurfaceRegistry& reg, float x, float y);
    // rub >= 1 speeds the growth up (rubbing back and forth).
    void sculptStep(float dt, SurfaceRegistry& reg, ParticleSystem* particles, float rub);
    void sculptEnd(SurfaceRegistry& reg, ParticleSystem* particles);
    const SculptState& sculpt() const { return sculpt_; }
    // Drift under (x, y) within `tol` px of its snow top (active, desktop layer, with snow).
    static bool findDriftAt(const SurfaceRegistry& reg, float x, float y, float tol, int& surfaceIndex, int& column);

    // Direct placement (tests / internal): a single resting ball on a surface.
    Pile& addPile(const Surface& s, float localX, float radius);

private:
    int heldIndex() const;
    bool stackable(const Pile& p, float radius) const;
    void settle(Ball& b, const Surface& s);
    void puff(const Ball& b, ParticleSystem* particles);
    void stepFalling(float dt, SurfaceRegistry& reg, ParticleSystem* particles);
    void stepDebris(float dt, const SurfaceRegistry& reg);
    void shatter(const Ball& b, SurfaceRegistry& reg, const SurfaceHit& hit, ParticleSystem* particles);
    void land(FallingSnowman& f, Surface& s, int column, ParticleSystem* particles);
    enum class Contact { None, Consumed, Deflected };
    Contact pileContact(Ball& b, float x1, float y1, const SurfaceRegistry& reg, ParticleSystem* particles);

    std::vector<Ball> balls_;
    std::vector<Pile> piles_;
    std::vector<FallingSnowman> falling_;
    std::vector<Debris> debris_;
    ChunkSystem* chunks_ = nullptr;
    SplatSystem* splats_ = nullptr;
    IcicleSystem* icicles_ = nullptr;
    // Icicles along a ball's path: each one broken costs the ball 20-35 % of its speed; a slow
    // ball bumps off.
    void ballIcicles(Ball& b, float x0, float y0, float x1, float y1, float minSpeed, float knockShare,
                     const SurfaceRegistry& reg, ParticleSystem* particles);
    Rng rng_{0x0B1EC7ull};
    int shattered_ = 0;
    int collapsed_ = 0;
    int crumbled_ = 0;
    float pending_[4]{};
    double crumbleDust_ = 0, lostBalls_ = 0;
    void popDecorations(const std::vector<SpriteInstance>& parts, int dir, float strength, float dpi, SurfaceId exclude);
    bool stepKnock(std::size_t index, Surface& s, float dt, ParticleSystem* particles);   // true: pile erased
    bool festiveSeason_ = false;
    int skin_ = -1;
    SculptState sculpt_;
    Pile* findPile(std::uint32_t id) {
        for (Pile& p : piles_)
            if (p.id == id) return &p;
        return nullptr;
    }
    void dressPop(Pile& p, const Surface& s, ParticleSystem* particles);
    float melt_ = 0;
    std::uint32_t nextId_ = 1;
    std::uint32_t lastLanded_ = 0;
    CraterResult lastCrater_;
};

}  // namespace snow
