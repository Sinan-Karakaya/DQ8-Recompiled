#include "gfx/backends/sdlgpu/sdlgpu_device.h"

#include "gs_draw.frag.spv.h"
#include "gs_draw.vert.spv.h"
#include "gs_reinterpret.frag.spv.h"
#include "gs_reinterpret.vert.spv.h"
#ifdef DQ8_GFX_HAS_MSL
#include "gs_draw.frag.msl.h"
#include "gs_draw.frag.fetch.msl.h"
#include "gs_draw.vert.msl.h"
#include "gs_reinterpret.frag.msl.h"
#include "gs_reinterpret.vert.msl.h"
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {

namespace {

// The GS blend equation is linear in Cs and Cd, so each operand's coefficient
// is `constant + factor * C` with constant in {0, 1} and factor in {-1, 0, 1}.
struct Coefficient {
    int constant = 0;
    int factor = 0;

    bool isZero() const { return constant == 0 && factor == 0; }
    bool isNegative() const { return constant == 0 && factor < 0; }
    // (1 + C) has no blend-factor equivalent.
    bool isExpressible() const { return !(constant == 1 && factor > 0); }
};

Coefficient coefficientFor(const GsAlphaState &alpha, GsBlendColor operand) {
    Coefficient result{};
    if (alpha.d == operand)
        result.constant = 1;
    if (alpha.a == operand)
        result.factor += 1;
    if (alpha.b == operand)
        result.factor -= 1;
    return result;
}

// Maps |coefficient| onto a blend factor. The sign is carried by the blend
// operation, so only the magnitude is encoded here.
SDL_GPUBlendFactor factorFor(const Coefficient &coefficient, GsBlendAlpha c) {
    if (coefficient.isZero())
        return SDL_GPU_BLENDFACTOR_ZERO;
    if (coefficient.factor == 0)
        return SDL_GPU_BLENDFACTOR_ONE;

    const bool oneMinus = coefficient.constant == 1;
    switch (c) {
    case kBlendAlphaSource:
        return oneMinus ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
                        : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case kBlendAlphaDest:
        return oneMinus ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA
                        : SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case kBlendAlphaFixed:
    default:
        return oneMinus ? SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR
                        : SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    }
}

} // namespace

GsBlendTranslation gsTranslateBlend(const GsAlphaState &alpha, bool blendEnabled) {
    GsBlendTranslation result{};
    if (!blendEnabled) {
        result.enabled = false;
        return result;
    }
    result.enabled = true;

    Coefficient source = coefficientFor(alpha, kBlendSource);
    Coefficient dest = coefficientFor(alpha, kBlendDest);

    if (!source.isExpressible() || !dest.isExpressible()) {
        // Cs*(1+C) and friends. Drop the constant term, which keeps the
        // factor-weighted part right and is the closer of the two truncations.
        result.exact = false;
        if (!source.isExpressible())
            source.constant = 0;
        if (!dest.isExpressible())
            dest.constant = 0;
    }

    const bool sourceNegative = source.isNegative();
    const bool destNegative = dest.isNegative();
    if (sourceNegative && destNegative) {
        // -(C*Cs) - (C*Cd) clamps to black for any non-negative input, so the
        // result is exactly what a ZERO/ZERO blend produces.
        result.sourceFactor = SDL_GPU_BLENDFACTOR_ZERO;
        result.destinationFactor = SDL_GPU_BLENDFACTOR_ZERO;
        result.operation = SDL_GPU_BLENDOP_ADD;
        return result;
    }

    result.sourceFactor = factorFor(source, alpha.c);
    result.destinationFactor = factorFor(dest, alpha.c);
    if (destNegative)
        result.operation = SDL_GPU_BLENDOP_SUBTRACT;
    else if (sourceNegative)
        result.operation = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    else
        result.operation = SDL_GPU_BLENDOP_ADD;

    if (alpha.c == kBlendAlphaFixed &&
        (result.sourceFactor == SDL_GPU_BLENDFACTOR_CONSTANT_COLOR ||
         result.sourceFactor == SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR ||
         result.destinationFactor == SDL_GPU_BLENDFACTOR_CONSTANT_COLOR ||
         result.destinationFactor == SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR)) {
        result.usesConstant = true;
        // FIX is a /128 fraction, so it reaches almost 2. A UNORM target
        // clamps the constant at 1, which is the one place this mapping loses
        // range rather than precision.
        result.constant = static_cast<float>(alpha.fix) / 128.0f;
        if (result.constant > 1.0f)
            result.exact = false;
    }
    return result;
}

SDL_GPUCompareOp gsTranslateDepthCompare(GsDepthTest test, bool enabled) {
    if (!enabled)
        return SDL_GPU_COMPAREOP_ALWAYS;
    switch (test) {
    case kDepthNever:
        return SDL_GPU_COMPAREOP_NEVER;
    case kDepthGEqual:
        return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case kDepthGreater:
        return SDL_GPU_COMPAREOP_GREATER;
    case kDepthAlways:
    default:
        return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

size_t GsPipelineKeyHash::operator()(const GsPipelineKey &key) const {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    };
    mix(key.colorFormat);
    mix(key.depthFormat);
    mix(static_cast<uint64_t>(key.hasDepth) | (static_cast<uint64_t>(key.depthCompare) << 8u) |
        (static_cast<uint64_t>(key.depthWrite) << 16u) |
        (static_cast<uint64_t>(key.colorWriteMask) << 24u) |
        (static_cast<uint64_t>(key.blendEnabled) << 32u) |
        (static_cast<uint64_t>(key.sourceFactor) << 40u) |
        (static_cast<uint64_t>(key.destinationFactor) << 48u) |
        (static_cast<uint64_t>(key.blendOperation) << 56u));
    return static_cast<size_t>(hash);
}

SdlGpuDevice::~SdlGpuDevice() {
    destroy();
}

bool SdlGpuDevice::create(std::string &error) {
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef DQ8_GFX_HAS_MSL
    formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
    m_device = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!m_device) {
        error = std::string("SDL_CreateGPUDevice: ") + SDL_GetError();
        return false;
    }

    for (SDL_GPUTextureFormat candidate : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                                           SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                           SDL_GPU_TEXTUREFORMAT_D16_UNORM}) {
        if (SDL_GPUTextureSupportsFormat(m_device, candidate, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            m_depthFormat = candidate;
            break;
        }
    }

    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawVertSpirv);
    vertexInfo.code_size = sizeof(kGsDrawVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
    const bool metal = (SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
    if (metal) {
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawVertSpirvMsl);
        vertexInfo.code_size = sizeof(kGsDrawVertSpirvMsl);
        vertexInfo.entrypoint = "main0";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_MSL;
    }
#endif
    m_vertexShader = SDL_CreateGPUShader(m_device, &vertexInfo);
    if (!m_vertexShader) {
        error = std::string("SDL_CreateGPUShader(vertex): ") + SDL_GetError();
        destroy();
        return false;
    }

    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawFragSpirv);
    fragmentInfo.code_size = sizeof(kGsDrawFragSpirv);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 1u;
    fragmentInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
    if (metal) {
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawFragSpirvMsl);
        fragmentInfo.code_size = sizeof(kGsDrawFragSpirvMsl);
        fragmentInfo.entrypoint = "main0";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_MSL;
#if defined(__aarch64__)
        if (!std::getenv("DQ8_GFX_DISABLE_FRAMEBUFFER_FETCH")) {
            fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDrawFetchSpirvMsl);
            fragmentInfo.code_size = sizeof(kGsDrawFetchSpirvMsl);
            m_framebufferFetch = true;
        }
