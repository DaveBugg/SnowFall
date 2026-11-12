// Embedded HLSL for the overlay renderer. Everything is drawn procedurally (signed-distance
// math in the pixel shaders) - there are no textures or external assets.
#pragma once

namespace snow::shaders {

inline constexpr char kCommon[] = R"hlsl(
cbuffer Frame : register(b0) {
    float2 gOrigin;    // monitor origin in virtual-desktop px
    float2 gInvHalf;   // 2 / overlay size
    float  gDt;        // seconds since the snapshot was taken (clamped)
    float  gOpacity;   // global opacity
    float2 gPad;
};
float4 toClip(float2 worldPx) {
    float2 n = (worldPx - gOrigin) * gInvHalf;
    return float4(n.x - 1.0, 1.0 - n.y, 0.0, 1.0);
}
)hlsl";

// ---------------------------------------------------------------------------------------
// Sprites: one instanced quad per SpriteInstance, corners generated from SV_VertexID.
// ---------------------------------------------------------------------------------------
inline constexpr char kSpriteVS[] = R"hlsl(
struct VIn {
    float4 posVel   : POSVEL;     // x, y, vx, vy
    float4 sizeRot  : SIZEROT;    // halfW, halfH, rotation, opacity
    uint2  shapeVar : SHAPE;
    uint   vid      : SV_VertexID;
};
struct VOut {
    float4 pos : SV_Position;
    float2 lp  : TEXCOORD0;                       // local px (unrotated sprite space)
    nointerpolation float2 hs : TEXCOORD1;        // half extents
    nointerpolation float  op : TEXCOORD2;
    nointerpolation uint2  sv : TEXCOORD3;
    nointerpolation float  ang : TEXCOORD4;   // GooglyEye: pupil direction
};
VOut main(VIn i) {
    VOut o;
    float2 c01 = float2((i.vid & 1) ? 1.0 : 0.0, (i.vid & 2) ? 1.0 : 0.0);
    float2 hs = max(i.sizeRot.xy, 0.5);
    float rot = i.sizeRot.z;
    float2 lo = -(hs + 1.5), hi = hs + 1.5;
    uint shape = i.shapeVar.x;
    if (shape == 11) {
        // Spark: quad aligned with the velocity, long enough for the motion tail behind it.
        float2 vel = i.posVel.zw;
        float sp = length(vel);
        float r = hs.x;
        float tail = min(sp * 0.06, max(12.0 * r, 18.0));
        float g = r * 3.0 + 2.0;
        lo = float2(-(tail + g), -g);
        hi = float2(g, g);
        rot = sp > 1.0 ? atan2(vel.y, vel.x) : 0.0;
        hs = float2(r, tail);
    } else if (shape == 12 && i.shapeVar.y >= 8) {
        float g = max(hs.x, hs.y) * 1.5 + 3.0;                  // lit bulb: room for the halo
        lo = -(hs + g);
        hi = hs + g;
    }
    float ang = 0.0;
    if (shape == 18 && i.shapeVar.y == 15) {
        ang = rot;                                              // googly eye: ball stays upright,
        rot = 0.0;                                              // rotation steers the pupil
    }
    float2 lp = lerp(lo, hi, c01);
    float s = sin(rot), co = cos(rot);
    float2 w = i.posVel.xy + i.posVel.zw * gDt + float2(lp.x * co - lp.y * s, lp.x * s + lp.y * co);
    o.pos = toClip(w);
    o.lp = lp;
    o.hs = hs;
    o.op = saturate(i.sizeRot.w);
    o.sv = i.shapeVar;
    o.ang = ang;
    return o;
}
)hlsl";

inline constexpr char kSpritePS[] = R"hlsl(
struct VOut {
    float4 pos : SV_Position;
    float2 lp  : TEXCOORD0;
    nointerpolation float2 hs : TEXCOORD1;
    nointerpolation float  op : TEXCOORD2;
    nointerpolation uint2  sv : TEXCOORD3;
    nointerpolation float  ang : TEXCOORD4;   // GooglyEye: pupil direction
};

float sdSeg(float2 p, float2 a, float2 b) {
    float2 pa = p - a, ba = b - a;
    float t = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
    return length(pa - ba * t);
}
float sdRBox(float2 p, float2 c, float2 b, float r) {
    float2 q = abs(p - c) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
float aa(float d) { return saturate(0.5 - d); }
float4 over(float4 top, float4 bot) { return top + bot * (1.0 - top.a); }
float4 pm(float3 c, float a) { return float4(c * a, a); }

// ---- snowflakes ------------------------------------------------------------------------
float4 flake(float2 p, float r, uint v) {
    const float3 tint = float3(0.95, 0.975, 1.0);
    float len = length(p);
    float soft = saturate(1.0 - len / (r + 0.75));
    soft = soft * soft * (3.0 - 2.0 * soft);
    // bluish towards the rim so flakes stay faintly readable on white backgrounds
    const float3 rimTint = float3(0.62, 0.76, 0.96);
    float4 res = pm(lerp(rimTint, tint, soft * soft), soft * 0.92);
    [branch] if (v != 0 && r > 2.5) {
        // Six-fold symmetry: fold the plane into one 60-degree sector around the +X arm.
        const float k = 1.04719755;
        float ang = atan2(p.y, p.x);
        float a6 = ang - k * floor(ang / k + 0.5);
        float2 q = len * float2(cos(a6), abs(sin(a6)));
        const float2 dir60 = float2(0.5, 0.8660254);
        float d;
        float4 under = pm(rimTint, soft * 0.30);
        if (v == 1) {
            // star: fat tapered arms with one pair of barbs
            float w = lerp(0.17 * r, 0.03 * r, saturate(q.x / (0.95 * r)));
            d = max(q.y - w, q.x - 0.95 * r);
            d = min(d, sdSeg(q, float2(0.50 * r, 0.0), float2(0.50 * r, 0.0) + dir60 * (0.24 * r)) - max(0.4, 0.05 * r));
            d = min(d, len - 0.20 * r);
        } else if (v == 2) {
            // dendrite: thin arms, two pairs of side branches
            float w = max(0.42, 0.055 * r);
            d = sdSeg(q, float2(0.0, 0.0), float2(0.96 * r, 0.0)) - w;
            d = min(d, sdSeg(q, float2(0.36 * r, 0.0), float2(0.36 * r, 0.0) + dir60 * (0.36 * r)) - w * 0.9);
            d = min(d, sdSeg(q, float2(0.66 * r, 0.0), float2(0.66 * r, 0.0) + dir60 * (0.22 * r)) - w * 0.85);
            d = min(d, len - 0.10 * r);
        } else {
            // plate: hexagonal plate with a bright rim and short knobbed arms
            float w = max(0.42, 0.06 * r);
            float hex = q.x * 0.8660254 + q.y * 0.5 - 0.48 * r * 0.8660254;
            float rim = abs(hex) - w;
            float arm = sdSeg(q, float2(0.30 * r, 0.0), float2(0.90 * r, 0.0)) - w;
            float knob = length(q - float2(0.84 * r, 0.0)) - 0.11 * r;
            float inner = abs(q.x * 0.8660254 + q.y * 0.5 - 0.22 * r * 0.8660254) - w * 0.7;
            d = min(min(rim, arm), min(knob, inner));
            under = over(pm(lerp(rimTint, tint, 0.5), aa(hex) * 0.40), under * 0.6);
        }
        float4 crystal = over(pm(float3(0.90, 0.945, 1.0), aa(d) * 0.97), under);
        res = lerp(res, crystal, saturate(r - 2.5));
    }
    return res;
}

// ---- snowman parts ---------------------------------------------------------------------
float4 snowball(float2 p, float r) {
    float len = length(p);
    float d = len - r;
    float2 n = p / r;
    float sh = smoothstep(-0.30, 1.0, dot(n, float2(0.34, 0.94)));
    float3 col = lerp(float3(1.0, 1.0, 1.0), float3(0.70, 0.80, 0.95), sh * 0.9);
    float rim = saturate((d + 1.7) / 1.2);
    col = lerp(col, float3(0.50, 0.62, 0.80), rim * 0.55);
    return pm(col, aa(d));
}
float4 coal(float2 p, float r) {
    float d = length(p) - r;
    float3 col = float3(0.06, 0.06, 0.07);
    col = lerp(col, float3(0.24, 0.24, 0.27), saturate((d + 1.5) / 1.5) * 0.6);
    float hl = saturate(1.0 - length(p - r * float2(-0.30, -0.34)) / max(0.38 * r, 0.6));
    col += 0.40 * hl * hl;
    return pm(col, aa(d));
}
float4 carrot(float2 p, float2 hs) {
    float t = saturate((p.x + hs.x) / (2.0 * hs.x));      // 0 at the base, 1 at the tip (+X)
    float w = hs.y * (1.0 - t);
    float slope = hs.y / (2.0 * hs.x);
    float d = (abs(p.y) - w) * rsqrt(1.0 + slope * slope);
    d = max(d, -hs.x - p.x) - 0.6;
    float3 col = lerp(float3(1.0, 0.60, 0.17), float3(0.82, 0.37, 0.07), saturate(p.y / max(w, 0.5) * 0.5 + 0.5));
    col *= 1.0 - 0.14 * step(0.78, frac(t * 4.0 + 0.35)) * (1.0 - t);
    col = lerp(col, float3(0.55, 0.23, 0.04), saturate(d + 1.7) * 0.6);
    return pm(col, aa(d));
}
float4 stick(float2 p, float2 hs) {
    float th = max(0.7, hs.y * 0.22);
    float d = sdSeg(p, float2(-hs.x, 0.0), float2(hs.x * 0.96, 0.0)) - th;
    d = min(d, sdSeg(p, float2(0.22 * hs.x, 0.0), float2(0.66 * hs.x, -0.86 * hs.y)) - th * 0.78);
    float3 col = lerp(float3(0.47, 0.31, 0.18), float3(0.27, 0.16, 0.08), saturate(d + 1.3));
    return pm(col, aa(d));
}

)hlsl"  // split: MSVC limits one string literal to 16 KB
R"hlsl(
// ---- tool cursors ----------------------------------------------------------------------
float4 mitten(float2 p, float2 hs, uint v) {
    float s = min(hs.x, hs.y);
    float2 q = p / s;
    float body = sdRBox(q, float2(0.08, -0.18), float2(0.44, 0.52), 0.40);
    float thumb = sdSeg(q, float2(-0.36, 0.10), float2(-0.66, -0.16)) - 0.17;
    float m = min(body, thumb);
    float3 base = (v == 1) ? float3(0.16, 0.42, 0.86) : float3(0.86, 0.15, 0.20);
    float3 col = base * (1.0 - 0.28 * saturate(q.y * 0.8 + 0.35));
    col += 0.16 * saturate(1.0 - length(q - float2(-0.08, -0.42)) / 0.5);
    // knitted white zig-zag band across the palm
    float zig = abs(q.y + 0.02 - 0.07 * abs(frac(q.x * 4.0) * 2.0 - 1.0)) - 0.035;
    col = lerp(col, float3(1.0, 1.0, 1.0), aa(zig * s) * 0.9 * step(body, -0.05));
    col = lerp(col, base * 0.42, saturate((m + 0.075) * s / 1.2 + 0.5) * 0.85);
    float4 res = pm(col, aa(m * s));

    float cuff = sdRBox(q, float2(0.08, 0.60), float2(0.50, 0.21), 0.10);
    float3 cc = float3(0.98, 0.98, 1.0) * (1.0 - 0.10 * step(0.5, frac(q.x * 6.0 + 0.25)));
    cc = lerp(cc, float3(0.58, 0.64, 0.78), saturate((cuff + 0.06) * s / 1.2 + 0.5) * 0.8);
    return over(pm(cc, aa(cuff * s)), res);
}
float4 shovel(float2 p, float2 hs) {
    float s = min(hs.x, hs.y);
    float2 q = p / s;
    // handle
    float hd = sdSeg(q, float2(0.0, -0.60), float2(0.0, 0.22)) - 0.065;
    float3 hc = lerp(float3(0.80, 0.60, 0.34), float3(0.42, 0.28, 0.12), saturate((hd + 0.045) * s / 1.2 + 0.5) * 0.85);
    float4 res = pm(hc, aa(hd * s));
    // D-grip
    float gd = abs(sdRBox(q, float2(0.0, -0.76), float2(0.21, 0.15), 0.11)) - 0.058;
    float3 gc = lerp(float3(0.90, 0.24, 0.22), float3(0.48, 0.08, 0.08), saturate((gd + 0.04) * s / 1.2 + 0.5) * 0.85);
    res = over(pm(gc, aa(gd * s)), res);
    // blade (slightly wider at the bottom)
    float2 bq = float2(q.x / (0.90 + 0.14 * saturate(q.y)), q.y);
    float bd = sdRBox(bq, float2(0.0, 0.52), float2(0.52, 0.38), 0.12);
    float3 bc = lerp(float3(0.42, 0.66, 0.95), float3(0.24, 0.47, 0.84), saturate(q.y - 0.1));
    float rib = min(abs(q.x), min(abs(q.x - 0.26), abs(q.x + 0.26))) - 0.028;
    bc = lerp(bc, float3(0.72, 0.86, 1.0), aa(rib * s) * 0.7 * step(q.y, 0.74));
    bc = lerp(bc, float3(0.86, 0.93, 1.0), smoothstep(0.76, 0.80, q.y));
    bc = lerp(bc, float3(0.10, 0.22, 0.48), saturate((bd + 0.06) * s / 1.2 + 0.5) * 0.85);
    return over(pm(bc, aa(bd * s)), res);
}
float4 ring(float2 p, float2 hs) {
    float dd = abs(length(p) - (min(hs.x, hs.y) - 1.0));
    float4 white = pm(float3(1.0, 1.0, 1.0), saturate(1.25 - dd) * 0.85);
    float4 dark = pm(float3(0.0, 0.03, 0.08), saturate(2.25 - dd) * 0.32);
    return over(white, dark);
}

