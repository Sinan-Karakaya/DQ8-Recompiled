#include "gfx/gs/gs_vram.h"

#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <mutex>

namespace dq8::gfx {

namespace {

// GSMem's swizzle tables are computed once at runtime, not constant-initialised.
void ensureLookupTables() {
    static std::once_flag once;
    std::call_once(once, [] { GSMem::InitLookupTables(); });
}

} // namespace

GsPageExtent gsPageExtent(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return {64u, 32u};
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return {64u, 64u};
    case GS_PSM_T8:
        return {128u, 64u};
    case GS_PSM_T4:
        return {128u, 128u};
    default:
        return {};
    }
}

bool gsIsIndexedPsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_T8:
    case GS_PSM_T4:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return true;
    default:
        return false;
    }
}

bool gsIsDepthPsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return true;
    default:
        return false;
    }
}

bool gsIsColorFramePsm(uint32_t psm) {
    switch (psm & 0x3fu) {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return true;
    default:
        return false;
    }
}

uint32_t gsBitsPerPixel(uint32_t psm) {
    return static_cast<uint32_t>(
        GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(psm & 0x3fu)));
}

void gsMarkPages(GsPageSet &pages,
                 uint32_t baseBlock,
                 uint32_t bufferWidth,
                 uint32_t psm,
                 uint32_t width,
                 uint32_t height,
                 uint32_t x,
                 uint32_t y) {
    if (width == 0u || height == 0u)
        return;
    const GsPageExtent extent = gsPageExtent(psm);
    if (!extent.valid()) {
        pages.set();
        return;
    }
    const uint32_t pixelsPerRow = std::max<uint32_t>(bufferWidth, 1u) * 64u;
    const uint32_t pagesPerRow = std::max<uint32_t>(pixelsPerRow / extent.width, 1u);
    const uint32_t lastColumn = (x + width - 1u) / extent.width;
    const uint32_t lastRow = (y + height - 1u) / extent.height;
    const uint32_t basePage = baseBlock >> 5u;

    for (uint32_t row = y / extent.height; row <= lastRow; ++row) {
        for (uint32_t column = x / extent.width; column <= lastColumn; ++column) {
            const uint32_t page = basePage + row * pagesPerRow + column;
            pages.set(page % kGsPageCount);
            // A block base inside a page pushes the tail of the rectangle into
            // the next one.
            if ((baseBlock & 31u) != 0u)
                pages.set((page + 1u) % kGsPageCount);
        }
    }
}

GsVram::GsVram() {
    ensureLookupTables();
}

void GsVram::attach(uint8_t *storage, uint32_t sizeBytes) {
    ensureLookupTables();
    m_data = storage;
    m_size = storage != nullptr ? sizeBytes : 0u;
}

uint32_t GsVram::read(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const {
    if (!attached())
        return 0u;
    // GSMem's accessors take a mutable pointer even to read.
    uint8_t *storage = const_cast<uint8_t *>(m_data);
    using namespace GSMem;
    switch (psm & 0x3fu) {
    case GS_PSM_CT32: return ReadCT32(storage, base, bw, x, y);
    case GS_PSM_CT24: return ReadCT24(storage, base, bw, x, y);
    case GS_PSM_CT16: return ReadCT16(storage, base, bw, x, y);
    case GS_PSM_CT16S: return ReadCT16S(storage, base, bw, x, y);
    case GS_PSM_T8: return ReadP8(storage, base, bw, x, y);
    case GS_PSM_T4: return ReadP4(storage, base, bw, x, y);
    case GS_PSM_T8H: return ReadP8H(storage, base, bw, x, y);
    case GS_PSM_T4HL: return ReadP4HL(storage, base, bw, x, y);
    case GS_PSM_T4HH: return ReadP4HH(storage, base, bw, x, y);
    case GS_PSM_Z32: return ReadZ32(storage, base, bw, x, y);
    case GS_PSM_Z24: return ReadZ24(storage, base, bw, x, y);
    case GS_PSM_Z16: return ReadZ16(storage, base, bw, x, y);
    case GS_PSM_Z16S: return ReadZ16S(storage, base, bw, x, y);
    default: return 0u;
    }
}

void GsVram::write(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) {
    if (!attached())
        return;
    using namespace GSMem;
    switch (psm & 0x3fu) {
    case GS_PSM_CT32: WriteCT32(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT24: WriteCT24(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT16: WriteCT16(m_data, base, bw, x, y, value); break;
    case GS_PSM_CT16S: WriteCT16S(m_data, base, bw, x, y, value); break;
    case GS_PSM_T8: WriteP8(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4: WriteP4(m_data, base, bw, x, y, value); break;
    case GS_PSM_T8H: WriteP8H(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4HL: WriteP4HL(m_data, base, bw, x, y, value); break;
    case GS_PSM_T4HH: WriteP4HH(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z32: WriteZ32(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z24: WriteZ24(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z16: WriteZ16(m_data, base, bw, x, y, value); break;
    case GS_PSM_Z16S: WriteZ16S(m_data, base, bw, x, y, value); break;
    default: break;
    }
}

} // namespace dq8::gfx
