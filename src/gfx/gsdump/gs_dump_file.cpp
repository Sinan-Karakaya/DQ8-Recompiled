#include "gs_dump_file.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>

#if defined(DQ8_GSDUMP_HAVE_ZSTD)
#include <zstd.h>
#endif
#if defined(DQ8_GSDUMP_HAVE_LZMA)
#include <lzma.h>
#endif

namespace dq8::gfx {
namespace {

bool endsWith(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Fallback for builds without libzstd/liblzma headers: stream the dump through
// the corresponding command-line tool. Fedora and Debian ship `zstd` and `xz`
// in the base install even when the -devel packages are absent, so this keeps
// the harness dependency-free without silently rejecting compressed dumps.
bool decompressViaPipe(const char *tool, const std::string &path, std::vector<uint8_t> &out,
                       std::string &error) {
    if (path.find('\'') != std::string::npos) {
        error = "refusing to shell out for a path containing a quote";
        return false;
    }
    const std::string cmd = std::string(tool) + " -dc '" + path + "'";
    std::FILE *pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        error = std::string("cannot run ") + tool;
        return false;
    }
    out.clear();
    std::vector<uint8_t> chunk(1u << 20);
    for (;;) {
        const size_t got = std::fread(chunk.data(), 1, chunk.size(), pipe);
        if (got == 0)
            break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + got);
    }
    const int rc = ::pclose(pipe);
    if (rc != 0 || out.empty()) {
        error = std::string(tool) + " failed on " + path;
        return false;
    }
    return true;
}

bool readWholeFile(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
    std::FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        error = "cannot open " + path;
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(fp);
        error = "empty file " + path;
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t got = std::fread(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    if (got != out.size()) {
        error = "short read on " + path;
        return false;
    }
    return true;
}

#if defined(DQ8_GSDUMP_HAVE_ZSTD)
bool decompressZstd(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, std::string &error) {
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!ds) {
        error = "ZSTD_createDStream failed";
        return false;
    }
    ZSTD_initDStream(ds);
    ZSTD_inBuffer inBuf{in.data(), in.size(), 0};
    std::vector<uint8_t> chunk(1u << 20);
    out.clear();
    while (inBuf.pos < inBuf.size) {
        ZSTD_outBuffer outBuf{chunk.data(), chunk.size(), 0};
        const size_t rc = ZSTD_decompressStream(ds, &outBuf, &inBuf);
        if (ZSTD_isError(rc)) {
            error = std::string("zstd: ") + ZSTD_getErrorName(rc);
            ZSTD_freeDStream(ds);
            return false;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + outBuf.pos);
        if (rc == 0 && inBuf.pos >= inBuf.size)
            break;
        if (outBuf.pos == 0 && rc != 0 && inBuf.pos >= inBuf.size)
            break;
    }
    ZSTD_freeDStream(ds);
    return true;
}
#endif

#if defined(DQ8_GSDUMP_HAVE_LZMA)
bool decompressXz(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, std::string &error) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) {
        error = "lzma_stream_decoder failed";
        return false;
    }
    std::vector<uint8_t> chunk(1u << 20);
    strm.next_in = in.data();
    strm.avail_in = in.size();
    out.clear();
    for (;;) {
        strm.next_out = chunk.data();
        strm.avail_out = chunk.size();
        const lzma_ret rc = lzma_code(&strm, strm.avail_in ? LZMA_RUN : LZMA_FINISH);
        out.insert(out.end(), chunk.begin(), chunk.end() - strm.avail_out);
        if (rc == LZMA_STREAM_END)
            break;
        if (rc != LZMA_OK) {
            error = "xz decode error " + std::to_string(static_cast<int>(rc));
            lzma_end(&strm);
            return false;
        }
    }
    lzma_end(&strm);
    return true;
}
#endif

template <typename T>
T loadLE(const uint8_t *p) {
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

} // namespace

const char *toString(GsPacketType t) {
    switch (t) {
    case GsPacketType::Transfer: return "Transfer";
    case GsPacketType::VSync: return "VSync";
    case GsPacketType::ReadFIFO2: return "ReadFIFO2";
    case GsPacketType::Registers: return "Registers";
    }
    return "?";
}

