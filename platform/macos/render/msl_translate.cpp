#include "render/msl_translate.h"

#include <regex>

// The single source of truth for the look: the Windows HLSL (pure string constants, no
// Windows headers).
#include "../../windows/rendering/d3d11/shaders.h"

namespace snow::mac {
namespace {

// ---- MSL prelude ---------------------------------------------------------------------
// SFFrame mirrors FrameConstants in metal_renderer.mm (32 bytes). Everything the HLSL sees
// is in PIXELS of the render target: the wrappers multiply point coordinates by gScale
// (backing scale factor), exactly as if Windows ran the simulation at dpiScale = gScale.
const char kPrelude[] = R"msl(
#include <metal_stdlib>
using namespace metal;

struct SFFrame {
    float2 gOrigin;    // render target origin in virtual-desktop PIXELS (points * gScale)
    float2 gInvHalf;   // 2 / render target size in pixels
    float  gDt;        // seconds since the snapshot was taken (extrapolation)
    float  gOpacity;   // global opacity
    float  gScale;     // pixels per point
    float  gPad;
};

// HLSL intrinsics accept vector/scalar mixes that MSL's same-type overloads do not.
inline float sf_max(float a, float b) { return max(a, b); }
inline float sf_min(float a, float b) { return min(a, b); }
inline float sf_clamp(float x, float a, float b) { return clamp(x, a, b); }
inline float sf_step(float e, float x) { return step(e, x); }
inline float sf_smoothstep(float a, float b, float x) { return smoothstep(a, b, x); }
inline float sf_lerp(float a, float b, float t) { return mix(a, b, t); }
inline float sf_pow(float a, float b) { return pow(a, b); }
#define SF_VEC_OVERLOADS(T) \
inline T sf_max(T a, T b) { return max(a, b); } \
inline T sf_max(T a, float b) { return max(a, T(b)); } \
inline T sf_max(float a, T b) { return max(T(a), b); } \
inline T sf_min(T a, T b) { return min(a, b); } \
inline T sf_min(T a, float b) { return min(a, T(b)); } \
inline T sf_min(float a, T b) { return min(T(a), b); } \
inline T sf_clamp(T x, T a, T b) { return clamp(x, a, b); } \
inline T sf_clamp(T x, float a, float b) { return clamp(x, T(a), T(b)); } \
inline T sf_step(T e, T x) { return step(e, x); } \
inline T sf_step(float e, T x) { return step(T(e), x); } \
inline T sf_step(T e, float x) { return step(e, T(x)); } \
inline T sf_smoothstep(T a, T b, T x) { return smoothstep(a, b, x); } \
inline T sf_smoothstep(float a, float b, T x) { return smoothstep(T(a), T(b), x); } \
inline T sf_lerp(T a, T b, T t) { return mix(a, b, t); } \
inline T sf_lerp(T a, T b, float t) { return mix(a, b, T(t)); } \
inline T sf_pow(T a, T b) { return pow(a, b); } \
inline T sf_pow(T a, float b) { return pow(a, T(b)); }
SF_VEC_OVERLOADS(float2)
SF_VEC_OVERLOADS(float3)
SF_VEC_OVERLOADS(float4)
#undef SF_VEC_OVERLOADS
)msl";

// ---- Metal entry points ----------------------------------------------------------------
// Instance data is read as raw floats/bytes (layout of snow::SpriteInstance / DriftVertex,
// 36 bytes each, static_asserted in metal_renderer.mm) so no struct packing rules apply.
const char kSpriteVertexWrapper[] = R"msl(
struct SFSpriteVarying {
    float4 pos [[position]];
    float2 lp  [[user(sf_lp)]];
    float2 hs  [[user(sf_hs), flat]];
    float  op  [[user(sf_op), flat]];
    uint2  sv  [[user(sf_sv), flat]];
    float  ang [[user(sf_ang), flat]];
};
vertex SFSpriteVarying sf_sprite_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                        const device float* inst [[buffer(0)]],
                                        constant SFFrame& F_ [[buffer(1)]]) {
    const device float* f = inst + iid * 9u;
    const device uchar* b = (const device uchar*)(f + 8);
    float S = F_.gScale;
    VIn i;
    i.posVel = float4(f[0] * S, f[1] * S, f[2] * S, f[3] * S);
    i.sizeRot = float4(f[4] * S, f[5] * S, f[6], f[7]);
    i.shapeVar = uint2(uint(b[0]), uint(b[1]));
    i.vid = vid;
    VOut o = sf_sprite_vs_main(F_, i);
    SFSpriteVarying r;
    r.pos = o.pos;
    r.lp = o.lp;
    r.hs = o.hs;
    r.op = o.op;
    r.sv = o.sv;
    r.ang = o.ang;
    return r;
}
)msl";

