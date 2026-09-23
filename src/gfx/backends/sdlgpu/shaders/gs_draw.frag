#version 450

// GS pixel pipeline: texture lookup, TFX combine, fog, alpha test.
//
// Apple GPUs use framebuffer fetch for GS integer blending and destination
// tests. Other devices use the fixed-function mapping in sdlgpu_device.cpp.
//
// Filtering and wrapping are done by hand rather than by the sampler because
// GS wrap modes REGION_CLAMP and REGION_REPEAT have no sampler equivalent:
// REGION_REPEAT is a bitwise and/or on the texel index.
//
// SDL3 GPU's SPIR-V binding convention: fragment textures live in set 2 and
// fragment uniform buffers in set 3.

layout(location = 0) noperspective in vec2 vTexCoord;
layout(location = 1) noperspective in float vQ;
layout(location = 2) noperspective in vec4 vColor;
layout(location = 3) noperspective in float vFog;

layout(set = 2, binding = 0) uniform sampler2D gsTexture;
#ifdef DQ8_FRAMEBUFFER_FETCH
layout(input_attachment_index = 0, set = 2, binding = 1) uniform subpassInput gsDestination;
#endif

layout(set = 3, binding = 0) uniform FragmentParams {
    // x: feature bits (FLAG_*)
    // y: alpha-test comparison (GsAlphaTest)
    // z: texture function (GsTextureFunction)
    // w: wrap modes, U in bits 0-1 and V in bits 2-3
    uvec4 control;
    // xyz: FOGCOL. w: fixed alpha reference for the alpha test, 0..1.
    vec4 fog;
    // xy: texture size in texels. zw: its reciprocal.
    vec4 textureSize;
    // REGION_* bounds in texels: minU, maxU, minV, maxV.
    vec4 region;
    // x: factor applied to the emitted alpha so fixed-function blending sees
    //    the GS's /128 scale. 1.0 when the alpha channel must stay exact.
    // y: physical texels per GS texel. Above 1 when the bound texture is a
    //    render target being sampled directly, which is stored at the render
    //    scale rather than the GS's own resolution.
    // zw: live-target format (1=CT24, 2=CT16) and TA0 | TA1<<8 | AEM<<16.
    vec4 misc;
    // ALPHA operands/FIX, FBMSK, TEST, ABE/COLCLAMP/PABE/FBA and framebuffer PSM.
    uvec4 blend;
} params;

layout(location = 0) out vec4 outColor;

const uint FLAG_TME = 1u << 0u;
const uint FLAG_FST = 1u << 1u;
const uint FLAG_TCC = 1u << 2u;
const uint FLAG_FGE = 1u << 3u;
const uint FLAG_ATE = 1u << 4u;
const uint FLAG_LINEAR = 1u << 5u;

const uint WRAP_REPEAT = 0u;
const uint WRAP_CLAMP = 1u;
const uint WRAP_REGION_CLAMP = 2u;
const uint WRAP_REGION_REPEAT = 3u;

const uint TFX_MODULATE = 0u;
const uint TFX_DECAL = 1u;
const uint TFX_HIGHLIGHT = 2u;
const uint TFX_HIGHLIGHT2 = 3u;

const uint ATST_NEVER = 0u;
const uint ATST_ALWAYS = 1u;
const uint ATST_LESS = 2u;
const uint ATST_LEQUAL = 3u;
const uint ATST_EQUAL = 4u;
const uint ATST_GEQUAL = 5u;
const uint ATST_GREATER = 6u;
const uint ATST_NOTEQUAL = 7u;

int wrapAxis(int texel, uint mode, int size, int regionMin, int regionMax) {
    if (mode == WRAP_CLAMP)
        return clamp(texel, 0, size - 1);
    if (mode == WRAP_REGION_CLAMP)
        return clamp(texel, regionMin, regionMax);
    if (mode == WRAP_REGION_REPEAT)
        // MINU is a mask and MAXU the bits or-ed back in, not a range.
        return (texel & regionMin) | regionMax;
    // REPEAT wraps to the power-of-two texture size.
    return texel & (size - 1);
}

