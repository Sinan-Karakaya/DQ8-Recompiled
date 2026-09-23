#include "gfx/backends/sdlgpu/sdlgpu_targets.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct32.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {

namespace {

// A GS buffer has no height register, so surfaces grow to fit what is drawn
// into them. The cap is generous next to the 1024-line maximum the CRTC can
// scan and keeps a nonsense scissor from asking for a huge allocation.
constexpr uint32_t kMaxSurfaceHeight = 2048u;
constexpr uint32_t kMaxSurfaceWidth = 2048u;

constexpr auto kBlockPositions = [] {
    std::array<std::array<uint32_t, 2>, 32> result{};
    for (uint32_t y = 0u; y < 4u; ++y)
        for (uint32_t x = 0u; x < 8u; ++x)
            result[GSPSMCT32::blockTable32[y][x]] = {x * 8u, y * 8u};
    return result;
}();

uint64_t ct32Words(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (x0 == 0u && y0 == 0u && x1 == 8u && y1 == 8u)
        return ~uint64_t(0u);
    uint64_t mask = 0u;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            mask |= uint64_t(1u) << GSPSMCT32::columnTable32[y][x];
    return mask;
}

void addCpuPatch(GsSurface &surface, GsRegion region) {
    if (region.empty()) return;
    // Join rectangles only along a complete edge; bounding disjoint writes
    // would upload stale CPU pixels over their GPU-owned neighbours.
    for (auto it = surface.cpuPatches.rbegin(); it != surface.cpuPatches.rend(); ++it) {
        if (it->contains(region)) return;
        if ((it->x0 == region.x0 && it->x1 == region.x1 &&
             it->y0 <= region.y1 && region.y0 <= it->y1) ||
            (it->y0 == region.y0 && it->y1 == region.y1 &&
             it->x0 <= region.x1 && region.x0 <= it->x1)) {
            it->merge(region);
            return;
        }
    }
    surface.cpuPatches.push_back(region);
}

uint32_t packColorToPsm(uint32_t psm, const uint8_t *rgba) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT24:
        return static_cast<uint32_t>(rgba[0]) |
               (static_cast<uint32_t>(rgba[1]) << 8u) |
               (static_cast<uint32_t>(rgba[2]) << 16u);
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        // RGBA5551. The GS treats alpha at or above 0x80 as the set bit.
        return static_cast<uint32_t>(((rgba[0] >> 3u) & 0x1fu) |
                                     (((rgba[1] >> 3u) & 0x1fu) << 5u) |
                                     (((rgba[2] >> 3u) & 0x1fu) << 10u) |
                                     ((rgba[3] >= 0x80u) ? 0x8000u : 0u));
    case GS_PSM_CT32:
    default:
        return static_cast<uint32_t>(rgba[0]) |
               (static_cast<uint32_t>(rgba[1]) << 8u) |
               (static_cast<uint32_t>(rgba[2]) << 16u) |
               (static_cast<uint32_t>(rgba[3]) << 24u);
    }
}

void unpackPsmToColor(uint32_t psm, uint32_t value, uint8_t *rgba) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT16:
    case GS_PSM_CT16S: {
        const uint32_t r = value & 0x1fu;
        const uint32_t g = (value >> 5u) & 0x1fu;
        const uint32_t b = (value >> 10u) & 0x1fu;
        rgba[0] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
        rgba[1] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
        rgba[2] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
        rgba[3] = ((value & 0x8000u) != 0u) ? 0x80u : 0u;
        break;
    }
    case GS_PSM_CT24:
        rgba[0] = static_cast<uint8_t>(value);
        rgba[1] = static_cast<uint8_t>(value >> 8u);
        rgba[2] = static_cast<uint8_t>(value >> 16u);
        rgba[3] = 0u;
        break;
    case GS_PSM_CT32:
    default:
        rgba[0] = static_cast<uint8_t>(value);
        rgba[1] = static_cast<uint8_t>(value >> 8u);
        rgba[2] = static_cast<uint8_t>(value >> 16u);
        rgba[3] = static_cast<uint8_t>(value >> 24u);
        break;
    }
}

// Ownership moves between local memory and the GPU on every transfer, draw and
// present, and getting that order wrong shows up as a buffer that is correct
// but blank. DQ8_GFX_TRACE_TARGETS prints the moves.
bool traceEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DQ8_GFX_TRACE_TARGETS");
        return value != nullptr && *value != '\0';
    }();
    return enabled;
}

void traceSurface(const char *what, const GsSurface &surface, uint64_t extra = 0u) {
    if (!traceEnabled())
        return;
    std::fprintf(stderr,
                 "[target] %-10s %s base=%05x fbw=%u psm=%02x %ux%u scale=%u "
                 "gpuDirty=%ux%u upload=%ux%u undefined=%d extra=%llu\n",
                 what, surface.depth ? "Z" : "C", surface.base, surface.bufferWidth,
                 surface.psm, surface.width, surface.height, surface.scale,
                 surface.gpuDirty.width(), surface.gpuDirty.height(),
                 surface.needsUpload.width(), surface.needsUpload.height(),
                 surface.undefined ? 1 : 0, static_cast<unsigned long long>(extra));
}

} // namespace

GsTargetCache::GsTargetCache(SdlGpuDevice &device, GsVram &vram)
    : m_device(device), m_vram(vram) {}

GsTargetCache::~GsTargetCache() {
    reset();
}

void GsTargetCache::setScale(uint32_t scale) {
    const uint32_t clamped = std::clamp<uint32_t>(scale, 1u, 8u);
    if (clamped == m_scale)
        return;
    // Every texture is sized in scaled pixels, so none of them survive.
    reset();
    m_scale = clamped;
}

