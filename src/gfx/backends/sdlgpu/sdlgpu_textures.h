// Texture cache: GS texture regions expanded to RGBA8 GPU textures.
//
// Everything the GS can put in front of the texture unit ends up as one
// R8G8B8A8 texture here -- indexed formats resolved through their CLUT, 16-bit
// formats expanded, TEXA applied. The fragment shader then only has to deal
// with wrapping and filtering, not with PSM variety.
//
// Entries are keyed on everything that changes the texels, and invalidated by
// local-memory page, so transfers refresh only affected texels on next use.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#include "gfx/gs/gs_vram.h"

#include "runtime/gs/gs_types.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dq8::gfx {

class SdlGpuDevice;
class GsTargetCache;

struct GsTextureKey {
    uint32_t tbp0 = 0u;
    uint32_t tbw = 1u;
    uint32_t psm = 0u;
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t cbp = 0u;
    uint32_t cpsm = 0u;
    uint32_t csm = 0u;
    uint32_t csa = 0u;
    // TA0 | AEM << 8 | TA1 << 16: part of the key because TEXA decides the
    // alpha of every texel in a 16- or 24-bit format.
    uint32_t texa = 0u;
    // CBW | COU << 8 | COV << 16, which addresses a CSM2 palette.
    uint32_t texclut = 0u;

    bool operator==(const GsTextureKey &other) const = default;
};

struct GsTextureKeyHash {
    size_t operator()(const GsTextureKey &key) const;
};

// Content identity independent of the cache address. Separate texel and CLUT
// hashes let replacements match base art across palette animation.
struct GsTextureIdentity {
    uint64_t texels = 0u;
    uint64_t clut = 0u;  // zero when the format is not indexed

    bool operator==(const GsTextureIdentity &other) const = default;
};

struct GsTextureCacheStats {
    uint64_t lookups = 0u;
    uint64_t hits = 0u;
    uint64_t builds = 0u;
    uint64_t invalidations = 0u;
    uint64_t invalidationsFromHostWrite = 0u;
    uint64_t invalidationsFromDraw = 0u;
    // Entries kept and patched in place rather than discarded, and the texels
    // that cost. The difference between this and texelsExpanded is the whole
    // point: a movie writing tiles into a texture should not re-expand the
    // texture.
    uint64_t partialUpdates = 0u;
    uint64_t texelsUpdated = 0u;
    uint64_t evictions = 0u;
    uint64_t texelsExpanded = 0u;
    // Textures sourced from a region a render target owns. Correct, but the
    // resolve costs the extra detail a scaled target holds.
    uint64_t renderTargetSources = 0u;

    double hitRate() const {
        return lookups == 0u ? 0.0
                             : static_cast<double>(hits) / static_cast<double>(lookups);
    }
};

class GsTextureCache {
public:
    GsTextureCache(SdlGpuDevice &device, GsVram &vram, GsTargetCache &targets);
    ~GsTextureCache();

    GsTextureCache(const GsTextureCache &) = delete;
    GsTextureCache &operator=(const GsTextureCache &) = delete;

    void reset();

    // Returns the GPU texture for this draw state's TEX0/TEXA/TEXCLUT, building
    // it from local memory on a miss. Null on failure, with `error` filled.
    SDL_GPUTexture *acquire(const GSDrawState &state, std::string &error);

    // Why a range of local memory stopped being trustworthy. Kept apart
    // because the two have very different fixes: a host write really did
    // change the texels, whereas a draw only wrote pixels that may share pages
    // with a texture and often leaves its content identical.
    enum class InvalidationSource { HostWrite, Draw };

    // Marks entries stale; repeated tile writes coalesce until the next acquire.
    void invalidate(const GsPageSet &pages, InvalidationSource source);

    const GsTextureCacheStats &stats() const { return m_stats; }
    void resetStats() { m_stats = {}; }
    size_t size() const { return m_entries.size(); }

    // True when the next miss would clear the cache. Callers holding texture
    // pointers -- a queued draw does -- must retire them before that happens.
    bool evictionImminent() const;

