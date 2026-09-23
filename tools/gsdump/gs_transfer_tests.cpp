// GsTransferEngine against GSCpuBackend, on identical seeded local memory.
//
// The transfer paths are pure swizzle arithmetic with no device involved, so
// they are checked here rather than in the GPU tests: a failure in this file is
// a GS semantics bug, never a driver one.

#include "gfx/gs/gs_transfer.h"
#include "gfx/gs/gs_vram.h"

#include "runtime/gs/gs_cpu_backend.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kVramBytes = dq8::gfx::kGsVramBytes;

std::vector<uint8_t> seedVram(uint32_t salt) {
    std::vector<uint8_t> vram(kVramBytes);
    for (uint32_t i = 0u; i < kVramBytes; ++i)
        vram[i] = static_cast<uint8_t>((i * 37u) ^ (i >> 7u) ^ (i >> 17u) ^ salt);
    return vram;
}

GSTransferCommand makeTransfer(uint32_t direction,
                               uint32_t dbp, uint32_t dbw, uint8_t dpsm,
                               uint32_t dsax, uint32_t dsay,
                               uint32_t rrw, uint32_t rrh) {
    GSTransferCommand command{};
    command.direction = direction;
    command.bitbltbuf.dbp = dbp;
    command.bitbltbuf.dbw = static_cast<uint8_t>(dbw);
    command.bitbltbuf.dpsm = dpsm;
    command.trxpos.dsax = static_cast<uint16_t>(dsax);
    command.trxpos.dsay = static_cast<uint16_t>(dsay);
    command.trxreg.rrw = static_cast<uint16_t>(rrw);
    command.trxreg.rrh = static_cast<uint16_t>(rrh);
    return command;
}

// Every case runs the same script against both implementations and then
// compares all 4 MiB, so a transfer that writes outside its rectangle fails
// just as loudly as one that writes the wrong values inside it.
struct Harness {
    explicit Harness(uint32_t salt)
        : cpuVram(seedVram(salt)), gpuVram(seedVram(salt)), engine(vram) {
        cpu.Initialize(cpuVram.data(), kVramBytes);
        vram.attach(gpuVram.data(), kVramBytes);
    }

    void begin(const GSTransferCommand &command) {
        cpu.BeginTransfer(command);
        engine.begin(command);
    }

    void upload(const std::vector<uint8_t> &payload, size_t chunkSize) {
        for (size_t offset = 0u; offset < payload.size(); offset += chunkSize) {
            const uint32_t count = static_cast<uint32_t>(
                std::min(chunkSize, payload.size() - offset));
            cpu.UploadImage(payload.data() + offset, count);
            engine.upload(payload.data() + offset, count);
        }
    }

    bool compare(const char *caseName) {
        size_t differing = 0u;
        size_t first = kVramBytes;
        for (size_t i = 0u; i < kVramBytes; ++i) {
            if (cpuVram[i] != gpuVram[i]) {
                if (first == kVramBytes)
                    first = i;
                ++differing;
            }
        }
        if (differing != 0u) {
            std::fprintf(stderr,
                         "FAIL: %s: %zu VRAM bytes differ, first at 0x%zx "
                         "(engine=%02x cpu=%02x)\n",
                         caseName, differing, first, gpuVram[first], cpuVram[first]);
            return false;
        }
        return true;
    }

    std::vector<uint8_t> cpuVram;
    std::vector<uint8_t> gpuVram;
    GSCpuBackend cpu;
    dq8::gfx::GsVram vram;
    dq8::gfx::GsTransferEngine engine;
};

std::vector<uint8_t> makePayload(size_t bytes, uint32_t salt) {
    std::vector<uint8_t> payload(bytes);
    for (size_t i = 0u; i < bytes; ++i)
        payload[i] = static_cast<uint8_t>((i * 101u) ^ (i >> 5u) ^ salt);
    return payload;
}

struct FormatCase {
    const char *name;
    uint8_t psm;
    uint32_t bitsPerPixel;
};

// Every PSM a host->local transfer can name. T4 variants pack two pixels per
// byte, so their payload is half the pixel count.
constexpr FormatCase kFormats[] = {
    {"CT32", GS_PSM_CT32, 32u}, {"CT24", GS_PSM_CT24, 24u},
    {"CT16", GS_PSM_CT16, 16u}, {"CT16S", GS_PSM_CT16S, 16u},
    {"Z32", GS_PSM_Z32, 32u},   {"Z24", GS_PSM_Z24, 24u},
    {"Z16", GS_PSM_Z16, 16u},   {"Z16S", GS_PSM_Z16S, 16u},
    {"T8", GS_PSM_T8, 8u},      {"T8H", GS_PSM_T8H, 8u},
    {"T4", GS_PSM_T4, 4u},      {"T4HL", GS_PSM_T4HL, 4u},
    {"T4HH", GS_PSM_T4HH, 4u},
};

