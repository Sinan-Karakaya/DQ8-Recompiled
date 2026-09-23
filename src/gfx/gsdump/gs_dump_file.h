// PCSX2 .gs dump container reader.
//
// Format reference (PCSX2 v2.x, GS state version 9):
//   pcsx2/GS/GSDump.cpp      GSDumpBase::AddHeader / Transfer / ReadFIFO / VSync
//   pcsx2/GS/GSLzma.cpp      GSDumpFile::ReadFile
//   pcsx2/GS/GSState.cpp     GSState::Freeze  (layout of the "state" blob)
//   pcsx2/GSDumpReplayer.cpp GSDumpReplayerCpuStep (replay semantics)
//
// Fields are little-endian and packed without alignment padding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dq8::gfx {

// GSDumpTypes::GSType (GSLzma.h)
enum class GsPacketType : uint8_t {
    Transfer = 0,
    VSync = 1,
    ReadFIFO2 = 2,
    Registers = 3,
};

// GSDumpTypes::GSTransferPath (GSLzma.h). The numbering follows PCSX2's
// GSState::Transfer<index> template argument, so 0 and 3 are both PATH1.
enum class GsTransferPath : uint8_t {
    Path1Old = 0,
    Path2 = 1,
    Path3 = 2,
    Path1New = 3,
    Dummy = 4,
};

const char *toString(GsPacketType t);
const char *toString(GsTransferPath p);

struct GsPacket {
    GsPacketType type = GsPacketType::Transfer;
    GsTransferPath path = GsTransferPath::Dummy;
    // Offset/length into GsDumpFile::packetData(). For ReadFIFO2 the payload is
    // the 4-byte qword count; for VSync it is the 1-byte field index; for
    // Registers it is a full 8192-byte GSPrivRegSet image.
    size_t offset = 0;
    uint32_t length = 0;
};

// pcsx2/GS/GSDump.h struct GSDumpHeader, 36 bytes, only present when the
// leading CRC word reads 0xFFFFFFFF ("new header" marker).
struct GsDumpHeader {
    uint32_t stateVersion = 0;
    uint32_t stateSize = 0;
    uint32_t serialOffset = 0;
    uint32_t serialSize = 0;
    uint32_t crc = 0;
    uint32_t screenshotWidth = 0;
    uint32_t screenshotHeight = 0;
    uint32_t screenshotOffset = 0;
    uint32_t screenshotSize = 0;
};
static_assert(sizeof(GsDumpHeader) == 36, "GSDumpHeader is 9 packed u32s");

// Decoded view of the GS "freeze" blob written by GSState::Freeze. Only the
// parts a replay needs are broken out; the rest is preserved verbatim.
struct GsFreezeState {
    uint32_t version = 0;
    bool valid = false;

    // Global environment registers, as raw 64-bit GIFReg values (i.e. exactly
    // what would be written to the matching GS register address).
    uint64_t prim = 0;
    uint64_t prmodecont = 0;
    uint64_t texclut = 0;
    uint64_t scanmsk = 0;
    uint64_t texa = 0;
    uint64_t fogcol = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    uint64_t pabe = 0;
    uint64_t bitbltbuf = 0;
    uint64_t trxdir = 0;
    uint64_t trxpos = 0;
    uint64_t trxreg = 0;

    // Per-context registers, ctx[0] == context 1, ctx[1] == context 2.
    struct Context {
        uint64_t xyoffset = 0;
        uint64_t tex0 = 0;
        uint64_t tex1 = 0;
        uint64_t clamp = 0;
        uint64_t miptbp1 = 0;
        uint64_t miptbp2 = 0;
        uint64_t scissor = 0;
        uint64_t alpha = 0;
        uint64_t test = 0;
        uint64_t fba = 0;
        uint64_t frame = 0;
        uint64_t zbuf = 0;
    } ctx[2];

    // Latched vertex attributes.
    uint64_t rgbaq = 0;
    uint64_t st = 0;
    uint64_t uv = 0;
    uint64_t fog = 0;
    uint64_t xyz = 0;
    float q = 1.0f;

    // Offset/size of the 4 MiB GS local memory image inside the state blob.
    size_t vramOffset = 0;
    size_t vramSize = 0;
};