// Toy snowplow truck with a tall moldboard. Unit = half height (q.y in -1..1, +Y down).
// Wheels' bottom touches the sprite bottom (q.y = 1); the blade spans from the ground line up
// to ~0.9 of the sprite height (q.y = -0.8) so a heap of snow can be piled against its face.
float rimF(float d, float s) { return saturate((d * s + 1.7) / 1.3); }   // 1 near the outline
float4 snowplow(float2 p, float2 hs, uint v) {
    float s = hs.y;
    float2 q = p / s;
    if (v == 1) q.x = -q.x;                       // variant 1 drives left
    // ---- distances -------------------------------------------------------------------
    float chassis = sdRBox(q, float2(-0.36, 0.40), float2(1.08, 0.20), 0.09);
    float bed     = sdRBox(q, float2(-0.86, 0.03), float2(0.56, 0.21), 0.08);
    float cab     = sdRBox(q, float2(0.36, -0.12), float2(0.34, 0.40), 0.13);
    float win     = sdRBox(q, float2(0.43, -0.20), float2(0.19, 0.18), 0.06);
    float beacon  = max(length(q - float2(0.31, -0.56)) - 0.12, q.y + 0.52);
    float bBase   = sdRBox(q, float2(0.31, -0.535), float2(0.15, 0.035), 0.02);
    // push frame (lower arms) and a hydraulic ram from the cab to the upper blade
    float arms    = min(sdSeg(q, float2(0.62, 0.36), float2(1.00, 0.36)) - 0.065,
                        sdSeg(q, float2(0.62, 0.56), float2(1.00, 0.70)) - 0.055);
    float ramCyl  = sdSeg(q, float2(0.66, -0.02), float2(0.86, -0.20)) - 0.055;
    float ramRod  = sdSeg(q, float2(0.84, -0.18), float2(1.02, -0.40)) - 0.030;
    // blade (moldboard): tall plate, tilted a little, concave face toward +X
    float2 bc = q - float2(1.13, 0.10);
    const float ca = 0.9928, sa = 0.1197;         // ~7 deg: top leans forward
    float2 br = float2(bc.x * ca + bc.y * sa, -bc.x * sa + bc.y * ca);
    br.x -= 0.20 * (br.y * br.y - 0.30);          // curved moldboard
    float blade = sdRBox(br, float2(0.0, 0.0), float2(0.15, 0.90), 0.08);
    float wheelR = 0.30;
    float2 w1 = float2(-1.12, 1.0 - wheelR), w2 = float2(-0.52, 1.0 - wheelR), w3 = float2(0.36, 1.0 - wheelR);
    float wd = min(min(length(q - w1), length(q - w2)), length(q - w3));
    float tire = wd - wheelR;
    float body = min(min(chassis, bed), cab);
    float hyd = min(ramCyl, ramRod);
    float all = min(min(min(body, min(arms, hyd)), min(blade, tire)), min(beacon, bBase));

    // ---- outline halo so the toy reads on white and dark backgrounds --------------------
    float4 res = pm(float3(0.10, 0.10, 0.14), aa(all * s - 1.1) * 0.55);
    // push arms, then the ram (cylinder dark, rod shiny)
    float3 ac = lerp(float3(0.36, 0.38, 0.42), float3(0.14, 0.15, 0.18), rimF(arms, s));
    res = over(pm(ac, aa(arms * s)), res);
    float3 cy = lerp(float3(0.30, 0.31, 0.35), float3(0.12, 0.12, 0.15), rimF(ramCyl, s));
    res = over(pm(cy, aa(ramCyl * s)), res);
    float3 rc = lerp(float3(0.92, 0.94, 0.97), float3(0.48, 0.50, 0.56), rimF(ramRod, s));
    res = over(pm(rc, aa(ramRod * s)), res);
    // blade: steel, bright highlight along the curve, red/white band at the top, dark cutting edge
    float3 sc = lerp(float3(0.86, 0.89, 0.94), float3(0.54, 0.58, 0.66), saturate(br.x / 0.30 + 0.5));
    float hb = (br.y + 0.20) / 0.12;
    sc = lerp(sc, float3(0.98, 0.99, 1.0), exp(-hb * hb) * 0.6);
    float warn = step(0.5, frac((br.y - br.x * 0.8) * 3.4)) * step(br.y, -0.56) * step(-0.84, br.y);
    sc = lerp(sc, float3(0.90, 0.18, 0.16), warn * 0.85);
    float rib = abs(frac(br.y * 2.2 + 0.25) - 0.5) - 0.05;
    sc = lerp(sc, float3(0.44, 0.47, 0.54), aa(rib * s / 2.2) * step(br.x, -0.06) * 0.6);
    sc = lerp(sc, float3(0.32, 0.34, 0.40), smoothstep(0.76, 0.82, br.y));
    sc = lerp(sc, float3(0.22, 0.24, 0.30), rimF(blade, s) * 0.9);
    res = over(pm(sc, aa(blade * s)), res);
    // chassis + bed + cab: bright orange-yellow toy plastic
    float3 orange = float3(1.0, 0.56, 0.10), yellow = float3(1.0, 0.80, 0.18);
    float3 col = lerp(yellow, orange, saturate(q.y * 0.9 + 0.5));
    col = lerp(col, orange * 0.80, step(bed, 0.0) * step(0.0, cab) * 0.35);   // bed slightly darker
    col += 0.18 * saturate(1.0 - abs(q.y + 0.24) / 0.06) * step(cab, -0.04);  // cab roof shine
    float seam = min(abs(bed), abs(cab));
    col = lerp(col, float3(0.62, 0.30, 0.04), aa(seam * s - 0.4) * 0.6 * step(body, -0.02));
    float stripe = step(0.5, frac((q.x - q.y) * 2.6)) * step(abs(q.y - 0.46), 0.07) * step(chassis, -0.06);
    col = lerp(col, float3(0.13, 0.12, 0.12), stripe * 0.85);
    col = lerp(col, float3(0.55, 0.24, 0.03), rimF(body, s) * 0.85);
    res = over(pm(col, aa(body * s)), res);
    // cab window with a glint
    float3 wc = lerp(float3(0.72, 0.90, 1.0), float3(0.40, 0.66, 0.92), saturate(q.y + 0.35));
    wc = lerp(wc, float3(1.0, 1.0, 1.0), aa((abs(q.x - q.y * 0.6 - 0.55) - 0.035) * s) * 0.7);
    wc = lerp(wc, float3(0.20, 0.32, 0.52), rimF(win, s) * 0.8);
    res = over(pm(wc, aa(win * s)), res);
    // beacon
    res = over(pm(float3(0.30, 0.30, 0.34), aa(bBase * s)), res);
    float3 amber = lerp(float3(1.0, 0.86, 0.35), float3(1.0, 0.55, 0.0), saturate((q.y + 0.60) * 6.0));
    amber = lerp(amber, float3(0.70, 0.32, 0.0), rimF(beacon, s) * 0.7);
    res = over(pm(amber, aa(beacon * s)), res);
    // wheels: tyre, grey hub, centre bolt
    float3 tc = lerp(float3(0.16, 0.16, 0.18), float3(0.05, 0.05, 0.06), rimF(tire, s));
    res = over(pm(tc, aa(tire * s)), res);
    float hub = wd - 0.14;
    float3 hcol = lerp(float3(0.80, 0.82, 0.86), float3(0.45, 0.47, 0.52), rimF(hub, s));
    res = over(pm(hcol, aa(hub * s)), res);
    res = over(pm(float3(0.30, 0.31, 0.35), aa((wd - 0.045) * s)), res);
    return res;
}

)hlsl"  // split: MSVC limits one string literal to 16 KB
R"hlsl(
// ---- broken-off snow chunk ---------------------------------------------------------------
// Everything is measured in pixels so a long slab gets many small bumps instead of a few
// stretched ones. Local -Y is the lumpy top, +Y the flat side that lay on the window edge.
float chash(float n) { return frac(sin(n * 127.1 + 311.7) * 43758.5453); }
float vnoise(float x, float seed) {
    float i = floor(x), f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(chash(i + seed), chash(i + 1.0 + seed), f);
}
float chunkTop(float x, float hy, float seed, float wl) {
    float th = 2.0 * hy;
    float n = vnoise(x / wl, seed) * 0.65 + vnoise(x / (wl * 0.42), seed + 17.3) * 0.35;
    float big = vnoise(x / (wl * 3.3), seed + 41.9);
    return -hy + min(0.30 * th, 6.0) * n + min(0.22 * th, 9.0) * big;
}
float chunkEnd(float y, float2 hs, float seed, float slant) {
    // distance the broken face is recessed from hs.x at height y (rough, slightly slanted)
    float ra = min(4.5, 0.28 * hs.x);
    float r = ra * (vnoise(y / 4.0, seed) * 0.55 + vnoise(y / 1.7, seed + 7.7) * 0.30 +
                    vnoise(y / 0.8, seed + 2.9) * 0.15);
    r += slant >= 0.0 ? slant * (y + hs.y) : -slant * (hs.y - y);
    return min(r, 0.45 * hs.x);
}
float4 snowChunk(float2 p, float2 hs, uint v) {
    float seed = float(v) * 23.17 + 3.1;
    float wl = clamp(hs.y * 1.1, 4.5, 11.0);              // bump wavelength in px
    float slR = (chash(seed + 1.0) - 0.5) * 0.8;
    float slL = (chash(seed + 2.0) - 0.5) * 0.8;
    // top surface
    float top = chunkTop(p.x, hs.y, seed, wl);
    float dtdx = (chunkTop(p.x + 0.5, hs.y, seed, wl) - chunkTop(p.x - 0.5, hs.y, seed, wl));
    float dTop = (top - p.y) * rsqrt(1.0 + dtdx * dtdx);
    // broken ends
    float dEnd = p.x >= 0.0 ? (p.x - (hs.x - chunkEnd(p.y, hs, seed + 5.0, slR))) * rsqrt(1.0 + slR * slR)
                            : (-p.x - (hs.x - chunkEnd(p.y, hs, seed + 9.0, slL))) * rsqrt(1.0 + slL * slL);
    // flat-ish bottom
    float bottom = hs.y - 0.7 * vnoise(p.x / 5.0, seed + 3.0);
    float dBot = p.y - bottom;
    // rounded shoulders on top, small radius at the bottom corners
    float r1 = min(min(hs.x, hs.y) * 0.85, 11.0);
    float2 q1 = float2(dTop, dEnd) + r1;
    float d = length(max(q1, 0.0)) + min(max(q1.x, q1.y), 0.0) - r1;
    float r2 = min(min(hs.x, hs.y) * 0.3, 1.5);
    float2 q2 = float2(d, dBot) + r2;
    d = length(max(q2, 0.0)) + min(max(q2.x, q2.y), 0.0) - r2;

    // shading, matched to the drifts
    float depth = max(p.y - top, 0.0);
    float th = max(bottom - top, 1.0);
    float t = 1.0 - exp(-depth / 14.0);
    float3 col = lerp(float3(1.0, 1.0, 1.0), float3(0.80, 0.87, 0.97), t);
    float u = saturate(depth / th);
    col = lerp(col, float3(0.70, 0.80, 0.95), smoothstep(0.45, 1.0, u) * 0.55);     // underside
    col = lerp(col, float3(0.76, 0.84, 0.96), saturate(1.0 + dEnd / 3.0) * 0.40);  // fracture faces
    float hb = (depth - 2.0) / 1.8;
    col = lerp(col, float3(1.0, 1.0, 1.0), exp(-hb * hb) * 0.55);                  // crest highlight
    float n = chash(dot(floor(p / 1.6), float2(1.0, 57.0)) + seed);
    col = lerp(col, float3(0.74, 0.82, 0.95), step(n, 0.05) * 0.35 * saturate(-d - 1.5));
    float glint = step(0.975, n) * saturate(-d - 1.5) * exp(-depth / 10.0);
    col = lerp(col, float3(1.0, 1.0, 1.0), glint);
    col = lerp(col, float3(0.58, 0.68, 0.86), saturate((d + 1.6) / 1.3) * 0.6);     // soft rim
    return pm(col, aa(d) * 0.95);
}