// Wrapping is defined on GS texel indices, so it is applied in that space and
// the result scaled afterwards. `subTexel` selects a point inside the GS texel
// when the bound texture holds several physical texels per GS one, which is
// what keeps a render target's extra detail instead of averaging it away.
vec4 fetchTexelAt(ivec2 texel, vec2 subTexel) {
    const ivec2 size = ivec2(params.textureSize.xy);
    const uint wrapU = params.control.w & 3u;
    const uint wrapV = (params.control.w >> 2u) & 3u;
    const ivec4 region = ivec4(params.region);
    ivec2 wrapped = ivec2(wrapAxis(texel.x, wrapU, size.x, region.x, region.y),
                          wrapAxis(texel.y, wrapV, size.y, region.z, region.w));
    wrapped = clamp(wrapped, ivec2(0), size - 1);

    const int scale = max(int(params.misc.y), 1);
    ivec2 physical = wrapped * scale;
    if (scale > 1)
        physical += ivec2(clamp(subTexel, vec2(0.0), vec2(0.999)) * float(scale));
    vec4 color = texelFetch(gsTexture, clamp(physical, ivec2(0), size * scale - 1), 0);
    const uint format = uint(params.misc.z);
    if (format != 0u) {
        uvec4 raw = uvec4(round(color * 255.0));
        const uint texa = uint(params.misc.w);
        const bool alphaBit = format == 2u && raw.a >= 128u;
        if (format == 2u)
            raw.rgb = (raw.rgb >> 3u) << 3u;
        const bool transparent = !alphaBit && (texa & 65536u) != 0u &&
                                 all(equal(raw.rgb, uvec3(0u)));
        raw.a = transparent ? 0u : alphaBit ? ((texa >> 8u) & 255u) : (texa & 255u);
        color = vec4(raw) / 255.0;
    }
    return color;
}

vec4 fetchTexel(ivec2 texel) {
    // Centre of the GS texel: the right point for a corner of a bilinear tap.
    return fetchTexelAt(texel, vec2(0.5));
}

vec4 sampleTexture() {
    vec2 texel;
    if ((params.control.x & FLAG_FST) != 0u) {
        // UV is already in texels, at 1/16 precision.
        texel = vTexCoord;
    } else {
        // STQ: the GS divides per pixel, which is why Q is interpolated
        // separately rather than folded into gl_Position.
        const float q = abs(vQ) > 1.0e-8 ? vQ : 1.0;
        texel = (vTexCoord / q) * params.textureSize.xy;
    }

    if ((params.control.x & FLAG_LINEAR) == 0u) {
        const vec2 base = floor(texel);
        return fetchTexelAt(ivec2(base), texel - base);
    }

    // Bilinear about the texel centre, matching the GS's half-texel offset.
    const vec2 base = texel - 0.5;
    const vec2 fraction = fract(base);
    const ivec2 corner = ivec2(floor(base));
    const vec4 c00 = fetchTexel(corner);
    const vec4 c10 = fetchTexel(corner + ivec2(1, 0));
    const vec4 c01 = fetchTexel(corner + ivec2(0, 1));
    const vec4 c11 = fetchTexel(corner + ivec2(1, 1));
    return mix(mix(c00, c10, fraction.x), mix(c01, c11, fraction.x), fraction.y);
}

// TFX. The GS computes Ct * Cv >> 7 on 0..255 integers, so a vertex colour of
// 0x80 is unity. Both operands arrive here divided by 255, which leaves the
// product short by exactly 255/128 -- not 2, which would over-brighten every
// textured pixel by one part in 255.
const float TFX_SCALE = 255.0 / 128.0;

vec4 applyTextureFunction(vec4 texel, vec4 vertexColor) {
    const uint tfx = params.control.z;
    const bool useTextureAlpha = (params.control.x & FLAG_TCC) != 0u;
    vec4 result;

    if (tfx == TFX_DECAL) {
        result.rgb = texel.rgb;
        result.a = useTextureAlpha ? texel.a : vertexColor.a;
    } else if (tfx == TFX_HIGHLIGHT) {
        result.rgb = texel.rgb * vertexColor.rgb * TFX_SCALE + vertexColor.a;
        result.a = useTextureAlpha ? texel.a + vertexColor.a : vertexColor.a;
    } else if (tfx == TFX_HIGHLIGHT2) {
        result.rgb = texel.rgb * vertexColor.rgb * TFX_SCALE + vertexColor.a;
        result.a = useTextureAlpha ? texel.a : vertexColor.a;
    } else {
        result.rgb = texel.rgb * vertexColor.rgb * TFX_SCALE;
        result.a = useTextureAlpha ? texel.a * vertexColor.a * TFX_SCALE : vertexColor.a;
    }
    return clamp(result, 0.0, 1.0);
}

