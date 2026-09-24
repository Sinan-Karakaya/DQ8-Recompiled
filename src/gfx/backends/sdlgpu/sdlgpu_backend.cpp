#include "gfx/backends/sdlgpu/sdlgpu_backend.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#include "gfx/backends/sdlgpu/sdlgpu_textures.h"
#include "gfx/gs/gs_state.h"
#include "gfx/gs/gs_transfer.h"
#include "gfx/gs/gs_vram.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

// Debug accounting published by ps2xRuntime's GIF frontend (gs_frontend.cpp).
// Declared here rather than in a header so measuring costs no rebuild of the
// recompiled corpus, which includes those headers.
extern "C++" {
extern std::atomic<uint64_t> g_gsFrontendPacketNanos;
extern std::atomic<uint64_t> g_gsFrontendPacketCount;
extern std::atomic<uint64_t> g_gsUploadNativeNanos;
extern std::atomic<uint64_t> g_gsUploadNativeCount;
}

// The movie counters live in ps2xRuntime's MPEG stub, which the standalone GS
// dump harness does not link -- it has no EE and no kernel. Defined weakly here
// so both builds link; the stub's strong definitions win wherever it is
// present, and the harness just reports zeros.
#if defined(__GNUC__) || defined(__clang__)
#define DQ8_WEAK_COUNTER __attribute__((weak))
#else
#define DQ8_WEAK_COUNTER
#endif
extern "C++" {
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_mpegGetPictureNanos{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_mpegGetPictureCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_mpegWriteFrameNanos{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_mpegWriteFrameCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_mpegDemuxNanos{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_mpegDemuxCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_mpegDemuxRefusedCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_mpegPendingEsPeakBytes{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_eeGuestDispatchCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_eeTransferThrowCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_eeTransferSuspendCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_eeRunLoopIterations{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_eeRunLoopResumeCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_eeProcessPendingEventsCount{0};
DQ8_WEAK_COUNTER std::atomic<uint64_t> g_eeEnterGuestCount{0};
DQ8_WEAK_COUNTER std::atomic<uint32_t> g_mpegDemuxThreadId{0};
DQ8_WEAK_COUNTER std::atomic<uint32_t> g_mpegGetPictureThreadId{0};
}

namespace dq8::gfx {

namespace {

// DQ8_GFX_SCREENSHOT_DIR makes the runtime latch CPU-composed pixels, so the
// paths that would skip or discard that compose have to stand down.
bool gsScreenshotEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DQ8_GFX_SCREENSHOT_DIR");
        return value != nullptr && *value != '\0';
    }();
    return enabled;
}

// SDL3 GPU normalises every backend onto a Y-up clip space, so a GS window
// coordinate (Y down from the top-left) is flipped on the way out of the
// vertex shader. Verified by sdlgpu_raster_tests' orientation case, which
// fails loudly rather than mirroring the image if this is wrong.
constexpr float kClipYDirection = -1.0f;

// GS samples at integer window coordinates; the host samples at n + 0.5.
// Without this shift, bilinear copies mix adjacent texels and corrupt the
// channel-shuffle passes which reinterpret framebuffer pixels as CT16.
constexpr float kSampleOffset = 0.5f;

uint32_t depthMaximumForPsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return 0xffffu;
    case GS_PSM_Z24:
        return 0xffffffu;
    case GS_PSM_Z32:
    default:
        return 0xffffffffu;
    }
}

// FBMSK masks individual bits; a host colour write mask is per channel. A
// channel is dropped only when every one of its bits is masked, and a partial
// mask is counted so a scene that depends on one is visible in the stats.
uint8_t colorWriteMaskFromFbmsk(uint32_t fbmsk, bool &partial) {
    uint8_t mask = 0u;
    partial = false;
    const uint32_t channels[4] = {fbmsk & 0xffu, (fbmsk >> 8u) & 0xffu,
                                  (fbmsk >> 16u) & 0xffu, (fbmsk >> 24u) & 0xffu};
    const uint8_t flags[4] = {SDL_GPU_COLORCOMPONENT_R, SDL_GPU_COLORCOMPONENT_G,
                              SDL_GPU_COLORCOMPONENT_B, SDL_GPU_COLORCOMPONENT_A};
    for (int i = 0; i < 4; ++i) {
        if (channels[i] == 0xffu)
            continue;
        if (channels[i] != 0u)
            partial = true;
        mask |= flags[i];
    }
    return mask;
}

struct DrawBatch {
    GsSurface *color = nullptr;
    GsSurface *depth = nullptr;
    GsPipelineKey pipeline{};
    GsVertexUniforms vertexUniforms{};
    GsFragmentUniforms fragmentUniforms{};
    SDL_GPUTexture *texture = nullptr;
    bool usesBlendConstant = false;
    float blendConstant = 0.0f;
    SDL_Rect scissor{};
    GsRegion written{};
    GsRegion overwritten{};
    GsPageSet writtenPages{};
    uint32_t firstVertex = 0u;
    uint32_t vertexCount = 0u;
    // A live target sampled directly. Its texture is bound when the batch is
    // flushed, after any queued draws into it.
    GsSurface *source = nullptr;
    // Samples its own colour target through a snapshot, which the flush takes
    // before this batch when `snapshot` is set and otherwise shares with the
    // batch before it.
    bool feedback = false;
    bool snapshot = false;

    // Everything except the vertex range; two consecutive primitives with the
    // same answer here can share one draw call. One that needs a snapshot of
    // its own starts a new batch.
    bool sameStateAs(const DrawBatch &other) const {
        return !other.snapshot && feedback == other.feedback && source == other.source &&
               color == other.color && depth == other.depth &&
               pipeline == other.pipeline && texture == other.texture &&
               usesBlendConstant == other.usesBlendConstant &&
               blendConstant == other.blendConstant &&
               scissor.x == other.scissor.x && scissor.y == other.scissor.y &&
               scissor.w == other.scissor.w && scissor.h == other.scissor.h &&
               std::memcmp(&vertexUniforms, &other.vertexUniforms,
                           sizeof(vertexUniforms)) == 0 &&
               std::memcmp(&fragmentUniforms, &other.fragmentUniforms,
                           sizeof(fragmentUniforms)) == 0;
    }
};

GsRegion primitiveBounds(const GSPrimitiveBatch &batch) {
    const GSContext &context = batch.state.context;
    const GsRegion scissor{context.scissor.x0, context.scissor.y0,
                          context.scissor.x1 + 1u, context.scissor.y1 + 1u};
    const float offsetX = static_cast<float>(context.xyoffset.ofx) / 16.0f;
    const float offsetY = static_cast<float>(context.xyoffset.ofy) / 16.0f;
    float minX = batch.vertices[0].x - offsetX, maxX = minX;
    float minY = batch.vertices[0].y - offsetY, maxY = minY;
    for (uint8_t i = 0u; i < batch.vertexCount; ++i) {
        const float x = batch.vertices[i].x - offsetX;
        const float y = batch.vertices[i].y - offsetY;
        if (!std::isfinite(x) || !std::isfinite(y))
            return scissor;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
    // Include point/line expansion and fractional edges at every render scale.
    auto clipped = [](float value, uint32_t low, uint32_t high) {
        return static_cast<uint32_t>(std::clamp(value, static_cast<float>(low),
                                               static_cast<float>(high)));
    };
    return {clipped(std::floor(minX) - 1.0f, scissor.x0, scissor.x1),
            clipped(std::floor(minY) - 1.0f, scissor.y0, scissor.y1),
            clipped(std::ceil(maxX) + 1.0f, scissor.x0, scissor.x1),
            clipped(std::ceil(maxY) + 1.0f, scissor.y0, scissor.y1)};
}

GsRegion opaqueSpriteBounds(const GSPrimitiveBatch &batch, const GsSurface &surface,
                            const GsTestState &test) {
    const auto &state = batch.state;
    const auto &context = state.context;
    if (surface.scale != 1u || surface.psm != GS_PSM_CT32 ||
        state.prim.type != GS_PRIM_SPRITE || batch.vertexCount != 2u ||
        state.prim.tme || state.prim.abe ||
        context.frame.fbmsk != 0u || test.destinationAlphaTestEnabled ||
        (test.alphaTestEnabled && test.alphaTest != kAlphaAlways) ||
        (test.depthTestEnabled && test.depthTest != kDepthAlways))
        return {};

    const double x0 = batch.vertices[0].x - context.xyoffset.ofx / 16.0;
    const double y0 = batch.vertices[0].y - context.xyoffset.ofy / 16.0;
    const double x1 = batch.vertices[1].x - context.xyoffset.ofx / 16.0;
    const double y1 = batch.vertices[1].y - context.xyoffset.ofy / 16.0;
    for (double value : {x0, y0, x1, y1})
        if (!std::isfinite(value) || std::floor(value) != value) return {};
    auto clipped = [](double value, uint32_t low, uint32_t high) {
        return static_cast<uint32_t>(std::clamp(value, double(low), double(high)));
    };
    return {clipped(std::min(x0, x1), context.scissor.x0, context.scissor.x1 + 1u),
            clipped(std::min(y0, y1), context.scissor.y0, context.scissor.y1 + 1u),
            clipped(std::max(x0, x1), context.scissor.x0, context.scissor.x1 + 1u),
            clipped(std::max(y0, y1), context.scissor.y0, context.scissor.y1 + 1u)};
}

bool samplesFitSurface(const GSPrimitiveBatch &batch, const GsSurface &surface,
                       GsRegion *coverage = nullptr) {
    const auto &state = batch.state;
    if (!coverage && state.textureWidth <= surface.width && state.textureHeight <= surface.height)
        return true;
    if (!state.prim.fst)
        return false;
    const auto clamp = gsDecodeClamp(state.context.clamp);
    if (clamp.wrapU > kWrapClamp || clamp.wrapV > kWrapClamp)
        return false;
    double minU = batch.vertices[0].u / 16.0, maxU = minU;
    double minV = batch.vertices[0].v / 16.0, maxV = minV;
    for (uint8_t i = 1u; i < batch.vertexCount; ++i) {
        minU = std::min(minU, batch.vertices[i].u / 16.0);
        maxU = std::max(maxU, batch.vertices[i].u / 16.0);
        minV = std::min(minV, batch.vertices[i].v / 16.0);
        maxV = std::max(maxV, batch.vertices[i].v / 16.0);
    }
    if (state.prim.type == GS_PRIM_SPRITE) {
        // Sprite edges are exclusive. Check the samples actually covered,
        // including subpixels at higher scales, rather than the unused far edge.
        const auto &a = batch.vertices[0];
        const auto &b = batch.vertices[1];
        auto range = [&](double p0, double p1, double t0, double t1,
                         uint32_t clip0, uint32_t clip1, double &lo, double &hi) {
            if (p0 == p1 || !std::isfinite(p0) || !std::isfinite(p1)) return false;
            const double scale = surface.scale;
            const double first = std::max(std::ceil((std::min(p0, p1) + 0.5) * scale - 0.5),
                                          clip0 * scale);
            const double last = std::min(std::ceil((std::max(p0, p1) + 0.5) * scale - 0.5) - 1.0,
                                         (clip1 + 1.0) * scale - 1.0);
            if (first > last) return false;
            auto sample = [&](double pixel) {
                const double position = (pixel + 0.5) / scale - 0.5;
                return t0 + (t1 - t0) * (position - p0) / (p1 - p0);
            };
            lo = std::min(sample(first), sample(last));
            hi = std::max(sample(first), sample(last));
            return true;
        };
        const auto &c = state.context;
        if (!range(a.x - c.xyoffset.ofx / 16.0, b.x - c.xyoffset.ofx / 16.0,
                   a.u / 16.0, b.u / 16.0, c.scissor.x0, c.scissor.x1, minU, maxU) ||
            !range(a.y - c.xyoffset.ofy / 16.0, b.y - c.xyoffset.ofy / 16.0,
                   a.v / 16.0, b.v / 16.0, c.scissor.y0, c.scissor.y1, minV, maxV))
            return false;
    }
    auto fits = [&](double lo, double hi, uint32_t size, uint32_t extent, GsWrapMode mode,
                    uint32_t &first, uint32_t &end) {
        const double shift = state.linearFilter ? 0.5 : 0.0;
        lo = std::floor(lo - shift);
        hi = state.linearFilter ? std::ceil(hi - shift) : std::floor(hi);
        if (mode == kWrapClamp) {
            lo = std::clamp(lo, 0.0, double(size - 1u));
            hi = std::clamp(hi, 0.0, double(size - 1u));
        } else {
            if (std::floor(lo / size) != std::floor(hi / size)) {
                lo = 0.0;
                hi = size - 1u;
            } else {
                const double wrap = std::floor(lo / size) * size;
                lo -= wrap;
                hi -= wrap;
            }
        }
        first = uint32_t(lo);
        end = uint32_t(hi) + 1u;
        return lo >= 0.0 && hi < extent;
    };
    GsRegion taps;
    const bool fitsU = fits(minU, maxU, std::max<uint16_t>(state.textureWidth, 1u), surface.width,
                           clamp.wrapU, taps.x0, taps.x1);
    const bool fitsV = fits(minV, maxV, std::max<uint16_t>(state.textureHeight, 1u), surface.height,
                           clamp.wrapV, taps.y0, taps.y1);
    if (coverage) *coverage = taps;
    return fitsU && fitsV;
}

// The pixels a draw can write: exact for a sprite at scale 1, where the vertex
// shader's half-pixel shift covers [ceil(x0), ceil(x1)) as samplesFitSurface
// assumes, and padded like primitiveBounds otherwise.
GsRegion drawnPixels(const GSPrimitiveBatch &batch, const GsSurface &surface) {
    const GSContext &context = batch.state.context;
    if (surface.scale != 1u || batch.state.prim.type != GS_PRIM_SPRITE || batch.vertexCount != 2u)
        return primitiveBounds(batch);
    const double offsetX = context.xyoffset.ofx / 16.0, offsetY = context.xyoffset.ofy / 16.0;
    const double x0 = batch.vertices[0].x - offsetX, x1 = batch.vertices[1].x - offsetX;
    const double y0 = batch.vertices[0].y - offsetY, y1 = batch.vertices[1].y - offsetY;
    for (double value : {x0, x1, y0, y1})
        if (!std::isfinite(value))
            return primitiveBounds(batch);
    auto span = [](double a, double b, uint32_t low, uint32_t high, uint32_t &first, uint32_t &end) {
        first = static_cast<uint32_t>(std::clamp(std::ceil(std::min(a, b)), double(low), double(high)));
        end = static_cast<uint32_t>(std::clamp(std::ceil(std::max(a, b)), double(low), double(high)));
    };
    GsRegion region;
    span(x0, x1, context.scissor.x0, context.scissor.x1 + 1u, region.x0, region.x1);
    span(y0, y1, context.scissor.y0, context.scissor.y1 + 1u, region.y0, region.y1);
    return region;
}

// The texels a draw can sample from a surface, or all of them when its
// coordinates cannot be bounded.
GsRegion sampledTexels(const GSPrimitiveBatch &batch, const GsSurface &surface) {
    GsRegion taps;
    if (samplesFitSurface(batch, surface, &taps) && !taps.empty())
        return taps;
    return surface.wholeRegion();
}

struct FeedbackPadding {
    // The column tail and bottom row are separate: their bounding rectangle
    // would include source pixels still owned by the GPU.
    GsRegion uploads[2];
    bool rightColumn = false;
};

bool canSnapshotFeedback(const GSPrimitiveBatch &batch, const GsSurface &surface,
                          FeedbackPadding &padding) {
    padding = {};
    if (surface.psm != GS_PSM_CT32 && surface.psm != GS_PSM_CT16 &&
        surface.psm != GS_PSM_CT16S) return false;
    if (samplesFitSurface(batch, surface)) return true;
    GsRegion taps;
    samplesFitSurface(batch, surface, &taps);
    if (taps.empty() || surface.scale != 1u || taps.x1 > surface.width + 1u ||
        taps.y1 > surface.height + 1u) return false;
    const bool rightColumn = taps.x1 > surface.width;
    const bool bottomRow = taps.y1 > surface.height;
    if (rightColumn) {
        if (surface.psm != GS_PSM_CT32 || surface.width != surface.bufferWidth * 64u ||
            surface.height < 32u || surface.height % 32u != 0u) return false;
        // CT32 pages are 64x32. At the complete GS row pitch, (width,y)
        // aliases (0,y+32): most padding belongs to the live GPU source.
        // Only the last page row lies outside that source and can use VRAM.
        padding.rightColumn = true;
        padding.uploads[0] = {surface.width, surface.height - 32u,
                             surface.width + 1u, surface.height};
    }
    if (bottomRow)
        padding.uploads[1] = {0u, surface.height, surface.width + uint32_t(rightColumn),
                             surface.height + 1u};
    GsPageSet tailPages;
    for (const auto &region : padding.uploads)
        gsMarkPages(tailPages, surface.base, surface.bufferWidth, surface.psm,
                    region.width(), region.height(), region.x0, region.y0);
    // CPU-supplied padding must not alias any live source page, even when
    // the GS address wraps around VRAM or the base is inside a page.
    if ((tailPages & surface.pages).any()) return false;
    return true;
}

// DQ8_GFX_TRACE_BATCHES prints one line per draw call: which target, which
// pipeline state, how much geometry. The question it answers is "why is this
// buffer empty" -- either nothing targets it, or something does and is being
// rejected by a test.
bool batchTraceEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DQ8_GFX_TRACE_BATCHES");
        return value != nullptr && *value != '\0';
    }();
    return enabled;
}

} // namespace

struct SdlGpuBackend::Impl {
    struct CancelCommands {
        void operator()(SDL_GPUCommandBuffer *commands) const {
            SDL_CancelGPUCommandBuffer(commands);
        }
    };
    using CommandBuffer = std::unique_ptr<SDL_GPUCommandBuffer, CancelCommands>;
    mutable std::mutex mutex;
    SdlGpuDevice device;
    GsVram vram;
    std::unique_ptr<GsTransferEngine> transfer;
    GSTransferCommand transferCommand{};
    bool patchingHostWrite = false;
    std::unique_ptr<GsTargetCache> targets;
    std::unique_ptr<GsTextureCache> textures;

    std::vector<GsGpuVertex> vertices;
    std::vector<DrawBatch> batches;

    SDL_GPUBuffer *vertexBuffer = nullptr;
    uint32_t vertexBufferCapacity = 0u;
    SDL_GPUTransferBuffer *vertexUpload = nullptr;
    uint32_t vertexUploadCapacity = 0u;
    SDL_GPUTexture *dummyTexture = nullptr;
    // Snapshot textures, one per size. Copies into them cycle, so each draw
    // keeps the snapshot it sampled while later ones are taken.
    struct FeedbackTexture {
        uint32_t width, height;
        SDL_GPUTexture *texture;
    };
    std::vector<FeedbackTexture> feedbackTextures;
    // What the feedback batches at the end of the queue have drawn since their
    // snapshot. A draw that reads none of it can share that snapshot.
    GsRegion feedbackWritten;

    // Set when this backend owns the window. Present() then composes into the
    // swapchain instead of handing pixels back through host memory.
    SDL_Window *window = nullptr;
    // Staging for the fallback path, where the frame had to be composed on the
    // CPU and has to get back onto the GPU to be shown.
    SDL_GPUTexture *presentUpload = nullptr;
    uint32_t presentUploadWidth = 0u;
    uint32_t presentUploadHeight = 0u;
    struct PresentationSlot {
        SDL_GPUTexture *texture = nullptr;
        uint32_t width = 0u, height = 0u;
        SDL_GPUFence *prepared = nullptr, *displayed = nullptr;
        bool leased = false;
    };
    struct PresentationPool {
        std::mutex mutex;
        std::condition_variable available;
        std::array<PresentationSlot, 3> slots{};
        bool closing = false;
    };
    struct PreparedFrame final : GSPreparedPresentation {
        std::shared_ptr<PresentationPool> pool;
        size_t slot;
        PresentationFrame frame;
        bool haveFrame = false, gsActive = false, download = false;
        PreparedFrame(std::shared_ptr<PresentationPool> pool, size_t slot)
            : pool(std::move(pool)), slot(slot) {}
        ~PreparedFrame() override {
            std::lock_guard lock(pool->mutex);
            pool->slots[slot].leased = false;
            pool->available.notify_one();
        }
    };
    std::shared_ptr<PresentationPool> presentationPool = std::make_shared<PresentationPool>();
    PresentationSlot *preparingSlot = nullptr;
    // Worker-only aliases into the slot currently being prepared.
    SDL_GPUTexture *presentSource = nullptr;
    uint32_t presentSourceWidth = 0u;
    uint32_t presentSourceHeight = 0u;
    bool windowCloseRequested = false;
    SdlPadInput pad;
    uint64_t previousPresentDraws = 0u;
    uint64_t previousPresentTransfers = 0u;
    uint64_t titlePresents = 0u;
    uint64_t titleActiveFrames = 0u;
    uint64_t titleCompletedStart = 0u;
    bool nativeCaptureAttempted = false;
    uint32_t nativeCaptureNumber = 0u;
    SdlGpuBackend::CompletedRenderCounter completedRenderCounter = nullptr;
    std::chrono::steady_clock::time_point titleSampleStart{};

    uint32_t scale = 1u;
    SdlGpuStats stats{};
    std::string lastError;

    // Wall time spent inside backend entry points, so a slow frame can be
    // attributed to the renderer or to everything else -- guest code, the GIF
    // frontend, the movie decoder -- without a profiler.
    uint64_t backendNanos = 0u;
    std::atomic<uint64_t> displayNanos{0u};

    struct ScopedTimer {
        uint64_t &sink;
        std::chrono::steady_clock::time_point start;
        explicit ScopedTimer(uint64_t &target)
            : sink(target), start(std::chrono::steady_clock::now()) {}
        ~ScopedTimer() {
            sink += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }
    };
    struct DisplayTimer {
        std::atomic<uint64_t> &sink;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        explicit DisplayTimer(std::atomic<uint64_t> &sink) : sink(sink) {}
        ~DisplayTimer() {
            sink.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        }
    };

    bool setError(std::string message) {
        lastError = std::move(message);
        return false;
    }

    // -----------------------------------------------------------------
    // Local-memory synchronisation
    // -----------------------------------------------------------------

    // Draws are deferred, so anything that touches local memory has to land
    // them first: otherwise a transfer would overwrite a region whose pending
    // draws had not happened yet, and the draws would then be applied on top.
    bool settleFor(const GsPageSet &pages, bool hostWrite = false) {
        if (!flushDraws())
            return false;
        std::string error;
        if (!(hostWrite ? targets->resolveForHostWrite(pages, error) : targets->resolve(pages, error)))
            return setError(std::move(error));
        return true;
    }

    bool settleAll() {
        if (!flushDraws())
            return false;
        std::string error;
        if (!targets->resolveAll(error))
            return setError(std::move(error));
        return true;
    }

    // True when a queued batch draws into any of these pages, so their content
    // is not yet in the render target -- let alone in local memory.
    bool pendingDrawsTouch(const GsPageSet &pages) const {
        for (const DrawBatch &batch : batches) {
            if ((batch.writtenPages & pages).any())
                return true;
        }
        return false;
    }

    bool pendingDrawsTouchOthers(const GsPageSet &pages, const GsSurface *surface) const {
        for (const DrawBatch &batch : batches) {
            if (batch.color != surface && (batch.writtenPages & pages).any())
                return true;
        }
        return false;
    }

    // A write to local memory invalidates both caches: a render target holding
    // that region is now stale, and so is any texture built from it.
    void invalidatePages(const GsPageSet &pages) {
        if (!patchingHostWrite)
            targets->invalidate(pages);
        if (textures)
            textures->invalidate(pages, GsTextureCache::InvalidationSource::HostWrite);
    }

    // -----------------------------------------------------------------
    // Primitive assembly
    // -----------------------------------------------------------------

    static GsGpuVertex makeVertex(const GSVertex &vertex,
                                  const GSDrawState &state,
                                  float offsetX,
                                  float offsetY,
                                  float depthScale) {
        GsGpuVertex out{};
        out.x = vertex.x - offsetX;
        out.y = vertex.y - offsetY;
        out.z = std::clamp(static_cast<float>(vertex.z * depthScale), 0.0f, 1.0f);
        out.q = vertex.q;
        if (state.prim.fst) {
            // UV is a 1/16-texel fixed-point pair.
            out.s = static_cast<float>(vertex.u) / 16.0f;
            out.t = static_cast<float>(vertex.v) / 16.0f;
        } else {
            out.s = vertex.s;
            out.t = vertex.t;
        }
        out.r = static_cast<float>(vertex.r) / 255.0f;
        out.g = static_cast<float>(vertex.g) / 255.0f;
        out.b = static_cast<float>(vertex.b) / 255.0f;
        out.a = static_cast<float>(vertex.a) / 255.0f;
        out.fog = static_cast<float>(vertex.fog) / 255.0f;
        return out;
    }

    // Expands one GS primitive into triangles. Points and lines become thin
    // quads rather than host points and lines, whose rasterisation rules and
    // width limits vary by driver.
    void appendPrimitive(const GSPrimitiveBatch &batch,
                         float offsetX,
                         float offsetY,
                         float depthScale) {
        const GSDrawState &state = batch.state;
        auto convert = [&](const GSVertex &vertex) {
            return makeVertex(vertex, state, offsetX, offsetY, depthScale);
        };
        auto quad = [&](GsGpuVertex a, GsGpuVertex b, GsGpuVertex c, GsGpuVertex d) {
            vertices.push_back(a);
            vertices.push_back(b);
            vertices.push_back(c);
            vertices.push_back(a);
            vertices.push_back(c);
            vertices.push_back(d);
        };

        switch (state.prim.type) {
        case GS_PRIM_POINT: {
            const GsGpuVertex v = convert(batch.vertices[0]);
            GsGpuVertex a = v, b = v, c = v, d = v;
            a.x = std::floor(v.x);
            a.y = std::floor(v.y);
            b.x = a.x + 1.0f;
            b.y = a.y;
            c.x = a.x + 1.0f;
            c.y = a.y + 1.0f;
            d.x = a.x;
            d.y = a.y + 1.0f;
            quad(a, b, c, d);
            break;
        }
        case GS_PRIM_LINE:
        case GS_PRIM_LINESTRIP: {
            const GsGpuVertex v0 = convert(batch.vertices[0]);
            const GsGpuVertex v1 = convert(batch.vertices[1]);
            float dx = v1.x - v0.x;
            float dy = v1.y - v0.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length < 1.0e-6f) {
                GsGpuVertex a = v0, b = v0, c = v0, d = v0;
                b.x += 1.0f;
                c.x += 1.0f;
                c.y += 1.0f;
                d.y += 1.0f;
                quad(a, b, c, d);
                break;
            }
            // Half a pixel either side of the centre line.
            const float nx = -dy / length * 0.5f;
            const float ny = dx / length * 0.5f;
            GsGpuVertex a = v0, b = v0, c = v1, d = v1;
            a.x = v0.x + nx;
            a.y = v0.y + ny;
            b.x = v0.x - nx;
            b.y = v0.y - ny;
            c.x = v1.x - nx;
            c.y = v1.y - ny;
            d.x = v1.x + nx;
            d.y = v1.y + ny;
            quad(a, b, c, d);
            break;
        }
        case GS_PRIM_TRIANGLE:
        case GS_PRIM_TRISTRIP:
        case GS_PRIM_TRIFAN:
            vertices.push_back(convert(batch.vertices[0]));
            vertices.push_back(convert(batch.vertices[1]));
            vertices.push_back(convert(batch.vertices[2]));
            break;
        case GS_PRIM_SPRITE: {
            const GsGpuVertex v0 = convert(batch.vertices[0]);
            const GsGpuVertex v1 = convert(batch.vertices[1]);
            // A sprite takes its colour, depth and fog from the second vertex;
            // only position and texture coordinates come from both.
            GsGpuVertex corner = v1;
            const float x0 = std::min(v0.x, v1.x);
            const float x1 = std::max(v0.x, v1.x);
            const float y0 = std::min(v0.y, v1.y);
            const float y1 = std::max(v0.y, v1.y);
            const float s0 = (v0.x <= v1.x) ? v0.s : v1.s;
            const float s1 = (v0.x <= v1.x) ? v1.s : v0.s;
            const float t0 = (v0.y <= v1.y) ? v0.t : v1.t;
            const float t1 = (v0.y <= v1.y) ? v1.t : v0.t;

            GsGpuVertex a = corner, b = corner, c = corner, d = corner;
            a.x = x0; a.y = y0; a.s = s0; a.t = t0;
            b.x = x1; b.y = y0; b.s = s1; b.t = t0;
            c.x = x1; c.y = y1; c.s = s1; c.t = t1;
            d.x = x0; d.y = y1; d.s = s0; d.t = t1;
            quad(a, b, c, d);
            break;
        }
        default:
            break;
        }
    }

    // -----------------------------------------------------------------
    // Batching
    // -----------------------------------------------------------------

    SDL_GPUTexture *feedbackTextureFor(uint32_t width, uint32_t height, std::string &error) {
        for (const FeedbackTexture &feedback : feedbackTextures)
            if (feedback.width == width && feedback.height == height)
                return feedback.texture;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(device.handle(), &info);
        if (!texture) {
            error = std::string("SDL_CreateGPUTexture(feedback): ") + SDL_GetError();
            return nullptr;
        }
        feedbackTextures.push_back({width, height, texture});
        return texture;
    }

    // Copies a surface into its snapshot texture as part of a flush, after
    // the batches recorded before it.
    SDL_GPUTexture *recordSnapshot(SDL_GPUCommandBuffer *commands, const GsSurface &surface) {
        const uint32_t width = surface.width * surface.scale, height = surface.height * surface.scale;
        std::string error;
        SDL_GPUTexture *snapshot = feedbackTextureFor(width, height, error);
        if (!snapshot) {
            setError(std::move(error));
            return nullptr;
        }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        if (!copy) {
            setError(std::string("SDL_BeginGPUCopyPass(snapshot): ") + SDL_GetError());
            return nullptr;
        }
        SDL_GPUTextureLocation source{}, destination{};
        source.texture = surface.texture;
        destination.texture = snapshot;
        SDL_CopyGPUTextureToTexture(copy, &source, &destination, width, height, 1u, true);
        SDL_EndGPUCopyPass(copy);
        ++stats.feedbackCopies;
        return snapshot;
    }

    SDL_GPUTexture *snapshotFeedback(GsSurface &surface, std::string &error,
                                     const FeedbackPadding &padding, CommandBuffer &ownedCommands) {
        if (!flushDraws() || !targets->refresh(surface, error))
            return nullptr;
        const uint32_t sourceWidth = surface.width * surface.scale;
        const uint32_t sourceHeight = surface.height * surface.scale;
        uint32_t width = sourceWidth, height = sourceHeight, uploadBytes = 0u;
        GsPageSet tailPages;
        for (const auto &region : padding.uploads) {
            width = std::max(width, region.x1);
            height = std::max(height, region.y1);
            uploadBytes += (region.width() * region.height() * 4u + 255u) & ~255u;
            gsMarkPages(tailPages, surface.base, surface.bufferWidth, surface.psm,
                        region.width(), region.height(), region.x0, region.y0);
        }
        const auto releaseUpload = [&](SDL_GPUTransferBuffer *buffer) {
            SDL_ReleaseGPUTransferBuffer(device.handle(), buffer);
        };
        std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseUpload)> upload(nullptr, releaseUpload);
        if (uploadBytes != 0u) {
            if (!targets->resolve(tailPages, error)) return nullptr;
            SDL_GPUTransferBufferCreateInfo info{};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = uploadBytes;
            upload.reset(SDL_CreateGPUTransferBuffer(device.handle(), &info));
            if (!upload) {
                error = std::string("SDL_CreateGPUTransferBuffer(feedback tail): ") + SDL_GetError();
                return nullptr;
            }
            auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device.handle(), upload.get(), false));
            if (!mapped) {
                error = std::string("SDL_MapGPUTransferBuffer(feedback tail): ") + SDL_GetError();
                return nullptr;
            }
            uint32_t offset = 0u;
            for (const auto &region : padding.uploads) {
                auto *pixels = reinterpret_cast<uint32_t *>(mapped + offset);
                for (uint32_t y = region.y0; y < region.y1; ++y) {
                    for (uint32_t x = region.x0; x < region.x1; ++x) {
                        uint32_t color = vram.read(surface.psm, surface.base, surface.bufferWidth, x, y);
                        if (surface.psm != GS_PSM_CT32) {
                            const uint32_t r = color & 31u, g = (color >> 5u) & 31u, b = (color >> 10u) & 31u;
                            color = ((r << 3u) | (r >> 2u)) | (((g << 3u) | (g >> 2u)) << 8u) |
                                    (((b << 3u) | (b >> 2u)) << 16u) | ((color & 0x8000u) << 16u);
                        }
                        *pixels++ = color;
                    }
                }
                offset += (region.width() * region.height() * 4u + 255u) & ~255u;
            }
            SDL_UnmapGPUTransferBuffer(device.handle(), upload.get());
        }
        SDL_GPUTexture *feedbackTexture = feedbackTextureFor(width, height, error);
        if (!feedbackTexture)
            return nullptr;
        ownedCommands.reset(SDL_AcquireGPUCommandBuffer(device.handle()));
        SDL_GPUCommandBuffer *commands = ownedCommands.get();
        if (!commands) {
            error = std::string("SDL_AcquireGPUCommandBuffer(feedback): ") + SDL_GetError();
            return nullptr;
        }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        if (!copy) {
            error = std::string("SDL_BeginGPUCopyPass(feedback): ") + SDL_GetError();
            return nullptr;
        }
        SDL_GPUTextureLocation source{}, destination{};
        source.texture = surface.texture;
        destination.texture = feedbackTexture;
        // Cycle the complete snapshot so submitted draws retain their version.
        SDL_CopyGPUTextureToTexture(copy, &source, &destination, sourceWidth, sourceHeight, 1u, true);
        if (padding.rightColumn && sourceHeight > 32u) {
            source.y = 32u;
            destination.x = sourceWidth;
            SDL_CopyGPUTextureToTexture(copy, &source, &destination, 1u, sourceHeight - 32u, 1u, false);
        }
        if (upload) {
            uint32_t offset = 0u;
            for (const auto &region : padding.uploads) {
                if (region.empty()) continue;
                SDL_GPUTextureTransferInfo from{};
                from.transfer_buffer = upload.get();
                from.offset = offset;
                from.pixels_per_row = region.width();
                from.rows_per_layer = region.height();
                SDL_GPUTextureRegion to{};
                to.texture = feedbackTexture;
                to.x = region.x0;
                to.y = region.y0;
                to.w = region.width();
                to.h = region.height();
                to.d = 1u;
                // The preceding copy already cycled the snapshot's storage.
                SDL_UploadToGPUTexture(copy, &from, &to, false);
                offset += (region.width() * region.height() * 4u + 255u) & ~255u;
            }
        }
        SDL_EndGPUCopyPass(copy);
        ++stats.feedbackCopies;
        return feedbackTexture;
    }

    void submit(const GSPrimitiveBatch &batch) {
        ++stats.primitivesSubmitted;
        if (!device.valid() || !vram.attached() || batch.vertexCount == 0u)
            return;

        const GSDrawState &state = batch.state;
        const GSContext &context = state.context;
        if (context.scissor.x0 > context.scissor.x1 ||
            context.scissor.y0 > context.scissor.y1)
            return;
        if (!gsIsColorFramePsm(context.frame.psm))
            return;

        const GsRegion written = primitiveBounds(batch);
        if (written.empty())
            return;

        const uint32_t neededWidth = static_cast<uint32_t>(context.scissor.x1) + 1u;
        const uint32_t neededHeight = static_cast<uint32_t>(context.scissor.y1) + 1u;

        std::string error;
        GsSurface *color = targets->acquire(context.frame.fbp << 5u, context.frame.fbw,
                                            context.frame.psm, false, neededWidth,
                                            neededHeight, error);
        if (!color) {
            setError(std::move(error));
            return;
        }

        // Distinct FRAME views can name the same GS pages. Finish the old
        // view before importing its pixels into the new one.
        const bool pendingAlias = std::any_of(batches.begin(), batches.end(),
            [&](const DrawBatch &pending) {
                return pending.color != color && (pending.writtenPages & color->pages).any();
            });
        if (pendingAlias && !flushDraws())
            return;
        if (!targets->prepareColorView(*color, error)) {
            setError(std::move(error));
            return;
        }

        const GsTestState test = gsDecodeTest(context.test);
        // Inactive depth must not grow/discard a live surface during colour-only passes.
        const bool wantsDepth = gsIsDepthPsm(context.zbuf.psm) && test.depthTestEnabled &&
            (!context.zbuf.zmask || test.depthTest != kDepthAlways);
        GsSurface *depth = nullptr;
        if (wantsDepth) {
            depth = targets->acquire(context.zbuf.zbp << 5u, context.frame.fbw,
                                     context.zbuf.psm, true, neededWidth,
                                     neededHeight, error);
            if (!depth) {
                setError(std::move(error));
                return;
            }
        }

        DrawBatch draw{};
        draw.color = color;
        draw.depth = depth;
        draw.written = written;
        draw.overwritten = opaqueSpriteBounds(batch, *color, test);
        gsMarkPages(draw.writtenPages, color->base, color->bufferWidth, color->psm,
                    written.width(), written.height(), written.x0, written.y0);

        GsAlphaState alpha = gsDecodeAlpha(context.alpha);
        // A constant factor leaves the shader free to store the original alpha.
        if (!device.framebufferFetch() && state.prim.abe && alpha.c == kBlendAlphaSource &&
            (!state.prim.tme || !context.tex0.tcc)) {
            const uint8_t a = batch.vertices[batch.vertexCount - 1u].a;
            const bool constant = !state.prim.iip || state.prim.type == GS_PRIM_SPRITE ||
                std::all_of(batch.vertices.begin(), batch.vertices.begin() + batch.vertexCount,
                            [a](const GSVertex &v) { return v.a == a; });
            if (constant && a <= 0x80u) {
                alpha.c = kBlendAlphaFixed;
                alpha.fix = a;
            }
        }
        const GsBlendTranslation blend = gsTranslateBlend(alpha, state.prim.abe);
        const bool framebufferFetch = device.framebufferFetch();
        if (!framebufferFetch && blend.enabled && !blend.exact)
            ++stats.inexactBlends;
        if (!framebufferFetch && state.colclamp == 0u)
            ++stats.disabledColorClamps;
        if (!framebufferFetch && test.destinationAlphaTestEnabled)
            ++stats.destinationAlphaTests;
        if (test.alphaTestEnabled && test.alphaFail != kAfailKeep)
            ++stats.alphaFailModes;

        bool partialMask = false;
        uint8_t writeMask = colorWriteMaskFromFbmsk(context.frame.fbmsk, partialMask);
        if (!framebufferFetch && partialMask)
            ++stats.partialChannelMasks;

        // The GS blend factor is A/128 while the host reads the emitted alpha
        // as A/255, so the shader scales it. That makes the stored alpha wrong,
        // which only matters if the alpha channel is written at all -- so it is
        // dropped from the write mask instead. Framebuffer fetch avoids this
        // fallback limitation, including later effects which sample alpha.
        if (!framebufferFetch && blend.enabled && alpha.c == kBlendAlphaDest)
            ++stats.destinationAlphaFactors;
        const bool scaleAlpha = !framebufferFetch && blend.enabled && alpha.c == kBlendAlphaSource;
        if (scaleAlpha) {
            writeMask &= static_cast<uint8_t>(~SDL_GPU_COLORCOMPONENT_A);
            // Alpha above 0x80 asks for a blend factor greater than one, which
            // a fixed-point attachment cannot carry.
            for (uint8_t index = 0u; index < batch.vertexCount; ++index) {
                if (batch.vertices[index].a > 0x80u) {
                    ++stats.saturatedBlendFactors;
                    break;
                }
            }
        }

        draw.pipeline.colorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        draw.pipeline.depthFormat = device.depthFormat();
        draw.pipeline.hasDepth = depth != nullptr ? 1u : 0u;
        draw.pipeline.depthCompare =
            static_cast<uint8_t>(gsTranslateDepthCompare(test.depthTest, test.depthTestEnabled));
        draw.pipeline.depthWrite = (depth != nullptr && !context.zbuf.zmask) ? 1u : 0u;
        draw.pipeline.colorWriteMask = writeMask;
        draw.pipeline.blendEnabled = !framebufferFetch && blend.enabled ? 1u : 0u;
        draw.pipeline.sourceFactor = static_cast<uint8_t>(blend.sourceFactor);
        draw.pipeline.destinationFactor = static_cast<uint8_t>(blend.destinationFactor);
        draw.pipeline.blendOperation = static_cast<uint8_t>(blend.operation);
        draw.usesBlendConstant = !framebufferFetch && blend.usesConstant;
        draw.blendConstant = framebufferFetch ? 0.0f : blend.constant;
        draw.fragmentUniforms.blend[0] = uint32_t(context.alpha & 0xffu) |
            (uint32_t((context.alpha >> 32u) & 0xffu) << 8u);
        draw.fragmentUniforms.blend[1] = context.frame.fbmsk;
        draw.fragmentUniforms.blend[2] = uint32_t(context.test);
        draw.fragmentUniforms.blend[3] = (state.prim.abe ? 1u : 0u) |
            (state.colclamp ? 2u : 0u) | (state.pabe ? 4u : 0u) |
            ((context.fba & 1u) ? 8u : 0u) | (uint32_t(context.frame.psm) << 8u);

        draw.vertexUniforms.targetSize[0] = static_cast<float>(color->width);
        draw.vertexUniforms.targetSize[1] = static_cast<float>(color->height);
        draw.vertexUniforms.targetSize[2] = 1.0f / static_cast<float>(color->width);
        draw.vertexUniforms.targetSize[3] = 1.0f / static_cast<float>(color->height);
        draw.vertexUniforms.adjust[0] = kSampleOffset;
        draw.vertexUniforms.adjust[1] = kSampleOffset;
        draw.vertexUniforms.adjust[2] = kClipYDirection;

        uint32_t control = 0u;
        if (state.prim.fge)
            control |= kFragFlagFge;
        if (test.alphaTestEnabled)
            control |= kFragFlagAte;

        float textureWidth = 1.0f;
        float textureHeight = 1.0f;
        CommandBuffer feedbackCommands;
        GsClampState clampState{};
        draw.texture = dummyTexture;
        float textureScale = 1.0f;
        if (state.prim.tme) {
            ++stats.texturedPrimitives;

            // A texture read of a region a render target already holds can
            // bind that target instead of resolving it to local memory and
            // rebuilding a native-resolution copy. It is the common case for
            // render-to-texture, and at a render scale above 1 it is also the
            // more accurate one: the copy would be downsampled.
            //
            // Not when it is the target being drawn into -- reading and
            // writing one attachment in a single draw is undefined.
            SDL_GPUTexture *texture = nullptr;
            GsSurface *sampled = nullptr;
            FeedbackPadding feedbackPadding;
            const GsPageSet sourcePages = textures->sourcePagesFor(state);
            // A live target that owes nothing to local memory needs no flush:
            // draws queued into it stay ahead of this one within the flush. The
            // target being drawn into is read through a snapshot taken there.
            GsSurface *const candidate = gsIsIndexedPsm(context.tex0.psm) ? nullptr
                : targets->sampleCandidate(context.tex0.tbp0, context.tex0.psm, context.tex0.tbw);
            const bool inOrder = candidate && candidate->cpuPatches.empty() && candidate->needsUpload.empty() &&
                                 !pendingDrawsTouchOthers(sourcePages, candidate);
            const bool readsItself = inOrder && candidate == color &&
                canSnapshotFeedback(batch, *color, feedbackPadding) &&
                feedbackPadding.uploads[0].empty() && feedbackPadding.uploads[1].empty();
            if (inOrder && candidate != color && samplesFitSurface(batch, *candidate)) {
                sampled = candidate;
            } else if (!readsItself) {
                if (pendingDrawsTouch(sourcePages) && !flushDraws())
                    return;
                if (!gsIsIndexedPsm(context.tex0.psm)) {
                    sampled = targets->findSampleSource(context.tex0.tbp0, context.tex0.psm,
                                                        context.tex0.tbw);
                }
            }
            const bool directSample = sampled && sampled != color && samplesFitSurface(batch, *sampled);
            const bool copyFeedback = sampled && !directSample &&
                canSnapshotFeedback(batch, *sampled, feedbackPadding);
            // Live targets have not passed through texture-cache TEXA expansion.
            const auto liveTargetTexa = [&] {
                if (context.tex0.psm == GS_PSM_CT24 || context.tex0.psm == GS_PSM_CT16 ||
                    context.tex0.psm == GS_PSM_CT16S) {
                    draw.fragmentUniforms.misc[2] = context.tex0.psm == GS_PSM_CT24 ? 1.0f : 2.0f;
                    draw.fragmentUniforms.misc[3] = static_cast<float>(
                        uint32_t(state.texa.ta0) | (uint32_t(state.texa.ta1) << 8u) |
                        (state.texa.aem ? 0x10000u : 0u));
                }
            };
            if (readsItself) {
                ++stats.textureFeedbackHazards;
                texture = feedbackTextureFor(color->width * color->scale, color->height * color->scale, error);
                if (!texture) {
                    setError(std::move(error));
                    return;
                }
                // The strips of one blur or channel-shuffle pass mostly read
                // pixels no earlier strip of the pass has drawn, so they can
                // share one snapshot, and then one draw call.
                draw.feedback = true;
                draw.snapshot = batches.empty() || !batches.back().feedback || batches.back().color != color ||
                                sampledTexels(batch, *color).intersects(feedbackWritten);
                textureScale = static_cast<float>(color->scale);
                liveTargetTexa();
            } else if (directSample) {
                texture = sampled->texture;
                draw.source = sampled;
                textureScale = static_cast<float>(sampled->scale);
                ++stats.texturesFromLiveTargets;
                liveTargetTexa();
            } else if (copyFeedback) {
                if (sampled == color)
                    ++stats.textureFeedbackHazards;
                else
                    ++stats.texturesFromLiveTargets;
                texture = snapshotFeedback(*sampled, error, feedbackPadding, feedbackCommands);
                if (!texture) {
                    if (!error.empty())
                        setError(std::move(error));
                    return;
                }
                textureScale = static_cast<float>(sampled->scale);
                liveTargetTexa();
            } else {
                if (sampled == color && sampled != nullptr)
                    ++stats.textureFeedbackHazards;

                // Two reasons a queued batch has to land before a texture is
                // built. Building one reads local memory, so anything still
                // queued that draws into the region it reads owes it content.
                // And a full cache clears itself on the next miss, which would
                // leave every queued batch holding a released texture.
                if (!batches.empty() &&
                    (textures->evictionImminent() ||
                     pendingDrawsTouch(textures->sourcePagesFor(state)))) {
                    if (!flushDraws())
                        return;
                }

                std::string textureError;
                texture = textures->acquire(state, textureError);
                if (!texture && !textureError.empty())
                    setError(std::move(textureError));
            }

            if (texture) {
                control |= kFragFlagTme;
                if (state.prim.fst)
                    control |= kFragFlagFst;
                if (context.tex0.tcc != 0u)
                    control |= kFragFlagTcc;
                if (state.linearFilter)
                    control |= kFragFlagLinear;
                textureWidth = static_cast<float>(std::max<uint16_t>(state.textureWidth, 1u));
                textureHeight = static_cast<float>(std::max<uint16_t>(state.textureHeight, 1u));
                clampState = gsDecodeClamp(context.clamp);
                draw.texture = texture;
            } else {
                // A texture that cannot be built leaves the primitive drawing
                // with vertex colour alone rather than dropping it, so the
                // geometry stays visible and the count says how often.
                ++stats.untranslatedTextures;
            }
        }

        draw.fragmentUniforms.control[0] = control;
        draw.fragmentUniforms.control[1] = static_cast<uint32_t>(test.alphaTest);
        draw.fragmentUniforms.control[2] = context.tex0.tfx;
        draw.fragmentUniforms.control[3] = static_cast<uint32_t>(clampState.wrapU) |
                                           (static_cast<uint32_t>(clampState.wrapV) << 2u);
        draw.fragmentUniforms.fog[0] = static_cast<float>(state.fogR) / 255.0f;
        draw.fragmentUniforms.fog[1] = static_cast<float>(state.fogG) / 255.0f;
        draw.fragmentUniforms.fog[2] = static_cast<float>(state.fogB) / 255.0f;
        draw.fragmentUniforms.fog[3] = static_cast<float>(test.alphaReference) / 255.0f;
        draw.fragmentUniforms.textureSize[0] = textureWidth;
        draw.fragmentUniforms.textureSize[1] = textureHeight;
        draw.fragmentUniforms.textureSize[2] = 1.0f / textureWidth;
        draw.fragmentUniforms.textureSize[3] = 1.0f / textureHeight;
        draw.fragmentUniforms.region[0] = static_cast<float>(clampState.minU);
        draw.fragmentUniforms.region[1] = static_cast<float>(clampState.maxU);
        draw.fragmentUniforms.region[2] = static_cast<float>(clampState.minV);
        draw.fragmentUniforms.region[3] = static_cast<float>(clampState.maxV);
        draw.fragmentUniforms.misc[0] = scaleAlpha ? (255.0f / 128.0f) : 1.0f;
        draw.fragmentUniforms.misc[1] = textureScale;

        const uint32_t scissorScale = color->scale;
        draw.scissor.x = static_cast<int>(context.scissor.x0 * scissorScale);
        draw.scissor.y = static_cast<int>(context.scissor.y0 * scissorScale);
        draw.scissor.w = static_cast<int>((context.scissor.x1 - context.scissor.x0 + 1u) * scissorScale);
        draw.scissor.h = static_cast<int>((context.scissor.y1 - context.scissor.y0 + 1u) * scissorScale);

        const float offsetX = static_cast<float>(context.xyoffset.ofx) / 16.0f;
        const float offsetY = static_cast<float>(context.xyoffset.ofy) / 16.0f;
        const float depthScale =
            1.0f / static_cast<float>(depthMaximumForPsm(context.zbuf.psm));

        const uint32_t firstVertex = static_cast<uint32_t>(vertices.size());
        appendPrimitive(batch, offsetX, offsetY, depthScale);
        const uint32_t addedVertices = static_cast<uint32_t>(vertices.size()) - firstVertex;
        if (addedVertices == 0u)
            return;

        ++stats.primitivesDrawn;
        stats.trianglesDrawn += addedVertices / 3u;

        // Merge into the open batch when nothing about the state changed. The
        // frontend hands over one primitive at a time, so without this every
        // triangle would be its own draw call.
        if (!batches.empty() && batches.back().sameStateAs(draw)) {
            batches.back().vertexCount += addedVertices;
            batches.back().written.merge(draw.written);
            batches.back().writtenPages |= draw.writtenPages;
        } else {
            draw.firstVertex = firstVertex;
            draw.vertexCount = addedVertices;
            batches.push_back(draw);
            ++stats.batches;
        }
        if (draw.snapshot)
            feedbackWritten.clear();
        if (draw.feedback)
            feedbackWritten.merge(drawnPixels(batch, *color));
        // Copy and consume the snapshot before another submission can change its source.
        if (feedbackCommands)
            flushDraws(std::move(feedbackCommands));
    }

    // -----------------------------------------------------------------
    // Submission
    // -----------------------------------------------------------------

    bool ensureVertexBuffer(uint32_t requiredBytes) {
        if (vertexBuffer && vertexBufferCapacity >= requiredBytes)
            return true;
        if (vertexBuffer)
            SDL_ReleaseGPUBuffer(device.handle(), vertexBuffer);
        vertexBuffer = nullptr;

        uint32_t capacity = std::max<uint32_t>(requiredBytes, 64u * 1024u);
        // Round up so a slowly growing scene does not reallocate every frame.
        capacity = (capacity + 0xffffu) & ~0xffffu;
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = capacity;
        vertexBuffer = SDL_CreateGPUBuffer(device.handle(), &info);
        if (!vertexBuffer)
            return setError(std::string("SDL_CreateGPUBuffer(vertices): ") + SDL_GetError());
        vertexBufferCapacity = capacity;
        return true;
    }

    bool uploadVertices(SDL_GPUCommandBuffer *commands) {
        const uint32_t bytes =
            static_cast<uint32_t>(vertices.size() * sizeof(GsGpuVertex));
        if (bytes == 0u)
            return true;
        if (!ensureVertexBuffer(bytes))
            return false;

        if (!vertexUpload || vertexUploadCapacity < bytes) {
            if (vertexUpload)
                SDL_ReleaseGPUTransferBuffer(device.handle(), vertexUpload);
            SDL_GPUTransferBufferCreateInfo info{};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = vertexBufferCapacity;
            vertexUpload = SDL_CreateGPUTransferBuffer(device.handle(), &info);
            if (!vertexUpload)
                return setError(std::string("SDL_CreateGPUTransferBuffer(vertices): ") + SDL_GetError());
            vertexUploadCapacity = info.size;
        }
        // Cycle storage still referenced by queued GPU work instead of replacing it.
        void *mapped = SDL_MapGPUTransferBuffer(device.handle(), vertexUpload, true);
        if (!mapped)
            return setError(std::string("SDL_MapGPUTransferBuffer(vertices): ") + SDL_GetError());
        std::memcpy(mapped, vertices.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device.handle(), vertexUpload);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        if (!copy)
            return setError(std::string("SDL_BeginGPUCopyPass(vertices): ") + SDL_GetError());
        const SDL_GPUTransferBufferLocation source{vertexUpload, 0u};
        const SDL_GPUBufferRegion destination{vertexBuffer, 0u, bytes};
        SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        SDL_EndGPUCopyPass(copy);
        return true;
    }

    bool flushDraws(CommandBuffer ownedCommands = {}) {
        if (batches.empty()) {
            vertices.clear();
            return true;
        }
        if (!device.valid())
            return setError("SDL GPU device is not available");

        // Every target must agree with local memory before it is drawn into.
        std::string error;
        for (DrawBatch &batch : batches) {
            // The first queued primitive can replace every pixel an upload would supply.
            if (batch.color && batch.overwritten.contains(batch.color->needsUpload))
                batch.color->needsUpload.clear();
            if (batch.color && !targets->refresh(*batch.color, error))
                return setError(std::move(error));
            // A sampled target that grew since the batch was queued reloads here too.
            if (batch.source && !targets->refresh(*batch.source, error))
                return setError(std::move(error));
        }

        if (!ownedCommands)
            ownedCommands.reset(SDL_AcquireGPUCommandBuffer(device.handle()));
        SDL_GPUCommandBuffer *commands = ownedCommands.get();
        if (!commands)
            return setError(std::string("SDL_AcquireGPUCommandBuffer(draw): ") + SDL_GetError());
        // Keep the upload ordered with its consumers without another submission.
        if (!uploadVertices(commands)) {
            return false;
        }

        SDL_GPURenderPass *pass = nullptr;
        GsSurface *activeColor = nullptr;
        GsSurface *activeDepth = nullptr;
        GsPageSet drawnPages;

        auto endPass = [&]() {
            if (pass) {
                SDL_EndGPURenderPass(pass);
                pass = nullptr;
            }
        };

        for (const DrawBatch &batch : batches) {
            SDL_GPUTexture *sampledTexture = batch.source ? batch.source->texture : batch.texture;
            if (batch.snapshot) {
                endPass();
                sampledTexture = recordSnapshot(commands, *batch.color);
                if (!sampledTexture)
                    return false;
            }
            if (!pass || batch.color != activeColor || batch.depth != activeDepth) {
                endPass();
                SDL_GPUColorTargetInfo colorInfo{};
                colorInfo.texture = batch.color->texture;
                colorInfo.load_op = SDL_GPU_LOADOP_LOAD;
                colorInfo.store_op = SDL_GPU_STOREOP_STORE;

                SDL_GPUDepthStencilTargetInfo depthInfo{};
                if (batch.depth) {
                    depthInfo.texture = batch.depth->texture;
                    // A depth surface is never read back from local memory, so
                    // the first pass that touches one starts it at the far
                    // plane: GS Z counts up towards the viewer.
                    depthInfo.load_op = batch.depth->undefined ? SDL_GPU_LOADOP_CLEAR
                                                               : SDL_GPU_LOADOP_LOAD;
                    depthInfo.clear_depth = 0.0f;
                    depthInfo.store_op = SDL_GPU_STOREOP_STORE;
                    depthInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
                    depthInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                    batch.depth->undefined = false;
                }

                pass = SDL_BeginGPURenderPass(commands, &colorInfo, 1u,
                                              batch.depth ? &depthInfo : nullptr);
                if (!pass) {
                    return setError(std::string("SDL_BeginGPURenderPass: ") + SDL_GetError());
                }
                activeColor = batch.color;
                activeDepth = batch.depth;
                ++stats.renderPasses;

                SDL_GPUViewport viewport{};
                viewport.x = 0.0f;
                viewport.y = 0.0f;
                viewport.w = static_cast<float>(batch.color->width * batch.color->scale);
                viewport.h = static_cast<float>(batch.color->height * batch.color->scale);
                viewport.min_depth = 0.0f;
                viewport.max_depth = 1.0f;
                SDL_SetGPUViewport(pass, &viewport);

                const SDL_GPUBufferBinding binding{vertexBuffer, 0u};
                SDL_BindGPUVertexBuffers(pass, 0u, &binding, 1u);
            }

            SDL_GPUGraphicsPipeline *pipeline = device.pipeline(batch.pipeline, error);
            if (!pipeline) {
                endPass();
                return setError(std::move(error));
            }
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_SetGPUScissor(pass, &batch.scissor);
            if (batch.usesBlendConstant) {
                const SDL_FColor constant{batch.blendConstant, batch.blendConstant,
                                          batch.blendConstant, batch.blendConstant};
                SDL_SetGPUBlendConstants(pass, constant);
            }

            const SDL_GPUTextureSamplerBinding textureBinding{
                sampledTexture ? sampledTexture : dummyTexture, device.sampler()};
            SDL_BindGPUFragmentSamplers(pass, 0u, &textureBinding, 1u);

            SDL_PushGPUVertexUniformData(commands, 0u, &batch.vertexUniforms,
                                         sizeof(batch.vertexUniforms));
            SDL_PushGPUFragmentUniformData(commands, 0u, &batch.fragmentUniforms,
                                           sizeof(batch.fragmentUniforms));
            SDL_DrawGPUPrimitives(pass, batch.vertexCount, 1u, batch.firstVertex, 0u);
            ++stats.drawCalls;

            if (batchTraceEnabled()) {
                std::fprintf(
                    stderr,
                    "[batch] target=%05x psm=%02x %ux%u depth=%s verts=%u tme=%d "
                    "ate=%d atst=%u aref=%.0f tfx=%u blend=%d src=%u dst=%u op=%u "
                    "mask=%x zcmp=%u zwrite=%d scissor=(%d,%d %dx%d)\n",
                    batch.color->base, batch.color->psm, batch.color->width,
                    batch.color->height, batch.depth ? "yes" : "no", batch.vertexCount,
                    (batch.fragmentUniforms.control[0] & kFragFlagTme) != 0u,
                    (batch.fragmentUniforms.control[0] & kFragFlagAte) != 0u,
                    batch.fragmentUniforms.control[1],
                    batch.fragmentUniforms.fog[3] * 255.0f,
                    batch.fragmentUniforms.control[2], batch.pipeline.blendEnabled,
                    batch.pipeline.sourceFactor, batch.pipeline.destinationFactor,
                    batch.pipeline.blendOperation, batch.pipeline.colorWriteMask,
                    batch.pipeline.depthCompare, batch.pipeline.depthWrite,
                    batch.scissor.x, batch.scissor.y, batch.scissor.w, batch.scissor.h);
            }

            targets->markDrawn(*batch.color, batch.written);
            drawnPages |= batch.writtenPages;
        }

        endPass();
        if (!SDL_SubmitGPUCommandBuffer(ownedCommands.release()))
            return setError(std::string("SDL_SubmitGPUCommandBuffer(draw): ") + SDL_GetError());
        batches.clear();
        vertices.clear();

        // Drawing into a render target changes what a texture built from that
        // region should contain. Without this the cache keeps serving the copy
        // it made earlier, which is how a render-to-texture blit ends up
        // showing the frame before last -- or, on the first frame, nothing.
        if (textures && drawnPages.any())
            textures->invalidate(drawnPages, GsTextureCache::InvalidationSource::Draw);
        return true;
    }

    bool clearSurface(GsSurface &surface, float r, float g, float b, float a) {
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device.handle());
        if (!commands)
            return setError(std::string("SDL_AcquireGPUCommandBuffer(clear): ") + SDL_GetError());
        SDL_GPUColorTargetInfo info{};
        info.texture = surface.texture;
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.store_op = SDL_GPU_STOREOP_STORE;
        info.clear_color = SDL_FColor{r, g, b, a};
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &info, 1u, nullptr);
        if (!pass) {
            SDL_CancelGPUCommandBuffer(commands);
            return setError(std::string("SDL_BeginGPURenderPass(clear): ") + SDL_GetError());
        }
        SDL_EndGPURenderPass(pass);
        SDL_SubmitGPUCommandBuffer(commands);
        // A clear covers the whole surface and supersedes any pending upload.
        surface.needsUpload.clear();
        surface.cpuPatches.clear();
        targets->markDrawn(surface, surface.wholeRegion());
        return true;
    }

    // -----------------------------------------------------------------
    // Presentation
    // -----------------------------------------------------------------

    // Reads one CRT read circuit into a tightly packed RGBA8 image at `factor`
    // times its GS size. Preferring the render target over local memory is
    // what makes 4K real: nothing is downsampled on the way to the screen.
    void readCircuit(const GSPresentationRequest &request,
                     const GsDisplaySetup &setup,
                     bool halfHeightSource,
                     bool allowPreferred,
                     uint32_t factor,
                     std::vector<uint8_t> &out,
                     uint32_t &sourceFbp,
                     bool &usedPreferred) {
        const uint32_t width = setup.width * factor;
        const uint32_t height = setup.height * factor;
        out.assign(static_cast<size_t>(width) * height * 4u, 0u);

        uint32_t sourceBase = setup.fbp << 5u;
        uint32_t sourceWidth = setup.fbw;
        uint32_t sourcePsm = setup.psm;
        uint32_t originX = setup.originX;
        uint32_t originY = setup.originY;
        sourceFbp = setup.fbp;
        usedPreferred = false;

        // The frontend can point at the context frame that actually holds the
        // image when the CRTC's own DISPFB has not caught up.
        if (allowPreferred && request.hasPreferredSource &&
            request.preferredDestFbp == setup.fbp &&
            gsIsColorFramePsm(request.preferredSource.psm)) {
            sourceBase = request.preferredSource.fbp;
            sourceWidth = std::max<uint32_t>(request.preferredSource.fbw, 1u);
            sourcePsm = request.preferredSource.psm;
            originX = 0u;
            originY = 0u;
            sourceFbp = request.preferredSource.fbp;
            usedPreferred = true;
        }
        if (!gsIsColorFramePsm(sourcePsm))
            return;

        // Output row y maps to native row y/factor; the sub-row index is kept
        // so a scaled source contributes all of its detail.
        auto sourceRowFor = [&](uint32_t y) {
            const uint32_t nativeY = y / factor;
            const uint32_t sub = y % factor;
            // FFMD=1 reads a half-height buffer, so each source row covers two
            // output rows.
            const uint32_t nativeSource =
                originY + (halfHeightSource ? nativeY / 2u : nativeY);
            return nativeSource * factor + sub;
        };

        // DQ8_GFX_PRESENT_FROM_MEMORY forces the local-memory path, which is
        // what the software backend reads. It answers "is the difference in
        // what was drawn, or in how it was read back for display" in one run.
        static const bool forceMemoryPath = [] {
            const char *value = std::getenv("DQ8_GFX_PRESENT_FROM_MEMORY");
            return value != nullptr && *value != '\0';
        }();

        GsSurface *surface = forceMemoryPath
                                 ? nullptr
                                 : targets->findColorSurface(sourceBase, sourcePsm);
        // Host transfers can replace a cached target without another draw.
        // Read local memory until that target has consumed the new pixels.
        if (surface && !surface->undefined && surface->scale == factor &&
            surface->needsUpload.empty()) {
            std::vector<uint8_t> scaled;
            std::string error;
            if (downloadSurfaceScaled(*surface, scaled, error)) {
                const uint32_t scaledWidth = surface->width * factor;
                const uint32_t scaledHeight = surface->height * factor;
                for (uint32_t y = 0u; y < height; ++y) {
                    const uint32_t sourceRow = sourceRowFor(y);
                    if (sourceRow >= scaledHeight)
                        break;
                    uint8_t *destination = out.data() + static_cast<size_t>(y) * width * 4u;
                    const uint8_t *source =
                        scaled.data() + static_cast<size_t>(sourceRow) * scaledWidth * 4u;
                    for (uint32_t x = 0u; x < width; ++x) {
                        const uint32_t sourceColumn = originX * factor + x;
                        if (sourceColumn >= scaledWidth)
                            break;
                        std::memcpy(destination + x * 4u, source + sourceColumn * 4u, 4u);
                        // A render target is RGBA8 whatever it represents, so
                        // it still carries the alpha the draws wrote. The GS
                        // buffer it stands for may have nowhere to keep that:
                        // CT24 has no alpha at all, and CT16 has one bit. The
                        // PCRTC blends the two circuits using this value, so
                        // handing it the leftover would tint every pixel the
                        // buffer's own format would have discarded.
                        destination[x * 4u + 3u] =
                            displayAlphaForPsm(sourcePsm, destination[x * 4u + 3u]);
                    }
                }
                return;
            }
            setError(std::move(error));
        }

        // Nothing owns the buffer on the GPU -- it was filled by transfers,
        // which is exactly how DQ8 shows a movie frame. Make sure any target
        // that owns part of it has resolved, then read local memory.
        GsPageSet pages;
        gsMarkPages(pages, sourceBase, sourceWidth, sourcePsm,
                    originX + setup.width, originY + setup.height);
        std::string error;
        if (!targets->resolve(pages, error))
            setError(std::move(error));

        for (uint32_t y = 0u; y < height; ++y) {
            const uint32_t sourceRow = sourceRowFor(y) / factor;
            uint8_t *destination = out.data() + static_cast<size_t>(y) * width * 4u;
            for (uint32_t x = 0u; x < width; ++x) {
                const uint32_t value =
                    vram.read(sourcePsm, sourceBase, sourceWidth, originX + x / factor, sourceRow);
                unpackDisplayColor(sourcePsm, value, destination + x * 4u);
            }
        }
    }

    // What alpha a display buffer of this PSM can actually hold.
    static uint8_t displayAlphaForPsm(uint32_t psm, uint8_t stored) {
        switch (psm & 0x3fu) {
        case GS_PSM_CT24:
            return 0u;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            // One bit, expanded the way the GS reads it back.
            return (stored >= 0x80u) ? 0x80u : 0u;
        case GS_PSM_CT32:
        default:
            return stored;
        }
    }

    // Alpha is kept, not forced opaque: the PCRTC blends the two read circuits
    // using circuit 1's alpha, so it is still live data at this point.
    static void unpackDisplayColor(uint32_t psm, uint32_t value, uint8_t *rgba) {
        if ((psm & 0x3fu) == GS_PSM_CT16 || (psm & 0x3fu) == GS_PSM_CT16S) {
            const uint32_t r = value & 31u;
            const uint32_t g = (value >> 5u) & 31u;
            const uint32_t b = (value >> 10u) & 31u;
            rgba[0] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
            rgba[1] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
            rgba[2] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
            rgba[3] = ((value & 0x8000u) != 0u) ? 0x80u : 0u;
        } else {
            rgba[0] = static_cast<uint8_t>(value);
            rgba[1] = static_cast<uint8_t>(value >> 8u);
            rgba[2] = static_cast<uint8_t>(value >> 16u);
            rgba[3] = (psm & 0x3fu) == GS_PSM_CT24
                          ? 0u
                          : static_cast<uint8_t>(value >> 24u);
        }
    }

    static uint8_t blendCircuitChannel(uint8_t source, uint8_t destination, uint32_t factor) {
        const int delta = static_cast<int>(source) - static_cast<int>(destination);
        const int result = static_cast<int>(destination) +
                           (delta * static_cast<int>(factor)) / 255;
        return static_cast<uint8_t>(std::clamp(result, 0, 255));
    }

    bool downloadSurfaceScaled(GsSurface &surface,
                               std::vector<uint8_t> &out,
                               std::string &error) {
        return targets->download(surface, out, error);
    }

    // DQ8_GFX_STATS_EVERY=N prints a counter delta every N presented frames.
    // The movie path is transfer-dominated rather than draw-dominated, so
    // "how many transfers per frame and how long did the frame take" is the
    // question a profile of the whole process answers badly.
    void reportStats() {
        static const uint32_t interval = [] {
            const char *value = std::getenv("DQ8_GFX_STATS_EVERY");
            return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
        }();
        if (interval == 0u || (stats.presents % interval) != 0u)
            return;

        using Clock = std::chrono::steady_clock;
        static Clock::time_point last = Clock::now();
        static SdlGpuStats previous{};
        static bool primed = false;
        const Clock::time_point now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - last).count();
        last = now;
        // The first call establishes the baseline; reporting it would divide by
        // the time since this function was first entered, which is zero.
        if (!primed) {
            primed = true;
            previous = stats;
            return;
        }

        const auto delta = [](uint64_t a, uint64_t b) { return a - b; };
        static uint64_t previousBackendNanos = 0u;
        const uint64_t totalBackendNanos = backendNanos + displayNanos.load(std::memory_order_relaxed);
        const double backendSeconds =
            static_cast<double>(totalBackendNanos - previousBackendNanos) / 1e9;
        previousBackendNanos = totalBackendNanos;

        // The frontend's total includes the backend calls it makes, so its own
        // share is the difference.
        static uint64_t previousFrontendNanos = 0u;
        static uint64_t previousFrontendPackets = 0u;
        const uint64_t frontendNanos =
            g_gsFrontendPacketNanos.load(std::memory_order_relaxed);
        const uint64_t frontendPackets =
            g_gsFrontendPacketCount.load(std::memory_order_relaxed);
        // The two totals are accumulated on different threads, so the
        // subtraction can land slightly negative on a quiet interval.
        const double frontendSeconds = std::max(
            0.0, static_cast<double>(frontendNanos - previousFrontendNanos) / 1e9 - backendSeconds);
        previousFrontendNanos = frontendNanos;

        // The native image-upload path is where DQ8's movie tiles arrive: one
        // DMA chain per 16x16 tile, never touching processGIFPacket.
        static uint64_t previousUploadNanos = 0u;
        static uint64_t previousUploadCount = 0u;
        const uint64_t uploadNanos = g_gsUploadNativeNanos.load(std::memory_order_relaxed);
        const uint64_t uploadCount = g_gsUploadNativeCount.load(std::memory_order_relaxed);
        const double uploadSeconds =
            static_cast<double>(uploadNanos - previousUploadNanos) / 1e9;
        previousUploadNanos = uploadNanos;

        // The movie path. get-picture contains write-frame, so the difference
        // is decode; demux is the ring feed that precedes both.
        auto sample = [&seconds](std::atomic<uint64_t> &nanos, std::atomic<uint64_t> &count,
                                 uint64_t &previousNanos, uint64_t &outCount) {
            (void)seconds;
            const uint64_t currentNanos = nanos.load(std::memory_order_relaxed);
            outCount = count.load(std::memory_order_relaxed);
            const double elapsed = static_cast<double>(currentNanos - previousNanos) / 1e9;
            previousNanos = currentNanos;
            return elapsed;
        };
        static uint64_t previousGetPictureNanos = 0u, previousGetPictureCount = 0u;
        static uint64_t previousWriteFrameNanos = 0u, previousWriteFrameCount = 0u;
        static uint64_t previousDemuxNanos = 0u, previousDemuxCount = 0u;
        uint64_t getPictureCount = 0u, writeFrameCount = 0u, demuxCount = 0u;
        const double getPictureSeconds =
            sample(g_mpegGetPictureNanos, g_mpegGetPictureCount, previousGetPictureNanos, getPictureCount);
        const double writeFrameSeconds =
            sample(g_mpegWriteFrameNanos, g_mpegWriteFrameCount, previousWriteFrameNanos, writeFrameCount);
        const double demuxSeconds =
            sample(g_mpegDemuxNanos, g_mpegDemuxCount, previousDemuxNanos, demuxCount);
        static uint64_t previousRefusedCount = 0u;
        const uint64_t refusedCount = g_mpegDemuxRefusedCount.load(std::memory_order_relaxed);
        static uint64_t previousDispatchCount = 0u;
        const uint64_t dispatchCount = g_eeGuestDispatchCount.load(std::memory_order_relaxed);
        static uint64_t previousThrowCount = 0u;
        const uint64_t throwCount = g_eeTransferThrowCount.load(std::memory_order_relaxed);
        static uint64_t previousSuspendCount = 0u;
        const uint64_t suspendCount = g_eeTransferSuspendCount.load(std::memory_order_relaxed);
        static uint64_t previousRunLoopCount = 0u;
        const uint64_t runLoopCount = g_eeRunLoopIterations.load(std::memory_order_relaxed);
        static uint64_t previousRunLoopResumeCount = 0u;
        const uint64_t runLoopResumeCount = g_eeRunLoopResumeCount.load(std::memory_order_relaxed);
        static uint64_t previousPumpCount = 0u;
        const uint64_t pumpCount = g_eeProcessPendingEventsCount.load(std::memory_order_relaxed);
        static uint64_t previousEnterGuestCount = 0u;
        const uint64_t enterGuestCount = g_eeEnterGuestCount.load(std::memory_order_relaxed);

        std::fprintf(stderr,
                     "[gfx] %.1f fps (backend %.0f%% of wall) over %u frames | transfers=%llu (%llu KiB, "
                     "%llu forced flushes) draws=%llu passes=%llu | resolves=%llu "
                     "(%llu Kpx) refreshes=%llu (%llu Kpx) | tex builds=%llu "
                     "(%llu Ktexels) bound-live=%llu feedback-copies=%llu | tex-inval draw=%llu write=%llu | "
                     "partial-updates=%llu (%llu Ktexels) | gif packets=%llu frontend=%.0f%% | "
                     "native-uploads=%llu upload-path=%.0f%%\n"
                     "  movie: get-picture=%llu (%.0f%%) write-frame=%llu (%.0f%%) "
                     "demux=%llu (%.0f%%) refused=%llu es-buffer-peak=%lluKiB | threads: demux=%u get-picture=%u"
                     " | ee-dispatches=%llu (%llu/frame) throws=%llu\n"
                     "  sched: loop=%llu (%.0f/s) resumes=%llu pumps=%llu enter-guest=%llu suspends=%llu"
                     " | presents native=%llu composed=%llu gpu-composed=%llu dual-circuit=%llu\n",
                     seconds > 0.0 ? static_cast<double>(interval) / seconds : 0.0,
                     seconds > 0.0 ? (backendSeconds / seconds) * 100.0 : 0.0,
                     interval,
                     static_cast<unsigned long long>(delta(stats.transfersBegun, previous.transfersBegun)),
                     static_cast<unsigned long long>(delta(stats.transferBytes, previous.transferBytes) / 1024u),
                     static_cast<unsigned long long>(delta(stats.transferFlushes, previous.transferFlushes)),
                     static_cast<unsigned long long>(delta(stats.drawCalls, previous.drawCalls)),
                     static_cast<unsigned long long>(delta(stats.renderPasses, previous.renderPasses)),
                     static_cast<unsigned long long>(delta(targetStats().colorResolves, previous.colorResolves)),
                     static_cast<unsigned long long>(delta(targetStats().resolvedPixels, previous.resolvedPixels) / 1024u),
                     static_cast<unsigned long long>(delta(targetStats().colorRefreshes, previous.colorRefreshes)),
                     static_cast<unsigned long long>(delta(targetStats().refreshedPixels, previous.refreshedPixels) / 1024u),
                     static_cast<unsigned long long>(delta(textureStats().builds, previous.textureBuilds)),
                     static_cast<unsigned long long>(delta(textureStats().texelsExpanded, previous.texelsExpanded) / 1024u),
                     static_cast<unsigned long long>(delta(stats.texturesFromLiveTargets, previous.texturesFromLiveTargets)),
                     static_cast<unsigned long long>(delta(stats.feedbackCopies, previous.feedbackCopies)),
                     static_cast<unsigned long long>(delta(textureStats().invalidationsFromDraw, previous.textureInvalidationsFromDraw)),
                     static_cast<unsigned long long>(delta(textureStats().invalidationsFromHostWrite, previous.textureInvalidationsFromHostWrite)),
                     static_cast<unsigned long long>(delta(textureStats().partialUpdates, previous.texturePartialUpdates)),
                     static_cast<unsigned long long>(delta(textureStats().texelsUpdated, previous.texelsUpdated) / 1024u),
                     static_cast<unsigned long long>(frontendPackets - previousFrontendPackets),
                     seconds > 0.0 ? (frontendSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(uploadCount - previousUploadCount),
                     seconds > 0.0 ? (uploadSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(getPictureCount - previousGetPictureCount),
                     seconds > 0.0 ? (getPictureSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(writeFrameCount - previousWriteFrameCount),
                     seconds > 0.0 ? (writeFrameSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(demuxCount - previousDemuxCount),
                     seconds > 0.0 ? (demuxSeconds / seconds) * 100.0 : 0.0,
                     static_cast<unsigned long long>(refusedCount - previousRefusedCount),
                     static_cast<unsigned long long>(
                         g_mpegPendingEsPeakBytes.load(std::memory_order_relaxed) / 1024u),
                     g_mpegDemuxThreadId.load(std::memory_order_relaxed),
                     g_mpegGetPictureThreadId.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(dispatchCount - previousDispatchCount),
                     static_cast<unsigned long long>((dispatchCount - previousDispatchCount) /
                                                     std::max<uint32_t>(1u, interval)),
                     static_cast<unsigned long long>(throwCount - previousThrowCount),
                     static_cast<unsigned long long>(runLoopCount - previousRunLoopCount),
                     seconds > 0.0 ? static_cast<double>(runLoopCount - previousRunLoopCount) / seconds : 0.0,
                     static_cast<unsigned long long>(runLoopResumeCount - previousRunLoopResumeCount),
                     static_cast<unsigned long long>(pumpCount - previousPumpCount),
                     static_cast<unsigned long long>(enterGuestCount - previousEnterGuestCount),
                     static_cast<unsigned long long>(suspendCount - previousSuspendCount),
                     static_cast<unsigned long long>(stats.nativePresents),
                     static_cast<unsigned long long>(stats.composedPresents),
                     static_cast<unsigned long long>(stats.gpuComposedPresents),
                     static_cast<unsigned long long>(stats.secondaryDisplayCircuits));

        previous = stats;
        previous.textureInvalidationsFromDraw = textureStats().invalidationsFromDraw;
        previous.textureInvalidationsFromHostWrite = textureStats().invalidationsFromHostWrite;
        previous.texturePartialUpdates = textureStats().partialUpdates;
        previous.texelsUpdated = textureStats().texelsUpdated;
        previousRefusedCount = refusedCount;
        previousDispatchCount = dispatchCount;
        previousThrowCount = throwCount;
        previousSuspendCount = suspendCount;
        previousRunLoopCount = runLoopCount;
        previousRunLoopResumeCount = runLoopResumeCount;
        previousPumpCount = pumpCount;
        previousEnterGuestCount = enterGuestCount;
        previousFrontendPackets = frontendPackets;
        previousUploadCount = uploadCount;
        previousGetPictureCount = getPictureCount;
        previousWriteFrameCount = writeFrameCount;
        previousDemuxCount = demuxCount;
        previous.colorResolves = targetStats().colorResolves;
        previous.resolvedPixels = targetStats().resolvedPixels;
        previous.colorRefreshes = targetStats().colorRefreshes;
        previous.refreshedPixels = targetStats().refreshedPixels;
        previous.textureBuilds = textureStats().builds;
        previous.texelsExpanded = textureStats().texelsExpanded;
    }

    const GsTargetCacheStats &targetStats() const {
        static const GsTargetCacheStats empty{};
        return targets ? targets->stats() : empty;
    }
    const GsTextureCacheStats &textureStats() const {
        static const GsTextureCacheStats empty{};
        return textures ? textures->stats() : empty;
    }

    std::shared_ptr<PreparedFrame> leasePresentation() {
        std::unique_lock lock(presentationPool->mutex);
        presentationPool->available.wait(lock, [&] {
            return presentationPool->closing || std::any_of(presentationPool->slots.begin(),
                presentationPool->slots.end(), [](const auto &slot) { return !slot.leased; });
        });
        if (presentationPool->closing)
            throw std::runtime_error("SDL GPU presentation cancelled");
        size_t selected = presentationPool->slots.size();
        for (size_t index = 0; index < presentationPool->slots.size(); ++index) {
            auto &slot = presentationPool->slots[index];
            if (slot.leased) continue;
            if (selected == presentationPool->slots.size()) selected = index;
            if ((!slot.prepared || SDL_QueryGPUFence(device.handle(), slot.prepared)) &&
                (!slot.displayed || SDL_QueryGPUFence(device.handle(), slot.displayed))) {
                selected = index;
                break;
            }
        }
        auto result = std::make_shared<PreparedFrame>(presentationPool, selected);
        presentationPool->slots[selected].leased = true;
        return result;
    }

    void retirePresentationFences(PresentationSlot &slot) {
        for (auto **fence : {&slot.prepared, &slot.displayed}) {
            if (!*fence) continue;
            if (!SDL_WaitForGPUFences(device.handle(), true, fence, 1))
                throw std::runtime_error(std::string("SDL_WaitForGPUFences(presentation): ") + SDL_GetError());
            SDL_ReleaseGPUFence(device.handle(), *fence);
            *fence = nullptr;
        }
    }

    // A leased texture cannot be overwritten until both its CPU ticket and
    // all GPU reads are finished. Live render targets are never leased.
    bool ensurePresentSource(uint32_t width, uint32_t height) {
        if (presentSource != nullptr && presentSourceWidth == width &&
            presentSourceHeight == height)
            return true;
        if (presentSource)
            SDL_ReleaseGPUTexture(device.handle(), presentSource);
        presentSource = nullptr;
        preparingSlot->texture = nullptr;

        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        presentSource = SDL_CreateGPUTexture(device.handle(), &info);
        if (!presentSource)
            return setError(std::string("SDL_CreateGPUTexture(present source): ") + SDL_GetError());
        presentSourceWidth = width;
        presentSourceHeight = height;
        preparingSlot->texture = presentSource;
        preparingSlot->width = width;
        preparingSlot->height = height;
        return true;
    }

    // GPU-side copy of a render target into the present source. No swapchain,
    // no wait, so this is safe to do while holding the backend lock.
    bool copyToPresentSource(SDL_GPUTexture *source, uint32_t width, uint32_t height) {
        if (!ensurePresentSource(width, height))
            return false;
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device.handle());
        if (!commands)
            return setError(std::string("SDL_AcquireGPUCommandBuffer(present copy): ") + SDL_GetError());
        SDL_GPUBlitInfo blit{};
        blit.source.texture = source;
        blit.source.w = width;
        blit.source.h = height;
        blit.destination.texture = presentSource;
        blit.destination.w = width;
        blit.destination.h = height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
        preparingSlot->prepared = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (!preparingSlot->prepared)
            return setError(std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(present copy): ") + SDL_GetError());
        return true;
    }

    // Scales the present source into the swapchain, letterboxed to preserve
    // aspect and snapped to an integer multiple when upscaling -- a fractional
    // nearest-neighbour scale duplicates some pixel columns and not others,
    // which shreds one-pixel font stems.
    //
    // Called with the backend lock released: acquiring a swapchain texture
    // waits for the display, and holding the lock across that wait blocks
    // every Submit and transfer the EE thread makes for the whole frame.
    bool showPresentSource(PresentationSlot &slot, std::string &error) {
        if (!window || !slot.texture)
            return true;
        const uint32_t sourceWidth = slot.width;
        const uint32_t sourceHeight = slot.height;
        const auto fail = [&](const char *context) {
            error = std::string(context) + SDL_GetError();
            return false;
        };
        if (slot.displayed) {
            if (!SDL_WaitForGPUFences(device.handle(), true, &slot.displayed, 1))
                return fail("SDL_WaitForGPUFences(previous display): ");
            SDL_ReleaseGPUFence(device.handle(), slot.displayed);
            slot.displayed = nullptr;
        }

        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device.handle());
        if (!commands)
            return fail("SDL_AcquireGPUCommandBuffer(present): ");

        SDL_GPUTexture *swapchain = nullptr;
        uint32_t swapchainWidth = 0u;
        uint32_t swapchainHeight = 0u;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain,
                                                   &swapchainWidth, &swapchainHeight)) {
            SDL_CancelGPUCommandBuffer(commands);
            return fail("SDL_WaitAndAcquireGPUSwapchainTexture: ");
        }
        if (!swapchain || swapchainWidth == 0u || swapchainHeight == 0u) {
            // Minimised or otherwise unavailable this frame; nothing to show.
            if (!SDL_SubmitGPUCommandBuffer(commands))
                return fail("SDL_SubmitGPUCommandBuffer(minimized): ");
            return true;
        }

        SDL_GPUTexture *source = slot.texture;
        const float fitScale = std::min(static_cast<float>(swapchainWidth) / static_cast<float>(sourceWidth),
                                        static_cast<float>(swapchainHeight) / static_cast<float>(sourceHeight));
        const float scaleFactor = fitScale >= 1.0f ? std::floor(fitScale) : fitScale;
        const uint32_t targetWidth =
            std::max<uint32_t>(1u, static_cast<uint32_t>(static_cast<float>(sourceWidth) * scaleFactor));
        const uint32_t targetHeight =
            std::max<uint32_t>(1u, static_cast<uint32_t>(static_cast<float>(sourceHeight) * scaleFactor));

        SDL_GPUBlitInfo blit{};
        blit.source.texture = source;
        blit.source.w = sourceWidth;
        blit.source.h = sourceHeight;
        blit.destination.texture = swapchain;
        blit.destination.x = (swapchainWidth - targetWidth) / 2u;
        blit.destination.y = (swapchainHeight - targetHeight) / 2u;
        blit.destination.w = targetWidth;
        blit.destination.h = targetHeight;
        // CLEAR, not LOAD: the letterbox bars have to be painted, and the
        // swapchain image is recycled.
        blit.load_op = SDL_GPU_LOADOP_CLEAR;
        blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        blit.filter = scaleFactor >= 1.0f ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);

        SDL_GPUFence *displayed = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        if (!displayed)
            return fail("SDL_SubmitGPUCommandBufferAndAcquireFence(present): ");
        slot.displayed = displayed;
        return true;
    }

    // Uploads a CPU-composed frame into the present source. Only used when the
    // frame could not be taken straight from a render target.
    bool showHostPixels(const PresentationFrame &frame) {
        if (frame.pixels.empty() || frame.width == 0u || frame.height == 0u)
            return true;
        if (!ensurePresentSource(frame.width, frame.height))
            return false;

        SDL_GPUDevice *handle = device.handle();
        if (presentUpload == nullptr || presentUploadWidth != frame.width ||
            presentUploadHeight != frame.height) {
            if (presentUpload)
                SDL_ReleaseGPUTexture(handle, presentUpload);
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.width = frame.width;
            info.height = frame.height;
            info.layer_count_or_depth = 1u;
            info.num_levels = 1u;
            info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            presentUpload = SDL_CreateGPUTexture(handle, &info);
            if (!presentUpload)
                return setError(std::string("SDL_CreateGPUTexture(present): ") + SDL_GetError());
            presentUploadWidth = frame.width;
            presentUploadHeight = frame.height;
        }

        const size_t bytes = static_cast<size_t>(frame.width) * frame.height * 4u;
        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<uint32_t>(bytes);
        SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(handle, &transferInfo);
        if (!upload)
            return setError(std::string("SDL_CreateGPUTransferBuffer(present): ") + SDL_GetError());
        void *mapped = SDL_MapGPUTransferBuffer(handle, upload, false);
        if (!mapped) {
            SDL_ReleaseGPUTransferBuffer(handle, upload);
            return setError(std::string("SDL_MapGPUTransferBuffer(present): ") + SDL_GetError());
        }
        std::memcpy(mapped, frame.pixels.data(), bytes);
        SDL_UnmapGPUTransferBuffer(handle, upload);

        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(handle);
        if (!commands) {
            SDL_ReleaseGPUTransferBuffer(handle, upload);
            return setError(std::string("SDL_AcquireGPUCommandBuffer(present upload): ") + SDL_GetError());
        }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        if (!copy) {
            SDL_CancelGPUCommandBuffer(commands);
            SDL_ReleaseGPUTransferBuffer(handle, upload);
            return setError(std::string("SDL_BeginGPUCopyPass(present): ") + SDL_GetError());
        }
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = upload;
        source.pixels_per_row = frame.width;
        source.rows_per_layer = frame.height;
        SDL_GPUTextureRegion destination{};
        destination.texture = presentUpload;
        destination.w = frame.width;
        destination.h = frame.height;
        destination.d = 1u;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
        SDL_EndGPUCopyPass(copy);
        const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
        SDL_ReleaseGPUTransferBuffer(handle, upload);
        if (!submitted)
            return setError(std::string("SDL_SubmitGPUCommandBuffer(present upload): ") + SDL_GetError());

        return copyToPresentSource(presentUpload, frame.width, frame.height);
    }

    // Both composition stages run on the producer; window presentation only
    // reads the completed slot and never touches mutable GS state.
    struct PresentStage {
        bool haveFrame = false;
        bool gsActive = false;
        bool native = false;
        bool download = false;
        bool dualCircuit = false;
        GsPmodeState pmode{};
        GsDisplaySetup circuit1{}, circuit2{};
        uint32_t bgcolor = 0u;
        std::vector<uint8_t> crt1;
        std::vector<uint8_t> crt2;
        PresentationFrame frame{};
    };

    bool acquireGpuComposition(const GSPresentationRequest &request, const GsPmodeState &pmode,
                               const GsDisplaySetup &circuit1, const GsDisplaySetup &circuit2,
                               bool halfHeightSource, uint32_t factor, PresentStage &stage) {
        const char *disabled = std::getenv("DQ8_GFX_DISABLE_GPU_COMPOSE");
        const char *fromMemory = std::getenv("DQ8_GFX_PRESENT_FROM_MEMORY");
        if ((disabled && *disabled && std::strcmp(disabled, "0") != 0) ||
            (fromMemory && *fromMemory) || !gsIsColorFramePsm(circuit1.psm) ||
            !gsIsColorFramePsm(circuit2.psm))
            return false;
        GsSurface *surface1 = targets->findColorSurface(circuit1.fbp << 5u, circuit1.psm);
        GsSurface *surface2 = targets->findColorSurface(circuit2.fbp << 5u, circuit2.psm);
        const auto valid = [factor](const GsSurface *surface) {
            return surface && !surface->undefined && surface->scale == factor &&
                   surface->needsUpload.empty() && surface->cpuPatches.empty();
        };
        // Mirror readCircuit's valid texture view without changing ownership or
        // importing CPU pixels. Pending transfers and aliases use its fallback.
        if (!valid(surface1) || !valid(surface2))
            return false;
        const uint32_t width = std::max(circuit1.width, circuit2.width) * factor;
        const uint32_t height = std::max(circuit1.height, circuit2.height) * factor;
        if (!ensurePresentSource(width, height))
            return false;
        GsDisplayUniforms uniforms{};
        uniforms.control[0] = factor;
        uniforms.control[1] = halfHeightSource;
        uniforms.control[2] = pmode.useFixedAlpha;
        uniforms.control[3] = pmode.blendWithBackground;
        const auto configure = [factor](uint32_t (&out)[4], const GsDisplaySetup &circuit) {
            out[0] = circuit.originX;
            out[1] = circuit.originY;
            out[2] = circuit.width * factor;
            out[3] = circuit.height * factor;
        };
        configure(uniforms.circuit1, circuit1);
        configure(uniforms.circuit2, circuit2);
        uniforms.format[0] = circuit1.psm;
        uniforms.format[1] = circuit2.psm;
        uniforms.format[2] = pmode.fixedAlpha;
        uniforms.format[3] = static_cast<uint32_t>(request.bgcolor);
        std::string error;
        if (!device.composeDisplay(surface1->texture, surface2->texture, presentSource,
                                   width, height, uniforms, preparingSlot->prepared, error)) {
            setError(std::move(error));
            return false;
        }
        PresentationFrame &frame = stage.frame;
        frame.width = width;
        frame.height = height;
        frame.displayFbp = frame.sourceFbp = circuit1.fbp;
        if (!window || gsScreenshotEnabled()) {
            // The display consumer can read its immutable slot after later GS
            // work has started, without observing either live display circuit.
            stage.download = true;
            frame.mode = GSPresentationMode::HostPixels;
            frame.rowPitchBytes = width * 4u;
        } else {
            frame.mode = GSPresentationMode::BackendNative;
            frame.rowPitchBytes = 0u;
            ++stats.nativePresents;
        }
        ++stats.gpuComposedPresents;
        stage.native = true;
        stage.haveFrame = true;
        return true;
    }

    void acquirePresentation(const GSPresentationRequest &request, PresentStage &stage) {
        PresentationFrame &frame = stage.frame;
        ++stats.presents;
        if (!flushDraws() || !device.valid())
            return;
        stage.gsActive = stats.drawCalls != previousPresentDraws ||
                         stats.transferBytes != previousPresentTransfers;
        previousPresentDraws = stats.drawCalls;
        previousPresentTransfers = stats.transferBytes;

        const GsPmodeState pmode = gsDecodePmode(request.pmode);
        const GsSmode2State smode2 = gsDecodeSmode2(request.smode2);
        const bool halfHeightSource = smode2.interlaced && smode2.frameMode;

        const GsDisplaySetup circuit1 =
            gsDecodeDisplay(request.dispfb1, request.display1, pmode.enableCircuit1);
        const GsDisplaySetup circuit2 =
            gsDecodeDisplay(request.dispfb2, request.display2, pmode.enableCircuit2);
        if (!circuit1.valid && !circuit2.valid)
            return;
        if (circuit1.valid && circuit2.valid)
            ++stats.secondaryDisplayCircuits;

        const uint32_t factor = std::max<uint32_t>(scale, 1u);
        frame.mode = GSPresentationMode::HostPixels;

        if (circuit1.valid && circuit2.valid &&
            acquireGpuComposition(request, pmode, circuit1, circuit2, halfHeightSource, factor, stage))
            return;

        // The path that makes owning the window worth it: when a single read
        // circuit is showing a buffer a render target still holds, the frame is
        // already on the GPU at full scale and goes straight to the swapchain.
        // No readback, no CPU compose, no upload.
        // Screenshots read the CPU-composed frame, which this path skips.
        if (window != nullptr && !gsScreenshotEnabled() && circuit1.valid != circuit2.valid) {
            const GsDisplaySetup &only = circuit1.valid ? circuit1 : circuit2;
            GsSurface *surface = targets->findColorSurface(only.fbp << 5u, only.psm);
            if (surface != nullptr && !surface->undefined && surface->scale == factor &&
                surface->needsUpload.empty() && surface->cpuPatches.empty() &&
                only.originX == 0u && only.originY == 0u &&
                !halfHeightSource) {
                frame.width = only.width * factor;
                frame.height = only.height * factor;
                frame.rowPitchBytes = 0u;
                frame.displayFbp = only.fbp;
                frame.sourceFbp = only.fbp;
                frame.mode = GSPresentationMode::BackendNative;
                ++stats.nativePresents;
                copyToPresentSource(surface->texture, surface->width * factor,
                                    surface->height * factor);
                stage.native = true;
                stage.haveFrame = true;
                return;
            }
        }

        if (circuit1.valid && circuit2.valid) {
            uint32_t fbp1 = 0u, fbp2 = 0u;
            bool preferred1 = false, preferred2 = false;
            // Neither circuit takes the preferred-source override when both are
            // live; it exists to rescue a single stale DISPFB.
            readCircuit(request, circuit1, halfHeightSource, false, factor, stage.crt1, fbp1, preferred1);
            readCircuit(request, circuit2, halfHeightSource, false, factor, stage.crt2, fbp2, preferred2);

            frame.width = std::max(circuit1.width, circuit2.width) * factor;
            frame.height = std::max(circuit1.height, circuit2.height) * factor;
            frame.rowPitchBytes = frame.width * 4u;
            frame.displayFbp = circuit1.fbp;
            frame.sourceFbp = fbp1;
        } else {
            const GsDisplaySetup &primary = circuit1.valid ? circuit1 : circuit2;
            uint32_t sourceFbp = 0u;
            bool usedPreferred = false;
            readCircuit(request, primary, halfHeightSource, true, factor, frame.pixels,
                        sourceFbp, usedPreferred);
            frame.width = primary.width * factor;
            frame.height = primary.height * factor;
            frame.rowPitchBytes = frame.width * 4u;
            frame.displayFbp = primary.fbp;
            frame.sourceFbp = sourceFbp;
            frame.usedPreferred = usedPreferred;
        }

        stage.pmode = pmode;
        stage.circuit1 = circuit1;
        stage.circuit2 = circuit2;
        stage.bgcolor = request.bgcolor;
        stage.dualCircuit = circuit1.valid && circuit2.valid;
        stage.haveFrame = true;
    }

    void updatePerformanceTitle(bool gsActive) {
        static const bool enabled = [] {
            const char *value = std::getenv("DQ8_GFX_SHOW_FPS");
            return value && std::strcmp(value, "0") != 0;
        }();
        if (!enabled || !window)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (titleSampleStart == std::chrono::steady_clock::time_point{}) {
            titleSampleStart = now;
            titleCompletedStart = completedRenderCounter ? completedRenderCounter() : 0u;
            return;
        }
        ++titlePresents;
        titleActiveFrames += gsActive ? 1u : 0u;
        const double seconds = std::chrono::duration<double>(now - titleSampleStart).count();
        if (seconds < 2.0)
            return;
        char title[128];
        if (completedRenderCounter) {
            const uint64_t completed = completedRenderCounter();
            const double renders = completed >= titleCompletedStart
                ? (completed - titleCompletedStart) / seconds : 0.0;
            std::snprintf(title, sizeof(title), "DQ8Recomp | %.1f render FPS | SDL GPU %.1f presents/s",
                          renders, titlePresents / seconds);
            titleCompletedStart = completed;
        } else {
            std::snprintf(title, sizeof(title), "DQ8Recomp | SDL GPU: %.1f presents/s (%.1f active/s)",
                          titlePresents / seconds, titleActiveFrames / seconds);
        }
        SDL_SetWindowTitle(window, title);
        std::fprintf(stderr, "[present] %.2f fps, %.2f GS-active/s over %.3f s\n",
                     titlePresents / seconds, titleActiveFrames / seconds, seconds);
        titleSampleStart = now;
        titlePresents = titleActiveFrames = 0u;
    }

    void capturePresentedFrame(const PresentationSlot &slot, uint64_t tick) {
        static const char *path = std::getenv("DQ8_GFX_NATIVE_CAPTURE");
        static const bool series = std::getenv("DQ8_GFX_NATIVE_CAPTURE_SERIES") != nullptr;
        if (!path || !*path || (nativeCaptureAttempted && !series) || !slot.texture)
            return;
        const std::string trigger = std::string(path) + ".trigger";
        FILE *request = std::fopen(trigger.c_str(), "rb");
        if (!request)
            return;
        std::fclose(request);
        nativeCaptureAttempted = true;
        std::string output = path;
        if (series) {
            // Consume each request before readback so it cannot capture every frame.
            if (std::remove(trigger.c_str()) != 0)
                return;
            output += "." + std::to_string(++nativeCaptureNumber) + ".bmp";
        }

        // Read the leased snapshot that was actually displayed, after submission.
        // A one-shot request keeps readback stalls outside steady FPS measurements.
        std::vector<uint8_t> pixels;
        std::string error;
        if (device.downloadTexture(slot.texture, slot.width, slot.height, pixels, error)) {
            std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
                SDL_CreateSurfaceFrom(slot.width, slot.height, SDL_PIXELFORMAT_RGBA32,
                                      pixels.data(), slot.width * 4u), SDL_DestroySurface);
            if (surface && SDL_SaveBMP(surface.get(), output.c_str())) {
                std::fprintf(stderr, "[native-capture] %s %ux%u tick=%llu completed=%llu\n",
                             output.c_str(), slot.width, slot.height,
                             static_cast<unsigned long long>(tick),
                             static_cast<unsigned long long>(completedRenderCounter ? completedRenderCounter() : 0u));
                return;
            }
            error = SDL_GetError();
        }
        std::fprintf(stderr, "[native-capture] failed: %s\n", error.c_str());
    }

    void finishPresentation(PresentStage &stage) {
        PresentationFrame &frame = stage.frame;
        if (!stage.haveFrame || stage.native)
            return;

        if (stage.dualCircuit) {
            const GsPmodeState &pmode = stage.pmode;
            const GsDisplaySetup &circuit1 = stage.circuit1;
            const GsDisplaySetup &circuit2 = stage.circuit2;
            const uint32_t factor = std::max<uint32_t>(scale, 1u);

            // Background, then circuit 2, then circuit 1 blended over the top.
            const uint8_t background[3] = {static_cast<uint8_t>(stage.bgcolor),
                                           static_cast<uint8_t>(stage.bgcolor >> 8u),
                                           static_cast<uint8_t>(stage.bgcolor >> 16u)};
            frame.pixels.assign(static_cast<size_t>(frame.rowPitchBytes) * frame.height, 0u);
            for (uint32_t y = 0u; y < frame.height; ++y) {
                uint8_t *row = frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitchBytes;
                for (uint32_t x = 0u; x < frame.width; ++x) {
                    row[x * 4u] = background[0];
                    row[x * 4u + 1u] = background[1];
                    row[x * 4u + 2u] = background[2];
                    row[x * 4u + 3u] = pmode.fixedAlpha;
                }
            }

            const uint32_t width2 = circuit2.width * factor;
            const uint32_t height2 = circuit2.height * factor;
            if (!pmode.blendWithBackground) {
                for (uint32_t y = 0u; y < std::min(height2, frame.height); ++y) {
                    std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * frame.rowPitchBytes,
                                stage.crt2.data() + static_cast<size_t>(y) * width2 * 4u,
                                std::min(width2, frame.width) * 4u);
                }
            }

            const uint32_t width1 = circuit1.width * factor;
            const uint32_t height1 = circuit1.height * factor;
            for (uint32_t y = 0u; y < std::min(height1, frame.height); ++y) {
                const uint8_t *source = stage.crt1.data() + static_cast<size_t>(y) * width1 * 4u;
                uint8_t *destination = frame.pixels.data() +
                                       static_cast<size_t>(y) * frame.rowPitchBytes;
                for (uint32_t x = 0u; x < std::min(width1, frame.width); ++x) {
                    // MMOD picks ALP over circuit 1's own alpha; the doubling
                    // is the GS's /128 alpha convention again.
                    const uint32_t blendFactor =
                        pmode.useFixedAlpha
                            ? pmode.fixedAlpha
                            : std::min<uint32_t>(255u, static_cast<uint32_t>(source[x * 4u + 3u]) * 2u);
                    for (int channel = 0; channel < 3; ++channel) {
                        destination[x * 4u + channel] = blendCircuitChannel(
                            source[x * 4u + channel], destination[x * 4u + channel], blendFactor);
                    }
                    if (!pmode.alphaFromCircuit2)
                        destination[x * 4u + 3u] = source[x * 4u + 3u];
                }
            }
        }

        // The screen has no alpha channel; everything above needed it live.
        for (size_t offset = 3u; offset < frame.pixels.size(); offset += 4u)
            frame.pixels[offset] = 0xffu;

        // Composed on the CPU, but this backend still owns the window, so it
        // shows the frame itself rather than handing it back.
        if (window != nullptr) {
            ++stats.composedPresents;
            showHostPixels(frame);
            // Normally the pixels are dead once shown. While screenshots are on
            // the runtime wants them too, and it is this path -- not the native
            // one -- that it latches from.
            if (!gsScreenshotEnabled()) {
                frame.pixels.clear();
                frame.rowPitchBytes = 0u;
                frame.mode = GSPresentationMode::BackendNative;
            }
        }
    }

    bool pumpEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                windowCloseRequested = true;
            pad.handleEvent(event);
        }
        pad.poll(SDL_GetKeyboardState(nullptr), SDL_GetKeyboardFocus() == window);
        return !windowCloseRequested;
    }

    bool createDummyTexture() {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = 1u;
        info.height = 1u;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        dummyTexture = SDL_CreateGPUTexture(device.handle(), &info);
        if (!dummyTexture)
            return setError(std::string("SDL_CreateGPUTexture(placeholder): ") + SDL_GetError());
        return true;
    }
};