// ==== round 6 shapes =====================================================================
float sq(float x) { return x * x; }
float sdTaper(float2 p, float2 a, float2 b, float ra, float rb) {
    float2 pa = p - a, ba = b - a;
    float t = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
    return length(pa - ba * t) - lerp(ra, rb, t);
}
static const float3 kPal[8] = {
    float3(1.00, 0.80, 0.30), float3(1.00, 0.28, 0.24), float3(0.35, 1.00, 0.42), float3(0.38, 0.58, 1.00),
    float3(0.80, 0.45, 1.00), float3(1.00, 1.00, 1.00), float3(1.00, 0.58, 0.16), float3(0.35, 1.00, 1.00)};

// Firework spark. Sprite space: +X = direction of motion (set up by the vertex shader),
// hs = (core radius, tail length). Kept cheap: a handful of ALU ops, no loops.
float4 spark(float2 p, float2 hs, uint v) {
    float3 col = kPal[v & 7u];
    float r = hs.x, tail = hs.y;
    float dc = length(p);
    float core = saturate(1.0 - dc / (r + 0.6));
    float t = saturate(-p.x / max(tail, 0.001));
    float dt = length(float2(p.x + t * tail, p.y));
    float tl = saturate(1.0 - dt / (r * 0.8 + 0.45)) * sq(1.0 - t) * step(0.5, tail);
    float g = r * 3.0 + 1.5;
    float glow = exp(-dc * dc / (g * g * 0.3));
    float I = max(core, tl * 0.85);
    float3 hot = lerp(col, float3(1.0, 1.0, 1.0), core * core * 0.45);
    // luminous part: colour exceeds alpha -> additive look on dark backgrounds
    float4 bright = float4(hot * I + col * glow * 0.6, I * 0.75 + glow * 0.2);
    // darker tinted rim so sparks stay visible on light backgrounds
    float disc = saturate((r + 1.6 - dc) / 1.2);                // solid tinted disc + tail
    float ring = max(disc * 0.70, tl * 0.50);
    float4 darkRim = float4(col * 0.32 * ring, ring);
    return over(bright, darkRim);
}

// Christmas-light bulb, cap at -Y. variant & 7 = colour, +8 = lit.
float4 bulb(float2 p, float2 hs, uint v) {
    float3 col = kPal[v & 7u];
    bool on = v >= 8u;
    float w = hs.x, h = hs.y;
    float capH = 0.30 * h;
    float cap = sdRBox(p, float2(0.0, -h + capH * 0.5), float2(0.55 * w, capH * 0.5), 0.6);
    float gy0 = -h + capH * 0.8;
    float cyc = (gy0 + h) * 0.5, ry = (h - gy0) * 0.5;
    float tt = saturate((p.y - gy0) / (2.0 * ry));
    float rx = w * lerp(0.55, 1.0, saturate(tt * 1.7));       // narrow neck at the cap
    float2 e = float2(p.x / rx, (p.y - cyc) / ry);
    float d = (length(e) - 1.0) * min(rx, ry);
    float4 res = float4(0.0, 0.0, 0.0, 0.0);
    if (on) {
        float dd = max(d, 0.0);
        float gR = max(w, h) * 0.9 + 2.0;
        float gl = exp(-dd * dd / (gR * gR * 0.35)) * (1.0 - aa(d));
        res = float4(col * gl * 0.8, gl * 0.30);
    }
    float3 gc;
    if (on) gc = lerp(col, float3(1.0, 1.0, 1.0), saturate(1.0 - length(e - float2(0.0, 0.15))) * 0.8);
    else gc = col * 0.50 + 0.04;
    float2 hp = (p - float2(-0.42 * rx, cyc - 0.25 * ry)) / float2(0.20 * rx + 0.3, 0.38 * ry + 0.3);
    float hl = saturate(1.0 - dot(hp, hp));
    gc = lerp(gc, float3(1.0, 1.0, 1.0), hl * (on ? 0.45 : 0.85));
    gc = lerp(gc, col * (on ? 0.62 : 0.28), rimF(d, 1.0) * 0.8);
    res = over(pm(gc, aa(d) * (on ? 1.0 : 0.94)), res);
    float3 cc = lerp(float3(0.20, 0.42, 0.22), float3(0.05, 0.16, 0.07), saturate((p.y + h) / capH));
    cc = lerp(cc, float3(0.80, 0.66, 0.30), saturate(1.0 - abs(p.y - (gy0 + 0.6)) / 0.9) * 0.6);   // brass ring
    cc = lerp(cc, float3(0.02, 0.07, 0.03), rimF(cap, 1.0) * 0.7);
    return over(pm(cc, aa(cap)), res);
}

