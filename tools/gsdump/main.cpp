// dq8-gsreplay -- replay a PCSX2 .gs dump offline through ps2xRuntime's GS
// frontend and software rasteriser, and diff the result against a reference.
//
//   dq8-gsreplay info    dump.gs
//   dq8-gsreplay replay  dump.gs --out out/frame [--loops N] [--verbose]
//   dq8-gsreplay shot    dump.gs --out out/pcsx2      (extract PCSX2 screenshot)
//   dq8-gsreplay diff    a.raw32 b.raw32 [--fit a|b] [--tolerance N] [--heatmap h.png]
//                        [--max-diff-fraction F] [--min-psnr D] [--ignore-alpha]
//
// `replay` and `shot` write <out>.raw32 (canonical, for diffing) and
// <out>.png (for looking at). `diff` exits non-zero when a threshold is
// exceeded, so it drops straight into CI.
//
// `replay` defaults to the *last* presented frame: a dump's first VSyncs show
// the freeze state's display buffer, which is normally mid-flip and blank.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gs_dump_file.h"
#include "gs_frame_image.h"
#include "gs_replay.h"

using namespace dq8::gfx;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  dq8-gsreplay info   <dump.gs[.zst|.xz]>\n"
                 "  dq8-gsreplay replay <dump.gs> --out <prefix> [--loops N] [--frames N]\n"
                 "                      [--frame N] [--first] [--last] [--all]\n"
                 "                      [--no-seed-vram] [--no-seed-regs] [--paths LIST]\n"
                 "                      [--require-content] [--dump-vram <file>] [--verbose]\n"
                 "                      [--trace-out <backend.trace>]\n"
                 "                      [--gs sw|sdlgpu] [--scale N]\n"
                 "  dq8-gsreplay shot   <dump.gs> --out <prefix>\n"
                 "  dq8-gsreplay diff   <a.raw32> <b.raw32> [--fit a|b] [--tolerance N]\n"
                 "                      [--ignore-alpha] [--heatmap <file.png>]\n"
                 "                      [--max-diff-fraction F] [--min-psnr D]\n"
                 "\n"
                 "replay writes the last presented frame by default; --all writes every one.\n"
                 "diff --fit resamples the named image onto the other's grid (area average).\n");
    return 2;
}

const char *argValue(int argc, char **argv, int &i) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", argv[i]);
        std::exit(2);
    }
    return argv[++i];
}

