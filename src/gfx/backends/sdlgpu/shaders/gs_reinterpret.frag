#version 450

layout(set = 2, binding = 0) uniform sampler2D sourceTexture;
layout(set = 3, binding = 0) uniform ReinterpretParams {
    uvec4 control; // x: destination is CT16
} params;
layout(location = 0) out vec4 outColor;

uvec4 fetchBytes(uvec2 position) {
    return uvec4(round(texelFetch(sourceTexture, ivec2(position), 0) * 255.0));
}

uint pack16(uvec4 color) {
    return (color.r >> 3u) | ((color.g >> 3u) << 5u) |
           ((color.b >> 3u) << 10u) | (color.a >= 128u ? 32768u : 0u);
}

void main() {
    uvec2 p = uvec2(gl_FragCoord.xy);
    uvec4 result;
    if (params.control.x != 0u) {
        // CT16's x bit 3 selects a halfword. Its remaining page bits are
        // CT32's page bits with x[3:5] and y[3:5] exchanged. Page columns stay
        // at the same x; a 64-row CT16 page becomes a 32-row CT32 page.
        uvec2 source = uvec2((p.x & ~63u) | (p.x & 7u) | (p.y & 56u),
                            ((p.y & ~63u) >> 1u) | (p.y & 7u) | ((p.x & 48u) >> 1u));
        uvec4 bytes = fetchBytes(source);
        uint word = bytes.r | (bytes.g << 8u) | (bytes.b << 16u) | (bytes.a << 24u);
        uint halfword = (word >> ((p.x & 8u) * 2u)) & 65535u;
        uvec3 rgb = uvec3(halfword, halfword >> 5u, halfword >> 10u) & 31u;
        result = uvec4((rgb << 3u) | (rgb >> 2u), (halfword & 32768u) >> 8u);
    } else {
        uvec2 source = uvec2((p.x & ~63u) | (p.x & 7u) | ((p.y & 24u) << 1u),
                            ((p.y & ~31u) << 1u) | (p.y & 7u) | (p.x & 56u));
        uint word = pack16(fetchBytes(source)) | (pack16(fetchBytes(source + uvec2(8u, 0u))) << 16u);
        result = uvec4(word, word >> 8u, word >> 16u, word >> 24u) & 255u;
    }
    outColor = vec4(result) / 255.0;
}