// Garland wire: ends at local y = 0 (x = +-halfW), sags down to y = +halfH in the middle.
float4 garlandWire(float2 p, float2 hs) {
    float hw = hs.x, sag = hs.y;
    float xn = clamp(p.x / hw, -1.0, 1.0);
    float yc = sag * (1.0 - xn * xn);
    float sl = -2.0 * sag * xn / hw;
    float dy = (p.y - yc) * rsqrt(1.0 + sl * sl);
    float dx = max(abs(p.x) - hw, 0.0);
    float dist = length(float2(dy, dx));
    float tw = sin(p.x * 1.7 + dy * 3.0);                       // two twisted strands
    float3 col = lerp(float3(0.05, 0.20, 0.08), float3(0.30, 0.58, 0.32), 0.5 + 0.5 * tw);
    col = lerp(col, float3(0.02, 0.08, 0.03), saturate(dist - 0.3) * 0.7);
    return pm(col, aa(dist - 0.9));
}

// Icicle: root at -Y, sharp tip at +Y.
float icW(float y, float2 hs, float ph, float fq, uint v) {
    float t = saturate((y + hs.y) / (2.0 * hs.y));
    float w = hs.x * pow(1.0 - t, 0.85);
    w *= 1.0 + 0.16 * sin(t * fq + ph) * (1.0 - t);
    if (v == 3u) w += hs.x * 0.7 * exp(-sq((t - 0.70) / 0.07)) * (1.0 - t);   // frozen drip bulge
    if (v == 1u) w += hs.x * 0.25 * exp(-sq((t - 0.35) / 0.10));
    return w;
}
float4 icicle(float2 p, float2 hs, uint v) {
    float ph = float(v) * 1.9, fq = 9.0 + float(v) * 3.0;
    float w = icW(p.y, hs, ph, fq, v);
    float dw = icW(p.y + 0.5, hs, ph, fq, v) - icW(p.y - 0.5, hs, ph, fq, v);
    float d = (abs(p.x) - w) * rsqrt(1.0 + dw * dw);
    d = max(d, -hs.y - p.y);
    float t = saturate((p.y + hs.y) / (2.0 * hs.y));
    float xr = clamp(p.x / max(w, 0.35), -1.0, 1.0);
    float3 col = float3(0.78, 0.90, 1.0);
    float a = 0.40 + 0.30 * xr * xr;                            // thicker-looking edges
    float rg = sin(p.y * (0.8 + 0.25 * float(v)) + ph);         // internal ridges
    col = lerp(col, float3(0.60, 0.79, 0.97), (0.5 + 0.5 * rg) * 0.30);
    float spc = exp(-sq((xr + 0.42) / 0.20));                   // specular streak
    col = lerp(col, float3(1.0, 1.0, 1.0), spc * 0.9);
    a = max(a, spc * 0.85);
    float fr = saturate(1.0 - t / 0.12);                        // frosted root
    col = lerp(col, float3(0.96, 0.98, 1.0), fr * 0.8);
    a = lerp(a, 0.92, fr);
    float rim = rimF(d, 1.0);
    col = lerp(col, float3(0.28, 0.48, 0.78), rim * 0.75);
    a = lerp(a, 0.90, rim * 0.8);
    return pm(col, a * aa(d));
}

// Melt-water drop, tip at -Y. variant 0 falling, 1 hanging/forming (long neck).
float4 droplet(float2 p, float2 hs, uint v) {
    float R = min(hs.x, hs.y);
    float cy = hs.y - R;
    float e = v == 1u ? 2.2 : 0.9;
    float d;
    if (p.y >= cy) {
        d = length(p - float2(0.0, cy)) - R;
    } else {
        float L = max(cy + hs.y, 0.5);
        float t = saturate((p.y + hs.y) / L);
        float w = R * pow(t, e) + (v == 1u ? 0.5 : 0.0);
        float dw = R * e * pow(max(t, 0.02), e - 1.0) / L;
        d = (abs(p.x) - w) * rsqrt(1.0 + dw * dw);
        d = max(d, -hs.y - p.y);
    }
    float3 col = float3(0.62, 0.80, 0.98);
    float a = 0.28;
    float rim = rimF(d, 1.0);
    col = lerp(col, float3(0.14, 0.34, 0.62), rim * 0.85);
    a = lerp(a, 0.85, rim);
    float2 hp = (p - float2(-0.36 * R, cy - 0.30 * R)) / max(0.24 * R, 0.6);
    float hl = saturate(1.0 - dot(hp, hp));
    col = lerp(col, float3(1.0, 1.0, 1.0), hl);
    a = max(a, hl * 0.95);
    float2 cp = (p - float2(0.12 * R, cy + 0.55 * R)) / max(float2(0.36 * R, 0.14 * R), 0.5);
    float ca = saturate(1.0 - dot(cp, cp)) * 0.6;               // caustic at the bottom
    col = lerp(col, float3(0.92, 0.97, 1.0), ca);
    a = max(a, ca * 0.7);
    return pm(col, a * aa(d));
}
)hlsl"  // split: MSVC limits one string literal to 16 KB
R"hlsl(
// Snowball smashed on a pane.
float4 splat(float2 p, float2 hs, uint v) {
    float s = min(hs.x, hs.y);
    float2 q = p / hs;
    float seed = float(v) * 13.7 + 1.3;
    float ang = atan2(q.y, q.x);
    float rb = 0.48 + 0.06 * sin(3.0 * ang + seed) + 0.04 * sin(5.0 * ang + seed * 2.0) + 0.025 * sin(11.0 * ang + seed * 3.0);
    float d = length(q) - rb;
    [unroll] for (int k = 0; k < 7; ++k) {
        float fk = float(k);
        float ak = (fk + chash(seed + fk) * 0.7) * 0.8975979;   // 2pi / 7
        float2 dir = float2(cos(ak), sin(ak));
        float L = 0.60 + 0.30 * chash(seed + fk + 20.0);
        float rr = 0.06 + 0.06 * chash(seed + fk + 40.0);
        d = min(d, sdTaper(q, dir * 0.35, dir * L, 0.15, rr * 0.6));
        d = min(d, length(q - dir * L) - rr);
    }
    [unroll] for (int j = 0; j < 2; ++j) {                      // drips running down the glass
        float fj = float(j);
        float xj = (chash(seed + 60.0 + fj) - 0.5) * 0.7;
        float yj = 0.62 + 0.30 * chash(seed + 70.0 + fj);
        d = min(d, sdTaper(q, float2(xj, 0.30), float2(xj, yj), 0.09, 0.045));
        d = min(d, length(q - float2(xj, yj)) - 0.06);
    }
    [unroll] for (int m = 0; m < 8; ++m) {                      // loose specks
        float fm = float(m);
        float am = chash(seed + 80.0 + fm) * 6.2831853;
        float rm = 0.74 + 0.20 * chash(seed + 90.0 + fm);
        float sr = 0.025 + 0.035 * chash(seed + 100.0 + fm);
        d = min(d, length(q - rm * float2(cos(am), sin(am))) - sr);
    }
    float dpx = d * s;
    float dens = saturate(1.0 - length(q) / 0.55);
    float3 col = lerp(float3(0.84, 0.90, 0.98), float3(1.0, 1.0, 1.0), dens);
    col = lerp(col, float3(0.76, 0.84, 0.96), saturate(dot(q, float2(0.45, 0.65))) * 0.30);
    float g = chash(dot(floor(p / 1.5), float2(1.0, 57.0)) + seed);
    col = lerp(col, float3(0.74, 0.82, 0.95), step(g, 0.07) * 0.4);
    col = lerp(col, float3(1.0, 1.0, 1.0), step(0.975, g) * saturate(-dpx - 1.5));
    col = lerp(col, float3(0.55, 0.66, 0.86), rimF(dpx, 1.0) * 0.65);
    return pm(col, lerp(0.80, 0.97, dens) * aa(dpx));
}