const char kSpriteFragmentWrapper[] = R"msl(
struct SFSpriteVarying {
    float4 pos [[position]];
    float2 lp  [[user(sf_lp)]];
    float2 hs  [[user(sf_hs), flat]];
    float  op  [[user(sf_op), flat]];
    uint2  sv  [[user(sf_sv), flat]];
    float  ang [[user(sf_ang), flat]];
};
fragment float4 sf_sprite_fragment(SFSpriteVarying m [[stage_in]], constant SFFrame& F_ [[buffer(0)]]) {
    VOut i;
    i.pos = m.pos;
    i.lp = m.lp;
    i.hs = m.hs;
    i.op = m.op;
    i.sv = m.sv;
    i.ang = m.ang;
    return sf_sprite_ps_main(F_, i);
}
)msl";

const char kDriftVertexWrapper[] = R"msl(
struct SFDriftVarying {
    float4 pos   [[position]];
    float4 prof  [[user(sf_prof)]];
    float3 dpiOp [[user(sf_dpiop)]];
};
vertex SFDriftVarying sf_drift_vertex(uint vid [[vertex_id]], const device float* verts [[buffer(0)]],
                                      constant SFFrame& F_ [[buffer(1)]]) {
    const device float* f = verts + vid * 9u;
    float S = F_.gScale;
    VIn i;
    i.pos = float2(f[0], f[1]) * S;
    i.prof = float4(f[2], f[3], f[4], f[5]) * S;   // topY, height, baseY, x - x0
    i.dpiOp = float3(f[6] * S, f[7], f[8]);         // dpi, strip opacity, wetness
    VOut o = sf_drift_vs_main(F_, i);
    SFDriftVarying r;
    r.pos = o.pos;
    r.prof = o.prof;
    r.dpiOp = o.dpiOp;
    return r;
}
)msl";

const char kDriftFragmentWrapper[] = R"msl(
struct SFDriftVarying {
    float4 pos   [[position]];
    float4 prof  [[user(sf_prof)]];
    float3 dpiOp [[user(sf_dpiop)]];
};
fragment float4 sf_drift_fragment(SFDriftVarying m [[stage_in]], constant SFFrame& F_ [[buffer(0)]]) {
    VOut i;
    i.pos = m.pos;
    i.prof = m.prof;
    i.dpiOp = m.dpiOp;
    return sf_drift_ps_main(F_, i);
}
)msl";

std::string replaceAll(const std::string& s, const std::regex& re, const char* with) {
    return std::regex_replace(s, re, with);
}

// Drops HLSL semantics (": TEXCOORD0", ": SV_Position") inside struct bodies only, so a
// ternary "a ? b : C;" elsewhere can never be touched.
std::string stripStructSemantics(const std::string& src) {
    static const std::regex structRe(R"(struct\s+\w+\s*\{[^}]*\})");
    static const std::regex semRe(R"(\s*:\s*[A-Za-z_]\w*\s*;)");
    std::string out;
    auto it = std::sregex_iterator(src.begin(), src.end(), structRe);
    std::size_t last = 0;
    for (; it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        const std::size_t at = static_cast<std::size_t>(m.position(0));
        out.append(src, last, at - last);
        out += std::regex_replace(m.str(0), semRe, ";");
        last = at + static_cast<std::size_t>(m.length(0));
    }
    out.append(src, last, std::string::npos);
    return out;
}

}  // namespace

