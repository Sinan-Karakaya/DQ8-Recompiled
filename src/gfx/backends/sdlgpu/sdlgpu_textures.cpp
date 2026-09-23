#include "gfx/backends/sdlgpu/sdlgpu_textures.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {

namespace {

// Beyond this the cache is cleared wholesale rather than evicting cleverly.
// A GS scene addresses far fewer distinct textures than this; the cap exists
// so a pathological stream cannot exhaust GPU memory.
constexpr size_t kMaxEntries = 2048u;

uint32_t rgba5551To8888(uint32_t value) {
    const uint32_t r = (value & 0x1fu) << 3u;
    const uint32_t g = ((value >> 5u) & 0x1fu) << 3u;
    const uint32_t b = ((value >> 10u) & 0x1fu) << 3u;
    const uint32_t a = ((value >> 15u) & 0x01u) << 7u;
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

struct TexaState {
    uint8_t ta0 = 0u;
    bool aem = false;
    uint8_t ta1 = 0u;
};

TexaState unpackTexa(uint32_t packed) {
    return {static_cast<uint8_t>(packed & 0xffu),
            ((packed >> 8u) & 0x1u) != 0u,
            static_cast<uint8_t>((packed >> 16u) & 0xffu)};
}

// TEXA supplies the alpha a format has no room for: CT24 has none at all, and
// CT16's single bit selects between TA0 and TA1. AEM additionally forces alpha
// to zero when RGB is black, which is how a colour-keyed texture is done.
uint32_t applyTexa(const TexaState &texa, uint32_t psm, uint32_t texel) {
    if ((psm & 0x3fu) == GS_PSM_CT32)
        return texel;

    const bool rgbZero = (texel & 0x00ffffffu) == 0u;
    uint8_t alpha = static_cast<uint8_t>((texel >> 24u) & 0xffu);
    switch (psm & 0x3fu) {
    case GS_PSM_CT24:
        alpha = (texa.aem && rgbZero) ? 0u : texa.ta0;
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        if ((alpha & 0x80u) != 0u)
            alpha = texa.ta1;
        else
            alpha = (texa.aem && rgbZero) ? 0u : texa.ta0;
        break;
    default:
        break;
    }
    return (texel & 0x00ffffffu) | (static_cast<uint32_t>(alpha) << 24u);
}

// CSM1 swaps address bits 3 and 4 of the palette index; a 16-bit CLUT exposes
// a ninth bit through CSA[4].
uint32_t swizzleClutIndexCsm1(uint32_t index) {
    return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
}

uint32_t resolveClutIndex(uint32_t index, uint32_t cpsm, uint32_t csm, uint32_t csa,
                          uint32_t sourcePsm) {
    const bool fourBitSource = (sourcePsm & 0x3fu) == GS_PSM_T4 ||
                               (sourcePsm & 0x3fu) == GS_PSM_T4HH ||
                               (sourcePsm & 0x3fu) == GS_PSM_T4HL;
    // CSM2 addresses the palette directly through TEXCLUT, and CSA must be
    // zero there, so it does not offset anything.
    if (csm != 0u)
        return fourBitSource ? (index & 0x0fu) : index;

    const bool sixteenBitClut = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBitClut ? 0x1fu : 0x0fu;
    const uint32_t indexMask = sixteenBitClut ? 0x1ffu : 0x0ffu;
    const uint32_t base = (csa & csaMask) << 4u;

    uint32_t resolved = index;
    switch (sourcePsm & 0x3fu) {
    case GS_PSM_T4:
    case GS_PSM_T4HH:
    case GS_PSM_T4HL:
        resolved = base + (index & 0x0fu);
        break;
    case GS_PSM_T8:
    case GS_PSM_T8H:
        resolved = base + index;
        break;
    default:
        return index;
    }
    return swizzleClutIndexCsm1(resolved & indexMask);
}

// Writes an expanded texture as a PPM next to a one-line description, when
// DQ8_GFX_DUMP_TEXTURES names a directory. Expansion is where PSM, CLUT and
// TEXA all meet, so "is the texture wrong or is the shader wrong" is the first
// question every texturing bug asks.
void dumpTexture(const GsTextureKey &key,
                 const GsTextureIdentity &identity,
                 const std::vector<uint8_t> &rgba,
                 uint64_t index) {
    const char *directory = std::getenv("DQ8_GFX_DUMP_TEXTURES");
    if (!directory || *directory == '\0')
        return;

    // Named by identity, not by address: that is the name a replacement pack
    // would use, so a dump doubles as the pack's index.
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%016llx_%016llx_%ux%u_psm%02x.ppm", directory,
                  static_cast<unsigned long long>(identity.texels),
                  static_cast<unsigned long long>(identity.clut), key.width, key.height,
                  key.psm);
    std::FILE *file = std::fopen(path, "wb");
    if (!file)
        return;
    std::fprintf(file, "P6\n%u %u\n255\n", key.width, key.height);
    for (size_t i = 0u; i + 3u < rgba.size(); i += 4u)
        std::fwrite(rgba.data() + i, 1u, 3u, file);
    std::fclose(file);

    uint64_t opaque = 0u;
    uint64_t nonBlack = 0u;
    for (size_t i = 0u; i + 3u < rgba.size(); i += 4u) {
        if (rgba[i] != 0u || rgba[i + 1u] != 0u || rgba[i + 2u] != 0u)
            ++nonBlack;
        if (rgba[i + 3u] != 0u)
            ++opaque;
    }
    std::fprintf(stderr,
                 "[texture %llu] id=%016llx/%016llx tbp0=%04x psm=%02x %ux%u cbp=%04x "
                 "cpsm=%02x csa=%u non-black=%llu alpha>0=%llu\n",
                 static_cast<unsigned long long>(index),
                 static_cast<unsigned long long>(identity.texels),
                 static_cast<unsigned long long>(identity.clut), key.tbp0, key.psm,
                 key.width, key.height, key.cbp, key.cpsm, key.csa,
                 static_cast<unsigned long long>(nonBlack),
                 static_cast<unsigned long long>(opaque));
}

// FNV-1a. Not cryptographic, and does not need to be: a texture pack holds
// thousands of entries, not billions.
constexpr uint64_t kHashSeed = 1469598103934665603ull;

uint64_t hashStep(uint64_t hash, uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffull;
        hash *= 1099511628211ull;
    }
    return hash;
}