// Snowman hats, brim at +Y.
float4 hat(float2 p, float2 hs, uint v) {
    float w = hs.x, h = hs.y;
    float4 res = float4(0.0, 0.0, 0.0, 0.0);
    if (v == 0u) {                                             // top hat
        float brim = sdRBox(p, float2(0.0, 0.86 * h), float2(w, 0.12 * h), 0.08 * h);
        float crown = sdRBox(p, float2(0.0, -0.10 * h), float2(0.66 * w, 0.88 * h), 0.12 * w);
        float d = min(brim, crown);
        float3 col = float3(0.10, 0.10, 0.12);
        col += 0.18 * exp(-sq((p.x + 0.32 * w) / (0.14 * w))) * step(crown, 0.0);
        col = lerp(col, float3(0.80, 0.10, 0.12), step(abs(p.y - 0.52 * h), 0.13 * h) * step(crown, -0.5));
        col = lerp(col, float3(0.36, 0.37, 0.42), rimF(d, 1.0) * 0.6);
        res = pm(col, aa(d));
    } else if (v == 1u) {                                      // tin bucket, open end down
        float tt = saturate((p.y + h) / (2.0 * h));
        float bw = w * lerp(0.62, 0.86, tt);
        float body = max(abs(p.x) - bw, abs(p.y + 0.08 * h) - 0.86 * h);
        float rimB = sdRBox(p, float2(0.0, 0.84 * h), float2(0.94 * w, 0.13 * h), 0.06 * h);
        float d = min(body, rimB);
        float xr = clamp(p.x / bw, -1.0, 1.0);
        float3 col = lerp(float3(0.80, 0.83, 0.88), float3(0.44, 0.47, 0.53), xr * xr);
        col = lerp(col, float3(0.98, 0.99, 1.0), exp(-sq((xr + 0.40) / 0.13)) * 0.8);
        float ribs = min(abs(p.y + 0.45 * h), abs(p.y - 0.10 * h)) - 0.6;
        col = lerp(col, col * 0.72, aa(ribs) * step(body, -0.5));
        col = lerp(col, float3(0.88, 0.90, 0.94), step(rimB, 0.0) * step(0.0, body) * 0.5);
        col = lerp(col, float3(0.28, 0.30, 0.34), rimF(d, 1.0) * 0.75);
        res = pm(col, aa(d));
        float handle = abs(length((p - float2(0.0, -0.05 * h)) / float2(1.0 * w, 0.62 * h)) - 1.0) * min(w, h) - 0.5;
        handle = max(handle, -p.x);                              // only the side loop on +X
        res = over(res, pm(float3(0.38, 0.40, 0.45), aa(handle)));
    } else if (v == 2u) {                                      // knitted beanie
        float dome = sdRBox(p, float2(0.0, 0.10 * h), float2(0.92 * w, 0.78 * h), min(0.85 * w, 0.78 * h));
        float cuff = sdRBox(p, float2(0.0, 0.72 * h), float2(0.97 * w, 0.24 * h), 0.10 * h);
        float pr = 0.30 * min(w, h);
        float2 pp = p - float2(0.0, -0.98 * h + pr);
        float pom = length(pp) - pr * (1.0 + 0.08 * sin(atan2(pp.y, pp.x) * 9.0));
        float3 col = float3(0.82, 0.13, 0.16);
        float kx = frac(p.x / 3.0);
        col *= 0.86 + 0.14 * step(0.5, frac(p.y / 3.2 + abs(kx - 0.5)));
        col = lerp(col, col * 0.55, saturate(p.x / w) * 0.35);
        col = lerp(col, float3(0.45, 0.04, 0.06), rimF(dome, 1.0) * 0.8);
        res = pm(col, aa(dome));
        float3 cc = float3(0.97, 0.97, 1.0) * (0.88 + 0.12 * step(0.5, frac(p.x / 2.6)));
        cc = lerp(cc, float3(0.55, 0.62, 0.78), rimF(cuff, 1.0) * 0.7);
        res = over(pm(cc, aa(cuff)), res);
        float3 pc = lerp(float3(1.0, 1.0, 1.0), float3(0.78, 0.84, 0.95), saturate(pp.y / pr * 0.6 + 0.4));
        pc = lerp(pc, float3(0.55, 0.62, 0.78), rimF(pom, 1.0) * 0.6);
        res = over(pm(pc, aa(pom)), res);
    } else {                                                   // Santa hat flopping to +X
        float2 a0 = float2(-0.05 * w, 0.50 * h), a1 = float2(0.02 * w, -0.30 * h);
        float2 a2 = float2(0.40 * w, -0.78 * h), a3 = float2(0.74 * w, -0.32 * h);
        float cone = sdTaper(p, a0, a1, 0.80 * w, 0.52 * w);
        cone = min(cone, sdTaper(p, a1, a2, 0.52 * w, 0.24 * w));
        cone = min(cone, sdTaper(p, a2, a3, 0.24 * w, 0.09 * w));
        cone = max(cone, p.y - 0.6 * h);
        float3 col = lerp(float3(0.90, 0.14, 0.16), float3(0.55, 0.05, 0.08), saturate((p.x + 0.2 * w) / (1.2 * w)));
        col = lerp(col, float3(1.0, 0.40, 0.40), exp(-sq((p.x + 0.45 * w) / (0.12 * w))) * step(-0.2 * h, p.y) * 0.35);
        col = lerp(col, float3(0.38, 0.03, 0.06), rimF(cone, 1.0) * 0.8);
        res = pm(col, aa(cone));
        float tr = 0.22 * h;
        float trim = sdRBox(p, float2(0.0, 0.76 * h), float2(0.96 * w, tr), tr * 0.9);
        trim -= 0.6 * sin(p.x * 1.3) * sin(p.y * 1.7);         // fluffy
        float pr = 0.22 * w;
        float2 pp = p - float2(0.78 * w, -0.22 * h);
        float pom = length(pp) - pr * (1.0 + 0.08 * sin(atan2(pp.y, pp.x) * 9.0));
        float3 tc = lerp(float3(1.0, 1.0, 1.0), float3(0.80, 0.86, 0.96), saturate((p.y - 0.6 * h) / (0.4 * h)));
        tc = lerp(tc, float3(0.56, 0.63, 0.80), rimF(trim, 1.0) * 0.6);
        res = over(pm(tc, aa(trim)), res);
        float3 pc = lerp(float3(1.0, 1.0, 1.0), float3(0.78, 0.84, 0.95), saturate(pp.y / pr * 0.6 + 0.4));
        pc = lerp(pc, float3(0.56, 0.63, 0.80), rimF(pom, 1.0) * 0.6);
        res = over(pm(pc, aa(pom)), res);
    }
    return res;
}

