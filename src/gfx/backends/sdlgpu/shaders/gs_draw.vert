#version 450

// GS primitives, already assembled into triangles on the CPU and expressed in
// GS window pixels (XYOFFSET subtracted). Resolution scaling happens purely in
// the viewport, so nothing here changes between native and 4K.
//
// SDL3 GPU's SPIR-V binding convention: vertex uniform buffers live in set 1.

layout(location = 0) in vec2 inPosition;  // GS window pixels
layout(location = 1) in vec2 inDepthQ;    // normalised depth, Q
layout(location = 2) in vec2 inTexCoord;  // S/T, or U/V in texels when FST
layout(location = 3) in vec4 inColor;
layout(location = 4) in float inFog;

layout(set = 1, binding = 0) uniform VertexParams {
    // xy: render target size in GS pixels. zw: its reciprocal.
    vec4 targetSize;
    // xy: sample-point correction, in GS pixels.
    // z:  +1 or -1 for the clip-space Y direction.
    // w:  unused.
    vec4 adjust;
} params;

layout(location = 0) noperspective out vec2 vTexCoord;
layout(location = 1) noperspective out float vQ;
layout(location = 2) noperspective out vec4 vColor;
layout(location = 3) noperspective out float vFog;

void main() {
    vec2 normalised = (inPosition + params.adjust.xy) * params.targetSize.zw;
    vec2 clip = normalised * 2.0 - 1.0;
    gl_Position = vec4(clip.x, clip.y * params.adjust.z, inDepthQ.x, 1.0);

    // The GS interpolates S, T and Q linearly in screen space and divides
    // per-pixel; it has no W and does no perspective correction of its own.
    // `noperspective` reproduces that exactly, and the fragment stage does the
    // divide.
    vTexCoord = inTexCoord;
    vQ = inDepthQ.y;
    vColor = inColor;
    vFog = inFog;
}
