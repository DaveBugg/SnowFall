// Snow-removal primitives used by the mitten, the shovel and the snowball collector.
// They only touch visible (non-blocked) columns of active surfaces.
#pragma once
#include "core/settings.h"
#include "core/snapshot.h"
#include "core/types.h"

namespace snow {

class SurfaceRegistry;
class ParticleSystem;

// Removes the snow lying above the lower arc of the disc (x, y, radius): everything the
// disc touches plus what would be left hanging over it. Stops once `maxArea` px^2 were
// removed. Returns the removed area; `contact` (optional) receives a point on the snow that
// was touched.
float brushDisc(SurfaceRegistry& reg, float x, float y, float radius, float maxArea, Vec2* contact);

// Clears columns within halfWidth of x down to the base on every surface whose drift is
// within `reach` px (vertically) of y. Returns the removed area.
float shovelBand(SurfaceRegistry& reg, float x, float y, float halfWidth, float reach, Vec2* contact);

// Shovel scoop: takes up to `room` px^2 from the columns within halfWidth of x (nearest
// first) on drifts within `reach` of y. Returns the area taken.
float scoopBand(SurfaceRegistry& reg, float x, float y, float halfWidth, float reach, float room, Vec2* contact);
// Scoop along a stroke into `load` (up to `capacity`); once the shovel is full it clears
// like the old shovel (snow pushed aside as dust). Returns the area removed from drifts.
float scoopStroke(SurfaceRegistry& reg, float x0, float y0, float x1, float y1, float width, float& load,
                  float capacity, ParticleSystem* dust, float dpiScale);

// Strokes: apply the tool along the segment (x0,y0)-(x1,y1) without gaps. `dust` may be
// null (animations off). Return the removed area.
float mittenStroke(SurfaceRegistry& reg, float x0, float y0, float x1, float y1, float radius,
                   ParticleSystem* dust, float dpiScale);
float shovelStroke(SurfaceRegistry& reg, float x0, float y0, float x1, float y1, float width,
                   ParticleSystem* dust, float dpiScale);

// A few dust flakes flying up and out from (x, y); the amount grows with `area`.
void emitDust(ParticleSystem& particles, float x, float y, float area, float dpiScale);

// Tool cursor (the system cursor is hidden in interaction mode, so this IS the cursor).
// The renderer draws Mitten / Shovel upright and rotates around the sprite centre:
//   Mitten: fingertips at local -Y (top). Tilted -0.6 rad so they point up-left like the
//           arrow cursor; the hotspot is the fingertip edge.
//   Shovel: handle at local -Y, blade at +Y (bottom). Tilted +0.6 rad so the blade points
//           down-left onto the snow, handle up-right; the hotspot is the blade's leading edge.
//   Snowball: hotspot = centre.
inline constexpr float kMittenCursorAt96 = 26.0f;
inline constexpr float kShovelCursorAt96 = 30.0f;
inline constexpr float kSnowballCursorAt96 = 14.0f;
inline constexpr float kMittenCursorRotation = -0.6f;
inline constexpr float kShovelCursorRotation = 0.6f;
inline constexpr float kCursorHotspotShare = 0.85f;   // hotspot distance from the centre / half height
// Hotspot in the sprite's local (unrotated) frame, relative to the sprite centre.
Vec2 toolHotspotLocal(Tool tool, float dpiScale);
// Sprite placed so that its (rotated) hotspot lies exactly on the pointer (px, py).
SpriteInstance toolCursorSprite(Tool tool, float px, float py, float dpiScale);

}  // namespace snow