// The same FNV-1a over a byte string, one machine word at a time. The
// per-pixel hashStep it replaces cost eight multiply-xor rounds per texel,
// which at 512x448 was a third of what expansion cost.
uint64_t hashBytes(uint64_t hash, const uint8_t *data, size_t size) {
    size_t i = 0;
    for (; i + 8u <= size; i += 8u) {
        uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (; i < size; ++i)
        hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

} // namespace

size_t GsTextureKeyHash::operator()(const GsTextureKey &key) const {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    };
    mix(key.tbp0);
    mix(static_cast<uint64_t>(key.tbw) | (static_cast<uint64_t>(key.psm) << 16u) |
        (static_cast<uint64_t>(key.width) << 24u) |
        (static_cast<uint64_t>(key.height) << 40u));
    mix(key.cbp);
    mix(static_cast<uint64_t>(key.cpsm) | (static_cast<uint64_t>(key.csm) << 8u) |
        (static_cast<uint64_t>(key.csa) << 16u));
    mix(key.texa);
    mix(key.texclut);
    return static_cast<size_t>(hash);
}

GsTextureCache::GsTextureCache(SdlGpuDevice &device, GsVram &vram, GsTargetCache &targets)
    : m_device(device), m_vram(vram), m_targets(targets) {}

GsTextureCache::~GsTextureCache() {
    reset();
}

void GsTextureCache::reset() {
    if (m_device.valid()) {
        for (auto &[key, entry] : m_entries) {
            (void)key;
            if (entry.texture)
                SDL_ReleaseGPUTexture(m_device.handle(), entry.texture);
        }
    }
    m_entries.clear();
    releaseUploadPool();
}