void GsTargetCache::reset() {
    for (auto &surface : m_surfaces)
        destroySurface(*surface);
    m_surfaces.clear();
    m_pageGeneration.fill(0u);
    m_cpuWords.fill({});
    if (m_upload && m_device.valid())
        SDL_ReleaseGPUTransferBuffer(m_device.handle(), m_upload);
    m_upload = nullptr;
    m_uploadCapacity = 0u;
}

void GsTargetCache::destroySurface(GsSurface &surface) {
    if (surface.texture && m_device.valid())
        SDL_ReleaseGPUTexture(m_device.handle(), surface.texture);
    surface.texture = nullptr;
}

void GsTargetCache::recomputePages(GsSurface &surface) {
    surface.pages.reset();
    gsMarkPages(surface.pages, surface.base, surface.bufferWidth, surface.psm,
                surface.width, surface.height);
}

bool GsTargetCache::createTexture(GsSurface &surface, std::string &error) {
    if (!m_device.valid()) {
        error = "SDL GPU device is not available";
        return false;
    }
    if (surface.texture)
        SDL_ReleaseGPUTexture(m_device.handle(), surface.texture);
    surface.texture = nullptr;

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = surface.width * surface.scale;
    info.height = surface.height * surface.scale;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (surface.depth) {
        info.format = m_device.depthFormat();
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    } else {
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        // SAMPLER so a target can later be sampled as a texture, which is how
        // render-to-texture is handled without a round trip through memory.
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    }

    surface.texture = SDL_CreateGPUTexture(m_device.handle(), &info);
    if (!surface.texture) {
        error = std::string("SDL_CreateGPUTexture(render target): ") + SDL_GetError();
        return false;
    }
    surface.undefined = true;
    return true;
}

GsSurface *GsTargetCache::acquire(uint32_t base,
                                  uint32_t bufferWidth,
                                  uint32_t psm,
                                  bool depth,
                                  uint32_t minWidth,
                                  uint32_t minHeight,
                                  std::string &error) {
    bufferWidth = std::max<uint32_t>(bufferWidth, 1u);
    const uint32_t wantWidth =
        std::clamp(std::max(minWidth, bufferWidth * 64u), 1u, kMaxSurfaceWidth);
    const uint32_t wantHeight = std::clamp(minHeight, 1u, kMaxSurfaceHeight);

    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.base != base || surface.psm != psm || surface.depth != depth ||
            surface.bufferWidth != bufferWidth)
            continue;

        if (wantWidth <= surface.width && wantHeight <= surface.height)
            return &surface;

        // Growing loses the old contents unless they are preserved. Resolving
        // first and letting the reload path bring them back keeps that simple
        // and correct; growth happens a handful of times per scene.
        if (!surface.depth && !surface.gpuDirty.empty() &&
            !resolveSurface(surface, error))
            return nullptr;
        surface.width = std::max(surface.width, wantWidth);
        surface.height = std::max(surface.height, wantHeight);
        if (!createTexture(surface, error))
            return nullptr;
        recomputePages(surface);
        surface.gpuDirty.clear();
        surface.ownedPages.reset();
        surface.needsUpload = surface.depth ? GsRegion{} : surface.wholeRegion();
        return &surface;
    }

    auto created = std::make_unique<GsSurface>();
    created->base = base;
    created->bufferWidth = bufferWidth;
    created->psm = psm;
    created->depth = depth;
    created->width = wantWidth;
    created->height = wantHeight;
    created->scale = m_scale;
    if (!createTexture(*created, error))
        return nullptr;
    recomputePages(*created);
    // A colour surface starts by adopting whatever local memory already holds,
    // which is how a buffer the game filled with transfers becomes drawable.
    created->needsUpload = depth ? GsRegion{} : created->wholeRegion();
    ++m_stats.surfacesCreated;
    traceSurface("create", *created);
    m_surfaces.push_back(std::move(created));
    return m_surfaces.back().get();
}

GsSurface *GsTargetCache::findColorSurface(uint32_t base, uint32_t psm) const {
    for (const auto &candidate : m_surfaces) {
        if (!candidate->depth && candidate->base == base && candidate->psm == psm)
            return candidate.get();
    }
    return nullptr;
}

GsSurface *GsTargetCache::findSampleSource(uint32_t base, uint32_t psm,
                                           uint32_t bufferWidth) {
    bufferWidth = std::max<uint32_t>(bufferWidth, 1u);
    for (const auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        const bool compatible = surface.psm == psm ||
            (surface.psm == GS_PSM_CT32 && psm == GS_PSM_CT24);
        if (surface.depth || surface.base != base || !compatible)
            continue;
        if (surface.bufferWidth != bufferWidth)
            continue;
        if (surface.undefined)
            continue;

        // Native CT32 can upload exact CPU patches alongside owned GPU pages.
        // Other mixed views still require a resolve before sampling.
        if (!surface.cpuPatches.empty()) {
            std::string error;
            if (!applyCpuPatches(surface, error)) continue;
        }
        if (!surface.needsUpload.empty()) {
            if (!surface.gpuDirty.empty() &&
                !(surface.psm == GS_PSM_CT32 && surface.scale == 1u &&
                  (surface.base & 31u) == 0u && surface.width == surface.bufferWidth * 64u &&
                  surface.height % 32u == 0u))
                continue;
            std::string refreshError;
            if (!prepareColorView(surface, refreshError) || !refresh(surface, refreshError))
                continue;
        }
        return &surface;
    }
    return nullptr;
}

