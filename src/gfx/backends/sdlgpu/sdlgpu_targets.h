// Render-target cache: GS framebuffer and Z-buffer regions held as GPU
// textures at the configured resolution scale.
//
// This is the difference between this backend and the compute rasteriser it
// replaces. Nothing draws into swizzled local memory, so a target can be any
// multiple of its GS size; local memory is the backing store, synchronised at
// the boundaries where something outside the GPU can observe it.
//
// GPU page ownership and exact CPU patches can coexist within one surface:
//
//   drawing        -> the texture is ahead of local memory   (gpuDirty)
//   host writes    -> exact native CT32 patches, or a resolved upload region
//   resolve()      -> texture back into local memory
//   refresh()      -> local memory back into the texture
#pragma once

#include "gfx/gs/gs_vram.h"

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dq8::gfx {

class SdlGpuDevice;

// A half-open region of a surface, in GS pixels. Ownership is tracked per
// region rather than per surface: a game that redraws a status bar should not
// cost a 512x448 readback, and one that transfers a movie frame into part of a
// buffer should not invalidate the rest of it.
struct GsRegion {
    uint32_t x0 = 0u;
    uint32_t y0 = 0u;
    uint32_t x1 = 0u;  // exclusive
    uint32_t y1 = 0u;  // exclusive

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    uint32_t width() const { return empty() ? 0u : x1 - x0; }
    uint32_t height() const { return empty() ? 0u : y1 - y0; }

    void clear() { x0 = y0 = x1 = y1 = 0u; }

    void merge(const GsRegion &other) {
        if (other.empty())
            return;
        if (empty()) {
            *this = other;
            return;
        }
        x0 = std::min(x0, other.x0);
        y0 = std::min(y0, other.y0);
        x1 = std::max(x1, other.x1);
        y1 = std::max(y1, other.y1);
    }

    bool intersects(const GsRegion &other) const {
        return !empty() && !other.empty() && x0 < other.x1 && other.x0 < x1 &&
               y0 < other.y1 && other.y0 < y1;
    }

    bool contains(const GsRegion &other) const {
        return other.empty() ||
               (!empty() && x0 <= other.x0 && y0 <= other.y0 && x1 >= other.x1 &&
                y1 >= other.y1);
    }
};

struct GsSurface {
    // Identity, as the FRAME or ZBUF register names it.
    uint32_t base = 0u;         // 256-byte block address (fbp << 5)
    uint32_t bufferWidth = 1u;  // FBW, in units of 64 pixels
    uint32_t psm = 0u;
    bool depth = false;

    // Extent in GS pixels; the texture is this multiplied by `scale`.
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t scale = 1u;

    SDL_GPUTexture *texture = nullptr;

    // Bounds of GPU writes; only ownedPages within these bounds may resolve.
    GsRegion gpuDirty;
    // Where local memory holds pixels the texture does not.
    GsRegion needsUpload;
    // Exact CPU writes may coexist with GPU-owned neighbours in the same page.
    std::vector<GsRegion> cpuPatches;
    // Nothing has been drawn or uploaded yet, so a load can be discarded.
    bool undefined = true;

    GsPageSet pages;
    // Allocated pages can overlap; only this subset may write back to VRAM.
    GsPageSet ownedPages;

    bool ownsAnything() const { return !gpuDirty.empty(); }
    GsRegion wholeRegion() const { return {0u, 0u, width, height}; }
};

struct GsTargetCacheStats {
    uint64_t surfacesCreated = 0u;
    uint64_t colorResolves = 0u;
    uint64_t colorRefreshes = 0u;
    uint64_t depthDiscards = 0u;
    uint64_t resolvedPixels = 0u;
    uint64_t refreshedPixels = 0u;
};

class GsTargetCache {
public:
    GsTargetCache(SdlGpuDevice &device, GsVram &vram);
    ~GsTargetCache();

    GsTargetCache(const GsTargetCache &) = delete;
    GsTargetCache &operator=(const GsTargetCache &) = delete;

    void setScale(uint32_t scale);
    uint32_t scale() const { return m_scale; }

    void reset();

    // Finds or creates the surface for a FRAME/ZBUF region, growing it to at
    // least the requested extent. Returns null only on a device failure.
    GsSurface *acquire(uint32_t base,
                       uint32_t bufferWidth,
                       uint32_t psm,
                       bool depth,
                       uint32_t minWidth,
                       uint32_t minHeight,
                       std::string &error);

    // Makes local memory current for every surface overlapping `pages`.
    bool resolve(const GsPageSet &pages, std::string &error);
    bool resolveForHostWrite(const GsPageSet &pages, std::string &error);
    bool resolveAll(std::string &error);

    // Records that local memory changed under `pages`.
    void invalidate(const GsPageSet &pages, bool preserveOwned = false);

    // CT32 uploads can patch resident native targets across pitch changes.
    bool canPatchHostWrite(const GsPageSet &pages) const;
    void patchHostWrite(const GsPageSet &pages, uint32_t base, uint32_t bw,
                        uint32_t x, uint32_t y, uint32_t width,
                        uint32_t firstPixel, uint32_t endPixel);
    // Add only pages whose requested CT32 words are not already CPU-current.
    void markReadPagesCt32(GsPageSet &pages, uint32_t base, uint32_t bw,
                           const GsRegion &region) const;

    // True when a colour surface holds pixels for `pages` that local memory
    // does not, i.e. reading those pages would need a resolve first.
    bool ownsAny(const GsPageSet &pages) const;

    // Brings a surface's texture up to date with local memory if needed.
    bool refresh(GsSurface &surface, std::string &error);

    // Copy native CT32 pages across pitches, reinterpret matching CT32/CT16
    // views, or resolve unsupported aliases before this view uses them.
    bool prepareColorView(GsSurface &surface, std::string &error);

    // Records that a draw wrote this region of a surface.
    void markDrawn(GsSurface &surface, const GsRegion &region);

    // The region of a surface the given pages cover, clipped to it.
    GsRegion regionForPages(const GsSurface &surface, const GsPageSet &pages) const;

    // The colour surface currently holding a display buffer, or null. Lets
    // presentation composite from the scaled texture instead of forcing a
    // resolve down to native resolution.
    GsSurface *findColorSurface(uint32_t base, uint32_t psm) const;

    // A colour surface that can stand in for a texture read of this region --
    // same base, format and buffer width, and already holding drawn content.
    // Sampling it directly avoids resolving it to local memory and rebuilding
    // a native-resolution copy, and keeps the detail a scaled target holds.
    // Not const: a surface whose local memory moved on underneath is brought up
    // to date here rather than being rejected, since a partial upload beats
    // resolving it and rebuilding a texture from the result.
    GsSurface *findSampleSource(uint32_t base, uint32_t psm, uint32_t bufferWidth);

    // Reads a colour surface back at its full scaled resolution, as RGBA8 rows
    // of width*scale pixels. Used by presentation, which wants the scaled
    // image rather than the native-resolution one resolve() produces.
    bool download(GsSurface &surface, std::vector<uint8_t> &out, std::string &error);

    const GsTargetCacheStats &stats() const { return m_stats; }
    void resetStats() { m_stats = {}; }

private:
    bool resolveSurfaces(const std::vector<GsSurface *> &surfaces, std::string &error);
    bool applyCpuPatches(GsSurface &surface, std::string &error);
    bool importColorPages(GsSurface &surface, const std::vector<GsSurface *> &sources,
                           std::string &error);
    bool resolveSurface(GsSurface &surface, std::string &error);
    // By value, not by reference: callers pass surface.gpuDirty, which this
    // clears partway through.
    bool resolveRegion(GsSurface &surface, GsRegion region, std::string &error);
    void writeResolved(GsSurface &surface, GsRegion region, const uint8_t *pixels);
    bool createTexture(GsSurface &surface, std::string &error);
    void destroySurface(GsSurface &surface);
    void recomputePages(GsSurface &surface);

    // Point-samples the scaled texture down to one host RGBA8 image at GS
    // resolution, or the reverse. `staging` is reused across calls.
    bool downloadScaled(GsSurface &surface, const GsRegion &region,
                        std::vector<uint8_t> &out, std::string &error);
    bool uploadNative(GsSurface &surface, const GsRegion &region,
                      const std::vector<uint8_t> &pixels, std::string &error);

    SdlGpuDevice &m_device;
    GsVram &m_vram;
    uint32_t m_scale = 1u;
    std::vector<std::unique_ptr<GsSurface>> m_surfaces;
    std::vector<uint8_t> m_staging;
    struct CpuWords { uint64_t generation = 0u; uint64_t mask = 0u; };
    std::array<uint64_t, kGsPageCount> m_pageGeneration{};
    std::array<CpuWords, kGsVramBytes / 256u> m_cpuWords{};
    SDL_GPUTransferBuffer *m_upload = nullptr;
    uint32_t m_uploadCapacity = 0u;
    GsTargetCacheStats m_stats{};
};

} // namespace dq8::gfx