void GsTextureCache::invalidate(const GsPageSet &pages, InvalidationSource source) {
    (void)source;
    // Mapping pages to texels belongs at consumption, not at each of the
    // movie's 896 tiny writes. Bitsets merge those writes without a page walk.
    for (auto &[key, entry] : m_entries) {
        (void)key;
        entry.dirtyPages |= entry.texelPages & pages;
        entry.clutDirty = entry.clutDirty || (entry.clutPages & pages).any();
    }
}

void GsTextureCache::markTexelPages(const GsTextureKey &key, GsPageSet &pages) const {
    gsMarkPages(pages, key.tbp0, key.tbw, key.psm, key.width, key.height);
}

void GsTextureCache::markClutPages(const GsTextureKey &key, GsPageSet &pages) const {
    if (!gsIsIndexedPsm(key.psm))
        return;

    // The palette is a separate region and just as invalidatable: a game that
    // animates a CLUT rewrites it every frame while the indices stay put.
    const bool sixteenBitClut = key.cpsm == GS_PSM_CT16 || key.cpsm == GS_PSM_CT16S;
    const uint32_t entries = (key.csm == 0u && sixteenBitClut) ? 512u : 256u;
    const uint32_t clutWidth = (key.texclut & 0xffu) + 16u;
    const uint32_t clutHeight = ((key.texclut >> 16u) & 0xffffu) + (entries + 15u) / 16u;
    gsMarkPages(pages, key.cbp, std::max<uint32_t>(key.texclut & 0xffu, 1u), key.cpsm,
                clutWidth, clutHeight);
}

void GsTextureCache::markSourcePages(const GsTextureKey &key, GsPageSet &pages) const {
    markTexelPages(key, pages);
    markClutPages(key, pages);
}

bool GsTextureCache::evictionImminent() const {
    return m_entries.size() >= kMaxEntries;
}

GsPageSet GsTextureCache::sourcePagesFor(const GSDrawState &state) const {
    GsPageSet pages;
    markSourcePages(keyFor(state), pages);
    return pages;
}

GsTextureKey GsTextureCache::keyFor(const GSDrawState &state) {
    const GSTex0Reg &tex0 = state.context.tex0;

    GsTextureKey key{};
    key.tbp0 = tex0.tbp0;
    key.tbw = std::max<uint32_t>(tex0.tbw, 1u);
    key.psm = tex0.psm;
    key.width = std::max<uint32_t>(state.textureWidth, 1u);
    key.height = std::max<uint32_t>(state.textureHeight, 1u);
    key.cbp = tex0.cbp;
    key.cpsm = tex0.cpsm;
    key.csm = tex0.csm;
    key.csa = tex0.csa;
    key.texa = static_cast<uint32_t>(state.texa.ta0) |
               (state.texa.aem ? 0x100u : 0u) |
               (static_cast<uint32_t>(state.texa.ta1) << 16u);
    key.texclut = static_cast<uint32_t>(state.texclut.cbw) |
                  (static_cast<uint32_t>(state.texclut.cou) << 8u) |
                  (static_cast<uint32_t>(state.texclut.cov) << 16u);
    return key;
}

SDL_GPUTexture *GsTextureCache::acquire(const GSDrawState &state, std::string &error) {
    const GsTextureKey key = keyFor(state);

    ++m_stats.lookups;
    if (auto found = m_entries.find(key); found != m_entries.end()) {
        ++m_stats.hits;
        if (found->second.dirtyPages.any()) {
            found->second.dirty.merge(regionForPages(key, found->second.dirtyPages));
            found->second.dirtyPages.reset();
        }
        if (found->second.clutDirty) {
            if (!resolveSource(key, {}, error)) return nullptr;
            refreshClut(key, found->second);
        }
        if (!found->second.dirty.empty() && !update(key, found->second, error))
            return nullptr;
        return found->second.texture;
    }

    if (m_entries.size() >= kMaxEntries) {
        reset();
        ++m_stats.evictions;
    }

    Entry entry{};
    if (!build(key, entry, error))
        return nullptr;
    markSourcePages(key, entry.pages);
    markTexelPages(key, entry.texelPages);
    markClutPages(key, entry.clutPages);
    SDL_GPUTexture *texture = entry.texture;
    m_entries.emplace(key, std::move(entry));
    return texture;
}