const char *toString(GsTransferPath p) {
    switch (p) {
    case GsTransferPath::Path1Old: return "PATH1(old)";
    case GsTransferPath::Path2: return "PATH2";
    case GsTransferPath::Path3: return "PATH3";
    case GsTransferPath::Path1New: return "PATH1(new)";
    case GsTransferPath::Dummy: return "dummy";
    }
    return "?";
}

bool GsDumpFile::load(const std::string &path, std::string &error) {
    std::vector<uint8_t> raw;
    if (!readWholeFile(path, raw, error))
        return false;

    std::vector<uint8_t> plain;
    if (endsWith(path, ".zst") || endsWith(path, ".zstd")) {
#if defined(DQ8_GSDUMP_HAVE_ZSTD)
        if (!decompressZstd(raw, plain, error))
            return false;
#else
        if (!decompressViaPipe("zstd", path, plain, error))
            return false;
#endif
    } else if (endsWith(path, ".xz")) {
#if defined(DQ8_GSDUMP_HAVE_LZMA)
        if (!decompressXz(raw, plain, error))
            return false;
#else
        if (!decompressViaPipe("xz", path, plain, error))
            return false;
#endif
    } else {
        plain = std::move(raw);
    }

    if (plain.size() < 8) {
        error = "truncated dump (no header)";
        return false;
    }

    size_t pos = 0;
    m_crc = loadLE<uint32_t>(plain.data() + pos);
    pos += 4;
    const uint32_t ss = loadLE<uint32_t>(plain.data() + pos);
    pos += 4;

    if (plain.size() < pos + ss) {
        error = "truncated dump (state block)";
        return false;
    }

    m_hasNewHeader = (m_crc == 0xFFFFFFFFu);
    if (m_hasNewHeader) {
        if (ss < sizeof(GsDumpHeader)) {
            error = "corrupt GSDumpHeader (too small)";
            return false;
        }
        std::memcpy(&m_header, plain.data() + pos, sizeof(GsDumpHeader));
        m_crc = m_header.crc;

        if (m_header.serialSize > 0 &&
            static_cast<uint64_t>(m_header.serialOffset) + m_header.serialSize <= ss) {
            m_serial.assign(reinterpret_cast<const char *>(plain.data() + pos + m_header.serialOffset),
                            m_header.serialSize);
        }
        const uint64_t shotEnd =
            static_cast<uint64_t>(m_header.screenshotOffset) + m_header.screenshotSize;
        if (m_header.screenshotSize > 0 && shotEnd <= ss &&
            m_header.screenshotSize >=
                static_cast<uint64_t>(m_header.screenshotWidth) * m_header.screenshotHeight * 4u) {
            m_screenshot.assign(plain.begin() + static_cast<long>(pos + m_header.screenshotOffset),
                                plain.begin() + static_cast<long>(pos + shotEnd));
        }

        pos += ss; // skip the secondary header block
        if (plain.size() < pos + m_header.stateSize) {
            error = "truncated dump (freeze state)";
            return false;
        }
        m_stateData.assign(plain.begin() + static_cast<long>(pos),
                           plain.begin() + static_cast<long>(pos + m_header.stateSize));
        pos += m_header.stateSize;
    } else {
        // Legacy layout: the leading word really is the CRC and `ss` is the
        // freeze-state size; no serial, no screenshot.
        m_stateData.assign(plain.begin() + static_cast<long>(pos),
                           plain.begin() + static_cast<long>(pos + ss));
        pos += ss;
    }

    if (plain.size() < pos + privreg::kSize) {
        error = "truncated dump (privileged registers)";
        return false;
    }
    m_regsData.assign(plain.begin() + static_cast<long>(pos),
                      plain.begin() + static_cast<long>(pos + privreg::kSize));
    pos += privreg::kSize;

    m_packetData.assign(plain.begin() + static_cast<long>(pos), plain.end());

    if (!decodeState(error))
        return false;
    return decodePackets(error);
}