// Which part of a surface a set of local-memory pages covers. Pages tile the
// surface in the PSM's page geometry, so this is arithmetic rather than a
// search; the result is a bounding box, which over-covers a scattered set but
// never under-covers one.
GsRegion GsTargetCache::regionForPages(const GsSurface &surface,
                                       const GsPageSet &pages) const {
    GsRegion region{};
    const GsPageExtent extent = gsPageExtent(surface.psm);
    if (!extent.valid())
        return surface.wholeRegion();

    const uint32_t pixelsPerRow = std::max<uint32_t>(surface.bufferWidth, 1u) * 64u;
    const uint32_t pagesPerRow = std::max<uint32_t>(pixelsPerRow / extent.width, 1u);
    const uint32_t basePage = surface.base >> 5u;
    const uint32_t pageRows = (surface.height + extent.height - 1u) / extent.height;

    for (uint32_t row = 0u; row < pageRows; ++row) {
        for (uint32_t column = 0u; column < pagesPerRow; ++column) {
            const uint32_t page = (basePage + row * pagesPerRow + column) % kGsPageCount;
            if (!pages.test(page))
                continue;
            GsRegion cell{column * extent.width, row * extent.height,
                          (column + 1u) * extent.width, (row + 1u) * extent.height};
            cell.x1 = std::min(cell.x1, surface.width);
            cell.y1 = std::min(cell.y1, surface.height);
            region.merge(cell);
        }
    }
    return region;
}

void GsTargetCache::markDrawn(GsSurface &surface, const GsRegion &region) {
    GsRegion clipped = region;
    clipped.x1 = std::min(clipped.x1, surface.width);
    clipped.y1 = std::min(clipped.y1, surface.height);
    surface.gpuDirty.merge(clipped);
    surface.undefined = false;
    GsPageSet writtenPages;
    gsMarkPages(writtenPages, surface.base, surface.bufferWidth, surface.psm,
                clipped.width(), clipped.height(), clipped.x0, clipped.y0);
    surface.ownedPages |= writtenPages;
    for (uint32_t page = 0u; page < kGsPageCount; ++page)
        if (writtenPages.test(page)) ++m_pageGeneration[page];
    for (auto &candidate : m_surfaces) {
        GsSurface &other = *candidate;
        if (&other != &surface && !other.depth && (other.pages & writtenPages).any())
            other.needsUpload.merge(regionForPages(other, writtenPages));
    }
    // The texture is now ahead of local memory here, so a pending upload of the
    // same region would undo the draw.
    if (surface.needsUpload.empty())
        return;
    if (clipped.contains(surface.needsUpload))
        surface.needsUpload.clear();
}

void GsTargetCache::invalidate(const GsPageSet &pages, bool preserveOwned) {
    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if ((surface.pages & pages).none())
            continue;
        if (preserveOwned && !surface.depth && !surface.gpuDirty.empty()) {
            surface.needsUpload.merge(regionForPages(surface, pages & ~surface.ownedPages));
            continue;
        }
        if (surface.depth) {
            // Depth is never read back, so a host write to its region simply
            // retires the texture; the next acquire starts clean.
            traceSurface("depth-discard", surface);
            surface.undefined = true;
            surface.gpuDirty.clear();
            surface.ownedPages.reset();
            ++m_stats.depthDiscards;
            continue;
        }

        const GsRegion written = regionForPages(surface, pages);
        if (written.empty())
            continue;

        // Local memory is authoritative for this region now, so the texture
        // must not be resolved back over it. Anything the GPU still owned there
        // was resolved before the write landed (see Impl::settleFor); leaving
        // it marked dirty lets a stale surface overwrite the new contents at
        // the next resolve, which blanked the display buffer.
        surface.needsUpload.merge(written);
        surface.ownedPages &= ~pages;
        if (surface.ownedPages.none() || written.contains(surface.gpuDirty)) {
            surface.gpuDirty.clear();
            surface.ownedPages.reset();
        }
        traceSurface("invalidate", surface);
    }
}

bool GsTargetCache::canPatchHostWrite(const GsPageSet &pages) const {
    bool owned = false;
    for (const auto &candidate : m_surfaces) {
        const GsSurface &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty() || (surface.pages & pages).none())
            continue;
        owned = true;
        if (surface.psm != GS_PSM_CT32 || surface.scale != 1u ||
            (surface.base & 31u) != 0u || surface.width != surface.bufferWidth * 64u ||
            surface.pages.count() != surface.bufferWidth * ((surface.height + 31u) / 32u) ||
            !surface.needsUpload.empty() || surface.undefined)
            return false;
    }
    return owned;
}

