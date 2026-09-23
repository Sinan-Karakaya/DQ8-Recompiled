// Exercise DQ8's 896-tile movie upload with persistent texture-cache entries.
#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_textures.h"
#include "gfx/gs/gs_transfer.h"
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dq8::gfx;

namespace {
GSDrawState textureState(uint32_t base = 0) {
    GSDrawState state{};
    state.context.tex0.tbp0 = base;
    state.context.tex0.tbw = 8;
    state.context.tex0.psm = GS_PSM_CT32;
    state.textureWidth = state.textureHeight = 512;
    return state;
}

void tile(GsTransferEngine &transfers, uint32_t x, uint32_t y, uint32_t color) {
    GSTransferCommand command{};
    command.direction = 0;
    command.bitbltbuf.dbw = 8;
    command.bitbltbuf.dpsm = GS_PSM_CT32;
    command.trxpos.dsax = x;
    command.trxpos.dsay = y;
    command.trxreg.rrw = command.trxreg.rrh = 16;
    std::array<uint32_t, 256> pixels;
    pixels.fill(color);
    transfers.begin(command);
    transfers.upload(reinterpret_cast<const uint8_t *>(pixels.data()), sizeof(pixels));
}

bool pixelsMatch(SdlGpuDevice &device, SDL_GPUTexture *texture, GsVram &vram,
                 uint32_t base) {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = 512 * 512 * 4;
    auto *download = SDL_CreateGPUTransferBuffer(device.handle(), &info);
    auto *commands = SDL_AcquireGPUCommandBuffer(device.handle());
    if (!download || !commands)
        return false;
    auto *copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureRegion source{};
    source.texture = texture;
    source.w = source.h = 512;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = download;
    destination.pixels_per_row = destination.rows_per_layer = 512;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence)
        return false;
    SDL_WaitForGPUFences(device.handle(), true, &fence, 1);
    auto *pixels = static_cast<const uint32_t *>(
        SDL_MapGPUTransferBuffer(device.handle(), download, false));
    bool same = pixels != nullptr;
    for (uint32_t y = 0; same && y < 512; ++y)
        for (uint32_t x = 0; x < 512; ++x)
            if (pixels[y * 512 + x] != vram.read(GS_PSM_CT32, base, 8, x, y)) {
                std::fprintf(stderr, "FAIL: stale movie texture at %u,%u (base=%u)\n", x, y, base);
                same = false;
                break;
            }
    if (pixels)
        SDL_UnmapGPUTransferBuffer(device.handle(), download);
    SDL_ReleaseGPUFence(device.handle(), fence);
    SDL_ReleaseGPUTransferBuffer(device.handle(), download);
    return same;
}

bool run(SdlGpuDevice &device, bool benchmarkOnly) {
    std::vector<uint8_t> storage(kGsVramBytes);
    GsVram vram;
    vram.attach(storage.data(), storage.size());
    GsTargetCache targets(device, vram);
    GsTextureCache textures(device, vram, targets);
    GsTransferEngine transfers(vram);
    transfers.setInvalidateHook([&](const GsPageSet &pages) {
        textures.invalidate(pages, GsTextureCache::InvalidationSource::HostWrite);
    });
    std::string error;
    auto state = textureState();
    if (!textures.acquire(state, error))
        return false;
    if (!benchmarkOnly) {
        textures.resetStats();
        tile(transfers, 496, 432, 0x80654321);
        auto *texture = textures.acquire(state, error);
        if (!texture || !pixelsMatch(device, texture, vram, 0))
            return false;
        if (textures.stats().texelsUpdated != 64 * 32) {
            std::fprintf(stderr, "FAIL: one movie tile expanded %llu texels, expected one 2048-texel page\n",
                         static_cast<unsigned long long>(textures.stats().texelsUpdated));
            return false;
        }
        // GS texture bases may begin within a page, including the end of VRAM.
        for (uint32_t base : {31u, 16383u}) {
            auto alias = textureState(base);
            if (!textures.acquire(alias, error))
                return false;
            tile(transfers, 0, 32, 0x80abcdef + base);
            texture = textures.acquire(alias, error);
            if (!texture || !pixelsMatch(device, texture, vram, base))
                return false;
        }
    }
    // Multiple cached views model the overlapping movie and title textures.
    for (uint32_t i = 0; i < 32; ++i)
        if (!textures.acquire(textureState(i * 32), error))
            return false;
    textures.resetStats();
    constexpr int frames = 30;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        for (uint32_t x = 0; x < 512; x += 16)
            for (uint32_t y = 0; y < 448; y += 16)
                tile(transfers, x, y, 0x80000000 | (frame << 16) | (x << 8) | y);
        if (!textures.acquire(state, error))
            return false;
    }
    SDL_WaitForGPUIdle(device.handle());
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("Movie upload: %.3f ms/frame (%d frames, 896 tiles/frame, 32 cached views), builds=%llu updates=%llu\n",
                ms / frames, frames, static_cast<unsigned long long>(textures.stats().builds),
                static_cast<unsigned long long>(textures.stats().partialUpdates));
    return pixelsMatch(device, textures.acquire(state, error), vram, 0);
}
}

int main(int argc, char **argv) {
    if (!SDL_Init(SDL_INIT_VIDEO))
        return 77;
    int result;
    {
        SdlGpuDevice device;
        std::string error;
        if (!device.create(error)) {
            std::fprintf(stderr, "SKIP: %s\n", error.c_str());
            return 77;
        }
        result = run(device, argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) ? 0 : 1;
    }
    SDL_Quit();
    return result;
}