// Scarf: band around the neck (top part of the sprite) + hanging tail on +X.
float4 scarf(float2 p, float2 hs, uint v) {
    float w = hs.x, h = hs.y;
    float3 base;
    if (v == 0u) base = float3(0.86, 0.16, 0.18);
    else if (v == 1u) base = float3(0.14, 0.56, 0.26);
    else if (v == 2u) base = float3(0.20, 0.42, 0.88);
    else base = float3(1.00, 0.80, 0.16);
    // band: curves down in the middle like a real wrap
    float yb = p.y - 0.14 * h * (1.0 - sq(p.x / w));
    float band = sdRBox(float2(p.x, yb), float2(-0.04 * w, -0.62 * h), float2(0.92 * w, 0.22 * h), 0.18 * h);
    float3 bc = base * (0.88 + 0.12 * step(0.5, frac(p.x / 2.4)));
    if (v == 1u) bc = lerp(bc, float3(0.97, 0.97, 0.97), step(0.5, frac(p.x / (0.26 * w))));
    bc = lerp(bc, bc * 0.62, saturate((yb + 0.62 * h) / (0.22 * h)) * 0.4);   // lower fold in shadow
    bc = lerp(bc, base * 0.40, rimF(band, 1.0) * 0.8);
    float4 res = pm(bc, aa(band));
    // tail
    float2 a = float2(0.40 * w, -0.52 * h), b = float2(0.56 * w, 0.74 * h);
    float L = length(b - a);
    float2 u = (b - a) / L, n = float2(u.y, -u.x);
    float2 lp = float2(dot(p - a, n), dot(p - a, u));
    float tw = 0.17 * w;
    float tail = sdRBox(lp, float2(0.0, 0.5 * L), float2(tw, 0.5 * L), 0.05 * w);
    // fringe
    float fx = abs(frac((lp.x + tw) / (tw * 0.5)) - 0.5) * tw * 0.5 - 0.6;
    float fringe = max(max(fx, abs(lp.x) - tw), max(lp.y - (L + 0.20 * h), L - 1.0 - lp.y));
    float3 tc = base * (0.88 + 0.12 * step(0.5, frac(lp.x / 2.2)));
    if (v == 1u) tc = lerp(tc, float3(0.97, 0.97, 0.97), step(0.5, frac(lp.y / (0.24 * h))));
    tc = lerp(tc, tc * 0.70, saturate(1.0 - lp.y / (0.25 * h)) * 0.5);   // shadow under the band
    float dT = min(tail, fringe);
    tc = lerp(tc, base * 0.40, rimF(dT, 1.0) * 0.8);
    return over(res, pm(tc, aa(dT)));
}

)hlsl"  // split: MSVC limits one string literal to 16 KB
R"hlsl(
// ==== round 7: snowman accessories (variant = snow::Accessory) ===========================
// Unit u = half height: q = p / u, so q.y spans -1..1 and q.x spans -A..A (A = halfW/halfH).
float sdTri(float2 p, float2 p0, float2 p1, float2 p2) {
    float2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    float2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    float2 pq0 = v0 - e0 * saturate(dot(v0, e0) / dot(e0, e0));
    float2 pq1 = v1 - e1 * saturate(dot(v1, e1) / dot(e1, e1));
    float2 pq2 = v2 - e2 * saturate(dot(v2, e2) / dot(e2, e2));
    float sg = sign(e0.x * e2.y - e0.y * e2.x);
    float2 d = min(min(float2(dot(pq0, pq0), sg * (v0.x * e0.y - v0.y * e0.x)),
                       float2(dot(pq1, pq1), sg * (v1.x * e1.y - v1.y * e1.x))),
                   float2(dot(pq2, pq2), sg * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}
// One shaded layer: base colour, darker rim near the edge, anti-aliased, composited over `res`.
float4 accLayer(float4 res, float d, float u, float3 c, float3 rc) {
    return over(pm(lerp(c, rc, rimF(d, u) * 0.85), aa(d * u)), res);
}
// Soft dark outline halo behind the whole accessory.
float4 accHalo(float dAll, float u) { return pm(float3(0.08, 0.08, 0.12), aa(dAll * u - 1.0) * 0.45); }
float smin(float a, float b, float k) {
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}

float4 accGlasses(float2 q, float u, bool shades) {
    float2 aq = float2(abs(q.x), q.y);
    float lens, frame;
    if (shades) {
        lens = sdRBox(aq, float2(1.22, 0.08), float2(0.92, 0.72), 0.42);
        frame = lens - 0.10;
    } else {
        lens = length(aq - float2(1.20, 0.0)) - 0.78;
        frame = abs(lens + 0.06) - 0.10;
    }
    float bridge = sdSeg(q, float2(-0.40, -0.28), float2(0.40, -0.28)) - (shades ? 0.13 : 0.09);
    float temple = sdSeg(aq, float2(2.05, -0.42), float2(2.45, -0.62)) - 0.09;
    float fr = min(min(frame, bridge), temple);
    float4 res = accHalo(min(fr, lens), u);
    if (shades) {
        float3 lc = lerp(float3(0.10, 0.12, 0.18), float3(0.02, 0.02, 0.04), saturate(q.y * 0.6 + 0.5));
        float refl = step(abs(aq.x - 1.0 + q.y * 0.7 - 0.05), 0.16) + step(abs(aq.x - 1.45 + q.y * 0.7), 0.06);
        lc = lerp(lc, float3(0.55, 0.65, 0.85), saturate(refl) * 0.45);
        res = over(pm(lc, aa(lens * u) * 0.96), res);
        res = accLayer(res, min(min(bridge, temple), max(frame, -lens - 0.12)), u, float3(0.05, 0.05, 0.06), float3(0.30, 0.30, 0.34));
    } else {
        float hl = saturate(1.0 - length((aq - float2(0.95, -0.32)) / float2(0.22, 0.14)));
        res = over(pm(float3(0.85, 0.93, 1.0), aa(lens * u) * (0.14 + hl * 0.7)), res);
        res = accLayer(res, fr, u, float3(0.22, 0.14, 0.08), float3(0.05, 0.03, 0.02));
    }
    return res;
}

float4 accEyePatch(float2 q, float u) {
    float strap = sdSeg(q, float2(-2.30, -0.88), float2(2.30, 0.50)) - 0.09;
    float patch = length(q / float2(0.78, 0.66)) - 1.0;
    patch *= 0.66;
    float4 res = accHalo(min(strap, patch), u);
    res = accLayer(res, strap, u, float3(0.12, 0.11, 0.12), float3(0.0, 0.0, 0.0));
    float3 pc = lerp(float3(0.16, 0.15, 0.17), float3(0.04, 0.04, 0.05), saturate(q.y + 0.5));
    pc += 0.12 * saturate(1.0 - length((q - float2(-0.25, -0.30)) / float2(0.25, 0.14)));
    return accLayer(res, patch, u, pc, float3(0.0, 0.0, 0.0));
}

float4 accHeadset(float2 q, float u, bool muffs) {
    float2 aq = float2(abs(q.x), q.y);
    float band = abs(length(q - float2(0.0, 0.18)) - 0.98) - (muffs ? 0.06 : 0.10);
    band = max(band, q.y - 0.10);
    float cup;
    if (muffs) {
        float2 mp = aq - float2(0.96, 0.24);
        cup = length(mp) - 0.34 * (1.0 + 0.07 * sin(atan2(mp.y, mp.x) * 10.0));
    } else {
        cup = sdRBox(aq, float2(0.98, 0.24), float2(0.19, 0.40), 0.14);
    }
    float4 res = accHalo(min(band, cup), u);
    if (muffs) {
        res = accLayer(res, band, u, float3(0.85, 0.86, 0.90), float3(0.40, 0.42, 0.48));
        float2 mp = aq - float2(0.96, 0.24);
        float3 mc = lerp(float3(1.0, 0.80, 0.86), float3(0.92, 0.52, 0.66), saturate(mp.y / 0.34 * 0.6 + 0.4));
        float fluff = chash(dot(floor(q * u / 1.5), float2(1.0, 57.0)));
        mc = lerp(mc, float3(1.0, 0.94, 0.96), step(0.8, fluff) * 0.6);
        res = accLayer(res, cup, u, mc, float3(0.72, 0.30, 0.45));
    } else {
        res = accLayer(res, band, u, float3(0.20, 0.21, 0.25), float3(0.04, 0.04, 0.06));
        float3 cc = lerp(float3(0.30, 0.31, 0.36), float3(0.12, 0.12, 0.15), saturate(aq.x - 0.9));
        res = accLayer(res, cup, u, cc, float3(0.03, 0.03, 0.05));
        float ring = abs(length((aq - float2(1.02, 0.24)) / float2(0.10, 0.24)) - 1.0) * 0.10 - 0.025;
        res = over(pm(float3(0.20, 0.85, 0.95), aa(ring * u)), res);
    }
    return res;
}

float4 accBowTie(float2 q, float u, float3 col) {
    float ax = abs(q.x);
    float wing = max(abs(q.y) - (0.30 + 0.62 * ax / 1.7), ax - 1.68) * 0.85;
    wing = max(wing, -ax + 0.05);
    float knot = sdRBox(q, float2(0.0, 0.0), float2(0.30, 0.38), 0.12);
    float4 res = accHalo(min(wing, knot), u);
    float3 wc = col * (1.0 - 0.30 * saturate(ax / 1.7) * saturate(abs(q.y) * 1.5));
    wc = lerp(wc, col * 0.65, aa((abs(abs(q.y) - 0.35 * ax / 1.7) - 0.04) * u) * 0.5 * step(0.5, ax));
    res = accLayer(res, wing, u, wc, col * 0.40);
    return accLayer(res, knot, u, col * 0.82, col * 0.40);
}

float4 accCrown(float2 q, float u) {
    float tri = abs(frac(q.x + 0.5) - 0.5) * 2.0;           // 0 at x = -1, 0, 1 (spike tips)
    float top = -0.80 + 0.95 * tri;
    float body = max(max(top - q.y, q.y - 0.95), abs(q.x) - 1.18) * 0.7;
    float tips = min(min(length(q - float2(-1.0, -0.84)), length(q - float2(0.0, -0.84))), length(q - float2(1.0, -0.84))) - 0.15;
    float4 res = accHalo(min(body, tips), u);
    float3 gc = lerp(float3(1.0, 0.88, 0.36), float3(0.88, 0.56, 0.08), saturate(q.y * 0.5 + 0.5));
    gc = lerp(gc, float3(1.0, 0.97, 0.75), exp(-sq((q.x + 0.55) / 0.12)) * 0.5);
    gc = lerp(gc, float3(0.72, 0.42, 0.04), aa((abs(q.y - 0.36) - 0.03) * u) * 0.8);   // band edge
    res = accLayer(res, body, u, gc, float3(0.55, 0.32, 0.02));
    res = accLayer(res, tips, u, float3(1.0, 0.92, 0.55), float3(0.62, 0.38, 0.05));
    float2 gq = float2(q.x - clamp(round(q.x / 0.72), -1.0, 1.0) * 0.72, q.y - 0.64);
    float gem = length(gq / float2(0.17, 0.20)) - 1.0;
    float gi = clamp(round(q.x / 0.72), -1.0, 1.0);
    float3 gemc = gi < -0.5 ? float3(0.20, 0.45, 0.95) : (gi > 0.5 ? float3(0.15, 0.75, 0.35) : float3(0.95, 0.15, 0.20));
    gemc = lerp(gemc, float3(1.0, 1.0, 1.0), saturate(1.0 - length((gq - float2(-0.05, -0.06)) / 0.06)) * 0.8);
    return accLayer(res, gem * 0.17, u, gemc, gemc * 0.45);
}

float4 accTricorn(float2 q, float u) {
    float A = 1.9;
    float domeTop = -0.95 + 0.85 * sq(q.x / 1.15);
    float cornerTop = -0.72 + 0.55 * sq((abs(q.x) - A) / 0.85);
    float top = min(domeTop, cornerTop);
    float bottom = 0.95 - 0.40 * sq(q.x / A);
    float body = max(max(top - q.y, q.y - bottom), abs(q.x) - A) * 0.6;
    float4 res = accHalo(body, u);
    float3 c = lerp(float3(0.14, 0.13, 0.16), float3(0.05, 0.05, 0.06), saturate(q.y * 0.6 + 0.4));
    c = lerp(c, float3(0.95, 0.76, 0.25), aa((abs(q.y - (bottom - 0.12)) - 0.05) * u));   // gold trim
    res = accLayer(res, body, u, c, float3(0.36, 0.36, 0.42));
    // skull and crossbones
    float2 sq0 = q - float2(0.0, -0.18);
    float skull = min(length(sq0 / float2(0.24, 0.21)) - 1.0, sdRBox(sq0, float2(0.0, 0.18), float2(0.13, 0.08), 0.03) / 0.2) * 0.2;
    float bones = min(sdSeg(sq0, float2(-0.32, 0.08), float2(0.32, 0.40)), sdSeg(sq0, float2(0.32, 0.08), float2(-0.32, 0.40))) - 0.045;
    res = over(pm(float3(0.95, 0.95, 0.92), aa(min(skull, bones) * u)), res);
    float eyes = min(length(sq0 - float2(-0.09, 0.0)), length(sq0 - float2(0.09, 0.0))) - 0.055;
    return over(pm(float3(0.06, 0.06, 0.07), aa(eyes * u)), res);
}
)hlsl"  // split: MSVC limits one string literal to 16 KB
R"hlsl(
float4 accCowboy(float2 q, float u) {
    float A = 1.9;
    float yb = 0.66 - 0.48 * pow(abs(q.x) / A, 3.0);
    float brim = max(abs(q.y - yb) - 0.15, abs(q.x) - A) * 0.8;
    float crown = sdRBox(q, float2(0.0, -0.22), float2(0.92, 0.72), 0.36);
    crown = max(crown, -(length((q - float2(0.0, -1.10)) / float2(0.30, 0.22)) - 1.0) * 0.22);   // dent
    float4 res = accHalo(min(brim, crown), u);
    float3 bc = lerp(float3(0.66, 0.43, 0.22), float3(0.42, 0.25, 0.11), saturate((q.y - yb) * 3.0 + 0.5));
    res = accLayer(res, brim, u, bc, float3(0.25, 0.14, 0.05));
    float3 cc = lerp(float3(0.74, 0.50, 0.27), float3(0.52, 0.32, 0.15), saturate(q.x * 0.5 + 0.5));
    cc = lerp(cc, float3(0.25, 0.14, 0.06), step(abs(q.y - 0.30), 0.12));      // band
    cc = lerp(cc, float3(0.90, 0.75, 0.40), aa((length(q - float2(0.45, 0.30)) - 0.08) * u));   // buckle
    return accLayer(res, crown, u, cc, float3(0.28, 0.16, 0.06));
}

float4 accChef(float2 q, float u) {
    float band = sdRBox(q, float2(0.0, 0.64), float2(0.66, 0.31), 0.08);
    float puff = length(q - float2(-0.48, -0.22)) - 0.50;
    puff = smin(puff, length(q - float2(0.48, -0.22)) - 0.50, 0.15);
    puff = smin(puff, length(q - float2(0.0, -0.46)) - 0.52, 0.15);
    puff = smin(puff, sdRBox(q, float2(0.0, 0.05), float2(0.60, 0.40), 0.2), 0.10);
    float4 res = accHalo(min(band, puff), u);
    float3 pc = lerp(float3(1.0, 1.0, 1.0), float3(0.80, 0.86, 0.95), saturate(q.y * 0.8 + 0.6));
    float crease = min(abs(q.x - 0.0), abs(abs(q.x) - 0.42)) - 0.025;
    pc = lerp(pc, float3(0.78, 0.84, 0.94), aa(crease * u) * step(q.y, 0.3) * step(-0.7, q.y) * 0.6);
    res = accLayer(res, puff, u, pc, float3(0.55, 0.62, 0.78));
    float3 bc = float3(0.98, 0.98, 1.0) * (0.92 + 0.08 * step(0.5, frac(q.x * 4.0)));
    return accLayer(res, band, u, bc, float3(0.55, 0.62, 0.78));
}

float4 accCatEars(float2 q, float u) {
    float2 aq = float2(abs(q.x), q.y);
    float ear = sdTri(aq, float2(0.62, 0.95), float2(1.62, 0.95), float2(1.30, -0.95)) - 0.06;
    float inner = sdTri(aq, float2(0.86, 0.82), float2(1.42, 0.82), float2(1.26, -0.45)) - 0.04;
    float4 res = accHalo(ear, u);
    float3 ec = lerp(float3(0.96, 0.96, 0.99), float3(0.78, 0.82, 0.92), saturate(q.y * 0.5 + 0.5));
    res = accLayer(res, ear, u, ec, float3(0.45, 0.48, 0.60));
    return accLayer(res, inner, u, float3(1.0, 0.68, 0.78), float3(0.85, 0.45, 0.58));
}

float4 accWhiskers(float2 q, float u) {
    float2 aq = float2(abs(q.x), q.y);
    float d = 1e3;
    [unroll] for (int k = 0; k < 3; ++k) {
        float fk = float(k) - 1.0;
        float2 a = float2(0.55, fk * 0.28);
        float2 b = float2(2.95, fk * 0.75 - 0.10);
        float2 m = (a + b) * 0.5 + float2(0.0, -0.12);
        d = min(d, min(sdSeg(aq, a, m), sdSeg(aq, m, b)));
    }
    d = d * u - max(0.55, 0.05 * u);
    float4 res = pm(float3(0.85, 0.88, 0.95), aa(d - 0.9) * 0.35);   // light halo for dark backgrounds
    return over(pm(float3(0.12, 0.12, 0.14), aa(d)), res);
}

float4 accMustache(float2 q, float u) {
    float2 aq = float2(abs(q.x), q.y);
    float lobe = sdTaper(aq, float2(0.10, -0.05), float2(1.45, 0.10), 0.50, 0.17);
    float curl = max(abs(length(aq - float2(1.78, -0.22)) - 0.34) - 0.12, aq.y - (-0.12));
    curl = min(curl, max(abs(length(aq - float2(1.78, -0.22)) - 0.34) - 0.12, aq.x - 1.78));
    float d = smin(lobe, curl, 0.08);
    float4 res = accHalo(d, u);
    float3 c = lerp(float3(0.20, 0.14, 0.10), float3(0.07, 0.05, 0.04), saturate(q.y + 0.4));
    c += 0.18 * exp(-sq((q.y + 0.22) / 0.07)) * step(aq.x, 1.3);
    return accLayer(res, d, u, c, float3(0.0, 0.0, 0.0));
}

float4 accBeard(float2 q, float u) {
    float ang = atan2(q.y, q.x);
    float2 bq = q / float2(1.0, 1.05);
    float body = length(bq - float2(0.0, -0.10)) - 0.92 - 0.05 * sin(ang * 11.0);
    body = max(body, -0.62 - q.y);
    float d = body;
    float4 res = accHalo(d, u);
    float3 c = lerp(float3(1.0, 1.0, 1.0), float3(0.80, 0.86, 0.96), saturate(q.y * 0.7 + 0.2));
    float waves = abs(frac(q.x * 2.2 + 0.25 * sin(q.y * 7.0)) - 0.5) - 0.44;
    c = lerp(c, float3(0.78, 0.84, 0.95), aa(waves * u / 2.2) * step(-0.35, q.y) * 0.45);
    c = lerp(c, float3(1.0, 1.0, 1.0), exp(-sq((q.y + 0.45) / 0.12)) * 0.5);
    return accLayer(res, d, u, c, float3(0.50, 0.58, 0.76));
}

float4 accPipe(float2 q, float u) {
    float stem = sdSeg(q, float2(-1.45, -0.05), float2(0.60, 0.22)) - 0.11;
    float bowl = sdRBox(q, float2(0.98, -0.18), float2(0.36, 0.58), 0.20);
    bowl = smin(bowl, sdSeg(q, float2(0.55, 0.22), float2(0.9, 0.3)) - 0.16, 0.1);
    float4 res = accHalo(min(stem, bowl), u);
    float3 sc = q.x < -1.05 ? float3(0.08, 0.07, 0.07) : float3(0.40, 0.22, 0.10);
    res = accLayer(res, stem, u, sc, float3(0.12, 0.06, 0.02));
    float3 bc = lerp(float3(0.66, 0.38, 0.17), float3(0.40, 0.21, 0.08), saturate(q.x - 0.7));
    bc *= 0.92 + 0.08 * sin(q.y * 22.0 + q.x * 4.0);
    res = accLayer(res, bowl, u, bc, float3(0.20, 0.10, 0.03));
    float hole = length((q - float2(0.98, -0.70)) / float2(0.26, 0.09)) - 1.0;
    return over(pm(float3(0.10, 0.06, 0.04), aa(hole * 0.09 * u)), res);
}

float4 accGooglyEye(float2 q, float u, float ang) {
    float ball = length(q) - 0.92;
    float4 res = accHalo(ball, u);
    float3 c = lerp(float3(1.0, 1.0, 1.0), float3(0.82, 0.86, 0.94), saturate(q.y * 0.6 + 0.3));
    res = accLayer(res, ball, u, c, float3(0.35, 0.38, 0.46));
    float2 pc = 0.32 * float2(cos(ang), sin(ang));            // ~35% of the ball radius
    float pupil = length(q - pc) - 0.48;
    res = over(pm(float3(0.03, 0.03, 0.04), aa(pupil * u)), res);
    float hl = length(q - pc - float2(-0.14, -0.16)) - 0.12;
    return over(pm(float3(1.0, 1.0, 1.0), aa(hl * u) * 0.9), res);
}

float4 accFlower(float2 q, float u) {
    float ang = atan2(q.y, q.x) + 0.3;
    float k = 1.2566371;                                       // 2 pi / 5
    float a5 = ang - k * floor(ang / k + 0.5);
    float2 pq = length(q) * float2(cos(a5), sin(a5));
    float petal = length((pq - float2(0.55, 0.0)) / float2(0.42, 0.33)) - 1.0;
    petal *= 0.33;
    float centre = length(q) - 0.30;
    float4 res = accHalo(min(petal, centre), u);
    float3 pc = lerp(float3(1.0, 0.72, 0.84), float3(0.92, 0.30, 0.52), saturate(length(q) * 1.2 - 0.3));
    res = accLayer(res, petal, u, pc, float3(0.62, 0.12, 0.30));
    float3 cc = lerp(float3(1.0, 0.90, 0.30), float3(0.92, 0.62, 0.08), saturate(q.y * 1.5 + 0.5));
    return accLayer(res, centre, u, cc, float3(0.62, 0.38, 0.02));
}

float4 accBroom(float2 q, float u) {
    float stick = sdSeg(q, float2(0.0, -0.98), float2(0.0, 0.30)) - 0.07;
    float tq = saturate((q.y - 0.22) / 0.76);
    float bw = lerp(0.22, 0.52, tq);
    float bristle = max(abs(q.x) - bw, max(0.22 - q.y, q.y - 0.98));
    bristle = max(bristle, sdRBox(q, float2(0.0, 0.60), float2(0.56, 0.40), 0.12));
    float fingers = min(sdSeg(q, float2(-0.20, -0.18), float2(0.18, -0.10)), sdSeg(q, float2(-0.20, -0.05), float2(0.18, 0.02))) - 0.05;
    float4 res = accHalo(min(min(stick, bristle), fingers), u);
    res = accLayer(res, stick, u, float3(0.62, 0.42, 0.22), float3(0.30, 0.18, 0.07));
    float3 bc = lerp(float3(0.96, 0.82, 0.42), float3(0.80, 0.60, 0.22), saturate(q.y - 0.3));
    bc *= 0.86 + 0.14 * step(0.5, frac(q.x * u / 2.0 + 0.3));
    bc = lerp(bc, float3(0.80, 0.18, 0.15), step(abs(q.y - 0.36), 0.05));
    res = accLayer(res, bristle, u, bc, float3(0.50, 0.34, 0.08));
    return accLayer(res, fingers, u, float3(0.47, 0.31, 0.18), float3(0.27, 0.16, 0.08));
}

float4 accBandana(float2 q, float u) {
    float band = sdRBox(q, float2(0.0, -0.68), float2(1.30, 0.24), 0.18);
    float tri = sdTri(q, float2(-0.95, -0.62), float2(0.95, -0.62), float2(0.05, 0.92)) - 0.06;
    float d = min(band, tri);
    float4 res = accHalo(d, u);
    float3 c = lerp(float3(0.92, 0.18, 0.18), float3(0.62, 0.08, 0.10), saturate(q.y * 0.4 + 0.3));
    float2 dq = frac(q * 2.4) - 0.5;
    c = lerp(c, float3(1.0, 0.96, 0.94), aa((length(dq) - 0.10) * u / 2.4) * 0.9);
    c = lerp(c, c * 0.7, aa((abs(q.y + 0.46) - 0.025) * u) * 0.8);
    return accLayer(res, d, u, c, float3(0.40, 0.04, 0.05));
}

float4 accMonocle(float2 q, float u) {
    float A = 0.8;
    float ring = abs(length(q) - 0.40) - 0.085;
    float2 c0 = float2(0.30, 0.28), c1 = float2(0.62, 0.96);
    float t = saturate((q.y - c0.y) / (c1.y - c0.y));
    float cx = lerp(c0.x, c1.x, t) - 0.12 * sin(t * 3.14159);
    float chainD = max(abs(q.x - cx) - 0.06, max(c0.y - q.y, q.y - c1.y));
    float links = abs(frac(q.y * 5.0) - 0.5) - 0.40;
    chainD = max(chainD, links / 5.0);
    chainD = max(chainD, abs(q.x) - A);
    float4 res = accHalo(min(ring, chainD), u);
    float lens = length(q) - 0.40;
    float hl = saturate(1.0 - length((q - float2(-0.14, -0.15)) / float2(0.12, 0.07)));
    res = over(pm(float3(0.88, 0.95, 1.0), aa(lens * u) * (0.15 + 0.7 * hl)), res);
    res = accLayer(res, chainD, u, float3(0.95, 0.76, 0.30), float3(0.55, 0.38, 0.06));
    return accLayer(res, ring, u, float3(1.0, 0.84, 0.36), float3(0.58, 0.40, 0.05));
}

float4 accessory(float2 p, float2 hs, uint v, float ang) {
    float u = max(hs.y, 0.5);
    float2 q = p / u;
    float4 c = float4(0.0, 0.0, 0.0, 0.0);
    [forcecase] switch (v) {
        case 0:  c = accGlasses(q, u, true); break;
        case 1:  c = accGlasses(q, u, false); break;
        case 2:  c = accEyePatch(q, u); break;
        case 3:  c = accHeadset(q, u, false); break;
        case 4:  c = accHeadset(q, u, true); break;
        case 5:  c = accBowTie(q, u, float3(0.88, 0.14, 0.18)); break;
        case 6:  c = accCrown(q, u); break;
        case 7:  c = accTricorn(q, u); break;
        case 8:  c = accCowboy(q, u); break;
        case 9:  c = accChef(q, u); break;
        case 10: c = accCatEars(q, u); break;
        case 11: c = accWhiskers(q, u); break;
        case 12: c = accMustache(q, u); break;
        case 13: c = accBeard(q, u); break;
        case 14: c = accPipe(q, u); break;
        case 15: c = accGooglyEye(q, u, ang); break;
        case 16: c = accFlower(q, u); break;
        case 17: c = accBroom(q, u); break;
        case 18: c = accBandana(q, u); break;
        default: c = accMonocle(q, u); break;
    }
    return c;
}
)hlsl"  // split: MSVC limits one string literal to 16 KB
R"hlsl(

