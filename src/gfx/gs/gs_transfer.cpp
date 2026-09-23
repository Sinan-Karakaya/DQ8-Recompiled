#include "gfx/gs/gs_transfer.h"

#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <cstring>

namespace dq8::gfx {

namespace {

// TRXDIR values.
constexpr uint32_t kHostToLocal = 0u;
constexpr uint32_t kLocalToHost = 1u;
constexpr uint32_t kLocalToLocal = 2u;
constexpr uint32_t kTransferIdle = 3u;

} // namespace

GsTransferEngine::GsTransferEngine(GsVram &vram)
    : m_vram(vram) {
    m_state.direction = kTransferIdle;
}

void GsTransferEngine::reset() {
    m_command = {};
    m_state = {};
    m_state.direction = kTransferIdle;
    m_partialPixelBytes = 0u;
    m_localToHost.clear();
    m_localToHostReadPos = 0u;
}

// Movie tiles must invalidate their own pages, not the rectangle back to (0,0).
GsPageSet GsTransferEngine::destinationPages() const {
    GsPageSet pages;
    gsMarkPages(pages, m_command.bitbltbuf.dbp,
                std::max<uint32_t>(m_command.bitbltbuf.dbw, 1u),
                m_command.bitbltbuf.dpsm,
                m_command.trxreg.rrw, m_command.trxreg.rrh,
                m_command.trxpos.dsax, m_command.trxpos.dsay);
    return pages;
}

GsPageSet GsTransferEngine::sourcePages() const {
    GsPageSet pages;
    gsMarkPages(pages, m_command.bitbltbuf.sbp,
                std::max<uint32_t>(m_command.bitbltbuf.sbw, 1u),
                m_command.bitbltbuf.spsm,
                m_command.trxreg.rrw, m_command.trxreg.rrh,
                m_command.trxpos.ssax, m_command.trxpos.ssay);
    return pages;
}

void GsTransferEngine::begin(const GSTransferCommand &command) {
    m_command = command;
    m_partialPixelBytes = 0u;
    m_state.x = command.trxpos.dsax;
    m_state.y = command.trxpos.dsay;
    m_state.totalPixels =
        static_cast<uint32_t>(command.trxreg.rrw) * command.trxreg.rrh;
    m_state.copiedPixels = 0u;
    m_state.direction = command.direction;
    m_state.localToHostPendingBytes = 0u;

    if (command.direction == kLocalToLocal)
        performLocalToLocal();
    else if (command.direction == kLocalToHost)
        performLocalToHost();
}

void GsTransferEngine::performLocalToLocal() {
    const uint32_t rrw = m_command.trxreg.rrw;
    const uint32_t rrh = m_command.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u || !m_vram.attached()) {
        m_state.direction = kTransferIdle;
        return;
    }

    if (m_resolve) {
        GsPageSet touched = sourcePages();
        touched |= destinationPages();
        m_resolve(touched);
    }

