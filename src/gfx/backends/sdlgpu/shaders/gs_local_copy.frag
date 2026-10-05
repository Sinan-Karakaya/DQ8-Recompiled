#version 450

// A GS local-to-local transfer between CT32 render targets, done on the GPU
// instead of reading both back. The CPU works out, from the swizzle tables,
// which source pixel and byte lands in each byte of each destination pixel;
// this pass applies that per sub-sample plane of an upscaled target, so the
// copy keeps every sample. Bytes the transfer leaves alone keep their value.

layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 2, binding = 1) uniform sampler2D destination;
// One texel per destination pixel of the transfer's bounding box; per byte,
// bit 31 set for a copied byte, the source byte in bits 24-25 and the source
// pixel's y in bits 12-23 and x in bits 0-11.
layout(set = 2, binding = 2) uniform usampler2D mapping;
layout(set = 3, binding = 0) uniform LocalCopyParams {
    // x: resolution scale, yz: the bounding box's origin in GS pixels
    uvec4 control;
} params;
layout(location = 0) out vec4 outColor;

uvec4 bytesAt(sampler2D texture, ivec2 position) {
    return uvec4(round(texelFetch(texture, position, 0) * 255.0));
}

void main() {
    uint scale = max(params.control.x, 1u);
    uvec2 physical = uvec2(gl_FragCoord.xy);
    uvec2 pixel = physical / scale;
    uvec2 plane = physical % scale;
    uvec4 entry = texelFetch(mapping, ivec2(pixel - params.control.yz), 0);
    uvec4 result = bytesAt(destination, ivec2(physical));
    for (int i = 0; i < 4; ++i) {
        uint code = entry[i];
        if ((code & 0x80000000u) == 0u)
            continue;
        uvec2 from = uvec2(code & 4095u, (code >> 12u) & 4095u);
        result[i] = bytesAt(source, ivec2(from * scale + plane))[(code >> 24u) & 3u];
    }
    outColor = vec4(result) / 255.0;
}
