#include "gfx/gs/gs_state.h"

#include <algorithm>

namespace dq8::gfx {

namespace {

// A CRTC programmed with a degenerate size is treated as standard NTSC rather
// than propagated; the frontend's software backend makes the same choice, so
// image diffs between the two stay meaningful.
constexpr uint32_t kFallbackDisplayWidth = 640u;
constexpr uint32_t kFallbackDisplayHeight = 448u;
constexpr uint32_t kMaxDisplayWidth = 1024u;
constexpr uint32_t kMaxDisplayHeight = 1024u;

} // namespace

GsTestState gsDecodeTest(uint64_t test) {
    GsTestState state{};
    state.alphaTestEnabled = (test & 0x1ull) != 0ull;
    state.alphaTest = static_cast<GsAlphaTest>((test >> 1u) & 0x7ull);
    state.alphaReference = static_cast<uint8_t>((test >> 4u) & 0xffull);
    state.alphaFail = static_cast<GsAlphaFail>((test >> 12u) & 0x3ull);
    state.destinationAlphaTestEnabled = ((test >> 14u) & 0x1ull) != 0ull;
    state.destinationAlphaMode = ((test >> 15u) & 0x1ull) != 0ull;
    state.depthTestEnabled = ((test >> 16u) & 0x1ull) != 0ull;
    state.depthTest = static_cast<GsDepthTest>((test >> 17u) & 0x3ull);
    return state;
}

GsAlphaState gsDecodeAlpha(uint64_t alpha) {
    GsAlphaState state{};
    // A, B and D select Cs/Cd/0; the two reserved encodings also mean 0.
    auto decodeColor = [](uint64_t value) {
        return value >= 2u ? kBlendZero : static_cast<GsBlendColor>(value);
    };
    state.a = decodeColor(alpha & 0x3ull);
    state.b = decodeColor((alpha >> 2u) & 0x3ull);
    // C selects As/Ad/FIX, with 3 aliasing FIX.
    const uint64_t c = (alpha >> 4u) & 0x3ull;
    state.c = c >= 2u ? kBlendAlphaFixed : static_cast<GsBlendAlpha>(c);
    state.d = decodeColor((alpha >> 6u) & 0x3ull);
    state.fix = static_cast<uint8_t>((alpha >> 32u) & 0xffull);
    return state;
}

GsClampState gsDecodeClamp(uint64_t clamp) {
    GsClampState state{};
    state.wrapU = static_cast<GsWrapMode>(clamp & 0x3ull);
    state.wrapV = static_cast<GsWrapMode>((clamp >> 2u) & 0x3ull);
    state.minU = static_cast<uint32_t>((clamp >> 4u) & 0x3ffull);
    state.maxU = static_cast<uint32_t>((clamp >> 14u) & 0x3ffull);
    state.minV = static_cast<uint32_t>((clamp >> 24u) & 0x3ffull);
    state.maxV = static_cast<uint32_t>((clamp >> 34u) & 0x3ffull);
    return state;
}

GsTex1State gsDecodeTex1(uint64_t tex1) {
    GsTex1State state{};
    state.magLinear = ((tex1 >> 5u) & 0x1ull) != 0ull;
    state.minFilter = static_cast<uint8_t>((tex1 >> 6u) & 0x7ull);
    // MMIN 0 and 1 are the non-mipmapped NEAREST/LINEAR pair; 2..5 add a mip
    // selection whose base filter is linear for the odd encodings.
    state.minLinear = state.minFilter == 1u || state.minFilter == 3u ||
                      state.minFilter == 5u;
    state.maxMipLevel = static_cast<uint8_t>((tex1 >> 2u) & 0x7ull);
    return state;
}

GsPmodeState gsDecodePmode(uint64_t pmode) {
    GsPmodeState state{};
    state.enableCircuit1 = (pmode & 0x1ull) != 0ull;
    state.enableCircuit2 = (pmode & 0x2ull) != 0ull;
    state.useFixedAlpha = ((pmode >> 5u) & 0x1ull) != 0ull;
    state.alphaFromCircuit2 = ((pmode >> 6u) & 0x1ull) != 0ull;
    state.blendWithBackground = ((pmode >> 7u) & 0x1ull) != 0ull;
    state.fixedAlpha = static_cast<uint8_t>((pmode >> 8u) & 0xffull);
    return state;
}

GsSmode2State gsDecodeSmode2(uint64_t smode2) {
    GsSmode2State state{};
    state.interlaced = (smode2 & 0x1ull) != 0ull;
    state.frameMode = ((smode2 >> 1u) & 0x1ull) != 0ull;
    return state;
}

GsDisplaySetup gsDecodeDisplay(uint64_t dispfb, uint64_t display, bool enabled) {
    GsDisplaySetup setup{};
    if (!enabled)
        return setup;

    const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9u) & 0x3full);
    const uint32_t dw = static_cast<uint32_t>((display >> 32u) & 0x0fffull);
    const uint32_t dh = static_cast<uint32_t>((display >> 44u) & 0x07ffull);
    const uint32_t magh = static_cast<uint32_t>((display >> 23u) & 0x0full);
    if (fbw == 0u && dw == 0u && dh == 0u && magh == 0u)
        return setup;

    setup.width = (dw + 1u) / (magh + 1u);
    setup.height = dh + 1u;
    if (setup.width < 64u || setup.height < 64u) {
        setup.width = kFallbackDisplayWidth;
        setup.height = kFallbackDisplayHeight;
    }
    setup.width = std::min(setup.width, kMaxDisplayWidth);
    setup.height = std::min(setup.height, kMaxDisplayHeight);
    setup.fbp = static_cast<uint32_t>(dispfb & 0x1ffull);
    setup.fbw = std::max<uint32_t>(fbw, 1u);
    setup.psm = static_cast<uint32_t>((dispfb >> 15u) & 0x1full);
    setup.originX = static_cast<uint32_t>((dispfb >> 32u) & 0x7ffull);
    setup.originY = static_cast<uint32_t>((dispfb >> 43u) & 0x7ffull);
    setup.valid = true;
    return setup;
}

} // namespace dq8::gfx