// Whole-payload uploads: the carry never engages, so the CPU oracle applies.
bool hostToLocalCase() {
    uint32_t salt = 0x11u;
    for (const FormatCase &format : kFormats) {
        Harness harness(salt++);
        constexpr uint32_t kWidth = 32u;
        constexpr uint32_t kHeight = 12u;
        const size_t payloadBytes =
            (static_cast<size_t>(kWidth) * kHeight * format.bitsPerPixel) / 8u;
        harness.begin(makeTransfer(0u, 96u << 5u, 2u, format.psm, 8u, 5u,
                                   kWidth, kHeight));
        harness.upload(makePayload(payloadBytes, format.psm), payloadBytes);

        const std::string name = std::string("host->local ") + format.name;
        if (!harness.compare(name.c_str()))
            return false;
    }
    return true;
}

// The same transfers arriving in several payloads, split only where a packed
// pixel ends. GSCpuBackend is a valid oracle exactly here: it `return`s the
// moment a payload cannot complete the next pixel, so any other split makes it
// drop bytes rather than carry them.
bool hostToLocalChunkedCase() {
    uint32_t salt = 0x51u;
    for (const FormatCase &format : kFormats) {
        // Below a byte per pixel every chunk boundary is pixel-aligned.
        const size_t pixelBytes = std::max<size_t>(format.bitsPerPixel / 8u, 1u);
        for (size_t pixelsPerChunk : {size_t{1u}, size_t{5u}, size_t{16u}}) {
            Harness harness(salt++);
            constexpr uint32_t kWidth = 16u;
            constexpr uint32_t kHeight = 9u;
            const size_t payloadBytes =
                (static_cast<size_t>(kWidth) * kHeight * format.bitsPerPixel) / 8u;
            harness.begin(makeTransfer(0u, 104u << 5u, 2u, format.psm, 3u, 2u,
                                       kWidth, kHeight));
            harness.upload(makePayload(payloadBytes, format.psm),
                           pixelBytes * pixelsPerChunk);

            const std::string name = std::string("host->local chunked ") +
                                     format.name + " pixels/chunk=" +
                                     std::to_string(pixelsPerChunk);
            if (!harness.compare(name.c_str()))
                return false;
        }
    }
    return true;
}

// GIF IMAGE data is one continuous byte stream, so a payload boundary can fall
// anywhere -- including inside a packed pixel. Splitting a transfer must not
// change what lands in local memory.
//
// No oracle here, because GSCpuBackend is the thing being improved on: it
// returns early on a partial pixel and drops those bytes for good. The
// invariant is checked against the engine's own whole-payload result, which
// hostToLocalCase has already matched against the oracle.
bool payloadCarryCase() {
    for (const FormatCase &format : kFormats) {
        constexpr uint32_t kBase = 120u << 5u;
        constexpr uint32_t kWidth = 14u;
        constexpr uint32_t kHeight = 6u;
        const size_t payloadBytes =
            (static_cast<size_t>(kWidth) * kHeight * format.bitsPerPixel) / 8u;
        const std::vector<uint8_t> payload = makePayload(payloadBytes, format.psm);
        const GSTransferCommand command =
            makeTransfer(0u, kBase, 2u, format.psm, 5u, 3u, kWidth, kHeight);

        std::vector<uint8_t> whole = seedVram(0x66u);
        {
            dq8::gfx::GsVram vram;
            vram.attach(whole.data(), kVramBytes);
            dq8::gfx::GsTransferEngine engine(vram);
            engine.begin(command);
            engine.upload(payload.data(), static_cast<uint32_t>(payload.size()));
        }

        for (size_t chunkSize : {size_t{1u}, size_t{3u}, size_t{7u}, size_t{13u}}) {
            std::vector<uint8_t> split = seedVram(0x66u);
            dq8::gfx::GsVram vram;
            vram.attach(split.data(), kVramBytes);
            dq8::gfx::GsTransferEngine engine(vram);
            engine.begin(command);
            for (size_t offset = 0u; offset < payload.size(); offset += chunkSize) {
                const uint32_t count = static_cast<uint32_t>(
                    std::min(chunkSize, payload.size() - offset));
                engine.upload(payload.data() + offset, count);
            }

            if (split != whole) {
                size_t first = 0u;
                while (first < split.size() && split[first] == whole[first])
                    ++first;
                std::fprintf(stderr,
                             "FAIL: payload carry %s chunk=%zu: differs from the "
                             "single-payload result at 0x%zx (%02x vs %02x)\n",
                             format.name, chunkSize, first, split[first], whole[first]);
                return false;
            }
        }
    }
    return true;
}

