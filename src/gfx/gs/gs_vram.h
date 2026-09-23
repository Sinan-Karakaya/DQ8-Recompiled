// GS local memory: typed pixel access and page-range tracking.
//
// The 4 MiB the GS calls local memory stays the backing store for every
// backend. A hardware backend caches parts of it as GPU textures, but the bytes
// here remain the definition of what those caches hold, so cache lookup,
// invalidation and resolve all speak in terms of this file's page sets.
//
// Nothing here knows about SDL or any graphics API.
#pragma once

#include <bitset>
#include <cstdint>

namespace dq8::gfx {

// 4 MiB in 8 KiB pages. A page is the GS's own allocation granularity and the
// finest unit at which two resources can be said not to overlap.
inline constexpr uint32_t kGsVramBytes = 4u * 1024u * 1024u;
inline constexpr uint32_t kGsPageBytes = 8192u;
inline constexpr uint32_t kGsPageCount = kGsVramBytes / kGsPageBytes;

using GsPageSet = std::bitset<kGsPageCount>;

// Pixel dimensions of one 8 KiB page in a given PSM. Zero for PSMs with no
// defined page geometry, which callers must treat as "assume everything".
struct GsPageExtent {
    uint32_t width = 0u;
    uint32_t height = 0u;

    bool valid() const { return width != 0u && height != 0u; }
};

GsPageExtent gsPageExtent(uint32_t psm);

bool gsIsIndexedPsm(uint32_t psm);
bool gsIsDepthPsm(uint32_t psm);
// True for the PSMs a FRAME register may legally name.
bool gsIsColorFramePsm(uint32_t psm);
uint32_t gsBitsPerPixel(uint32_t psm);

// Marks every page a (base, bufferWidth, psm) rectangle can touch.
//
// Deliberately conservative in two directions: a block base that is not page
// aligned can carry into the next physical page, and an unknown PSM marks
// everything. Over-marking costs a spurious cache invalidation; under-marking
// silently renders stale data.
void gsMarkPages(GsPageSet &pages,
                 uint32_t baseBlock,
                 uint32_t bufferWidth,
                 uint32_t psm,
                 uint32_t width,
                 uint32_t height,
                 uint32_t x = 0u,
                 uint32_t y = 0u);

// A view over externally owned GS local memory. GS::init() hands the backend
// the frontend's allocation and other code may read it, so backends write
// through to it rather than keeping a private copy that can silently diverge.
class GsVram {
public:
    GsVram();

    void attach(uint8_t *storage, uint32_t sizeBytes);
    bool attached() const { return m_data != nullptr && m_size != 0u; }

    uint8_t *data() { return m_data; }
    const uint8_t *data() const { return m_data; }
    uint32_t size() const { return m_size; }

    uint32_t read(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void write(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

private:
    uint8_t *m_data = nullptr;
    uint32_t m_size = 0u;
};

} // namespace dq8::gfx