std::string translateHlsl(const std::string& hlsl, const std::string& mainName) {
    std::string s = hlsl;
    // Constant buffer: the values travel as `constant SFFrame& F_`.
    s = replaceAll(s, std::regex(R"(cbuffer\s+\w+\s*:\s*register\s*\(\s*\w+\s*\)\s*\{[^}]*\}\s*;?)"), "");
    // MSL keywords that are plain identifiers in HLSL.
    s = replaceAll(s, std::regex(R"(\b(patch|vertex|fragment|kernel|device|thread|threadgroup|sampler|texture|constant)\b)"),
                   "$1_");
    s = replaceAll(s, std::regex(R"(\[(branch|unroll|forcecase|flatten|loop|fastopt|call)\]\s*)"), "");
    s = replaceAll(s, std::regex(R"(\bstatic\s+const\b)"), "constant");
    s = replaceAll(s, std::regex(R"(\bnointerpolation\s+)"), "");
    s = stripStructSemantics(s);
    s = replaceAll(s, std::regex(R"(\)\s*:\s*SV_\w+\s*\{)"), ") {");
    // Intrinsics.
    s = replaceAll(s, std::regex(R"(\blerp\s*\()"), "sf_lerp(");
    s = replaceAll(s, std::regex(R"(\bfrac\s*\()"), "fract(");
    s = replaceAll(s, std::regex(R"(\bddx\s*\()"), "dfdx(");
    s = replaceAll(s, std::regex(R"(\bddy\s*\()"), "dfdy(");
    s = replaceAll(s, std::regex(R"(\b(max|min|clamp|step|smoothstep|pow)\s*\()"), "sf_$1(");
    // Frame constants.
    s = replaceAll(s, std::regex(R"(\b(gOrigin|gInvHalf|gDt|gOpacity|gPad)\b)"), "F_.$1");
    s = replaceAll(s, std::regex(R"(\btoClip\s*\()"), "toClip(F_, ");
    s = replaceAll(s, std::regex(R"((\bfloat4\s+)toClip\(F_, )"), "$1toClip(constant SFFrame& F_, ");
    s = std::regex_replace(s, std::regex(R"((\b\w+\s+)main\s*\()"), "$1" + mainName + "(constant SFFrame& F_, ");
    return s;
}

const char* mslEntryPoint(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::SpriteVertex: return "sf_sprite_vertex";
        case ShaderStage::SpriteFragment: return "sf_sprite_fragment";
        case ShaderStage::DriftVertex: return "sf_drift_vertex";
        case ShaderStage::DriftFragment: return "sf_drift_fragment";
    }
    return "";
}

std::string buildMslLibrary(ShaderStage stage) {
    std::string src = kPrelude;
    src += translateHlsl(snow::shaders::kCommon, "sf_common_main");
    switch (stage) {
        case ShaderStage::SpriteVertex:
            src += translateHlsl(snow::shaders::kSpriteVS, "sf_sprite_vs_main");
            src += kSpriteVertexWrapper;
            break;
        case ShaderStage::SpriteFragment:
            src += translateHlsl(snow::shaders::kSpritePS, "sf_sprite_ps_main");
            src += kSpriteFragmentWrapper;
            break;
        case ShaderStage::DriftVertex:
            src += translateHlsl(snow::shaders::kDriftVS, "sf_drift_vs_main");
            src += kDriftVertexWrapper;
            break;
        case ShaderStage::DriftFragment:
            src += translateHlsl(snow::shaders::kDriftPS, "sf_drift_ps_main");
            src += kDriftFragmentWrapper;
            break;
    }
    return src;
}

}  // namespace snow::mac
