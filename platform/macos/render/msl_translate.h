// HLSL -> Metal Shading Language translation of the overlay shaders.
//
// The look of every sprite and drift is defined ONCE, in the HLSL of
// platform/windows/rendering/d3d11/shaders.h. Instead of a hand-maintained second copy, the
// macOS renderer translates that source at start-up with a handful of mechanical rewrites
// (the HLSL is written in a deliberately small subset: plain functions, float/uint vectors,
// no textures, no out-parameters) and wraps each stage in a hand-written Metal entry point
// (see kWrappers in msl_translate.cpp). The result is compiled with
// -[MTLDevice newLibraryWithSource:], so the CI does not need the offline Metal toolchain.
//
// Rewrites (all covered by tests/mac_unit_tests.cpp, the compile itself by --selftest):
//   * cbuffer Frame {...}            -> removed; the values come from `constant SFFrame& F_`
//   * gOrigin/gInvHalf/gDt/gOpacity  -> F_.gOrigin ...; toClip() and main() get the F_ parameter
//   * main(...) : SV_Target          -> <stage>_main(constant SFFrame& F_, ...)
//   * struct member semantics        -> dropped (": TEXCOORD0", "nointerpolation")
//   * [branch] [unroll] [forcecase]  -> dropped
//   * static const                   -> constant (program-scope tables)
//   * lerp/frac/ddx/ddy              -> sf_lerp/fract/dfdx/dfdy
//   * max/min/clamp/step/smoothstep/pow -> sf_* overloads that accept HLSL's vector-with-scalar
//                                          argument mixes (MSL only has same-type overloads)
//   * MSL keywords used as names     -> renamed (e.g. `patch` -> `patch_`)
// Pure C++, no Apple headers: unit-testable anywhere.
#pragma once
#include <string>

namespace snow::mac {

enum class ShaderStage { SpriteVertex, SpriteFragment, DriftVertex, DriftFragment };

// Full, self-contained MSL source for one stage: prelude + translated common block +
// translated stage + Metal entry point.
std::string buildMslLibrary(ShaderStage stage);

// Name of the Metal entry point defined by buildMslLibrary(stage).
const char* mslEntryPoint(ShaderStage stage);

// The mechanical translation of one HLSL block. `mainName` replaces the HLSL `main`.
std::string translateHlsl(const std::string& hlsl, const std::string& mainName);

}  // namespace snow::mac