SdlGpuBackend::SdlGpuBackend() : m_impl(std::make_unique<Impl>()) {}

SdlGpuBackend::~SdlGpuBackend() {
    CancelPreparedPresentations();
    if (m_impl->device.valid()) {
        for (auto &slot : m_impl->presentationPool->slots) {
            for (auto *fence : {slot.prepared, slot.displayed}) {
                if (!fence) continue;
                SDL_WaitForGPUFences(m_impl->device.handle(), true, &fence, 1);
                SDL_ReleaseGPUFence(m_impl->device.handle(), fence);
            }
            if (slot.texture) SDL_ReleaseGPUTexture(m_impl->device.handle(), slot.texture);
            slot.texture = nullptr;
            slot.prepared = slot.displayed = nullptr;
        }
        m_impl->textures.reset();
        m_impl->targets.reset();
        if (m_impl->vertexBuffer)
            SDL_ReleaseGPUBuffer(m_impl->device.handle(), m_impl->vertexBuffer);
        if (m_impl->vertexUpload)
            SDL_ReleaseGPUTransferBuffer(m_impl->device.handle(), m_impl->vertexUpload);
        if (m_impl->dummyTexture)
            SDL_ReleaseGPUTexture(m_impl->device.handle(), m_impl->dummyTexture);
        for (const auto &feedback : m_impl->feedbackTextures)
            SDL_ReleaseGPUTexture(m_impl->device.handle(), feedback.texture);
        if (m_impl->presentUpload)
            SDL_ReleaseGPUTexture(m_impl->device.handle(), m_impl->presentUpload);
        if (m_impl->window) {
            SDL_ReleaseWindowFromGPUDevice(m_impl->device.handle(), m_impl->window);
            SDL_DestroyWindow(m_impl->window);
            m_impl->window = nullptr;
        }
    }
    m_impl->device.destroy();
}

void SdlGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->vram.attach(vram, vramSize);
    m_impl->transferCommand = {};
    m_impl->patchingHostWrite = false;
    m_impl->transfer = std::make_unique<GsTransferEngine>(m_impl->vram);
    m_impl->transfer->setResolveHook([impl = m_impl.get()](const GsPageSet &pages) {
        impl->settleFor(pages);
    });
    m_impl->transfer->setInvalidateHook([impl = m_impl.get()](const GsPageSet &pages) {
        impl->invalidatePages(pages);
    });
    if (!m_impl->targets)
        m_impl->targets = std::make_unique<GsTargetCache>(m_impl->device, m_impl->vram);
    if (!m_impl->textures)
        m_impl->textures = std::make_unique<GsTextureCache>(m_impl->device, m_impl->vram,
                                                            *m_impl->targets);
    m_impl->targets->setScale(m_impl->scale);
    m_impl->targets->reset();
    m_impl->textures->reset();
}

void SdlGpuBackend::Reset() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->batches.clear();
    m_impl->vertices.clear();
    m_impl->transferCommand = {};
    m_impl->patchingHostWrite = false;
    if (m_impl->transfer)
        m_impl->transfer->reset();
    if (m_impl->textures)
        m_impl->textures->reset();
    if (m_impl->targets)
        m_impl->targets->reset();
}

