// SDL3 GPU device, shader modules and the graphics-pipeline cache.
//
// Separated from the backend so pipeline state translation -- the part that
// encodes GS semantics -- can be read and tested without a device.
#pragma once

#include "gfx/gs/gs_state.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dq8::gfx {

// How a GS ALPHA equation was mapped onto the host blend unit.
//
// out = (A - B) * C / 128 + D, with A/B/D in {Cs, Cd, 0} and C in {As, Ad,
// FIX}. Every term is linear in Cs and Cd with at most one shared factor, so
// nearly all of the 3*3*3*3 combinations land on a fixed-function blend. The
// exceptions are the ones needing a coefficient of (1 + C) on an operand,
// which no blend factor can express; those are reported inexact and
// approximated rather than silently mapped.
struct GsBlendTranslation {
    bool enabled = false;
    SDL_GPUBlendFactor sourceFactor = SDL_GPU_BLENDFACTOR_ONE;
    SDL_GPUBlendFactor destinationFactor = SDL_GPU_BLENDFACTOR_ZERO;
    SDL_GPUBlendOp operation = SDL_GPU_BLENDOP_ADD;
    // Set when C is FIX; the caller programs it with SDL_SetGPUBlendConstants.
    bool usesConstant = false;
    float constant = 0.0f;
    // False when the equation had to be approximated.
    bool exact = true;
};

GsBlendTranslation gsTranslateBlend(const GsAlphaState &alpha, bool blendEnabled);

// GS depth compares are reversed relative to the usual convention: a larger Z
// is nearer.
SDL_GPUCompareOp gsTranslateDepthCompare(GsDepthTest test, bool enabled);

// One vertex as the pipeline consumes it. Positions stay in GS window pixels
// so resolution scaling is entirely a viewport concern.
struct GsGpuVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;  // normalised depth, 0..1
    float q = 1.0f;
    float s = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
    float fog = 1.0f;
    float pad = 0.0f;
};

static_assert(sizeof(GsGpuVertex) == 48u, "vertex layout must match gs_draw.vert");

// Uniform blocks, laid out to match the two shaders.
struct GsVertexUniforms {
    float targetSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float adjust[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

struct GsFragmentUniforms {
    uint32_t control[4] = {0u, 0u, 0u, 0u};
    float fog[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float textureSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float region[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float misc[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    uint32_t blend[4] = {};
};

struct GsDisplayUniforms {
    uint32_t control[4]{}; // scale, half-height source, fixed alpha, background blend
    uint32_t circuit1[4]{}; // native origin x/y, scaled output width/height
    uint32_t circuit2[4]{};
    uint32_t format[4]{}; // PSM1, PSM2, ALP, BGCOLOR
};

// Feature bits in GsFragmentUniforms::control[0]; must match gs_draw.frag.
inline constexpr uint32_t kFragFlagTme = 1u << 0u;
inline constexpr uint32_t kFragFlagFst = 1u << 1u;
inline constexpr uint32_t kFragFlagTcc = 1u << 2u;
inline constexpr uint32_t kFragFlagFge = 1u << 3u;
inline constexpr uint32_t kFragFlagAte = 1u << 4u;
inline constexpr uint32_t kFragFlagLinear = 1u << 5u;

struct GsPipelineKey {
    uint32_t colorFormat = 0u;
    uint32_t depthFormat = 0u;
    uint8_t hasDepth = 0u;
    uint8_t depthCompare = 0u;
    uint8_t depthWrite = 0u;
    uint8_t colorWriteMask = 0xfu;
    uint8_t blendEnabled = 0u;
    uint8_t sourceFactor = 0u;
    uint8_t destinationFactor = 0u;
    uint8_t blendOperation = 0u;

    bool operator==(const GsPipelineKey &other) const = default;
};

struct GsPipelineKeyHash {
    size_t operator()(const GsPipelineKey &key) const;
};

class SdlGpuDevice {
public:
    SdlGpuDevice() = default;
    ~SdlGpuDevice();

    SdlGpuDevice(const SdlGpuDevice &) = delete;
    SdlGpuDevice &operator=(const SdlGpuDevice &) = delete;

    bool create(std::string &error);
    void destroy();

    SDL_GPUDevice *handle() const { return m_device; }
    bool valid() const { return m_device != nullptr; }
    std::string driverName() const;

    // Depth format chosen at creation from what the device actually supports.
    SDL_GPUTextureFormat depthFormat() const { return m_depthFormat; }

    // Pipelines are created on demand and live for the device's lifetime;
    // the set of distinct GS states a game uses is small and bounded.
    SDL_GPUGraphicsPipeline *pipeline(const GsPipelineKey &key, std::string &error);

    // Reinterprets identical native GS pages between CT32 and CT16 views.
    // The caller validates extents/ownership and commits their state afterward.
    bool reinterpretColor(SDL_GPUTexture *source, SDL_GPUTexture *destination,
                          uint32_t width, uint32_t height, bool destination16,
                          std::string &error);

    bool composeDisplay(SDL_GPUTexture *circuit1, SDL_GPUTexture *circuit2,
                        SDL_GPUTexture *destination, uint32_t width, uint32_t height,
                        const GsDisplayUniforms &uniforms, SDL_GPUFence *&completed, std::string &error);

    bool downloadTexture(SDL_GPUTexture *texture, uint32_t width, uint32_t height,
                         std::vector<uint8_t> &pixels, std::string &error) const;

    // One point-sampled clamp sampler. Filtering and GS wrap modes are done in
    // the fragment shader, which needs raw texel fetches, so the sampler's own
    // parameters never vary.
    SDL_GPUSampler *sampler() const { return m_sampler; }

    size_t pipelineCount() const { return m_pipelines.size() + (m_reinterpretPipeline ? 1u : 0u) + (m_displayPipeline ? 1u : 0u); }
    bool framebufferFetch() const { return m_framebufferFetch; }

private:
    SDL_GPUDevice *m_device = nullptr;
    SDL_GPUShader *m_vertexShader = nullptr;
    SDL_GPUShader *m_fragmentShader = nullptr;
    SDL_GPUSampler *m_sampler = nullptr;
    SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    bool m_framebufferFetch = false;
    SDL_GPUGraphicsPipeline *m_reinterpretPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_displayPipeline = nullptr;
    std::unordered_map<GsPipelineKey, SDL_GPUGraphicsPipeline *, GsPipelineKeyHash> m_pipelines;
};

} // namespace dq8::gfx
