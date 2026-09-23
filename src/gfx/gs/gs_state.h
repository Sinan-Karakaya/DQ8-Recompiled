// Decoders for the GS registers the frontend hands through as raw 64-bit
// values, plus the CRTC-side registers Present() receives.
//
// GSDrawState carries TEST, ALPHA, CLAMP and TEX1 undecoded, so every backend
// would otherwise repeat the same bit extraction. Keeping it here means the
// field layouts are written down once and both backends agree by construction.
#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>

namespace dq8::gfx {

// TEST: alpha test, destination alpha test, depth test.
enum GsAlphaTest : uint8_t {
    kAlphaNever = 0,
    kAlphaAlways = 1,
    kAlphaLess = 2,
    kAlphaLEqual = 3,
    kAlphaEqual = 4,
    kAlphaGEqual = 5,
    kAlphaGreater = 6,
    kAlphaNotEqual = 7,
};

// What a fragment failing the alpha test may still update.
enum GsAlphaFail : uint8_t {
    kAfailKeep = 0,     // nothing
    kAfailFbOnly = 1,   // colour, not depth
    kAfailZbOnly = 2,   // depth, not colour
    kAfailRgbOnly = 3,  // colour without alpha, not depth
};

enum GsDepthTest : uint8_t {
    kDepthNever = 0,
    kDepthAlways = 1,
    kDepthGEqual = 2,
    kDepthGreater = 3,
};

struct GsTestState {
    bool alphaTestEnabled = false;
    GsAlphaTest alphaTest = kAlphaAlways;
    uint8_t alphaReference = 0u;
    GsAlphaFail alphaFail = kAfailKeep;
    bool destinationAlphaTestEnabled = false;
    // DATM: which destination alpha bit passes.
    bool destinationAlphaMode = false;
    bool depthTestEnabled = false;
    GsDepthTest depthTest = kDepthAlways;
};

GsTestState gsDecodeTest(uint64_t test);

// ALPHA: output = (A - B) * C >> 7 + D, over selectable operands.
enum GsBlendColor : uint8_t {
    kBlendSource = 0,
    kBlendDest = 1,
    kBlendZero = 2,
};

enum GsBlendAlpha : uint8_t {
    kBlendAlphaSource = 0,
    kBlendAlphaDest = 1,
    kBlendAlphaFixed = 2,
};

struct GsAlphaState {
    GsBlendColor a = kBlendSource;
    GsBlendColor b = kBlendSource;
    GsBlendAlpha c = kBlendAlphaSource;
    GsBlendColor d = kBlendSource;
    uint8_t fix = 0u;
};

GsAlphaState gsDecodeAlpha(uint64_t alpha);

// CLAMP: per-axis wrap mode plus the REGION_* bounds.
enum GsWrapMode : uint8_t {
    kWrapRepeat = 0,
    kWrapClamp = 1,
    kWrapRegionClamp = 2,
    kWrapRegionRepeat = 3,
};

struct GsClampState {
    GsWrapMode wrapU = kWrapRepeat;
    GsWrapMode wrapV = kWrapRepeat;
    uint32_t minU = 0u;
    uint32_t maxU = 0u;
    uint32_t minV = 0u;
    uint32_t maxV = 0u;
};

GsClampState gsDecodeClamp(uint64_t clamp);

// TEX1: filtering and mip selection. Only the filter bits are consumed today.
struct GsTex1State {
    bool magLinear = false;
    // MMIN encodes both the minification filter and whether mipmaps are used.
    uint8_t minFilter = 0u;
    bool minLinear = false;
    uint8_t maxMipLevel = 0u;
};

GsTex1State gsDecodeTex1(uint64_t tex1);

// TEX0.TFX: how texel and vertex colour combine.
enum GsTextureFunction : uint8_t {
    kTexModulate = 0,
    kTexDecal = 1,
    kTexHighlight = 2,
    kTexHighlight2 = 3,
};

// ---------------------------------------------------------------------------
// CRTC side.
// ---------------------------------------------------------------------------

struct GsPmodeState {
    bool enableCircuit1 = false;
    bool enableCircuit2 = false;
    // MMOD: alpha for the blend between circuits comes from ALP, not circuit 1.
    bool useFixedAlpha = false;
    bool alphaFromCircuit2 = false;
    // SLBG: circuit 2 blends against BGCOLOR instead of circuit 1.
    bool blendWithBackground = false;
    uint8_t fixedAlpha = 0u;
};

GsPmodeState gsDecodePmode(uint64_t pmode);

struct GsSmode2State {
    bool interlaced = false;
    // FFMD. Names what the display buffer *holds*, not just how it is scanned:
    // FRAME (true) reads a half-height buffer whole once per field and the host
    // frame line-doubles it; FIELD (false) holds a full frame whose two fields
    // are alternate rows, so weaving is just reading every row.
    bool frameMode = false;
};

GsSmode2State gsDecodeSmode2(uint64_t smode2);

struct GsDisplaySetup {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t fbp = 0u;
    uint32_t fbw = 0u;
    uint32_t psm = 0u;
    uint32_t originX = 0u;
    uint32_t originY = 0u;
    bool valid = false;
};

// Decodes one read circuit. `enabled` is the matching PMODE bit; a circuit
// whose DISPFB and DISPLAY are both blank is reported invalid even when
// enabled, which is the normal state before the game programs the CRTC.
GsDisplaySetup gsDecodeDisplay(uint64_t dispfb, uint64_t display, bool enabled);

} // namespace dq8::gfx
