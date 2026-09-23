// Offline replay of a PCSX2 .gs dump through ps2xRuntime's GS frontend.
//
// Runs independently of the game through the GS frontend and raster backend.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gs_dump_file.h"
#include "gs_frame_image.h"

namespace dq8::gfx {

enum class ReplayBackend : uint8_t {
    Software,
    SdlGpu,
};

struct ReplayOptions {
    // Which raster backend the frontend drives. Software is the reference.
    ReplayBackend backend = ReplayBackend::Software;
    // Internal render resolution for the hardware backend, as a multiple of
    // the GS's own. Ignored by the software backend, which has no such notion.
    uint32_t resolutionScale = 1;
    // Stop after this many VSyncs have been presented (0 = replay everything).
    uint32_t maxFrames = 0;
    // Replay the packet stream this many times, carrying GS local memory and
    // registers over. A dump's first pass starts from the freeze state, whose
    // display buffer is usually mid-flip and therefore blank; a second pass
    // sees the buffers the dump itself filled, which is what PCSX2's own
    // looping replayer converges to.
    uint32_t loops = 1;
    // Seed GS local memory from the dump's freeze state. Required for
    // single-frame dumps, whose textures were uploaded before the dump began.
    bool seedVram = true;
    // Seed the GS drawing environment (context registers, vertex latches).
    bool seedRegisters = true;
    // Emit a per-packet trace on stderr.
    bool verbose = false;
    // When set, the 4 MiB GS local memory is written here after the replay ends
    // (raw, in hardware swizzle) -- the quickest way to tell "nothing drew"
    // apart from "drew into a buffer the CRTC is not showing".
    std::string vramDumpPath;
    // Record backend commands after the first presentation for draw-level probes.
    std::string backendTracePath;
    // Feed only these transfer paths (bitmask over 1<<GsTransferPath). Default:
    // every path. Useful to isolate PATH3 (EE->GIF) from PATH1 (VU1 XGKICK).
    uint32_t pathMask = 0xFFFFFFFFu;
};

struct ReplayStats {
    uint64_t transfers = 0;
    uint64_t transferBytes = 0;
    uint64_t transfersByPath[5] = {0, 0, 0, 0, 0};
    uint64_t vsyncs = 0;
    uint64_t readFifos = 0;
    uint64_t registerPackets = 0;
    uint64_t framesPresented = 0;
    uint64_t framesEmpty = 0;
    uint32_t loopsRun = 0;
    // Index into the returned frame vector of the last frame with any non-black
    // pixel, or -1 when every presented frame was blank.
    int64_t lastNonBlankFrame = -1;
    // Filled in when the hardware backend ran, for --verbose reporting.
    std::string backendReport;
};

// Replays `dump`, returning one FrameImage per presented VSync (already
// de-strided from ps2xRuntime's fixed 640-wide presentation buffer).
bool replayDump(const GsDumpFile &dump, const ReplayOptions &options,
                std::vector<FrameImage> &frames, ReplayStats &stats, std::string &error);

// Extracts PCSX2's own screenshot from the dump header as a FrameImage.
// This is the cheapest available reference image, but note it is PCSX2's
// *renderer output* (possibly upscaled/post-processed), not a native-res
// software-GS reference. Returns false when the dump carries no screenshot.
bool extractScreenshot(const GsDumpFile &dump, FrameImage &out);

} // namespace dq8::gfx