// Expands one texel rectangle into m_staging as tightly packed RGBA8 rows of
// region.width() pixels, and returns the hash of the raw texels it read.
//
// Three fast paths carry everything DQ8 draws during boot: CT32/Z32 store
// straight into staging (the raw word is the RGBA byte sequence), P8 resolves
// the palette once and indexes it per texel, and the P8H/P4H* family reads
// its row through the CT32 layout and shifts the index out of each word. The
// per-pixel path below remains for every other PSM; it is correct for all of
// them and none of them is hot on this game.
uint64_t GsTextureCache::expand(const GsTextureKey &key, const GsRegion &region) {
    const TexaState texa = unpackTexa(key.texa);
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const bool indexed = gsIsIndexedPsm(key.psm);
    const uint32_t regionWidth = region.width();
    const uint32_t regionHeight = region.height();
    const uint32_t psm = key.psm & 0x3fu;

    m_staging.resize(static_cast<size_t>(regionWidth) * regionHeight * 4u);
    uint64_t texelHash =
        hashStep(hashStep(hashStep(kHashSeed, key.width), key.height), key.psm);

    if (indexed) {
        // The palette resolved to final RGBA once per expand, so the texel
        // loop is a load and a store. A per-pixel swizzled palette read here
        // was the single most expensive thing in the old loop.
        const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        m_clutRgba.resize(fourBit ? 16u : 256u);
        for (uint32_t raw = 0u; raw < m_clutRgba.size(); ++raw) {
            const uint32_t clutIndex =
                resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
            const uint32_t clutX = clutOriginU + (clutIndex & 0x0fu);
            const uint32_t clutY = clutOriginV + (clutIndex >> 4u);
            const uint32_t entryValue =
                m_vram.read(key.cpsm, key.cbp, clutWidth, clutX, clutY);
            uint32_t color = 0u;
            switch (key.cpsm & 0x3fu) {
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
                color = applyTexa(texa, key.cpsm, rgba5551To8888(entryValue));
                break;
            default:
                color = applyTexa(texa, key.cpsm, entryValue);
                break;
            }
            m_clutRgba[raw] = color;
        }
    }

    const bool rowIsWords = psm == GS_PSM_CT32 || psm == GS_PSM_Z32 ||
                            psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    m_rawRow.resize(static_cast<size_t>(regionWidth) * (rowIsWords ? 4u : 1u));
    m_idxRow.resize(regionWidth);

    for (uint32_t y = region.y0; y < region.y1; ++y) {
        uint8_t *row =
            m_staging.data() + static_cast<size_t>(y - region.y0) * regionWidth * 4u;
        switch (psm) {
        case GS_PSM_CT32:
        case GS_PSM_Z32: {
            // The raw word is the RGBA byte sequence; TEXA does not apply.
            const auto readRow = psm == GS_PSM_Z32 ? GSMem::ReadRowZ32 : GSMem::ReadRowCT32;
            readRow(m_vram.data(), key.tbp0, key.tbw, region.x0, y, regionWidth, row);
            texelHash = hashBytes(texelHash, row, static_cast<size_t>(regionWidth) * 4u);
            continue;
        }
        case GS_PSM_T8: {
            uint8_t *raw = m_rawRow.data();
            GSMem::ReadRowP8(m_vram.data(), key.tbp0, key.tbw, region.x0, y,
                             regionWidth, raw);
            texelHash = hashBytes(texelHash, raw, regionWidth);
            for (uint32_t x = 0u; x < regionWidth; ++x)
                std::memcpy(row + static_cast<size_t>(x) * 4u, &m_clutRgba[raw[x]], 4u);
            continue;
        }
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH: {
            // Indices ride in a 32-bit layout: read the row as CT32 words and
            // shift each index out. All the index bits are inside the pixel's
            // own word, so the +3-byte read offset the per-pixel path uses is
            // not needed here.
            uint32_t *words = reinterpret_cast<uint32_t *>(m_rawRow.data());
            uint8_t *raw = m_idxRow.data();
            GSMem::ReadRowCT32(m_vram.data(), key.tbp0, key.tbw, region.x0, y,
                               regionWidth, reinterpret_cast<uint8_t *>(words));
            const uint32_t shift = psm == GS_PSM_T4HH ? 28u : 24u;
            const uint32_t mask = psm == GS_PSM_T8H ? 0xffu : 0x0fu;
            for (uint32_t x = 0u; x < regionWidth; ++x)
                raw[x] = static_cast<uint8_t>((words[x] >> shift) & mask);
            if (psm == GS_PSM_T8H) {
                texelHash = hashBytes(texelHash, raw, regionWidth);
            } else {
                // Two nibbles per byte, low first, so the packed stream stays
                // the hash input.
                const uint32_t pairs = (regionWidth + 1u) / 2u;
                for (uint32_t p = 0u; p < pairs; ++p)
                    raw[p] = static_cast<uint8_t>(raw[p * 2u] |
                                                  ((p * 2u + 1u < regionWidth
                                                        ? raw[p * 2u + 1u]
                                                        : 0u)
                                                   << 4u));
                texelHash = hashBytes(texelHash, raw, pairs);
            }
            for (uint32_t x = 0u; x < regionWidth; ++x) {
                const uint32_t index = (words[x] >> shift) & mask;
                std::memcpy(row + static_cast<size_t>(x) * 4u, &m_clutRgba[index], 4u);
            }
            continue;
        }
        default:
            break;
        }

        // Formats with no fast path: one swizzled read and one hash word per
        // pixel, exactly as before.
        for (uint32_t x = region.x0; x < region.x1; ++x) {
            const uint32_t raw = m_vram.read(key.psm, key.tbp0, key.tbw, x, y);
            texelHash = hashStep(texelHash, raw);
            uint32_t color = 0u;

            if (indexed) {
                const uint32_t clutIndex =
                    resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
                const uint32_t clutX = clutOriginU + (clutIndex & 0x0fu);
                const uint32_t clutY = clutOriginV + (clutIndex >> 4u);
                const uint32_t entryValue =
                    m_vram.read(key.cpsm, key.cbp, clutWidth, clutX, clutY);
                switch (key.cpsm & 0x3fu) {
                case GS_PSM_CT16:
                case GS_PSM_CT16S:
                    color = applyTexa(texa, key.cpsm, rgba5551To8888(entryValue));
                    break;
                default:
                    color = applyTexa(texa, key.cpsm, entryValue);
                    break;
                }
            } else {
                switch (psm) {
                case GS_PSM_CT16:
                case GS_PSM_CT16S:
                case GS_PSM_Z16:
                case GS_PSM_Z16S:
                    color = applyTexa(texa, GS_PSM_CT16, rgba5551To8888(raw));
                    break;
                default:
                    color = applyTexa(texa, key.psm, raw);
                    break;
                }
            }

            const size_t offset = static_cast<size_t>(x - region.x0) * 4u;
            row[offset] = static_cast<uint8_t>(color);
            row[offset + 1u] = static_cast<uint8_t>(color >> 8u);
            row[offset + 2u] = static_cast<uint8_t>(color >> 16u);
            row[offset + 3u] = static_cast<uint8_t>(color >> 24u);
        }
    }
    return texelHash;
}