    // Pages a draw state's texture would be built from, so a caller can tell
    // whether queued work still owes it content.
    GsPageSet sourcePagesFor(const GSDrawState &state) const;

private:
    struct Entry {
        SDL_GPUTexture *texture = nullptr;
        // Texture pages and palette pages together, for callers that ask
        // "which pages does this entry read".
        GsPageSet pages;
        // The same two kept apart, because what a write to each one means is
        // different: a texel write changes what the texture shows, a palette
        // write only might. regionForPages maps texel pages; it has no answer
        // for a palette page, and treating "no answer" as "untrustworthy" was
        // dropping textures on every palette rewrite.
        GsPageSet texelPages;
        GsPageSet clutPages;
        GsTextureIdentity identity;
        // Texels a host write has invalidated but which have not been
        // re-expanded yet. Empty means the texture is current.
        GsRegion dirty;
        GsPageSet dirtyPages;
        // A palette page was written. On next use the palette window is
        // re-hashed; when it is unchanged -- the common case, a game
        // re-uploading the same CLUT every frame -- nothing else happens.
        bool clutDirty = false;
        // Set when a refresh saw the palette actually change and cleared by
        // the next whole-texture upload. The texel hash cannot see a palette
        // change (it hashes indices), so this is what stops an unchanged-
        // indices texture from skipping an upload it needs.
        bool clutUploadPending = false;
    };

    bool build(const GsTextureKey &key, Entry &entry, std::string &error);
    bool resolveSource(const GsTextureKey &key, const GsRegion &region, std::string &error);
    // Re-expands and re-uploads only `entry.dirty`.
    bool update(const GsTextureKey &key, Entry &entry, std::string &error);
    // Re-hashes the palette window after a write to a palette page, and
    // schedules a whole-texture re-expand when it really changed.
    void refreshClut(const GsTextureKey &key, Entry &entry);
    // Hash of the palette window this texture's indices resolve into.
    uint64_t hashClutWindow(const GsTextureKey &key) const;
    // Fills m_staging with RGBA8 for one texel rectangle. Returns the hash of
    // the raw texels it read, which is the whole texture's identity only when
    // the region is the whole texture.
    uint64_t expand(const GsTextureKey &key, const GsRegion &region);
    bool uploadRegion(SDL_GPUTexture *texture, const GsRegion &region, std::string &error);
    // Transfer buffers come from a fence-guarded pool: the movie path uploads
    // dozens of regions a frame, and creating a fresh buffer per region spent
    // a quarter of the EE thread inside the driver's allocator.
    SDL_GPUTransferBuffer *takeUploadBuffer(size_t bytes);
    void recycleUploadBuffer(SDL_GPUTransferBuffer *buffer, SDL_GPUFence *fence);
    void releaseUploadPool();
    void markSourcePages(const GsTextureKey &key, GsPageSet &pages) const;
    void markTexelPages(const GsTextureKey &key, GsPageSet &pages) const;
    void markClutPages(const GsTextureKey &key, GsPageSet &pages) const;
    // Which texels of a texture a set of local-memory pages covers.
    GsRegion regionForPages(const GsTextureKey &key, const GsPageSet &pages) const;
    static GsTextureKey keyFor(const GSDrawState &state);

    SdlGpuDevice &m_device;
    GsVram &m_vram;
    GsTargetCache &m_targets;
    std::unordered_map<GsTextureKey, Entry, GsTextureKeyHash> m_entries;
    std::vector<uint8_t> m_staging;
    // Expand scratch: raw texels for one row (indices for indexed PSMs, whole
    // words when the indices ride inside a 32-bit layout), and the palette
    // resolved to final RGBA so the texel loop is one lookup per pixel.
    std::vector<uint8_t> m_rawRow;
    std::vector<uint8_t> m_idxRow;
    std::vector<uint32_t> m_clutRgba;

    struct UploadBuffer {
        SDL_GPUTransferBuffer *buffer = nullptr;
        // Non-null while a submitted command buffer may still read the buffer.
        SDL_GPUFence *fence = nullptr;
        size_t bytes = 0u;
    };
    std::vector<UploadBuffer> m_uploadPool;
    GsTextureCacheStats m_stats{};
};

} // namespace dq8::gfx