void GsTargetCache::patchHostWrite(const GsPageSet &pages, uint32_t base, uint32_t bw,
                                  uint32_t x, uint32_t y, uint32_t width,
                                  uint32_t firstPixel, uint32_t endPixel) {
    auto patchRect = [&](const GsRegion &rect) {
        for (uint32_t ty = rect.y0; ty < rect.y1;) {
            const uint32_t ey = std::min(rect.y1, (ty | 7u) + 1u);
            for (uint32_t tx = rect.x0; tx < rect.x1;) {
                const uint32_t ex = std::min(rect.x1, (tx | 7u) + 1u);
                const uint32_t block = (GSPSMCT32::addrPSMCT32(base, bw, tx, ty) >> 8u) & 0x3fffu;
                const uint32_t page = block >> 5u;
                auto &words = m_cpuWords[block];
                if (words.generation != m_pageGeneration[page])
                    words = {m_pageGeneration[page], 0u};
                words.mask |= ct32Words(tx & 7u, ty & 7u, ((ex - 1u) & 7u) + 1u, ((ey - 1u) & 7u) + 1u);
                for (auto &candidate : m_surfaces) {
                    GsSurface &surface = *candidate;
                    if (surface.depth || surface.gpuDirty.empty() || !surface.ownedPages.test(page))
                        continue;
                    const uint32_t relativePage = (page + kGsPageCount - (surface.base >> 5u)) % kGsPageCount;
                    const auto position = kBlockPositions[block & 31u];
                    const uint32_t dx = (relativePage % surface.bufferWidth) * 64u + position[0] + (tx & 7u);
                    const uint32_t dy = (relativePage / surface.bufferWidth) * 32u + position[1] + (ty & 7u);
                    addCpuPatch(surface, {dx, dy, std::min(dx + ex - tx, surface.width),
                                          std::min(dy + ey - ty, surface.height)});
                }
                tx = ex;
            }
            ty = ey;
        }
    };
    // A byte payload may finish midway through a row or even a packed pixel.
    // Only completed pixels reported by the transfer engine are authoritative.
    if (firstPixel < endPixel && firstPixel % width != 0u) {
        const uint32_t count = std::min(endPixel - firstPixel, width - firstPixel % width);
        patchRect({x + firstPixel % width, y + firstPixel / width,
                   x + firstPixel % width + count, y + firstPixel / width + 1u});
        firstPixel += count;
    }
    const uint32_t rows = (endPixel - firstPixel) / width;
    if (rows != 0u) {
        patchRect({x, y + firstPixel / width, x + width, y + firstPixel / width + rows});
        firstPixel += rows * width;
    }
    if (firstPixel < endPixel)
        patchRect({x, y + firstPixel / width, x + endPixel - firstPixel, y + firstPixel / width + 1u});
    invalidate(pages, true);
}

void GsTargetCache::markReadPagesCt32(GsPageSet &pages, uint32_t base, uint32_t bw,
                                     const GsRegion &region) const {
    for (uint32_t y = region.y0; y < region.y1;) {
        const uint32_t ey = std::min(region.y1, (y | 7u) + 1u);
        for (uint32_t x = region.x0; x < region.x1;) {
            const uint32_t ex = std::min(region.x1, (x | 7u) + 1u);
            const uint32_t block = (GSPSMCT32::addrPSMCT32(base, bw, x, y) >> 8u) & 0x3fffu;
            const uint32_t page = block >> 5u;
            const auto &words = m_cpuWords[block];
            const uint64_t wanted = ct32Words(x & 7u, y & 7u, ((ex - 1u) & 7u) + 1u, ((ey - 1u) & 7u) + 1u);
            if (words.generation != m_pageGeneration[page] || (words.mask & wanted) != wanted)
                pages.set(page);
            x = ex;
        }
        y = ey;
    }
}

bool GsTargetCache::ownsAny(const GsPageSet &pages) const {
    for (const auto &candidate : m_surfaces) {
        if (candidate->depth || candidate->gpuDirty.empty())
            continue;
        if ((candidate->ownedPages & pages).any())
            return true;
    }
    return false;
}

bool GsTargetCache::resolve(const GsPageSet &pages, std::string &error) {
    std::vector<GsSurface *> surfaces;
    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty())
            continue;
        if ((surface.ownedPages & pages).none())
            continue;
        // Only the part the caller is about to read, intersected with the part
        // the GPU actually owns.
        const GsRegion wanted = regionForPages(surface, pages);
        if (!wanted.intersects(surface.gpuDirty))
            continue;
        surfaces.push_back(&surface);
    }
    return resolveSurfaces(surfaces, error);
}

bool GsTargetCache::resolveAll(std::string &error) {
    std::vector<GsSurface *> surfaces;
    for (auto &candidate : m_surfaces) {
        GsSurface &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty())
            continue;
        surfaces.push_back(&surface);
    }
    return resolveSurfaces(surfaces, error);
}

bool GsTargetCache::resolveForHostWrite(const GsPageSet &pages, std::string &error) {
    std::vector<GsSurface *> surfaces;
    for (auto &candidate : m_surfaces) {
        auto &surface = *candidate;
        if (surface.depth || surface.gpuDirty.empty() || (surface.pages & pages).none())
            continue;
        // Include earlier CPU writes: two disjoint invalidations can merge
        // across a GPU-owned middle even when neither alone touches it.
        GsRegion upload = surface.needsUpload;
        upload.merge(regionForPages(surface, pages));
        if (upload.intersects(surface.gpuDirty))
            surfaces.push_back(&surface);
    }
    return resolveSurfaces(surfaces, error);
}