// Which texels of a texture a set of local-memory pages covers. Same geometry
// as GsTargetCache::regionForPages, applied to a texture's own base and width.
GsRegion GsTextureCache::regionForPages(const GsTextureKey &key,
                                        const GsPageSet &pages) const {
    GsRegion region{};
    const GsPageExtent extent = gsPageExtent(key.psm);
    if (!extent.valid())
        return {0u, 0u, key.width, key.height};

    const uint32_t pixelsPerRow = std::max<uint32_t>(key.tbw, 1u) * 64u;
    const uint32_t pagesPerRow = std::max<uint32_t>(pixelsPerRow / extent.width, 1u);
    const uint32_t basePage = key.tbp0 >> 5u;
    const uint32_t pageRows = (key.height + extent.height - 1u) / extent.height;
    const uint32_t pageColumns = (key.width + extent.width - 1u) / extent.width;

    for (uint32_t row = 0u; row < pageRows; ++row) {
        for (uint32_t column = 0u; column < pageColumns; ++column) {
            const uint32_t page = (basePage + row * pagesPerRow + column) % kGsPageCount;
            const bool tailHit = (key.tbp0 & 31u) != 0u &&
                                 pages.test((page + 1u) % kGsPageCount);
            if (!pages.test(page) && !tailHit)
                continue;
            GsRegion cell{column * extent.width, row * extent.height,
                          std::min((column + 1u) * extent.width, key.width),
                          std::min((row + 1u) * extent.height, key.height)};
            region.merge(cell);
        }
    }
    return region;
}