void SdlGpuBackend::Submit(const GSPrimitiveBatch &batch) {
    std::lock_guard lock(m_impl->mutex);
    Impl::ScopedTimer timer(m_impl->backendNanos);
    m_impl->submit(batch);
}

void SdlGpuBackend::BeginTransfer(const GSTransferCommand &command) {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->transfer)
        return;
    Impl::ScopedTimer timer(m_impl->backendNanos);
    ++m_impl->stats.transfersBegun;
    if (command.direction == 0u || command.direction == 2u) {
        GsPageSet pages;
        gsMarkPages(pages, command.bitbltbuf.dbp,
                    std::max<uint32_t>(command.bitbltbuf.dbw, 1u),
                    command.bitbltbuf.dpsm,
                    command.trxreg.rrw, command.trxreg.rrh,
                    command.trxpos.dsax, command.trxpos.dsay);
        if (!m_impl->batches.empty())
            ++m_impl->stats.transferFlushes;
        if (!m_impl->flushDraws()) return;
        if (command.direction != 0u || command.bitbltbuf.dpsm != GS_PSM_CT32 ||
            !m_impl->targets->canPatchHostWrite(pages)) {
            if (!m_impl->settleFor(pages, true)) return;
        }
    }
    m_impl->transferCommand = command;
    m_impl->transfer->begin(command);
}

void SdlGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes) {
    std::lock_guard lock(m_impl->mutex);
    Impl::ScopedTimer timer(m_impl->backendNanos);
    m_impl->stats.transferBytes += sizeBytes;
    if (!m_impl->transfer) return;
    const auto before = m_impl->transfer->snapshot();
    const auto &command = m_impl->transferCommand;
    GsPageSet pages;
    if (before.direction == 0u && before.totalPixels != 0u &&
        command.bitbltbuf.dpsm == GS_PSM_CT32 && data && sizeBytes != 0u) {
        gsMarkPages(pages, command.bitbltbuf.dbp,
                    std::max<uint32_t>(command.bitbltbuf.dbw, 1u), GS_PSM_CT32,
                    command.trxreg.rrw, command.trxreg.rrh,
                    command.trxpos.dsax, command.trxpos.dsay);
        // Draws can occur between IMAGE fragments, so recheck ownership at
        // each payload before any CPU bytes change.
        if (!m_impl->flushDraws()) return;
        m_impl->patchingHostWrite = m_impl->targets->canPatchHostWrite(pages);
        if (!m_impl->patchingHostWrite && !m_impl->settleFor(pages, true)) return;
    }
    m_impl->transfer->upload(data, sizeBytes);
    if (m_impl->patchingHostWrite) {
        const auto after = m_impl->transfer->snapshot();
        if (after.copiedPixels > before.copiedPixels)
            m_impl->targets->patchHostWrite(pages, command.bitbltbuf.dbp,
                std::max<uint32_t>(command.bitbltbuf.dbw, 1u),
                command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw,
                before.copiedPixels, after.copiedPixels);
        m_impl->patchingHostWrite = false;
    }
}