bool GsTargetCache::resolveSurfaces(const std::vector<GsSurface *> &surfaces,
                                    std::string &error) {
    for (auto *surface : surfaces)
        if (!applyCpuPatches(*surface, error)) return false;
    uint64_t bytes = 0u;
    bool canBatch = surfaces.size() > 1u;
    for (const auto *surface : surfaces) {
        canBatch = canBatch && surface->scale == 1u;
        bytes += (uint64_t(surface->gpuDirty.width()) * surface->gpuDirty.height() * 4u + 255u) & ~uint64_t(255u);
    }
    if (!canBatch || bytes > 16u * 1024u * 1024u) {
        for (auto *surface : surfaces)
            if (!resolveSurface(*surface, error)) return false;
        return true;
    }
    if (traceEnabled())
        std::fprintf(stderr, "[target] resolve-batch surfaces=%zu bytes=%llu\n",
                     surfaces.size(), static_cast<unsigned long long>(bytes));

    SDL_GPUDevice *device = m_device.handle();
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = static_cast<uint32_t>(bytes);
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &info);
    if (!download) {
        error = std::string("SDL_CreateGPUTransferBuffer(resolve batch): ") + SDL_GetError();
        return false;
    }
    auto release = [&]() { SDL_ReleaseGPUTransferBuffer(device, download); };
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(resolve batch): ") + SDL_GetError();
        release();
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(resolve batch): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        release();
        return false;
    }
    uint32_t offset = 0u;
    for (auto *surface : surfaces) {
        const auto region = surface->gpuDirty;
        SDL_GPUTextureRegion source{};
        source.texture = surface->texture;
        source.x = region.x0;
        source.y = region.y0;
        source.w = region.width();
        source.h = region.height();
        source.d = 1u;
        SDL_GPUTextureTransferInfo destination{};
        destination.transfer_buffer = download;
        destination.offset = offset;
        destination.pixels_per_row = source.w;
        destination.rows_per_layer = source.h;
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        offset += (source.w * source.h * 4u + 255u) & ~255u;
    }
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(resolve batch): ") + SDL_GetError();
        release();
        return false;
    }
    SDL_GPUFence *fences[] = {fence};
    const bool waited = SDL_WaitForGPUFences(device, true, fences, 1u);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        error = std::string("SDL_WaitForGPUFences(resolve batch): ") + SDL_GetError();
        release();
        return false;
    }
    const auto *mapped = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device, download, false));
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(resolve batch): ") + SDL_GetError();
        release();
        return false;
    }
    // The textures are independent; preserve the original VRAM writeback order.
    offset = 0u;
    for (auto *surface : surfaces) {
        const auto region = surface->gpuDirty;
        writeResolved(*surface, region, mapped + offset);
        offset += (region.width() * region.height() * 4u + 255u) & ~255u;
    }
    SDL_UnmapGPUTransferBuffer(device, download);
    release();
    return true;
}

bool GsTargetCache::resolveSurface(GsSurface &surface, std::string &error) {
    if (surface.gpuDirty.empty())
        return true;
    return resolveRegion(surface, surface.gpuDirty, error);
}

bool GsTargetCache::resolveRegion(GsSurface &surface, GsRegion region,
                                  std::string &error) {
    if (region.empty())
        return true;
    if (!applyCpuPatches(surface, error))
        return false;
    if (!downloadScaled(surface, region, m_staging, error))
        return false;
    writeResolved(surface, region, m_staging.data());
    return true;
}

void GsTargetCache::writeResolved(GsSurface &surface, GsRegion region,
                                  const uint8_t *pixels) {
    if (traceEnabled()) {
        uint64_t nonBlack = 0u;
        const size_t bytes = size_t(region.width()) * region.height() * 4u;
        for (size_t i = 0u; i < bytes; i += 4u) {
            if (pixels[i] != 0u || pixels[i + 1u] != 0u || pixels[i + 2u] != 0u)
                ++nonBlack;
        }
        traceSurface("resolve", surface, nonBlack);
    }

    const uint32_t regionWidth = region.width();
    GsPageSet resolvedPages;
    gsMarkPages(resolvedPages, surface.base, surface.bufferWidth, surface.psm,
                region.width(), region.height(), region.x0, region.y0);
    if (surface.psm == GS_PSM_CT32 && (resolvedPages & ~surface.ownedPages).none()) {
        GSMem::WriteRunCT32(m_vram.data(), surface.base, surface.bufferWidth,
                            region.x0, region.x1, region.x0, region.y0,
                            pixels, regionWidth * region.height());
    } else {
        for (uint32_t y = region.y0; y < region.y1; ++y) {
            const uint8_t *row =
                pixels + static_cast<size_t>(y - region.y0) * regionWidth * 4u;
            for (uint32_t x = region.x0; x < region.x1; ++x) {
                if (surface.psm == GS_PSM_CT32) {
                    const uint32_t page = (GSPSMCT32::addrPSMCT32(surface.base,
                        surface.bufferWidth, x, y) >> 13u) % kGsPageCount;
                    if (!surface.ownedPages.test(page)) continue;
                }
                m_vram.write(surface.psm, surface.base, surface.bufferWidth, x, y,
                             packColorToPsm(surface.psm, row + (x - region.x0) * 4u));
            }
        }
    }

    if (region.contains(surface.gpuDirty)) {
        surface.gpuDirty.clear();
        surface.ownedPages.reset();
    }
    ++m_stats.colorResolves;
    m_stats.resolvedPixels += static_cast<uint64_t>(regionWidth) * region.height();
}

bool GsTargetCache::refresh(GsSurface &surface, std::string &error) {
    if (!applyCpuPatches(surface, error))
        return false;
    if (surface.needsUpload.empty())
        return true;
    const GsRegion region = surface.needsUpload;
    if (surface.depth) {
        surface.needsUpload.clear();
        return true;
    }

    if (surface.psm == GS_PSM_CT32 && surface.scale == 1u &&
        (surface.base & 31u) == 0u && surface.width == surface.bufferWidth * 64u &&
        surface.ownedPages.any()) {
        for (uint32_t row = 0u; row < (surface.height + 31u) / 32u; ++row) {
            for (uint32_t column = 0u; column < surface.bufferWidth; ++column) {
                const uint32_t page = ((surface.base >> 5u) + row * surface.bufferWidth + column) % kGsPageCount;
                if (surface.ownedPages.test(page)) continue;
                addCpuPatch(surface, {std::max(column * 64u, region.x0),
                    std::max(row * 32u, region.y0),
                    std::min((column + 1u) * 64u, region.x1),
                    std::min((row + 1u) * 32u, region.y1)});
            }
        }
        if (!applyCpuPatches(surface, error)) return false;
        surface.needsUpload.clear();
        surface.undefined = false;
        return true;
    }

    const uint32_t regionWidth = region.width();
    const uint32_t regionHeight = region.height();
    m_staging.resize(static_cast<size_t>(regionWidth) * regionHeight * 4u);
    for (uint32_t y = region.y0; y < region.y1; ++y) {
        uint8_t *row =
            m_staging.data() + static_cast<size_t>(y - region.y0) * regionWidth * 4u;
        if (surface.psm == GS_PSM_CT32 || surface.psm == GS_PSM_CT24) {
            GSMem::ReadRowCT32(m_vram.data(), surface.base, surface.bufferWidth,
                               region.x0, y, regionWidth, row);
            if (surface.psm == GS_PSM_CT24) {
                for (uint32_t x = 0; x < regionWidth; ++x)
                    row[x * 4u + 3u] = 0u;
            }
            continue;
        }
        for (uint32_t x = region.x0; x < region.x1; ++x) {
            const uint32_t value =
                m_vram.read(surface.psm, surface.base, surface.bufferWidth, x, y);
            unpackPsmToColor(surface.psm, value, row + (x - region.x0) * 4u);
        }
    }

    if (traceEnabled())
        traceSurface("refresh", surface, static_cast<uint64_t>(regionWidth) * regionHeight);

    if (!uploadNative(surface, region, m_staging, error))
        return false;
    surface.needsUpload.clear();
    surface.undefined = false;
    ++m_stats.colorRefreshes;
    m_stats.refreshedPixels += static_cast<uint64_t>(regionWidth) * regionHeight;
    return true;
}