// Transfer buffers come back once their fence completes. Reaping happens on
// take; the common case never waits because by the time a buffer is wanted
// again the previous upload has long finished.
SDL_GPUTransferBuffer *GsTextureCache::takeUploadBuffer(size_t bytes) {
    SDL_GPUDevice *device = m_device.handle();
    for (UploadBuffer &slot : m_uploadPool) {
        if (slot.fence != nullptr && SDL_QueryGPUFence(device, slot.fence)) {
            SDL_ReleaseGPUFence(device, slot.fence);
            slot.fence = nullptr;
        }
    }
    for (UploadBuffer &slot : m_uploadPool) {
        if (slot.fence == nullptr && slot.bytes >= bytes)
            return slot.buffer;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<uint32_t>(bytes);
    SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (buffer)
        m_uploadPool.push_back(UploadBuffer{buffer, nullptr, bytes});
    return buffer;
}

void GsTextureCache::recycleUploadBuffer(SDL_GPUTransferBuffer *buffer, SDL_GPUFence *fence) {
    for (UploadBuffer &slot : m_uploadPool) {
        if (slot.buffer == buffer) {
            slot.fence = fence;
            return;
        }
    }
}

void GsTextureCache::releaseUploadPool() {
    if (!m_device.valid())
        return;
    SDL_GPUDevice *device = m_device.handle();
    for (UploadBuffer &slot : m_uploadPool) {
        if (slot.fence != nullptr)
            SDL_ReleaseGPUFence(device, slot.fence);
        SDL_ReleaseGPUTransferBuffer(device, slot.buffer);
    }
    m_uploadPool.clear();
}

bool GsTextureCache::uploadRegion(SDL_GPUTexture *texture, const GsRegion &region,
                                  std::string &error) {
    SDL_GPUDevice *device = m_device.handle();
    const uint32_t width = region.width();
    const uint32_t height = region.height();
    const size_t bytes = static_cast<size_t>(width) * height * 4u;

    SDL_GPUTransferBuffer *upload = takeUploadBuffer(bytes);
    if (!upload) {
        error = std::string("SDL_CreateGPUTransferBuffer(texture cache): ") + SDL_GetError();
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(texture cache): ") + SDL_GetError();
        return false;
    }
    std::memcpy(mapped, m_staging.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(texture cache): ") + SDL_GetError();
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(texture cache): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination{};
    destination.texture = texture;
    destination.x = region.x0;
    destination.y = region.y0;
    destination.w = width;
    destination.h = height;
    destination.d = 1u;
    // cycle stays false: a cycled destination would come back with its
    // untouched texels undefined, which a partial update must not do. The
    // buffer side is guarded by the pool fence instead.
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence != nullptr) {
        recycleUploadBuffer(upload, fence);
    }
    return true;
}

// Hash of the palette window this texture's indices resolve into: only the
// entries the CLUT resolution can actually reach, CSA window included.
uint64_t GsTextureCache::hashClutWindow(const GsTextureKey &key) const {
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const bool sixteenBitClut = key.cpsm == GS_PSM_CT16 || key.cpsm == GS_PSM_CT16S;
    const uint32_t entries = ((key.psm & 0x3fu) == GS_PSM_T4 ||
                              (key.psm & 0x3fu) == GS_PSM_T4HL ||
                              (key.psm & 0x3fu) == GS_PSM_T4HH)
                                 ? 16u
                                 : (sixteenBitClut ? 512u : 256u);
    uint64_t clutHash = hashStep(hashStep(kHashSeed, key.cpsm), key.csm);
    for (uint32_t index = 0u; index < entries; ++index) {
        const uint32_t resolved =
            resolveClutIndex(index, key.cpsm, key.csm, key.csa, key.psm);
        const uint32_t clutX = clutOriginU + (resolved & 0x0fu);
        const uint32_t clutY = clutOriginV + (resolved >> 4u);
        clutHash = hashStep(clutHash, m_vram.read(key.cpsm, key.cbp, clutWidth, clutX, clutY));
    }
    return clutHash;
}

// A palette-page write landed. Re-hash the window: when it is unchanged --
// games re-upload the same CLUT every frame -- this is the whole cost of the
// write. When it moved, every expanded colour is stale, so the whole texture
// re-expands on next use.
void GsTextureCache::refreshClut(const GsTextureKey &key, Entry &entry) {
    entry.clutDirty = false;
    if (!gsIsIndexedPsm(key.psm))
        return;
    const uint64_t clutHash = hashClutWindow(key);
    if (clutHash == entry.identity.clut)
        return;
    entry.identity.clut = clutHash;
    entry.clutUploadPending = true;
    entry.dirty.merge(GsRegion{0u, 0u, key.width, key.height});
}

// Patches the texels a host write touched, instead of discarding a texture and
// re-expanding all of it. On the DQ8 movie path the written region is a small
// fraction of a 512x512 texture, and rebuilding it was the dominant cost.
bool GsTextureCache::resolveSource(const GsTextureKey &key, const GsRegion &region,
                                  std::string &error) {
    GsPageSet conservative;
    markSourcePages(key, conservative);
    if (!m_targets.ownsAny(conservative)) return true;

    GsPageSet pages;
    if (key.psm == GS_PSM_CT32 || key.psm == GS_PSM_CT24 ||
        key.psm == GS_PSM_T8H || key.psm == GS_PSM_T4HL || key.psm == GS_PSM_T4HH) {
        m_targets.markReadPagesCt32(pages, key.tbp0, key.tbw, region);
    } else if (!region.empty()) {
        gsMarkPages(pages, key.tbp0, key.tbw, key.psm,
                    region.width(), region.height(), region.x0, region.y0);
    }
    if (gsIsIndexedPsm(key.psm)) {
        if (key.cpsm == GS_PSM_CT32 || key.cpsm == GS_PSM_CT24) {
            const bool fourBit = key.psm == GS_PSM_T4 || key.psm == GS_PSM_T4HL || key.psm == GS_PSM_T4HH;
            const uint32_t count = fourBit ? 16u : 256u;
            const uint32_t bw = std::max<uint32_t>(key.texclut & 0xffu, 1u);
            for (uint32_t i = 0u; i < count; ++i) {
                const uint32_t index = resolveClutIndex(i, key.cpsm, key.csm, key.csa, key.psm);
                const uint32_t x = ((key.texclut >> 8u) & 0xffu) + (index & 15u);
                const uint32_t y = (key.texclut >> 16u) + (index >> 4u);
                m_targets.markReadPagesCt32(pages, key.cbp, bw, {x, y, x + 1u, y + 1u});
            }
        } else {
            markClutPages(key, pages);
        }
    }
    return m_targets.resolve(pages, error);
}

bool GsTextureCache::update(const GsTextureKey &key, Entry &entry, std::string &error) {
    const GsRegion region = entry.dirty;
    if (region.empty() || !entry.texture)
        return true;

    if (!resolveSource(key, region, error)) {
        return false;
    }

    const bool wholeTexture = region.x0 == 0u && region.y0 == 0u &&
                              region.x1 >= key.width && region.y1 >= key.height;
    const uint64_t texelHash = expand(key, region);
    // A whole-texture re-expand whose raw texels hash to what we already hold,
    // with no palette change waiting, is the title screen re-uploading its own
    // UI: the GPU texture is already right, so skip the transfer entirely.
    const bool unchanged = wholeTexture && !entry.clutUploadPending &&
                           texelHash == entry.identity.texels;
    if (!unchanged) {
        if (!uploadRegion(entry.texture, region, error))
            return false;
    }

    // The stored hash described the whole texture before this patch. A whole-
    // texture expand re-derived it; a partial one would leave it wrong, and
    // rehashing would mean reading every texel, so the identity is cleared
    // rather than left wrong -- nothing should match a partially updated
    // texture against a replacement pack.
    if (wholeTexture) {
        entry.identity.texels = texelHash;
        entry.clutUploadPending = false;
    } else {
        entry.identity.texels = 0u;
    }
    entry.dirty.clear();
    ++m_stats.partialUpdates;
    m_stats.texelsUpdated += static_cast<uint64_t>(region.width()) * region.height();
    return true;
}

bool GsTextureCache::build(const GsTextureKey &key, Entry &entry, std::string &error) {
    if (!m_device.valid()) {
        error = "SDL GPU device is not available";
        return false;
    }

    // A texture may be sourced from a region a render target still owns --
    // render-to-texture, which DQ8 uses. Resolving costs the extra detail a
    // scaled target holds; sampling the target directly is the fix, and is not
    // done yet.
    GsPageSet sourcePages;
    markSourcePages(key, sourcePages);
    if (m_targets.ownsAny(sourcePages))
        ++m_stats.renderTargetSources;
    if (!resolveSource(key, {0u, 0u, key.width, key.height}, error)) {
        return false;
    }

    const uint32_t width = key.width;
    const uint32_t height = key.height;
    m_staging.resize(static_cast<size_t>(width) * height * 4u);

    const bool indexed = gsIsIndexedPsm(key.psm);

    // Identity, computed off the same reads the expansion does. Dimensions and
    // format go into the texel hash because the same bytes read as 64x64 and as
    // 128x128 are different pictures.
    entry.identity = {};
    if (indexed) {
        // Only the palette window this texture can actually reach: CSA lets
        // many 4-bit textures share one CLUT, and folding in the whole thing
        // would make them all move together.
        entry.identity.clut = hashClutWindow(key);
    }

    entry.identity.texels = expand(key, {0u, 0u, width, height});
    dumpTexture(key, entry.identity, m_staging, m_stats.builds);

    SDL_GPUDevice *device = m_device.handle();
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    entry.texture = SDL_CreateGPUTexture(device, &info);
    if (!entry.texture) {
        error = std::string("SDL_CreateGPUTexture(texture cache): ") + SDL_GetError();
        return false;
    }

    SDL_GPUTransferBuffer *upload = takeUploadBuffer(m_staging.size());
    if (!upload) {
        error = std::string("SDL_CreateGPUTransferBuffer(texture cache): ") + SDL_GetError();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(texture cache): ") + SDL_GetError();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    std::memcpy(mapped, m_staging.data(), m_staging.size());
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(texture cache): ") + SDL_GetError();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(texture cache): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion region{};
    region.texture = entry.texture;
    region.w = width;
    region.h = height;
    region.d = 1u;
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence != nullptr) {
        recycleUploadBuffer(upload, fence);
    }

    ++m_stats.builds;
    m_stats.texelsExpanded += static_cast<uint64_t>(width) * height;
    return true;
}

} // namespace dq8::gfx