void SdlGpuBackend::Flush() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->flushDraws();
}

void SdlGpuBackend::TextureFlush() {
    // The texture cache is keyed on local-memory ranges and invalidated by the
    // transfers that write them, so an explicit TEXFLUSH has nothing to do.
}

void SdlGpuBackend::Sync(GSSyncReason reason) {
    std::lock_guard lock(m_impl->mutex);
    switch (reason) {
    case GSSyncReason::LocalToHost:
    case GSSyncReason::DebugReadback:
    case GSSyncReason::Reset:
        m_impl->settleAll();
        break;
    case GSSyncReason::Finish:
    case GSSyncReason::Presentation:
    default:
        m_impl->flushDraws();
        break;
    }
}

PresentationFrame SdlGpuBackend::Present(const GSPresentationRequest &request) {
    return DisplayPreparedPresentation(PreparePresentation(request));
}

GSPresentationTicket SdlGpuBackend::PreparePresentation(const GSPresentationRequest &request) {
    auto prepared = m_impl->leasePresentation();
    prepared->sourceVsyncTick = request.vsyncTick;
    auto &slot = prepared->pool->slots[prepared->slot];
    Impl::PresentStage stage;
    {
        std::lock_guard lock(m_impl->mutex);
        Impl::ScopedTimer timer(m_impl->backendNanos);
        m_impl->retirePresentationFences(slot);
        m_impl->preparingSlot = &slot;
        struct ClearPreparingSlot {
            Impl &impl;
            ~ClearPreparingSlot() { impl.preparingSlot = nullptr; }
        } clearPreparingSlot{*m_impl};
        m_impl->presentSource = slot.texture;
        m_impl->presentSourceWidth = slot.width;
        m_impl->presentSourceHeight = slot.height;
        m_impl->reportStats();
        m_impl->lastError.clear();
        m_impl->acquirePresentation(request, stage);
        m_impl->finishPresentation(stage);
        if (!m_impl->lastError.empty())
            throw std::runtime_error(m_impl->lastError);
    }
    prepared->haveFrame = stage.haveFrame;
    prepared->gsActive = stage.gsActive;
    prepared->download = stage.download;
    prepared->frame = std::move(stage.frame);
    return prepared;
}

