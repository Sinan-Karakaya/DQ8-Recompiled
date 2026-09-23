#include "gfx/backends/sdlgpu/sdlgpu_device.h"

#include "gs_display.frag.spv.h"
#include "gs_reinterpret.vert.spv.h"
#ifdef DQ8_GFX_HAS_MSL
#include "gs_display.frag.msl.h"
#include "gs_reinterpret.vert.msl.h"
#endif

namespace dq8::gfx {

bool SdlGpuDevice::composeDisplay(SDL_GPUTexture *circuit1, SDL_GPUTexture *circuit2,
                                  SDL_GPUTexture *destination, uint32_t width, uint32_t height,
                                  const GsDisplayUniforms &uniforms, SDL_GPUFence *&completed, std::string &error) {
    if (!m_displayPipeline) {
        SDL_GPUShaderCreateInfo vertexInfo{};
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
        vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
        vertexInfo.entrypoint = "main";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo fragmentInfo{};
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDisplayFragSpirv);
        fragmentInfo.code_size = sizeof(kGsDisplayFragSpirv);
        fragmentInfo.entrypoint = "main";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragmentInfo.num_samplers = 2u;
        fragmentInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
        if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirvMsl);
            vertexInfo.code_size = sizeof(kGsReinterpretVertSpirvMsl);
            vertexInfo.entrypoint = "main0";
            vertexInfo.format = SDL_GPU_SHADERFORMAT_MSL;
            fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDisplayFragSpirvMsl);
            fragmentInfo.code_size = sizeof(kGsDisplayFragSpirvMsl);
            fragmentInfo.entrypoint = "main0";
            fragmentInfo.format = SDL_GPU_SHADERFORMAT_MSL;
        }
#endif
        SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
        if (!vertex) {
            error = std::string("SDL_CreateGPUShader(display vertex): ") + SDL_GetError();
            return false;
        }
        SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
        if (!fragment) {
            error = std::string("SDL_CreateGPUShader(display fragment): ") + SDL_GetError();
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
        m_displayPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
        SDL_ReleaseGPUShader(m_device, fragment);
        SDL_ReleaseGPUShader(m_device, vertex);
        if (!m_displayPipeline) {
            error = std::string("SDL_CreateGPUGraphicsPipeline(display): ") + SDL_GetError();
            return false;
        }
    }

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(m_device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(display): ") + SDL_GetError();
        return false;
    }
    SDL_GPUColorTargetInfo target{};
    target.texture = destination;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(display): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_displayPipeline);
    const SDL_GPUViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_GPUTextureSamplerBinding bindings[] = {{circuit1, m_sampler}, {circuit2, m_sampler}};
    SDL_BindGPUFragmentSamplers(pass, 0u, bindings, 2u);
    SDL_PushGPUFragmentUniformData(commands, 0u, &uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    completed = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!completed) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(display): ") + SDL_GetError();
        return false;
    }
    return true;
}

} // namespace dq8::gfx
