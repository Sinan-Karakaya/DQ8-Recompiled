// SDL3 GPU implementation of ps2xRuntime's GSRasterBackend.
//
// Draws through the host graphics pipeline into render targets held as GPU
// textures at a configurable resolution scale, with GS local memory as the
// backing store rather than the drawing destination.
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>
#include <string>

namespace dq8::gfx {

struct SdlGpuStats {
    uint64_t presents = 0u;
    // Frames shown straight from a render target, with no readback or CPU
    // compose, versus ones that had to be composed on the CPU first.
    uint64_t nativePresents = 0u;
    uint64_t composedPresents = 0u;
    uint64_t gpuComposedPresents = 0u;
    // Host->local transfers, which is how a movie frame arrives: DQ8 uploads
    // one per 16x16 tile, ~900 per frame.
    uint64_t transfersBegun = 0u;
    uint64_t transferBytes = 0u;
    uint64_t transferFlushes = 0u;

    uint64_t primitivesSubmitted = 0u;
    uint64_t primitivesDrawn = 0u;
    uint64_t trianglesDrawn = 0u;
    uint64_t batches = 0u;
    uint64_t renderPasses = 0u;
    uint64_t drawCalls = 0u;

    // Places where GS semantics are approximated rather than reproduced. Each
    // one is a known gap; a nonzero count says how much a scene depends on it.
    uint64_t inexactBlends = 0u;
    // Blends whose source factor exceeds 1.0 because the GS divides alpha by
    // 128, not 255. A fixed-point colour attachment clamps blend factors to
    // [0,1], so these saturate; a floating-point render target would carry them.
    uint64_t saturatedBlendFactors = 0u;
    // Blends weighted by destination alpha. The same /128 scale applies, but
    // the value lives in the render target where the shader cannot reach it,
    // so the factor comes out at 128/255 of what the GS would use.
    uint64_t destinationAlphaFactors = 0u;
    uint64_t partialChannelMasks = 0u;
    uint64_t destinationAlphaTests = 0u;
    uint64_t alphaFailModes = 0u;
    uint64_t disabledColorClamps = 0u;
    uint64_t texturedPrimitives = 0u;
    uint64_t untranslatedTextures = 0u;
    uint64_t secondaryDisplayCircuits = 0u;

    uint64_t colorResolves = 0u;
    uint64_t colorRefreshes = 0u;
    uint64_t resolvedPixels = 0u;
    uint64_t refreshedPixels = 0u;
    uint64_t surfacesCreated = 0u;
    uint64_t pipelinesCreated = 0u;

    uint64_t textureLookups = 0u;
    uint64_t textureHits = 0u;
    uint64_t textureBuilds = 0u;
    uint64_t textureInvalidations = 0u;
    uint64_t textureInvalidationsFromDraw = 0u;
    uint64_t textureInvalidationsFromHostWrite = 0u;
    uint64_t textureEvictions = 0u;
    uint64_t texelsExpanded = 0u;
    uint64_t texturePartialUpdates = 0u;
    uint64_t texelsUpdated = 0u;
    uint64_t texturesFromRenderTargets = 0u;
    // Textured draws that bound a live render target instead of rebuilding a
    // native-resolution copy of it.
    uint64_t texturesFromLiveTargets = 0u;
    // Draws that sample their own target use a snapshot or local-memory copy.
    uint64_t textureFeedbackHazards = 0u;
    // Ordered target snapshots, including padded samples of other live targets.
    uint64_t feedbackCopies = 0u;

    double textureHitRate() const {
        return textureLookups == 0u ? 0.0
                                    : static_cast<double>(textureHits) /
                                          static_cast<double>(textureLookups);
    }

    double averageTrianglesPerDraw() const {
        return drawCalls == 0u ? 0.0
                               : static_cast<double>(trianglesDrawn) /
                                     static_cast<double>(drawCalls);
    }
};

class SdlGpuBackend final : public GSRasterBackend {
public:
    SdlGpuBackend();
    ~SdlGpuBackend() override;

    SdlGpuBackend(const SdlGpuBackend &) = delete;
    SdlGpuBackend &operator=(const SdlGpuBackend &) = delete;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool SupportsPreparedPresentation() const override { return true; }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override;
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override;
    void CancelPreparedPresentations() noexcept override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Internal render resolution, as a multiple of the GS's own. Changing it
    // discards every render target, so it is set once at startup in practice.
    void setResolutionScale(uint32_t scale);
    uint32_t resolutionScale() const;

    // Opens a window and claims it for the GPU device. Present() then composes
    // straight into the swapchain and reports BackendNative, so the frame never
    // makes a round trip through host memory.
    bool openWindow(const char *title, uint32_t width, uint32_t height, std::string &error);
    bool hasWindow() const;
    // Install before execution. The callback reads a thread-safe game counter.
    using CompletedRenderCounter = uint64_t (*)();
    void setCompletedRenderCounter(CompletedRenderCounter counter);
    // Pumps SDL events. Returns false when the window wants to close.
    bool pumpEvents();
    // Host keyboard/gamepad state in the runtime's pad encoding, sampled by
    // pumpEvents. Only meaningful when this backend owns the window.
    void hostPadState(uint32_t &held, uint32_t &pressed, uint32_t &sticks) const;

    bool deviceReady() const;
    std::string driverName() const;
    // SDL's video backend -- "wayland", "x11", "offscreen".
    std::string videoDriverName() const;
    std::string lastError() const;
    SdlGpuStats stats() const;
    void resetStats();

private:
    friend std::unique_ptr<SdlGpuBackend> createSdlGpuBackend(std::string &error);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Returns null and fills `error` when SDL cannot create a GPU device.
std::unique_ptr<SdlGpuBackend> createSdlGpuBackend(std::string &error);

} // namespace dq8::gfx