bool GsTargetCache::prepareColorView(GsSurface &surface, std::string &error) {
    std::vector<GsSurface *> surfaces;
    for (auto &candidate : m_surfaces) {
        GsSurface &other = *candidate;
        if (&other == &surface || other.depth || other.gpuDirty.empty() ||
            (other.ownedPages & surface.pages).none())
            continue;
        surfaces.push_back(&other);
    }
    if (surfaces.size() == 1u && surface.gpuDirty.empty()) {
        GsSurface &source = *surfaces.front();
        const bool to16 = source.psm == GS_PSM_CT32 && surface.psm == GS_PSM_CT16;
        const bool to32 = source.psm == GS_PSM_CT16 && surface.psm == GS_PSM_CT32;
        const uint32_t height32 = to16 ? source.height : surface.height;
        const uint32_t height16 = to16 ? surface.height : source.height;
        if ((to16 || to32) && source.scale == 1u && surface.scale == 1u &&
            source.base == surface.base && (source.base & 31u) == 0u &&
            source.bufferWidth == surface.bufferWidth && source.width == surface.width &&
            source.width == source.bufferWidth * 64u && height32 % 32u == 0u &&
            height16 == height32 * 2u && source.pages == surface.pages &&
            source.pages.count() == source.bufferWidth * (height32 / 32u) &&
            source.needsUpload.empty() && !source.undefined) {
            // The complete texture is authoritative: GPU-owned pixels plus
            // unchanged pixels imported from VRAM. Reinterpret those same GS
            // bytes and move ownership, so later resolves cannot write an old
            // view over the new format's draws. Mixed owners retain readback.
            if (!applyCpuPatches(source, error) ||
                !m_device.reinterpretColor(source.texture, surface.texture,
                                           surface.width, surface.height, to16, error))
                return false;
            source.gpuDirty.clear();
            source.ownedPages.reset();
            surface.needsUpload.clear();
            markDrawn(surface, surface.wholeRegion());
            traceSurface("reinterpret", surface, source.psm);
            return true;
        }
    }
    auto nativePages = [](const GsSurface &s) {
        return s.psm == GS_PSM_CT32 && s.scale == 1u && (s.base & 31u) == 0u &&
               s.width == s.bufferWidth * 64u && s.height % 32u == 0u &&
               s.pages.count() == s.bufferWidth * (s.height / 32u);
    };
    if (!surfaces.empty() && nativePages(surface) &&
        std::all_of(surfaces.begin(), surfaces.end(), [&](const GsSurface *s) {
            return nativePages(*s) && !s->undefined;
        })) {
        return importColorPages(surface, surfaces, error);
    }
    if (!resolveSurfaces(surfaces, error))
        return false;
    for (auto *other : surfaces) {
        surface.needsUpload.merge(regionForPages(surface, other->pages));
    }
    return true;
}