// Byte offsets inside the GSState::Freeze blob for state version 9. Derived
// from GSState::GetSaveStateSize(); every GIFReg is 8 bytes, GSVector4i is 16.
// Verified byte-for-byte against a real PCSX2 v2.6.3 dump of SLUS-21207:
// state_size in the header reads 4194813, which is exactly
//   4 (version)
// + 15*8 (PRIM..TRXREG, incl. the obsolete duplicate TRXREG)
// + 2*12*8 (per-context registers)
// + 8+8+4+4+8 (m_v RGBAQ, ST, UV(u32), FOG(u32), XYZ)  <- UV and FOG are u32
// + 8 (obsolete GIFReg)
// + 69 (m_tr: 4*int + 3*GIFReg + GSVector4i + 3*int + bool)
// = 425, then 4 MiB of GS local memory, then 4*20 (GIFTag+u32 per GIF path)
// and a trailing float m_q.
namespace freeze_v9 {
inline constexpr size_t kVramOffset = 425;
inline constexpr size_t kVramSize = 4u * 1024u * 1024u;
inline constexpr size_t kPathBytes = 4u * 20u;
inline constexpr size_t kTotalSize = kVramOffset + kVramSize + kPathBytes + 4u; // 4194813
} // namespace freeze_v9

class GsDumpFile {
public:
    // Reads a .gs / .gs.xz / .gs.zst dump. xz and zst support depend on
    // DQ8_GSDUMP_HAVE_LZMA / DQ8_GSDUMP_HAVE_ZSTD being compiled in; without
    // them a compressed dump is reported as an error rather than mis-parsed.
    bool load(const std::string &path, std::string &error);

    uint32_t crc() const { return m_crc; }
    const std::string &serial() const { return m_serial; }
    bool hasNewHeader() const { return m_hasNewHeader; }
    const GsDumpHeader &header() const { return m_header; }

    // Embedded screenshot (RGBA8, top-down) captured by PCSX2 at dump time.
    // Empty when the dump has no screenshot.
    const std::vector<uint8_t> &screenshotRgba() const { return m_screenshot; }
    uint32_t screenshotWidth() const { return m_header.screenshotWidth; }
    uint32_t screenshotHeight() const { return m_header.screenshotHeight; }

    const std::vector<uint8_t> &stateData() const { return m_stateData; }
    const GsFreezeState &freeze() const { return m_freeze; }

    // 8192-byte GSPrivRegSet image (privileged registers) captured with the
    // state. Register n sits at its hardware offset from 0x12000000.
    const std::vector<uint8_t> &regsData() const { return m_regsData; }

    const std::vector<uint8_t> &packetData() const { return m_packetData; }
    const std::vector<GsPacket> &packets() const { return m_packets; }

    const uint8_t *payload(const GsPacket &p) const {
        return m_packetData.data() + p.offset;
    }

private:
    bool decodeState(std::string &error);
    bool decodePackets(std::string &error);

    uint32_t m_crc = 0;
    bool m_hasNewHeader = false;
    GsDumpHeader m_header{};
    std::string m_serial;
    std::vector<uint8_t> m_screenshot;
    std::vector<uint8_t> m_stateData;
    std::vector<uint8_t> m_regsData;
    std::vector<uint8_t> m_packetData;
    std::vector<GsPacket> m_packets;
    GsFreezeState m_freeze{};
};

// Privileged register byte offsets inside the 8192-byte GSPrivRegSet blob.
// These match the hardware addresses minus 0x12000000 (GSRegs.h).
namespace privreg {
inline constexpr size_t kPMODE = 0x0000;
inline constexpr size_t kSMODE1 = 0x0010;
inline constexpr size_t kSMODE2 = 0x0020;
inline constexpr size_t kSRFSH = 0x0030;
inline constexpr size_t kSYNCH1 = 0x0040;
inline constexpr size_t kSYNCH2 = 0x0050;
inline constexpr size_t kSYNCV = 0x0060;
inline constexpr size_t kDISPFB1 = 0x0070;
inline constexpr size_t kDISPLAY1 = 0x0080;
inline constexpr size_t kDISPFB2 = 0x0090;
inline constexpr size_t kDISPLAY2 = 0x00A0;
inline constexpr size_t kEXTBUF = 0x00B0;
inline constexpr size_t kEXTDATA = 0x00C0;
inline constexpr size_t kEXTWRITE = 0x00D0;
inline constexpr size_t kBGCOLOR = 0x00E0;
inline constexpr size_t kCSR = 0x1000;
inline constexpr size_t kIMR = 0x1010;
inline constexpr size_t kBUSDIR = 0x1040;
inline constexpr size_t kSIGLBLID = 0x1080;
inline constexpr size_t kSize = 0x2000;
} // namespace privreg

} // namespace dq8::gfx
