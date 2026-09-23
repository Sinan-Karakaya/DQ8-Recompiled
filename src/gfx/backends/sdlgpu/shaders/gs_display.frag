#version 450

layout(set = 2, binding = 0) uniform sampler2D circuit1Texture;
layout(set = 2, binding = 1) uniform sampler2D circuit2Texture;
layout(set = 3, binding = 0) uniform DisplayParams {
    uvec4 control; // scale, half-height source, fixed alpha, background blend
    uvec4 circuit1; // native origin x/y, scaled output width/height
    uvec4 circuit2;
    uvec4 format; // PSM1, PSM2, ALP, BGCOLOR
} params;
layout(location = 0) out vec4 outColor;

ivec4 readCircuit(sampler2D source, uvec2 pixel, uvec4 circuit, uint psm) {
    uint factor = params.control.x;
    uint nativeY = pixel.y / factor;
    uint sourceY = circuit.y + (params.control.y != 0u ? nativeY / 2u : nativeY);
    uvec2 sourcePixel = uvec2(circuit.x * factor + pixel.x,
                             sourceY * factor + pixel.y % factor);
    if (any(greaterThanEqual(sourcePixel, uvec2(textureSize(source, 0)))))
        return ivec4(0);
    ivec4 color = ivec4(round(texelFetch(source, ivec2(sourcePixel), 0) * 255.0));
    // Match what each display-buffer format can retain from an RGBA8 target.
    if (psm == 1u) color.a = 0;
    else if (psm == 2u || psm == 10u) color.a = color.a >= 128 ? 128 : 0;
    return color;
}

void main() {
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    uint background = params.format.w;
    ivec3 color = ivec3(background, background >> 8u, background >> 16u) & 255;
    if (params.control.w == 0u && all(lessThan(pixel, params.circuit2.zw)))
        color = readCircuit(circuit2Texture, pixel, params.circuit2, params.format.y).rgb;
    if (all(lessThan(pixel, params.circuit1.zw))) {
        ivec4 source = readCircuit(circuit1Texture, pixel, params.circuit1, params.format.x);
        int alpha = params.control.z != 0u ? int(params.format.z) : min(255, source.a * 2);
        // Signed integer division truncates toward zero, including negative
        // differences, exactly as the existing CPU presentation composition.
        color = clamp(color + ((source.rgb - color) * alpha) / 255, ivec3(0), ivec3(255));
    }
    outColor = vec4(vec3(color) / 255.0, 1.0);
}
