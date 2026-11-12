// FrameSnapshot: immutable render data produced by the simulation thread and consumed by
// the render thread. All positions are physical pixels in virtual-desktop space; the
// renderer maps them onto each monitor's overlay.
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/types.h"

namespace snow {

enum class Shape : std::uint8_t {
    Flake = 0,   // falling snowflake / dust; `variant` selects one of kFlakeVariants procedural styles
    Snowball,    // shaded white ball (also snowman body parts)
    Coal,        // dark disc (snowman eyes, buttons)
    Carrot,      // orange cone; tip points along +X rotated by `rotation`
    Stick,       // brown twig (snowman arm); long axis along +X rotated by `rotation`
    Mitten,      // tool cursor
    Shovel,      // tool cursor
    Ring,        // thin circle outline (brush radius indicator)
    Snowplow,    // toy snowplow truck, blade in front. variant 0 = drives right (+X), 1 = drives left
    SnowChunk,   // broken-off piece of a snow cap: slab of packed snow, any aspect ratio
                 // (halfW 4..150, halfH 3..30). Flat-ish bottom (the side that lay on the window
                 // edge, local -Y is up), lumpy rounded top; variant 0..3 = different outlines.
                 // Particle-layer (foreground) like flakes.
    // ---- round 6 ----
    Icicle,      // translucent tapered ice, hangs DOWN: wide root at local -Y (top), tip at +Y.
                 // halfW 1.5..6, halfH 4..40. variant 0..3 = shape variations (drips/bulges).
    Spark,       // glowing firework spark with a short motion tail along -velocity direction
                 // (renderer may use vx/vy); additive-looking bright core. variant 0..7 = colour:
                 // 0 gold, 1 red, 2 green, 3 blue, 4 violet, 5 white, 6 orange, 7 cyan.
    Bulb,        // garland light bulb, base/cap at local -Y (hangs from the wire). variant 0..7 =
                 // colour (same table as Spark) + 8 if ON (glowing halo) — i.e. variant 0..15.
    GarlandWire, // dark green wire segment, long axis along +X, slight sag drawn by renderer.
                 // halfW = half segment length, halfH = sag depth (1..6 px).
    Splat,       // snowball smashed on a window pane: irregular flattened snow patch with a few
                 // radial blobs; halfW/halfH any. variant 0..3. Drawn clipped to the hit window.
    Droplet,     // melt-water drop (thaw): small clear-blue teardrop, tip at local -Y. variant 0..1
                 // (0 = falling drop, 1 = drop hanging/forming).
    Hat,         // snowman hat, brim at local +Y (bottom). variant 0 top hat (black), 1 bucket
                 // (tin grey), 2 knitted beanie (red with white pompom), 3 Santa hat (red/white).
    Scarf,       // snowman scarf wrapped around the neck, hanging tail on the +X side.
                 // variant 0..3 = colours: 0 red, 1 green/white stripes, 2 blue, 3 yellow.
    // ---- round 7 ----
    Accessory,   // snowman accessory; variant = snow::Accessory (core/skins.h), each described
                 // there with its anchor. Upright art (rotation applied around the centre).
};
inline constexpr int kFlakeVariants = 4;  // 0 = soft dot, 1..3 = crystal styles

struct SpriteInstance {
    float x = 0, y = 0;        // centre
    float vx = 0, vy = 0;      // px/s, used by the renderer to extrapolate between sim steps
    float halfW = 1, halfH = 1;  // half extents in px (before rotation)
    float rotation = 0;        // radians
    float opacity = 1;         // 0..1
    Shape shape = Shape::Flake;
    std::uint8_t variant = 0;
};

// A group of sprites sharing one clip region. Empty `clip` = not clipped.
// Clip rects are in virtual-desktop px and do not overlap each other.
struct SpriteBatch {
    std::vector<RectI> clip;
    std::vector<SpriteInstance> sprites;
};

// One snow drift lying on a horizontal edge (window top, taskbar top, screen bottom).
// Column i is at x = x0 + i * step and rises `heights[i]` px above `baseY` (towards -Y).
// The renderer draws a smooth filled profile, visible only inside `clip`.
struct DriftStrip {
    SurfaceId id = 0;
    float x0 = 0;
    float baseY = 0;
    float step = 3;
    float dpiScale = 1;
    float opacity = 1;               // 0..1, multiplies the whole drift (settings: driftOpacityPercent)
    float wetness = 0;               // 0..1, thaw: greyer/glossier, slightly translucent wet snow
    std::uint32_t version = 0;       // bumps whenever `heights` content changes
    std::vector<float> heights;
    std::vector<RectI> clip;         // visible parts (occlusion by windows above already removed);
                                     // EMPTY = fully occluded, nothing drawn (unlike SpriteBatch)
};

struct SimStats {
    std::uint32_t particles = 0;
    std::uint32_t surfaces = 0;
    std::uint32_t snowballs = 0;
    std::uint32_t snowmen = 0;
    float stepMs = 0;                // average simulation step cost
};

struct FrameSnapshot {
    std::uint64_t frame = 0;
    std::chrono::steady_clock::time_point taken{};  // when the sim state was captured
    std::vector<DriftStrip> drifts;                 // draw first
    // Snowballs / snowmen / snowplow. They live in the DESKTOP LAYER: only on the primary
    // monitor and behind every window (clip = primary monitor minus all window rects).
    std::vector<SpriteBatch> objects;
    SpriteBatch particles;                          // falling snow + dust, unclipped; opacity already applied
    SpriteBatch cursor;                             // tool cursor / brush ring, unclipped, drawn last
    // Rectangles where a left-button press should be captured by the interaction controller
    // (visible snow and grabbable objects). Empty when interaction mode is off.
    std::vector<RectI> hitRegions;
    SimStats stats;
};

// Latest-value mailbox between the simulation thread (writer) and render/UI threads (readers).
class SnapshotExchange {
public:
    void publish(std::shared_ptr<const FrameSnapshot> s) {
        std::lock_guard<std::mutex> lk(m_);
        latest_ = std::move(s);
    }
    std::shared_ptr<const FrameSnapshot> latest() const {
        std::lock_guard<std::mutex> lk(m_);
        return latest_;
    }

private:
    mutable std::mutex m_;
    std::shared_ptr<const FrameSnapshot> latest_;
};

}  // namespace snow