    const uint32_t sbw = std::max<uint32_t>(m_command.bitbltbuf.sbw, 1u);
    const uint32_t dbw = std::max<uint32_t>(m_command.bitbltbuf.dbw, 1u);
    for (uint32_t pixel = 0u; pixel < total; ++pixel) {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        // TRXPOS.DIR selects the scan order, which matters when source and
        // destination rectangles overlap.
        if ((m_command.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_command.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value =
            m_vram.read(m_command.bitbltbuf.spsm, m_command.bitbltbuf.sbp, sbw,
                        x + m_command.trxpos.ssax, y + m_command.trxpos.ssay);
        m_vram.write(m_command.bitbltbuf.dpsm, m_command.bitbltbuf.dbp, dbw,
                     x + m_command.trxpos.dsax, y + m_command.trxpos.dsay, value);
    }

    m_state.copiedPixels = total;
    m_state.direction = kTransferIdle;
    if (m_invalidate)
        m_invalidate(destinationPages());
}

void GsTransferEngine::performLocalToHost() {
    m_localToHost.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram.attached())
        return;

    if (m_resolve)
        m_resolve(sourcePages());

    const uint32_t rrw = m_command.trxreg.rrw;
    const uint32_t rrh = m_command.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_command.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_command.bitbltbuf.spsm;
    const uint32_t bpp = gsBitsPerPixel(spsm);
    const uint32_t total = rrw * rrh;
    m_localToHost.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel) {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = m_vram.read(spsm, m_command.bitbltbuf.sbp, sbw,
                                           x + m_command.trxpos.ssax,
                                           y + m_command.trxpos.ssay);
        switch (bpp) {
        case 32:
            m_localToHost.push_back(static_cast<uint8_t>(value));
            m_localToHost.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHost.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHost.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHost.push_back(static_cast<uint8_t>(value));
            m_localToHost.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHost.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHost.push_back(static_cast<uint8_t>(value));
            m_localToHost.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHost.push_back(static_cast<uint8_t>(value));
            break;
        case 4: {
            // Two indices per byte, low nibble first.
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total) {
                const uint32_t nextPixel = pixel + 1u;
                next = m_vram.read(spsm, m_command.bitbltbuf.sbp, sbw,
                                   (nextPixel % rrw) + m_command.trxpos.ssax,
                                   (nextPixel / rrw) + m_command.trxpos.ssay);
            }
            m_localToHost.push_back(
                static_cast<uint8_t>((value & 0x0fu) | ((next & 0x0fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_state.copiedPixels = total;
    m_state.localToHostPendingBytes = m_localToHost.size();
}

void GsTransferEngine::upload(const uint8_t *data, uint32_t sizeBytes) {
    if (!data || sizeBytes == 0u || !m_vram.attached() ||
        m_state.direction != kHostToLocal)
        return;
    if (m_command.trxreg.rrw == 0u || m_command.trxreg.rrh == 0u ||
        m_state.totalPixels == 0u)
        return;

    const uint32_t dbp = m_command.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_command.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_command.bitbltbuf.dpsm;
    const uint32_t rrw = m_command.trxreg.rrw;
    const uint32_t dsax = m_command.trxpos.dsax;
    const uint32_t dsay = m_command.trxpos.dsay;
    uint32_t offset = 0u;
    bool wrote = false;

    // Completes one packed pixel from the carry plus the incoming payload,
    // returning false when this payload ends mid-pixel.
    auto gatherPixel = [&](uint32_t requiredBytes) {
        while (m_partialPixelBytes < requiredBytes && offset < sizeBytes)
            m_partialPixel[m_partialPixelBytes++] = data[offset++];
        return m_partialPixelBytes == requiredBytes;
    };

    auto advance = [&](uint32_t count) {
        const uint32_t totalPixels = m_state.totalPixels;
        m_state.copiedPixels = std::min<uint32_t>(totalPixels, m_state.copiedPixels + count);
        if (m_state.copiedPixels >= totalPixels) {
            m_state.direction = kTransferIdle;
            m_state.totalPixels = 0u;
            return;
        }
        m_state.x = dsax + (m_state.copiedPixels % rrw);
        m_state.y = dsay + (m_state.copiedPixels / rrw);
    };

    // Bulk path for whole 32-bit pixels. The general loop below costs a byte-at-
    // a-time gather, an out-of-line switch on the PSM and two integer divisions
    // per pixel; DQ8's movie uploads a frame as 896 of these transfers, which
    // made it the hottest thing in the runtime once the scheduler stopped
    // throwing. Same writes, same order, just without the per-pixel overhead.
    if ((dpsm == GS_PSM_CT32 || dpsm == GS_PSM_Z32) && m_partialPixelBytes == 0u &&
        sizeBytes >= 4u) {
        // One call for the whole run: the per-pixel entry points are out-of-line
        // in another translation unit, so calling them 256 times per tile costs
        // 256 cross-TU calls and inlines nothing. CT32 and Z32 are both four
        // bytes per pixel but do not share a swizzle.
        const uint32_t remaining = m_state.totalPixels - m_state.copiedPixels;
        const uint32_t available = std::min<uint32_t>(sizeBytes / 4u, remaining);
        if (available != 0u) {
            const uint32_t rowEnd = dsax + rrw;
            if (dpsm == GS_PSM_CT32) {
                GSMem::WriteRunCT32(m_vram.data(), dbp, dbw, dsax, rowEnd, m_state.x, m_state.y,
                                    data + offset, available);
            } else {
                GSMem::WriteRunZ32(m_vram.data(), dbp, dbw, dsax, rowEnd, m_state.x, m_state.y,
                                   data + offset, available);
            }
            offset += available * 4u;
            wrote = true;
            advance(available);
        }
    }

    while (offset < sizeBytes && m_state.direction == kHostToLocal) {
        switch (dpsm) {
        case GS_PSM_CT32:
        case GS_PSM_Z32: {
            if (!gatherPixel(4u))
                break;
            uint32_t value = 0u;
            std::memcpy(&value, m_partialPixel.data(), sizeof(value));
            m_partialPixelBytes = 0u;
            m_vram.write(dpsm, dbp, dbw, m_state.x, m_state.y, value);
            advance(1u);
            wrote = true;
            continue;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24: {
            if (!gatherPixel(3u))
                break;
            const uint32_t value = static_cast<uint32_t>(m_partialPixel[0]) |
                                   (static_cast<uint32_t>(m_partialPixel[1]) << 8u) |
                                   (static_cast<uint32_t>(m_partialPixel[2]) << 16u);
            m_partialPixelBytes = 0u;
            m_vram.write(dpsm, dbp, dbw, m_state.x, m_state.y, value);
            advance(1u);
            wrote = true;
            continue;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S: {
            if (!gatherPixel(2u))
                break;
            uint16_t value = 0u;
            std::memcpy(&value, m_partialPixel.data(), sizeof(value));
            m_partialPixelBytes = 0u;
            m_vram.write(dpsm, dbp, dbw, m_state.x, m_state.y, value);
            advance(1u);
            wrote = true;
            continue;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            m_vram.write(dpsm, dbp, dbw, m_state.x, m_state.y, data[offset++]);
            advance(1u);
            wrote = true;
            continue;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH: {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_state.copiedPixels;
            m_vram.write(dpsm, dbp, dbw, dsax + (firstPixel % rrw),
                         dsay + (firstPixel / rrw), packed & 0x0fu);
            if (firstPixel + 1u < m_state.totalPixels) {
                const uint32_t secondPixel = firstPixel + 1u;
                m_vram.write(dpsm, dbp, dbw, dsax + (secondPixel % rrw),
                             dsay + (secondPixel / rrw), (packed >> 4u) & 0x0fu);
            }
            advance(std::min<uint32_t>(2u, m_state.totalPixels - firstPixel));
            wrote = true;
            continue;
        }
        default:
            break;
        }
        // Either the PSM is unsupported or this payload ended mid-pixel; the
        // carry keeps the bytes and the destination position does not move.
        break;
    }

    if (wrote && m_invalidate)
        m_invalidate(destinationPages());

    // DQ8_GS_UPLOAD_TRACE=N reports how upload() is actually being fed: the
    // per-call payload decides whether the cost is the pixel writes or the
    // per-call bookkeeping around them.
    static const uint64_t traceEvery = [] {
        const char *value = std::getenv("DQ8_GS_UPLOAD_TRACE");
        const long long parsed = value ? std::atoll(value) : 0;
        return parsed > 0 ? static_cast<uint64_t>(parsed) : 0ull;
    }();
    if (traceEvery != 0u) {
        static uint64_t calls = 0u;
        static uint64_t bytes = 0u;
        ++calls;
        bytes += sizeBytes;
        if ((calls % traceEvery) == 0u)
            std::fprintf(stderr, "[gs] upload calls=%llu avg-bytes=%llu\n",
                         static_cast<unsigned long long>(calls),
                         static_cast<unsigned long long>(bytes / calls));
    }
}

uint32_t GsTransferEngine::consumeLocalToHost(uint8_t *dst, uint32_t maxBytes) {
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHost.size())
        return 0u;
    const size_t count =
        std::min<size_t>(maxBytes, m_localToHost.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHost.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_state.localToHostPendingBytes = m_localToHost.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

} // namespace dq8::gfx