PresentationFrame SdlGpuBackend::DisplayPreparedPresentation(const GSPresentationTicket &ticket) {
    const auto *prepared = dynamic_cast<const Impl::PreparedFrame *>(ticket.get());
    if (!prepared || prepared->pool != m_impl->presentationPool)
        throw std::invalid_argument("SDL GPU presentation ticket belongs to another backend");
    Impl::DisplayTimer timer(m_impl->displayNanos);
    {
        std::lock_guard lock(prepared->pool->mutex);
        if (prepared->pool->closing)
            throw std::runtime_error("SDL GPU presentation cancelled");
    }
    auto &slot = prepared->pool->slots[prepared->slot];
    if (prepared->haveFrame && m_impl->window != nullptr) {
        if (slot.prepared && !SDL_WaitForGPUFences(m_impl->device.handle(), true, &slot.prepared, 1))
            throw std::runtime_error(std::string("SDL_WaitForGPUFences(snapshot): ") + SDL_GetError());
        std::string error;
        if (!m_impl->showPresentSource(slot, error))
            throw std::runtime_error(error);
        m_impl->updatePerformanceTitle(prepared->gsActive);
    }
    PresentationFrame frame = prepared->frame;
    if (prepared->download) {
        std::string error;
        if (!m_impl->device.downloadTexture(slot.texture, slot.width, slot.height, frame.pixels, error))
            throw std::runtime_error(error);
    }
    if (prepared->haveFrame)
        m_impl->capturePresentedFrame(slot, prepared->sourceVsyncTick);
    return frame;
}