bool alphaTestPasses(float alpha) {
    if ((params.control.x & FLAG_ATE) == 0u)
        return true;

    // Compared as the GS does, on 0..255 integers.
    const int value = int(round(alpha * 255.0));
    const int reference = int(round(params.fog.w * 255.0));
    switch (params.control.y) {
    case ATST_NEVER: return false;
    case ATST_ALWAYS: return true;
    case ATST_LESS: return value < reference;
    case ATST_LEQUAL: return value <= reference;
    case ATST_EQUAL: return value == reference;
    case ATST_GEQUAL: return value >= reference;
    case ATST_GREATER: return value > reference;
    case ATST_NOTEQUAL: return value != reference;
    }
    return true;
}

#ifdef DQ8_FRAMEBUFFER_FETCH
ivec3 blendOperand(uint selector, ivec3 source, ivec3 destination) {
    return selector == 0u ? source : selector == 1u ? destination : ivec3(0);
}

vec4 blendPixel(vec4 color) {
    ivec4 source = ivec4(round(color * 255.0));
    ivec4 destination = ivec4(round(subpassLoad(gsDestination) * 255.0));
    const uint flags = params.blend.w;
    const uint psm = flags >> 8u;
    const bool ct16 = psm == 2u || psm == 10u;
    if (ct16) {
        destination.rgb = (destination.rgb >> 3) << 3;
        destination.a = destination.a >= 128 ? 128 : 0;
    } else if (psm == 1u) {
        destination.a = 128;
    }
    const uint test = params.blend.z;
    if (psm != 1u && (test & 16384u) != 0u &&
        ((destination.a >= 128) != ((test & 32768u) != 0u)))
        discard;
    if ((flags & 1u) != 0u && ((flags & 4u) == 0u || source.a >= 128)) {
        const uint equation = params.blend.x;
        const uint c = (equation >> 4u) & 3u;
        const int factor = c == 0u ? source.a : c == 1u ? destination.a : int(equation >> 8u);
        const ivec3 a = blendOperand(equation & 3u, source.rgb, destination.rgb);
        const ivec3 b = blendOperand((equation >> 2u) & 3u, source.rgb, destination.rgb);
        const ivec3 d = blendOperand((equation >> 6u) & 3u, source.rgb, destination.rgb);
        source.rgb = (((a - b) * factor) >> 7) + d;
    }
    source.rgb = (flags & 2u) != 0u ? clamp(source.rgb, ivec3(0), ivec3(255)) : (source.rgb & 255);
    if ((flags & 8u) != 0u && psm != 1u)
        source.a |= 128;
    const uvec4 mask = (uvec4(params.blend.y) >> uvec4(0u, 8u, 16u, 24u)) & 255u;
    source = ivec4((uvec4(source) & ~mask) | (uvec4(destination) & mask));
    if (ct16) {
        source.rgb = (source.rgb >> 3) << 3;
        source.a = source.a >= 128 ? 128 : 0;
    }
    return vec4(source) / 255.0;
}
#endif

void main() {
    vec4 color = vColor;
    if ((params.control.x & FLAG_TME) != 0u)
        color = applyTextureFunction(sampleTexture(), vColor);

    if (!alphaTestPasses(color.a))
        discard;

    if ((params.control.x & FLAG_FGE) != 0u)
        color.rgb = mix(params.fog.rgb, color.rgb, vFog);

    // The GS blend factor is A/128, not A/255. Fixed-function blending reads
    // the emitted alpha, so the scale is folded in here; params.misc.x is 1.0
    // whenever the alpha channel is written and must stay exact instead.
#ifdef DQ8_FRAMEBUFFER_FETCH
    outColor = blendPixel(color);
#else
    outColor = vec4(color.rgb, min(color.a * params.misc.x, 1.0));
#endif
}