bool writePair(const std::string &prefix, const FrameImage &img) {
    std::string err;
    if (!writeRaw32(prefix + ".raw32", img, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    if (!writePng(prefix + ".png", img, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    std::printf("wrote %s.raw32 and %s.png (%ux%u, %llu non-black px)\n", prefix.c_str(),
                prefix.c_str(), img.width, img.height,
                (unsigned long long)nonBlackPixels(img));
    return true;
}

int cmdInfo(int argc, char **argv) {
    if (argc < 3)
        return usage();
    GsDumpFile dump;
    std::string err;
    if (!dump.load(argv[2], err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (!err.empty())
        std::fprintf(stderr, "warning: %s\n", err.c_str());

    std::printf("file            : %s\n", argv[2]);
    std::printf("header          : %s\n", dump.hasNewHeader() ? "new (CRC=0xFFFFFFFF marker)" : "legacy");
    std::printf("serial          : %s\n", dump.serial().empty() ? "(none)" : dump.serial().c_str());
    std::printf("crc             : 0x%08X\n", dump.crc());
    std::printf("state version   : %u\n", dump.freeze().version);
    std::printf("state bytes     : %zu\n", dump.stateData().size());
    std::printf("vram at         : +%zu (%zu bytes)\n", dump.freeze().vramOffset,
                dump.freeze().vramSize);
    std::printf("priv regs bytes : %zu\n", dump.regsData().size());
    std::printf("screenshot      : %ux%u (%zu bytes)\n", dump.screenshotWidth(),
                dump.screenshotHeight(), dump.screenshotRgba().size());
    std::printf("packet bytes    : %zu\n", dump.packetData().size());

    uint64_t counts[4] = {0, 0, 0, 0};
    uint64_t pathCounts[5] = {0, 0, 0, 0, 0};
    uint64_t pathBytes[5] = {0, 0, 0, 0, 0};
    for (const GsPacket &p : dump.packets()) {
        const unsigned t = static_cast<unsigned>(p.type);
        if (t < 4)
            ++counts[t];
        if (p.type == GsPacketType::Transfer) {
            const unsigned path = static_cast<unsigned>(p.path);
            if (path < 5) {
                ++pathCounts[path];
                pathBytes[path] += p.length;
            }
        }
    }
    std::printf("packets         : %zu\n", dump.packets().size());
    std::printf("  Transfer      : %llu\n", (unsigned long long)counts[0]);
    for (unsigned p = 0; p < 5; ++p) {
        if (pathCounts[p] == 0)
            continue;
        std::printf("    %-11s : %llu packets, %llu bytes\n",
                    toString(static_cast<GsTransferPath>(p)), (unsigned long long)pathCounts[p],
                    (unsigned long long)pathBytes[p]);
    }
    std::printf("  VSync         : %llu\n", (unsigned long long)counts[1]);
    std::printf("  ReadFIFO2     : %llu\n", (unsigned long long)counts[2]);
    std::printf("  Registers     : %llu\n", (unsigned long long)counts[3]);

    if (dump.regsData().size() >= privreg::kSize) {
        auto rd = [&](size_t off) {
            uint64_t v = 0;
            std::memcpy(&v, dump.regsData().data() + off, 8);
            return v;
        };
        std::printf("PMODE           : 0x%016llX\n", (unsigned long long)rd(privreg::kPMODE));
        std::printf("SMODE2          : 0x%016llX\n", (unsigned long long)rd(privreg::kSMODE2));
        std::printf("DISPFB1/DISPLAY1: 0x%016llX / 0x%016llX\n",
                    (unsigned long long)rd(privreg::kDISPFB1),
                    (unsigned long long)rd(privreg::kDISPLAY1));
        std::printf("DISPFB2/DISPLAY2: 0x%016llX / 0x%016llX\n",
                    (unsigned long long)rd(privreg::kDISPFB2),
                    (unsigned long long)rd(privreg::kDISPLAY2));
        std::printf("BGCOLOR         : 0x%016llX\n", (unsigned long long)rd(privreg::kBGCOLOR));
    }

    const GsFreezeState &fz = dump.freeze();
    if (fz.valid) {
        for (int i = 0; i < 2; ++i) {
            std::printf("ctx%d FRAME=0x%016llX ZBUF=0x%016llX TEX0=0x%016llX SCISSOR=0x%016llX\n",
                        i + 1, (unsigned long long)fz.ctx[i].frame,
                        (unsigned long long)fz.ctx[i].zbuf, (unsigned long long)fz.ctx[i].tex0,
                        (unsigned long long)fz.ctx[i].scissor);
        }
    }
    return 0;
}

int cmdReplay(int argc, char **argv) {
    if (argc < 3)
        return usage();
    const std::string path = argv[2];
    std::string outPrefix;
    ReplayOptions opt;
    enum class Pick { Last, First, Index, All };
    Pick pick = Pick::Last;
    uint32_t wantFrame = 0;
    bool requireContent = false;

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out")
            outPrefix = argValue(argc, argv, i);
        else if (a == "--loops")
            opt.loops = static_cast<uint32_t>(std::strtoul(argValue(argc, argv, i), nullptr, 10));
        else if (a == "--frames")
            opt.maxFrames = static_cast<uint32_t>(std::strtoul(argValue(argc, argv, i), nullptr, 10));
        else if (a == "--frame") {
            wantFrame = static_cast<uint32_t>(std::strtoul(argValue(argc, argv, i), nullptr, 10));
            pick = Pick::Index;
            if (opt.maxFrames != 0 && opt.maxFrames <= wantFrame)
                opt.maxFrames = wantFrame + 1;
        } else if (a == "--last")
            pick = Pick::Last;
        else if (a == "--first")
            pick = Pick::First;
        else if (a == "--all")
            pick = Pick::All;
        else if (a == "--require-content")
            requireContent = true;
        else if (a == "--no-seed-vram")
            opt.seedVram = false;
        else if (a == "--no-seed-regs")
            opt.seedRegisters = false;
        else if (a == "--verbose")
            opt.verbose = true;
        else if (a == "--dump-vram")
            opt.vramDumpPath = argValue(argc, argv, i);
        else if (a == "--trace-out")
            opt.backendTracePath = argValue(argc, argv, i);
        else if (a == "--gs") {
            const std::string name = argValue(argc, argv, i);
            if (name == "sw")
                opt.backend = ReplayBackend::Software;
            else if (name == "sdlgpu")
                opt.backend = ReplayBackend::SdlGpu;
            else {
                std::fprintf(stderr, "error: --gs expects sw or sdlgpu, got %s\n", name.c_str());
                return usage();
            }
        } else if (a == "--scale")
            opt.resolutionScale =
                static_cast<uint32_t>(std::strtoul(argValue(argc, argv, i), nullptr, 10));
        else if (a == "--paths") {
            const std::string list = argValue(argc, argv, i);
            uint32_t mask = 0;
            for (char c : list) {
                if (c == '1') mask |= (1u << 0) | (1u << 3); // both PATH1 encodings
                else if (c == '2') mask |= 1u << 1;
                else if (c == '3') mask |= 1u << 2;
            }
            if (mask)
                opt.pathMask = mask;
        } else {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return usage();
        }
    }

    GsDumpFile dump;
    std::string err;
    if (!dump.load(path, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (!err.empty())
        std::fprintf(stderr, "warning: %s\n", err.c_str());

    std::vector<FrameImage> frames;
    ReplayStats stats;
    if (!replayDump(dump, opt, frames, stats, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (!err.empty())
        std::fprintf(stderr, "warning: %s\n", err.c_str());

    std::printf("replayed %llu transfers (%llu bytes), %llu vsyncs, %llu readfifo, "
                "%llu register packets over %u loop(s)\n",
                (unsigned long long)stats.transfers, (unsigned long long)stats.transferBytes,
                (unsigned long long)stats.vsyncs, (unsigned long long)stats.readFifos,
                (unsigned long long)stats.registerPackets, stats.loopsRun);
    std::printf("frames presented: %llu (empty: %llu, last non-blank: %lld)\n",
                (unsigned long long)stats.framesPresented, (unsigned long long)stats.framesEmpty,
                (long long)stats.lastNonBlankFrame);
    if (!stats.backendReport.empty())
        std::printf("%s\n", stats.backendReport.c_str());

    if (frames.empty()) {
        std::fprintf(stderr, "error: no frame was presented -- nothing to write\n");
        return 1;
    }

    size_t index = frames.size() - 1;
    switch (pick) {
    case Pick::First:
        index = 0;
        break;
    case Pick::Index:
        if (wantFrame >= frames.size()) {
            std::fprintf(stderr, "error: dump only presented %zu frames\n", frames.size());
            return 1;
        }
        index = wantFrame;
        break;
    default:
        break;
    }

    if (requireContent && nonBlackPixels(frames[index]) == 0) {
        std::fprintf(stderr, "FAIL: selected frame %zu is entirely black\n", index);
        return 1;
    }
    if (outPrefix.empty())
        return 0;

    if (pick == Pick::All) {
        for (size_t i = 0; i < frames.size(); ++i) {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "_%03zu", i);
            if (!writePair(outPrefix + suffix, frames[i]))
                return 1;
        }
        return 0;
    }
    return writePair(outPrefix, frames[index]) ? 0 : 1;
}

int cmdShot(int argc, char **argv) {
    if (argc < 3)
        return usage();
    std::string outPrefix;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out")
            outPrefix = argValue(argc, argv, i);
        else
            return usage();
    }
    GsDumpFile dump;
    std::string err;
    if (!dump.load(argv[2], err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    FrameImage shot;
    if (!extractScreenshot(dump, shot)) {
        std::fprintf(stderr, "error: dump has no embedded screenshot\n");
        return 1;
    }
    if (outPrefix.empty()) {
        std::printf("screenshot %ux%u\n", shot.width, shot.height);
        return 0;
    }
    return writePair(outPrefix, shot) ? 0 : 1;
}

int cmdDiff(int argc, char **argv) {
    if (argc < 4)
        return usage();
    uint32_t tolerance = 0;
    bool ignoreAlpha = false;
    std::string heatmapPath;
    double maxDiffFraction = -1.0;
    double minPsnr = -1.0;
    char fit = 0; // 'a' or 'b': resample that image onto the other's grid

    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fit") {
            const std::string which = argValue(argc, argv, i);
            if (which != "a" && which != "b") {
                std::fprintf(stderr, "error: --fit takes 'a' or 'b'\n");
                return 2;
            }
            fit = which[0];
        } else if (a == "--tolerance")
            tolerance = static_cast<uint32_t>(std::strtoul(argValue(argc, argv, i), nullptr, 10));
        else if (a == "--ignore-alpha")
            ignoreAlpha = true;
        else if (a == "--heatmap")
            heatmapPath = argValue(argc, argv, i);
        else if (a == "--max-diff-fraction")
            maxDiffFraction = std::strtod(argValue(argc, argv, i), nullptr);
        else if (a == "--min-psnr")
            minPsnr = std::strtod(argValue(argc, argv, i), nullptr);
        else
            return usage();
    }

    FrameImage a, b;
    std::string err;
    if (!readRaw32(argv[2], a, err) || !readRaw32(argv[3], b, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    if (fit != 0 && (a.width != b.width || a.height != b.height)) {
        FrameImage &src = (fit == 'a') ? a : b;
        const FrameImage &dstRef = (fit == 'a') ? b : a;
        FrameImage scaled;
        if (!resampleTo(src, dstRef.width, dstRef.height, scaled)) {
            std::fprintf(stderr, "error: cannot resample %c\n", fit);
            return 1;
        }
        std::printf("fit               : %c resampled %ux%u -> %ux%u (area average)\n", fit,
                    src.width, src.height, scaled.width, scaled.height);
        src = std::move(scaled);
    }

    FrameImage heat;
    FrameImage *heatPtr = heatmapPath.empty() ? nullptr : &heat;
    const DiffResult r = ignoreAlpha ? diffImagesRgb(a, b, tolerance, heatPtr)
                                     : diffImages(a, b, tolerance, heatPtr);
    if (!r.comparable) {
        std::fprintf(stderr,
                     "error: images are not comparable (%ux%u vs %ux%u); pass --fit a|b to "
                     "resample one onto the other's grid\n",
                     a.width, a.height, b.width, b.height);
        return 1;
    }

    std::printf("size              : %ux%u\n", a.width, a.height);
    std::printf("differing pixels  : %llu / %llu (%.4f%%)\n",
                (unsigned long long)r.differingPixels, (unsigned long long)r.totalPixels,
                r.differingFraction() * 100.0);
    std::printf("over tolerance %-3u: %llu\n", tolerance,
                (unsigned long long)r.pixelsOverTolerance);
    std::printf("max channel delta : %u\n", r.maxChannelDelta);
    std::printf("mean abs error    : %.4f\n", r.meanAbsError);
    std::printf("psnr              : %.2f dB\n", r.psnr);
    if (r.x1 >= r.x0)
        std::printf("diff bbox         : (%d,%d)-(%d,%d)\n", r.x0, r.y0, r.x1, r.y1);
    else
        std::printf("diff bbox         : (none)\n");

    if (heatPtr && !writePng(heatmapPath, heat, err))
        std::fprintf(stderr, "warning: %s\n", err.c_str());

    int rc = 0;
    if (maxDiffFraction >= 0.0 && r.differingFraction() > maxDiffFraction) {
        std::fprintf(stderr, "FAIL: differing fraction %.6f > %.6f\n", r.differingFraction(),
                     maxDiffFraction);
        rc = 1;
    }
    if (minPsnr >= 0.0 && r.psnr < minPsnr) {
        std::fprintf(stderr, "FAIL: psnr %.2f < %.2f\n", r.psnr, minPsnr);
        rc = 1;
    }
    return rc;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2)
        return usage();
    const std::string cmd = argv[1];
    if (cmd == "info")
        return cmdInfo(argc, argv);
    if (cmd == "replay")
        return cmdReplay(argc, argv);
    if (cmd == "shot")
        return cmdShot(argc, argv);
    if (cmd == "diff")
        return cmdDiff(argc, argv);
    return usage();
}