bool GsTargetCache::importColorPages(GsSurface &surface,
                                    const std::vector<GsSurface *> &sources,
                                    std::string &error) {
    if (!applyCpuPatches(surface, error)) return false;
    GsPageSet gpuPages = surface.ownedPages;
    for (auto *source : sources) {
        if (!applyCpuPatches(*source, error)) return false;
        gpuPages |= source->ownedPages;
    }
    // Import CPU pages around the GPU-owned pages, then fill those holes with
    // GPU copies. A bounding upload would silently overwrite resident data.
    for (uint32_t row = 0u; row < surface.height / 32u; ++row) {
        for (uint32_t column = 0u; column < surface.bufferWidth; ++column) {
            const uint32_t page = ((surface.base >> 5u) + row * surface.bufferWidth + column) % kGsPageCount;
            if (gpuPages.test(page)) continue;
            GsRegion region{column * 64u, row * 32u, (column + 1u) * 64u, (row + 1u) * 32u};
            region.x0 = std::max(region.x0, surface.needsUpload.x0);
            region.y0 = std::max(region.y0, surface.needsUpload.y0);
            region.x1 = std::min(region.x1, surface.needsUpload.x1);
            region.y1 = std::min(region.y1, surface.needsUpload.y1);
            addCpuPatch(surface, region);
        }
    }
    if (!applyCpuPatches(surface, error)) return false;
    auto fail = [&](const char *operation) {
        error = std::string(operation) + "(target page import): " + SDL_GetError();
        return false;
    };
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(m_device.handle());
    if (!commands) return fail("SDL_AcquireGPUCommandBuffer");
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        SDL_CancelGPUCommandBuffer(commands);
        return fail("SDL_BeginGPUCopyPass");
    }
    for (auto *source : sources) {
        const GsPageSet shared = source->ownedPages & surface.pages;
        for (uint32_t page = 0u; page < kGsPageCount; ++page) {
            if (!shared.test(page)) continue;
            const uint32_t srcPage = (page + kGsPageCount - (source->base >> 5u)) % kGsPageCount;
            const uint32_t dstPage = (page + kGsPageCount - (surface.base >> 5u)) % kGsPageCount;
            SDL_GPUTextureLocation from{};
            from.texture = source->texture;
            from.x = (srcPage % source->bufferWidth) * 64u;
            from.y = (srcPage / source->bufferWidth) * 32u;
            SDL_GPUTextureLocation to{};
            to.texture = surface.texture;
            to.x = (dstPage % surface.bufferWidth) * 64u;
            to.y = (dstPage / surface.bufferWidth) * 32u;
            SDL_CopyGPUTextureToTexture(copy, &from, &to, 64u, 32u, 1u, false);
        }
    }
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(commands)) return fail("SDL_SubmitGPUCommandBuffer");
    for (auto *source : sources) {
        const GsPageSet shared = source->ownedPages & surface.pages;
        surface.ownedPages |= shared;
        surface.gpuDirty.merge(regionForPages(surface, shared));
        source->ownedPages &= ~shared;
        if (source->ownedPages.none()) {
            source->gpuDirty.clear();
        } else {
            const auto remaining = regionForPages(*source, source->ownedPages);
            source->gpuDirty.x0 = std::max(source->gpuDirty.x0, remaining.x0);
            source->gpuDirty.y0 = std::max(source->gpuDirty.y0, remaining.y0);
            source->gpuDirty.x1 = std::min(source->gpuDirty.x1, remaining.x1);
            source->gpuDirty.y1 = std::min(source->gpuDirty.y1, remaining.y1);
        }
        traceSurface("page-import", surface, shared.count());
    }
    surface.needsUpload.clear();
    surface.undefined = false;
    return true;
}

bool GsTargetCache::download(GsSurface &surface,
                             std::vector<uint8_t> &out,
                             std::string &error) {
    if (!applyCpuPatches(surface, error))
        return false;
    return m_device.downloadTexture(surface.texture, surface.width * surface.scale,
                                    surface.height * surface.scale, out, error);
}

// Reads one region of a surface back at GS resolution, as tightly packed RGBA8
// rows of region.width() pixels.
//
// At scale 1 the region comes straight out of the surface. Above it, the region
// is point-sampled down through a staging texture first, because local memory
// has nowhere to put the extra samples.
bool GsTargetCache::downloadScaled(GsSurface &surface,
                                   const GsRegion &region,
                                   std::vector<uint8_t> &out,
                                   std::string &error) {
    SDL_GPUDevice *device = m_device.handle();
    if (!device || !surface.texture || region.empty()) {
        error = "render target has no texture to resolve";
        return false;
    }

    const uint32_t width = region.width();
    const uint32_t height = region.height();
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    out.resize(bytes);

    SDL_GPUTexture *native = surface.texture;
    SDL_GPUTexture *staging = nullptr;
    uint32_t sourceX = region.x0;
    uint32_t sourceY = region.y0;

    if (surface.scale != 1u) {
        SDL_GPUTextureCreateInfo stagingInfo{};
        stagingInfo.type = SDL_GPU_TEXTURETYPE_2D;
        stagingInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        stagingInfo.width = width;
        stagingInfo.height = height;
        stagingInfo.layer_count_or_depth = 1u;
        stagingInfo.num_levels = 1u;
        stagingInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        stagingInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        staging = SDL_CreateGPUTexture(device, &stagingInfo);
        if (!staging) {
            error = std::string("SDL_CreateGPUTexture(resolve staging): ") + SDL_GetError();
            return false;
        }
        native = staging;
        sourceX = 0u;
        sourceY = 0u;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = static_cast<uint32_t>(bytes);
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (!download) {
        error = std::string("SDL_CreateGPUTransferBuffer(resolve): ") + SDL_GetError();
        if (staging)
            SDL_ReleaseGPUTexture(device, staging);
        return false;
    }

    auto release = [&]() {
        SDL_ReleaseGPUTransferBuffer(device, download);
        if (staging)
            SDL_ReleaseGPUTexture(device, staging);
    };

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(resolve): ") + SDL_GetError();
        release();
        return false;
    }

    if (staging) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = surface.texture;
        blit.source.x = region.x0 * surface.scale;
        blit.source.y = region.y0 * surface.scale;
        blit.source.w = width * surface.scale;
        blit.source.h = height * surface.scale;
        blit.destination.texture = staging;
        blit.destination.w = width;
        blit.destination.h = height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
    }

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(resolve): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        release();
        return false;
    }
    SDL_GPUTextureRegion source{};
    source.texture = native;
    source.x = sourceX;
    source.y = sourceY;
    source.w = width;
    source.h = height;
    source.d = 1u;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = download;
    destination.pixels_per_row = width;
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(resolve): ") + SDL_GetError();
        release();
        return false;
    }
    SDL_GPUFence *fences[] = {fence};
    const bool waited = SDL_WaitForGPUFences(device, true, fences, 1u);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        error = std::string("SDL_WaitForGPUFences(resolve): ") + SDL_GetError();
        release();
        return false;
    }

    void *mapped = SDL_MapGPUTransferBuffer(device, download, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(resolve): ") + SDL_GetError();
        release();
        return false;
    }
    std::memcpy(out.data(), mapped, bytes);
    SDL_UnmapGPUTransferBuffer(device, download);
    release();
    return true;
}