float4 main(VOut i) : SV_Target {
    float2 p = i.lp;
    float2 hs = i.hs;
    float r = min(hs.x, hs.y);
    uint shape = i.sv.x;
    uint v = i.sv.y;
    float4 c = float4(0.0, 0.0, 0.0, 0.0);
    [forcecase] switch (shape) {
        case 0:  c = flake(p, r, v); break;
        case 1:  c = snowball(p, r); break;
        case 2:  c = coal(p, r); break;
        case 3:  c = carrot(p, hs); break;
        case 4:  c = stick(p, hs); break;
        case 5:  c = mitten(p, hs, v); break;
        case 6:  c = shovel(p, hs); break;
        case 7:  c = ring(p, hs); break;
        case 8:  c = snowplow(p, hs, v); break;
        case 9:  c = snowChunk(p, hs, v & 3u); break;
        case 10: c = icicle(p, hs, v & 3u); break;
        case 11: c = spark(p, hs, v); break;
        case 12: c = bulb(p, hs, v & 15u); break;
        case 13: c = garlandWire(p, hs); break;
        case 14: c = splat(p, hs, v & 3u); break;
        case 15: c = droplet(p, hs, v & 1u); break;
        case 16: c = hat(p, hs, v & 3u); break;
        case 17: c = scarf(p, hs, v & 3u); break;
        default: c = accessory(p, hs, v, i.ang); break;
    }
    return c * (i.op * gOpacity);
}
)hlsl";