void SdlGpuBackend::CancelPreparedPresentations() noexcept {
    std::lock_guard lock(m_impl->presentationPool->mutex);
    m_impl->presentationPool->closing = true;
    m_impl->presentationPool->available.notify_all();
}

bool SdlGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba) {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->device.valid() || !gsIsColorFramePsm(context.frame.psm))
        return false;
    if (!m_impl->flushDraws())
        return false;

    std::string error;
    GsSurface *surface = m_impl->targets->acquire(
        context.frame.fbp << 5u, context.frame.fbw, context.frame.psm, false,
        static_cast<uint32_t>(context.scissor.x1) + 1u,
        static_cast<uint32_t>(context.scissor.y1) + 1u, error);
    if (!surface || !m_impl->targets->prepareColorView(*surface, error)) {
        m_impl->setError(std::move(error));
        return false;
    }
    return m_impl->clearSurface(*surface,
                                static_cast<float>(rgba & 0xffu) / 255.0f,
                                static_cast<float>((rgba >> 8u) & 0xffu) / 255.0f,
                                static_cast<float>((rgba >> 16u) & 0xffu) / 255.0f,
                                static_cast<float>((rgba >> 24u) & 0xffu) / 255.0f);
}

uint32_t SdlGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->transfer)
        return 0u;
    return m_impl->transfer->consumeLocalToHost(dst, maxBytes);
}

uint32_t SdlGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw,
                                 uint32_t x, uint32_t y) const {
    std::lock_guard lock(m_impl->mutex);
    GsPageSet pages;
    gsMarkPages(pages, base, bw, psm, x + 1u, y + 1u);
    m_impl->settleFor(pages);
    return m_impl->vram.read(psm, base, bw, x, y);
}

void SdlGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw,
                              uint32_t x, uint32_t y, uint32_t value) {
    std::lock_guard lock(m_impl->mutex);
    GsPageSet pages;
    gsMarkPages(pages, base, bw, psm, x + 1u, y + 1u);
    m_impl->settleFor(pages, true);
    m_impl->vram.write(psm, base, bw, x, y, value);
    m_impl->invalidatePages(pages);
}

void SdlGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const {
    std::lock_guard lock(m_impl->mutex);
    m_impl->settleAll();
    out.assign(m_impl->vram.data(), m_impl->vram.data() + m_impl->vram.size());
}

GSTransferSnapshot SdlGpuBackend::GetTransferSnapshot() const {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->transfer)
        return {};
    return m_impl->transfer->snapshot();
}

void SdlGpuBackend::setCompletedRenderCounter(CompletedRenderCounter counter) {
    m_impl->completedRenderCounter = counter;
    m_impl->titleSampleStart = {};
    m_impl->titlePresents = m_impl->titleActiveFrames = 0u;
}

void SdlGpuBackend::setResolutionScale(uint32_t scale) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->flushDraws();
    std::string error;
    if (m_impl->targets) {
        m_impl->targets->resolveAll(error);
        m_impl->targets->setScale(scale);
        m_impl->scale = m_impl->targets->scale();
    } else {
        m_impl->scale = std::clamp<uint32_t>(scale, 1u, 8u);
    }
}

uint32_t SdlGpuBackend::resolutionScale() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->scale;
}

bool SdlGpuBackend::openWindow(const char *title, uint32_t width, uint32_t height,
                               std::string &error) {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->device.valid()) {
        error = "SDL GPU device is not available";
        return false;
    }
    if (m_impl->window != nullptr)
        return true;

    m_impl->window = SDL_CreateWindow(title, static_cast<int>(width),
                                      static_cast<int>(height), SDL_WINDOW_RESIZABLE);
    if (!m_impl->window) {
        error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(m_impl->device.handle(), m_impl->window)) {
        error = std::string("SDL_ClaimWindowForGPUDevice: ") + SDL_GetError();
        SDL_DestroyWindow(m_impl->window);
        m_impl->window = nullptr;
        return false;
    }
    SDL_ShowWindow(m_impl->window);
    SDL_RaiseWindow(m_impl->window);
    std::string inputError;
    if (!m_impl->pad.initialize(inputError))
        std::fprintf(stderr, "[pad] %s; keyboard remains available\n", inputError.c_str());
    return true;
}

bool SdlGpuBackend::hasWindow() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->window != nullptr;
}

bool SdlGpuBackend::pumpEvents() {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->window)
        return true;
    return m_impl->pumpEvents();
}

void SdlGpuBackend::hostPadState(uint32_t &held, uint32_t &pressed, uint32_t &sticks) const {
    std::lock_guard lock(m_impl->mutex);
    m_impl->pad.read(held, pressed, sticks);
}

bool SdlGpuBackend::deviceReady() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->device.valid();
}

std::string SdlGpuBackend::driverName() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->device.driverName();
}

std::string SdlGpuBackend::videoDriverName() const {
    const char *driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr ? driver : "none";
}

std::string SdlGpuBackend::lastError() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->lastError;
}

SdlGpuStats SdlGpuBackend::stats() const {
    std::lock_guard lock(m_impl->mutex);
    SdlGpuStats result = m_impl->stats;
    if (m_impl->targets) {
        const GsTargetCacheStats &cache = m_impl->targets->stats();
        result.colorResolves = cache.colorResolves;
        result.colorRefreshes = cache.colorRefreshes;
        result.resolvedPixels = cache.resolvedPixels;
        result.refreshedPixels = cache.refreshedPixels;
        result.surfacesCreated = cache.surfacesCreated;
    }
    if (m_impl->textures) {
        const GsTextureCacheStats &cache = m_impl->textures->stats();
        result.textureLookups = cache.lookups;
        result.textureHits = cache.hits;
        result.textureBuilds = cache.builds;
        result.textureInvalidations = cache.invalidations;
        result.textureEvictions = cache.evictions;
        result.texelsExpanded = cache.texelsExpanded;
        result.texturesFromRenderTargets = cache.renderTargetSources;
    }
    result.pipelinesCreated = m_impl->device.pipelineCount();
    return result;
}

void SdlGpuBackend::resetStats() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->stats = {};
    if (m_impl->targets)
        m_impl->targets->resetStats();
    if (m_impl->textures)
        m_impl->textures->resetStats();
}

std::unique_ptr<SdlGpuBackend> createSdlGpuBackend(std::string &error) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        error = std::string("SDL_InitSubSystem(video): ") + SDL_GetError();
        return nullptr;
    }
    auto backend = std::make_unique<SdlGpuBackend>();
    if (!backend->m_impl->device.create(error))
        return nullptr;
    if (!backend->m_impl->createDummyTexture()) {
        error = backend->m_impl->lastError;
        return nullptr;
    }
    backend->m_impl->targets = std::make_unique<GsTargetCache>(
        backend->m_impl->device, backend->m_impl->vram);
    backend->m_impl->textures = std::make_unique<GsTextureCache>(
        backend->m_impl->device, backend->m_impl->vram, *backend->m_impl->targets);
    return backend;
}

} // namespace dq8::gfx