// Writes one region of GS-resolution pixels into a surface, point-expanding it
// when the surface is scaled.
bool GsTargetCache::applyCpuPatches(GsSurface &surface, std::string &error) {
    if (surface.cpuPatches.empty()) return true;
    uint32_t bytes = 0u;
    for (const auto &region : surface.cpuPatches)
        bytes += (region.width() * region.height() * 4u + 255u) & ~255u;
    SDL_GPUDevice *device = m_device.handle();
    if (!m_upload || m_uploadCapacity < bytes) {
        if (m_upload) SDL_ReleaseGPUTransferBuffer(device, m_upload);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = bytes;
        m_upload = SDL_CreateGPUTransferBuffer(device, &info);
        m_uploadCapacity = m_upload ? bytes : 0u;
    }
    auto fail = [&](const char *operation) {
        error = std::string(operation) + "(target CPU patches): " + SDL_GetError();
        return false;
    };
    if (!m_upload) return fail("SDL_CreateGPUTransferBuffer");
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, m_upload, true));
    if (!mapped) return fail("SDL_MapGPUTransferBuffer");
    uint32_t offset = 0u;
    for (const auto &region : surface.cpuPatches) {
        for (uint32_t y = region.y0; y < region.y1; ++y)
            GSMem::ReadRowCT32(m_vram.data(), surface.base, surface.bufferWidth,
                               region.x0, y, region.width(),
                               mapped + offset + (y - region.y0) * region.width() * 4u);
        offset += (region.width() * region.height() * 4u + 255u) & ~255u;
    }
    SDL_UnmapGPUTransferBuffer(device, m_upload);
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) return fail("SDL_AcquireGPUCommandBuffer");
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        SDL_CancelGPUCommandBuffer(commands);
        return fail("SDL_BeginGPUCopyPass");
    }
    offset = 0u;
    uint64_t pixels = 0u;
    for (const auto &region : surface.cpuPatches) {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = m_upload;
        source.offset = offset;
        source.pixels_per_row = region.width();
        source.rows_per_layer = region.height();
        SDL_GPUTextureRegion destination{};
        destination.texture = surface.texture;
        destination.x = region.x0;
        destination.y = region.y0;
        destination.w = region.width();
        destination.h = region.height();
        destination.d = 1u;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
        pixels += uint64_t(region.width()) * region.height();
        offset += (region.width() * region.height() * 4u + 255u) & ~255u;
    }
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(commands)) return fail("SDL_SubmitGPUCommandBuffer");
    traceSurface("cpu-patch", surface, pixels);
    surface.cpuPatches.clear();
    ++m_stats.colorRefreshes;
    m_stats.refreshedPixels += pixels;
    return true;
}

bool GsTargetCache::uploadNative(GsSurface &surface,
                                 const GsRegion &region,
                                 const std::vector<uint8_t> &pixels,
                                 std::string &error) {
    SDL_GPUDevice *device = m_device.handle();
    if (!device || !surface.texture || region.empty())
        return true;

    const uint32_t width = region.width();
    const uint32_t height = region.height();
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    if (pixels.size() < bytes) {
        error = "render target upload has too few pixels";
        return false;
    }

    SDL_GPUTexture *native = nullptr;
    if (surface.scale != 1u) {
        SDL_GPUTextureCreateInfo nativeInfo{};
        nativeInfo.type = SDL_GPU_TEXTURETYPE_2D;
        nativeInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        nativeInfo.width = width;
        nativeInfo.height = height;
        nativeInfo.layer_count_or_depth = 1u;
        nativeInfo.num_levels = 1u;
        nativeInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        nativeInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        native = SDL_CreateGPUTexture(device, &nativeInfo);
        if (!native) {
            error = std::string("SDL_CreateGPUTexture(refresh staging): ") + SDL_GetError();
            return false;
        }
    }

    auto release = [&]() {
        if (native)
            SDL_ReleaseGPUTexture(device, native);
    };

    if (!m_upload || m_uploadCapacity < bytes) {
        if (m_upload)
            SDL_ReleaseGPUTransferBuffer(device, m_upload);
        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<uint32_t>(bytes);
        m_upload = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        m_uploadCapacity = m_upload ? transferInfo.size : 0u;
        if (!m_upload) {
            error = std::string("SDL_CreateGPUTransferBuffer(refresh): ") + SDL_GetError();
            release();
            return false;
        }
    }
    // Submitted copies retain their storage while a later refresh maps a new cycle.
    void *mapped = SDL_MapGPUTransferBuffer(device, m_upload, true);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(refresh): ") + SDL_GetError();
        release();
        return false;
    }
    std::memcpy(mapped, pixels.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device, m_upload);

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(refresh): ") + SDL_GetError();
        release();
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(refresh): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        release();
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = m_upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination{};
    destination.texture = native ? native : surface.texture;
    destination.x = native ? 0u : region.x0;
    destination.y = native ? 0u : region.y0;
    destination.w = width;
    destination.h = height;
    destination.d = 1u;
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);

    if (native) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = native;
        blit.source.w = width;
        blit.source.h = height;
        blit.destination.texture = surface.texture;
        blit.destination.x = region.x0 * surface.scale;
        blit.destination.y = region.y0 * surface.scale;
        blit.destination.w = width * surface.scale;
        blit.destination.h = height * surface.scale;
        // A partial refresh must preserve the other target pixels.
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &blit);
    }

    const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
    release();
    if (!submitted)
        error = std::string("SDL_SubmitGPUCommandBuffer(refresh): ") + SDL_GetError();
    return submitted;
}

} // namespace dq8::gfx