bool GsDumpFile::decodeState(std::string &error) {
    m_freeze = GsFreezeState{};
    if (m_stateData.size() < 4)
        return true; // nothing to decode; replay will start from a reset GS

    const uint8_t *d = m_stateData.data();
    m_freeze.version = loadLE<uint32_t>(d);
    if (m_freeze.version != 9) {
        // Older/newer freeze layouts move the VRAM offset. Everything else in
        // the container is still usable, so this is a warning, not an error.
        error = "unsupported GS freeze state version " + std::to_string(m_freeze.version) +
                " (this reader knows version 9)";
        return true;
    }
    if (m_stateData.size() != freeze_v9::kTotalSize) {
        error = "freeze state is " + std::to_string(m_stateData.size()) + " bytes, expected " +
                std::to_string(freeze_v9::kTotalSize);
        return true;
    }

    size_t o = 4;
    auto reg = [&]() {
        const uint64_t v = loadLE<uint64_t>(d + o);
        o += 8;
        return v;
    };

    m_freeze.prim = reg();
    m_freeze.prmodecont = reg();
    m_freeze.texclut = reg();
    m_freeze.scanmsk = reg();
    m_freeze.texa = reg();
    m_freeze.fogcol = reg();
    m_freeze.dimx = reg();
    m_freeze.dthe = reg();
    m_freeze.colclamp = reg();
    m_freeze.pabe = reg();
    m_freeze.bitbltbuf = reg();
    m_freeze.trxdir = reg();
    m_freeze.trxpos = reg();
    m_freeze.trxreg = reg();
    reg(); // obsolete duplicate TRXREG

    for (auto &c : m_freeze.ctx) {
        c.xyoffset = reg();
        c.tex0 = reg();
        c.tex1 = reg();
        c.clamp = reg();
        c.miptbp1 = reg();
        c.miptbp2 = reg();
        c.scissor = reg();
        c.alpha = reg();
        c.test = reg();
        c.fba = reg();
        c.frame = reg();
        c.zbuf = reg();
    }

    // GSVertex packs UV and FOG as u32, not as 64-bit GIFRegs.
    m_freeze.rgbaq = reg();
    m_freeze.st = reg();
    m_freeze.uv = loadLE<uint32_t>(d + o);
    o += 4;
    m_freeze.fog = loadLE<uint32_t>(d + o);
    o += 4;
    m_freeze.xyz = reg();
    reg(); // obsolete GIFReg

    o += 4 + 4;          // m_tr.x, m_tr.y
    o += 4 + 4;          // m_tr.w, m_tr.h
    o += 8 + 8 + 8;      // m_tr.m_blit, m_pos, m_reg
    o += 16;             // m_tr.rect (GSVector4i)
    o += 4 + 4 + 4;      // m_tr.total, start, end
    o += 1;              // m_tr.write

    if (o != freeze_v9::kVramOffset) {
        error = "internal: computed VRAM offset " + std::to_string(o) + " != " +
                std::to_string(freeze_v9::kVramOffset);
        return false;
    }
    m_freeze.vramOffset = o;
    m_freeze.vramSize = freeze_v9::kVramSize;
    o += freeze_v9::kVramSize;
    o += freeze_v9::kPathBytes; // four GIF paths: GIFTag (16) + u32 register index
    if (o + 4 <= m_stateData.size())
        std::memcpy(&m_freeze.q, d + o, 4);
    m_freeze.valid = true;
    return true;
}

bool GsDumpFile::decodePackets(std::string &error) {
    m_packets.clear();
    size_t pos = 0;
    const size_t total = m_packetData.size();
    const uint8_t *d = m_packetData.data();

    while (pos < total) {
        GsPacket pkt{};
        pkt.type = static_cast<GsPacketType>(d[pos]);
        pos += 1;

        switch (pkt.type) {
        case GsPacketType::Transfer: {
            if (pos + 5 > total) {
                error = "truncated Transfer packet header";
                return false;
            }
            pkt.path = static_cast<GsTransferPath>(d[pos]);
            pos += 1;
            pkt.length = loadLE<uint32_t>(d + pos);
            pos += 4;
            break;
        }
        case GsPacketType::VSync:
            pkt.length = 1;
            break;
        case GsPacketType::ReadFIFO2:
            pkt.length = 4;
            break;
        case GsPacketType::Registers:
            pkt.length = static_cast<uint32_t>(privreg::kSize);
            break;
        default:
            error = "unknown packet type " + std::to_string(static_cast<unsigned>(pkt.type)) +
                    " at packet-stream offset " + std::to_string(pos - 1);
            return false;
        }

        if (pkt.length > 0) {
            if (total - pos < pkt.length) {
                // PCSX2 drops a trailing short packet rather than failing.
                break;
            }
            pkt.offset = pos;
            pos += pkt.length;
        }
        m_packets.push_back(pkt);
    }
    return true;
}

} // namespace dq8::gfx