#endif
    }
#endif
    m_fragmentShader = SDL_CreateGPUShader(m_device, &fragmentInfo);
    if (!m_fragmentShader) {
        error = std::string("SDL_CreateGPUShader(fragment): ") + SDL_GetError();
        destroy();
        return false;
    }

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_sampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (!m_sampler) {
        error = std::string("SDL_CreateGPUSampler: ") + SDL_GetError();
        destroy();
        return false;
    }
    return true;
}

void SdlGpuDevice::destroy() {
    m_framebufferFetch = false;
    if (!m_device)
        return;
    for (auto &[key, pipeline] : m_pipelines) {
        (void)key;
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(m_device, pipeline);
    }
    m_pipelines.clear();
    if (m_reinterpretPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_reinterpretPipeline);
    m_reinterpretPipeline = nullptr;
    if (m_displayPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_displayPipeline);
    m_displayPipeline = nullptr;
    if (m_sampler)
        SDL_ReleaseGPUSampler(m_device, m_sampler);
    if (m_fragmentShader)
        SDL_ReleaseGPUShader(m_device, m_fragmentShader);
    if (m_vertexShader)
        SDL_ReleaseGPUShader(m_device, m_vertexShader);
    m_sampler = nullptr;
    m_fragmentShader = nullptr;
    m_vertexShader = nullptr;
    SDL_DestroyGPUDevice(m_device);
    m_device = nullptr;
}

std::string SdlGpuDevice::driverName() const {
    if (!m_device)
        return {};
    const char *name = SDL_GetGPUDeviceDriver(m_device);
    return name ? name : std::string{};
}

bool SdlGpuDevice::reinterpretColor(SDL_GPUTexture *source, SDL_GPUTexture *destination,
                                    uint32_t width, uint32_t height, bool destination16,
                                    std::string &error) {
    if (!m_reinterpretPipeline) {
        SDL_GPUShaderCreateInfo vertexInfo{};
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
        vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
        vertexInfo.entrypoint = "main";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo fragmentInfo{};
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretFragSpirv);
        fragmentInfo.code_size = sizeof(kGsReinterpretFragSpirv);
        fragmentInfo.entrypoint = "main";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragmentInfo.num_samplers = 1u;
        fragmentInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
        if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirvMsl);
            vertexInfo.code_size = sizeof(kGsReinterpretVertSpirvMsl);
            vertexInfo.entrypoint = "main0";
            vertexInfo.format = SDL_GPU_SHADERFORMAT_MSL;
            fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretFragSpirvMsl);
            fragmentInfo.code_size = sizeof(kGsReinterpretFragSpirvMsl);
            fragmentInfo.entrypoint = "main0";
            fragmentInfo.format = SDL_GPU_SHADERFORMAT_MSL;
        }
