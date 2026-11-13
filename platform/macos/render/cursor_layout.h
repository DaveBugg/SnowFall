// macOS tool-cursor layouts (interaction mode). Pure C++, unit-tested.
//
// The core's cursor batch is laid out for Windows, where the system cursor is hidden and the
// tool sprite IS the cursor (hotspot on the pointer). On macOS:
//   Replace — the system cursor could be hidden (interaction.hideSystemCursor + private CGS
//             call): draw the batch unchanged, exactly like Windows.
//   Hold    — the system arrow stays visible: re-lay the batch out so the tool and the arrow
//             read as one object ("the tool holds the cursor"):
//               * the semi-transparent ring of the tool's reach stays centred on the arrow tip
//                 (mitten: brush radius, shovel: half width — the core's hover ring); the
//                 snowball tool gets a snug ring around the ball;
//               * mitten: the arrow tip sits inside the fingertip area, so the arrow looks
//                 held by the mitten (fingers tilted up-left along the arrow);
//               * shovel: the blade's leading edge on the arrow tip, handle up-right (as core);
//               * snowball: centred on the tip, the arrow sticks out of it.
//             The held-ball marker (ring + faint ball on the pointer) is left as it is.
// Positions are in points; `dpi` is the core's dpiScale for sizes (1 on macOS, 2 in tests).
#pragma once
#include <vector>

#include "core/snapshot.h"

namespace snow::mac {

enum class CursorDraw { Replace, Hold };

inline constexpr float kHoldMittenHotspotShare = 0.45f;   // tip 45 % of the half height above the centre
inline constexpr float kHoldRingOpacity = 0.35f;          // same as the core's hover ring
inline constexpr float kHoldBallRingGapAt96 = 3.0f;       // snowball ring = ball radius + gap

// The pointer (hotspot) a core cursor batch was laid out for; false when it cannot be told.
bool cursorHotspot(const std::vector<SpriteInstance>& batch, float& px, float& py);

// Hold-mode re-layout of a core cursor batch (see above).
std::vector<SpriteInstance> layoutCursorHold(const std::vector<SpriteInstance>& batch, float dpi);

}  // namespace snow::mac