// The carry spelled out on known bytes, so a failure names the wrong pixel
// rather than just an offset. CT24 is the case that motivated it: three bytes
// per pixel means most split points land mid-pixel.
bool ct24CarryCase() {
    std::vector<uint8_t> vramBytes = seedVram(0x77u);
    dq8::gfx::GsVram vram;
    vram.attach(vramBytes.data(), kVramBytes);
    dq8::gfx::GsTransferEngine engine(vram);

    constexpr uint32_t kBase = 112u << 5u;
    constexpr uint32_t kWidth = 4u;
    constexpr uint32_t kHeight = 1u;
    engine.begin(makeTransfer(0u, kBase, 2u, GS_PSM_CT24, 0u, 0u, kWidth, kHeight));

    const std::vector<uint8_t> payload = {0x11u, 0x22u, 0x33u,  // pixel 0
                                          0x44u, 0x55u, 0x66u,  // pixel 1
                                          0x77u, 0x88u, 0x99u,  // pixel 2
                                          0xaau, 0xbbu, 0xccu}; // pixel 3
    // Split so pixel 1 straddles the boundary and pixel 3 straddles the next.
    engine.upload(payload.data(), 4u);
    engine.upload(payload.data() + 4u, 6u);
    engine.upload(payload.data() + 10u, 2u);

    const uint32_t expected[kWidth] = {0x332211u, 0x665544u, 0x998877u, 0xccbbaau};
    for (uint32_t x = 0u; x < kWidth; ++x) {
        const uint32_t actual = vram.read(GS_PSM_CT24, kBase, 2u, x, 0u) & 0x00ffffffu;
        if (actual != expected[x]) {
            std::fprintf(stderr,
                         "FAIL: CT24 payload carry: pixel %u is %06x, expected %06x\n",
                         x, actual, expected[x]);
            return false;
        }
    }
    return true;
}

bool localToLocalCase() {
    uint32_t salt = 0x22u;
    // All four TRXPOS.DIR scan orders, over rectangles that overlap.
    for (uint32_t dir = 0u; dir < 4u; ++dir) {
        Harness harness(salt++);
        GSTransferCommand command = makeTransfer(2u, 128u << 5u, 2u, GS_PSM_CT32,
                                                 4u, 6u, 24u, 18u);
        command.bitbltbuf.sbp = 128u << 5u;
        command.bitbltbuf.sbw = 2u;
        command.bitbltbuf.spsm = GS_PSM_CT32;
        command.trxpos.ssax = 10u;
        command.trxpos.ssay = 12u;
        command.trxpos.dir = static_cast<uint8_t>(dir);
        harness.begin(command);

        const std::string name = "local->local dir=" + std::to_string(dir);
        if (!harness.compare(name.c_str()))
            return false;
    }

    // A format-converting blit, which is how games move a rendered buffer into
    // a texture of a different depth.
    Harness harness(salt++);
    GSTransferCommand command = makeTransfer(2u, 160u << 5u, 2u, GS_PSM_CT16,
                                             0u, 0u, 32u, 16u);
    command.bitbltbuf.sbp = 144u << 5u;
    command.bitbltbuf.sbw = 2u;
    command.bitbltbuf.spsm = GS_PSM_CT32;
    harness.begin(command);
    return harness.compare("local->local CT32->CT16");
}

bool localToHostCase() {
    uint32_t salt = 0x33u;
    for (const FormatCase &format : kFormats) {
        Harness harness(salt++);
        constexpr uint32_t kWidth = 20u;
        constexpr uint32_t kHeight = 7u;
        GSTransferCommand command = makeTransfer(1u, 0u, 1u, GS_PSM_CT32,
                                                 0u, 0u, kWidth, kHeight);
        command.bitbltbuf.sbp = 176u << 5u;
        command.bitbltbuf.sbw = 2u;
        command.bitbltbuf.spsm = format.psm;
        command.trxpos.ssax = 6u;
        command.trxpos.ssay = 3u;
        harness.begin(command);

        // Drained in small chunks: partial consumption is the normal case,
        // since the guest reads the FIFO a qword at a time.
        std::vector<uint8_t> cpuBytes;
        std::vector<uint8_t> engineBytes;
        for (;;) {
            uint8_t buffer[7]{};
            const uint32_t taken = harness.cpu.ConsumeLocalToHostBytes(buffer, sizeof(buffer));
            if (taken == 0u)
                break;
            cpuBytes.insert(cpuBytes.end(), buffer, buffer + taken);
        }
        for (;;) {
            uint8_t buffer[5]{};
            const uint32_t taken = harness.engine.consumeLocalToHost(buffer, sizeof(buffer));
            if (taken == 0u)
                break;
            engineBytes.insert(engineBytes.end(), buffer, buffer + taken);
        }

        if (cpuBytes != engineBytes) {
            std::fprintf(stderr,
                         "FAIL: local->host %s: %zu bytes vs oracle %zu\n",
                         format.name, engineBytes.size(), cpuBytes.size());
            return false;
        }
        if (engineBytes.empty()) {
            std::fprintf(stderr, "FAIL: local->host %s produced no bytes\n", format.name);
            return false;
        }
    }
    return true;
}