#endif
        SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
        if (!vertex) {
            error = std::string("SDL_CreateGPUShader(reinterpret vertex): ") + SDL_GetError();
            return false;
        }
        SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
        if (!fragment) {
            error = std::string("SDL_CreateGPUShader(reinterpret fragment): ") + SDL_GetError();
            SDL_ReleaseGPUShader(m_device, vertex);
            return false;
        }
        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &colorTarget;
        info.target_info.num_color_targets = 1u;
        m_reinterpretPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
        SDL_ReleaseGPUShader(m_device, fragment);
        SDL_ReleaseGPUShader(m_device, vertex);
        if (!m_reinterpretPipeline) {
            error = std::string("SDL_CreateGPUGraphicsPipeline(reinterpret): ") + SDL_GetError();
            return false;
        }
    }

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(m_device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(reinterpret): ") + SDL_GetError();
        return false;
    }
    SDL_GPUColorTargetInfo target{};
    target.texture = destination;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(reinterpret): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_reinterpretPipeline);
    SDL_GPUViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_GPUTextureSamplerBinding binding{source, m_sampler};
    SDL_BindGPUFragmentSamplers(pass, 0u, &binding, 1u);
    const uint32_t control[4] = {uint32_t(destination16), 0u, 0u, 0u};
    SDL_PushGPUFragmentUniformData(commands, 0u, control, sizeof(control));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(commands)) {
        error = std::string("SDL_SubmitGPUCommandBuffer(reinterpret): ") + SDL_GetError();
        return false;
    }
    return true;
}

SDL_GPUGraphicsPipeline *SdlGpuDevice::pipeline(const GsPipelineKey &key, std::string &error) {
    if (!m_device)
        return nullptr;
    if (auto found = m_pipelines.find(key); found != m_pipelines.end())
        return found->second;

    static constexpr std::array<SDL_GPUVertexAttribute, 5> kAttributes{{
        {0u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GsGpuVertex, x)},
        {1u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GsGpuVertex, z)},
        {2u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GsGpuVertex, s)},
        {3u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GsGpuVertex, r)},
        {4u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(GsGpuVertex, fog)},
    }};
    const SDL_GPUVertexBufferDescription bufferDescription{
        0u, sizeof(GsGpuVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0u};

    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend = key.blendEnabled != 0u;
    blend.src_color_blendfactor = static_cast<SDL_GPUBlendFactor>(key.sourceFactor);
    blend.dst_color_blendfactor = static_cast<SDL_GPUBlendFactor>(key.destinationFactor);
    blend.color_blend_op = static_cast<SDL_GPUBlendOp>(key.blendOperation);
    // GS ALPHA blends RGB only; the source alpha is stored unchanged.
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.color_write_mask = key.colorWriteMask;
    blend.enable_color_write_mask = true;

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = static_cast<SDL_GPUTextureFormat>(key.colorFormat);
    colorTarget.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = m_vertexShader;
    info.fragment_shader = m_fragmentShader;
    info.vertex_input_state.vertex_buffer_descriptions = &bufferDescription;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = kAttributes.data();
    info.vertex_input_state.num_vertex_attributes =
        static_cast<uint32_t>(kAttributes.size());
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    // The GS has no backface culling; every primitive it is given is drawn.
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = static_cast<SDL_GPUCompareOp>(key.depthCompare);
    info.depth_stencil_state.enable_depth_test = key.hasDepth != 0u;
    info.depth_stencil_state.enable_depth_write = key.hasDepth != 0u && key.depthWrite != 0u;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1u;
    info.target_info.has_depth_stencil_target = key.hasDepth != 0u;
    info.target_info.depth_stencil_format = static_cast<SDL_GPUTextureFormat>(key.depthFormat);

    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    if (!pipeline) {
        error = std::string("SDL_CreateGPUGraphicsPipeline: ") + SDL_GetError();
        return nullptr;
    }
    m_pipelines.emplace(key, pipeline);
    return pipeline;
}

bool SdlGpuDevice::downloadTexture(SDL_GPUTexture *texture, uint32_t width, uint32_t height,
                                    std::vector<uint8_t> &out, std::string &error) const {
    SDL_GPUDevice *device = m_device;
    if (!device || !texture) {
        error = "render target has no texture to download";
        return false;
    }
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    out.resize(bytes);

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = static_cast<uint32_t>(bytes);
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (!download) {
        error = std::string("SDL_CreateGPUTransferBuffer(download): ") + SDL_GetError();
        return false;
    }

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(download): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = width;
    region.h = height;
    region.d = 1u;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = download;
    destination.pixels_per_row = width;
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUFence *fences[] = {fence};
    const bool waited = SDL_WaitForGPUFences(device, true, fences, 1u);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        error = std::string("SDL_WaitForGPUFences(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, download, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(download): ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    std::memcpy(out.data(), mapped, bytes);
    SDL_UnmapGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, download);
    return true;
}

} // namespace dq8::gfx