// ---------------------------------------------------------------------------------------
// Drifts: the mesh is only a covering hull; the profile is evaluated per pixel.
// ---------------------------------------------------------------------------------------
inline constexpr char kDriftVS[] = R"hlsl(
struct VIn {
    float2 pos  : POSITION;   // virtual-desktop px
    float4 prof : PROFILE;    // topY, height, baseY, x - x0
    float3 dpiOp : DPIOP;     // dpiScale, strip opacity, wetness
};
struct VOut {
    float4 pos  : SV_Position;
    float4 prof : TEXCOORD0;
    float3 dpiOp : TEXCOORD1;
};
VOut main(VIn i) {
    VOut o;
    o.pos = toClip(i.pos);
    o.prof = i.prof;
    o.dpiOp = i.dpiOp;
    return o;
}
)hlsl";

inline constexpr char kDriftPS[] = R"hlsl(
struct VOut {
    float4 pos  : SV_Position;
    float4 prof : TEXCOORD0;
    float3 dpiOp : TEXCOORD1;
};
float hash21(float2 p) {
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}
float4 main(VOut i) : SV_Target {
    float wy = i.pos.y + gOrigin.y;
    float topY = i.prof.x, h = i.prof.y, baseY = i.prof.z, lx = i.prof.w;
    float s = max(i.dpiOp.x, 0.5);
    float rawSlope = ddx(topY);
    float d = (wy - topY) * rsqrt(1.0 + min(rawSlope * rawSlope, 9.0));
    // lumpy profiles have steep local slopes: shade with a bounded slope so lumps stay soft
    float slope = clamp(rawSlope, -1.2, 1.2);   // px below the snow surface
    float f = 1.5 * s;                                    // feather
    float aTop = saturate(d / f + 0.5);
    aTop = aTop * aTop * (3.0 - 2.0 * aTop);
    float aBot = saturate(baseY + 1.5 * s - wy + 0.5);
    float aH = saturate(h / s);
    float depth = max(d, 0.0) / s;                        // in 96-DPI px

    float t = 1.0 - exp(-depth / 14.0);
    float3 col = lerp(float3(1.0, 1.0, 1.0), float3(0.80, 0.87, 0.97), t);
    // light comes from the upper left: faces descending to the right get a bluish shade
    col = lerp(col, float3(0.72, 0.82, 0.96), saturate(slope * 0.7) * 0.55 * exp(-depth / 10.0));
    // shallow hollows are slightly bluer than thick snow
    col = lerp(col, float3(0.78, 0.86, 0.97), (1.0 - saturate(h / (7.0 * s))) * 0.35);
    // crest highlight
    float hb = (depth - 3.0) / 2.2;
    float wet = saturate(i.dpiOp.z);
    col = lerp(col, float3(1.0, 1.0, 1.0), exp(-hb * hb) * 0.65 * saturate(-slope * 0.8 + 0.75) * (1.0 - 0.8 * wet));
    // grain + sparkles, anchored to the strip so they travel with the window
    float2 cell = floor(float2(lx, wy - baseY) / (1.6 * s));
    float n = hash21(cell);
    float glint = step(0.972, n) * exp(-depth / 16.0) * saturate(depth - 1.0);
    col = lerp(col, float3(0.74, 0.82, 0.95), step(n, 0.05) * 0.35 * saturate(depth - 2.0));
    col = lerp(col, float3(1.0, 1.0, 1.0), glint);
    // thaw: greyer / bluer slush with a sharp glossy highlight just below the crest
    col = lerp(col, col * float3(0.70, 0.78, 0.88), wet);
    float hw = (depth - 1.6) / 0.8;
    float wetHl = exp(-hw * hw) * wet * saturate(-slope * 0.8 + 0.9);
    col = lerp(col, float3(1.0, 1.0, 1.0), wetHl);
    float wetGloss = step(0.955, n) * wet * saturate(depth - 1.5) * exp(-depth / 20.0);
    col = lerp(col, float3(1.0, 1.0, 1.0), wetGloss);
    // thin blue-grey rim so the crest stays readable on white backgrounds
    col = lerp(col, lerp(float3(0.64, 0.74, 0.90), float3(0.48, 0.58, 0.74), wet), (1.0 - saturate(d / f)) * 0.6);

    float alpha = lerp(0.88, 0.95, t);
    alpha = max(alpha, glint);
    alpha *= lerp(1.0, 0.62, wet);
    alpha = max(alpha, max(wetGloss, wetHl * 0.95));
    alpha *= aTop * aBot * aH;
    return float4(col * alpha, alpha) * (gOpacity * saturate(i.dpiOp.y));
}
)hlsl";

}  // namespace snow::shaders
