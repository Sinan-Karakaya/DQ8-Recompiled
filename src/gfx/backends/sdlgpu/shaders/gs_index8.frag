#version 450

// Expands an 8-bit indexed texture from the CT32 target that drew it, sparing a
// readback. A page is the same 8 KiB in both formats; `placement` maps a texel
// of a page to its target pixel and byte, built from the CPU swizzle tables.

layout(set = 2, binding = 0) uniform sampler2D target;
layout(set = 2, binding = 1) uniform sampler2D palette;
// 128x64, one texel per PSMT8 texel of a page: the CT32 pixel holding it in
// R (x) and G (y), and which of that pixel's bytes in B.
layout(set = 2, binding = 2) uniform sampler2D placement;
layout(set = 3, binding = 0) uniform Index8Params {
    // x: texture base page, y: texture pages per row,
    // z: target base page, w: target pages per row | resolution scale << 16
    uvec4 pages;
} params;
layout(location = 0) out vec4 outColor;

uvec4 bytesAt(sampler2D source, ivec2 position) {
    return uvec4(round(texelFetch(source, position, 0) * 255.0));
}

void main() {
    uvec2 p = uvec2(gl_FragCoord.xy);
    uint page = (params.pages.x + (p.y >> 6u) * params.pages.y + (p.x >> 7u)) & 511u;
    uint relative = (page + 512u - params.pages.z) & 511u;
    uint pagesPerRow = params.pages.w & 65535u;
    // An upscaled target is read at each GS pixel's sample point, the first
    // physical pixel of its block.
    uint scale = max(params.pages.w >> 16u, 1u);
    uvec2 origin = uvec2((relative % pagesPerRow) * 64u, (relative / pagesPerRow) * 32u);
    uvec4 place = bytesAt(placement, ivec2(p & uvec2(127u, 63u)));
    uvec4 pixel = bytesAt(target, ivec2((origin + place.xy) * scale));
    uint index = pixel[place.z];
    outColor = texelFetch(palette, ivec2(index, 0), 0);
}