// Page marking drives cache invalidation, so an under-marked range is a
// silently stale texture. Checked against pixel addresses rather than itself.
bool pageMarkingCase() {
    using namespace dq8::gfx;
    std::vector<uint8_t> vramBytes(kVramBytes, 0u);
    GsVram vram;
    vram.attach(vramBytes.data(), kVramBytes);

    struct Region {
        const char *name;
        uint32_t psm;
        uint32_t base;
        uint32_t bw;
        uint32_t width;
        uint32_t height;
    };
    constexpr Region kRegions[] = {
        {"CT32 640x448", GS_PSM_CT32, 0u, 10u, 640u, 448u},
        {"CT16 512x256", GS_PSM_CT16, 64u << 5u, 8u, 512u, 256u},
        {"T8 256x256", GS_PSM_T8, 200u << 5u, 4u, 256u, 256u},
        {"T4 128x128", GS_PSM_T4, 300u << 5u, 2u, 128u, 128u},
    };

    for (const Region &region : kRegions) {
        GsPageSet marked;
        gsMarkPages(marked, region.base, region.bw, region.psm,
                    region.width, region.height);

        // Write a sentinel to every pixel, then confirm each touched byte is in
        // a page the marker claimed.
        std::fill(vramBytes.begin(), vramBytes.end(), 0u);
        for (uint32_t y = 0u; y < region.height; ++y)
            for (uint32_t x = 0u; x < region.width; ++x)
                vram.write(region.psm, region.base, region.bw, x, y, 0xffffffffu);

        for (uint32_t byte = 0u; byte < kVramBytes; ++byte) {
            if (vramBytes[byte] == 0u)
                continue;
            const uint32_t page = byte / kGsPageBytes;
            if (!marked.test(page)) {
                std::fprintf(stderr,
                             "FAIL: page marking %s: byte 0x%x (page %u) written "
                             "but not marked\n",
                             region.name, byte, page);
                return false;
            }
        }
    }
    return true;
}

bool offsetPageMarkingCase() {
    using namespace dq8::gfx;
    std::vector<uint8_t> bytes(kVramBytes);
    GsVram vram;
    vram.attach(bytes.data(), bytes.size());
    for (const auto &format : kFormats) {
        for (uint32_t base : {0u, 31u, 16383u}) {
            for (const auto origin : {std::pair{60u, 30u}, std::pair{496u, 432u}}) {
                std::fill(bytes.begin(), bytes.end(), 0);
                GsPageSet pages;
                gsMarkPages(pages, base, 8, format.psm, 16, 16, origin.first, origin.second);
                for (uint32_t y = origin.second; y < origin.second + 16; ++y)
                    for (uint32_t x = origin.first; x < origin.first + 16; ++x)
                        vram.write(format.psm, base, 8, x, y, 0xffffffffu);
                for (uint32_t i = 0; i < bytes.size(); ++i)
                    if (bytes[i] && !pages.test(i / kGsPageBytes)) {
                        std::fprintf(stderr, "FAIL: offset page marking %s base=%u byte=%u\n",
                                     format.name, base, i);
                        return false;
                    }
            }
        }
    }
    GsPageSet pages;
    gsMarkPages(pages, 0, 8, GS_PSM_CT32, 16, 16, 496, 432);
    if (pages.count() != 1 || !pages.test(111)) {
        std::fprintf(stderr, "FAIL: movie tile invalidated unrelated pages\n");
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!hostToLocalCase() || !hostToLocalChunkedCase() || !payloadCarryCase() ||
        !ct24CarryCase() || !localToLocalCase() || !localToHostCase() ||
        !pageMarkingCase() || !offsetPageMarkingCase())
        return 1;
    std::printf("PASS: GS transfer engine matches the CPU oracle\n");
    return 0;
}
