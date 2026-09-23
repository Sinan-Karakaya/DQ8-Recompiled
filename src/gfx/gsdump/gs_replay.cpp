#include "gs_replay.h"
#include "gs_backend_trace.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_types.h"
#include "runtime/ps2_memory.h" // struct GSRegisters

#if defined(DQ8_GSDUMP_HAS_SDLGPU)
#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#endif

namespace dq8::gfx {
namespace {

constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;
// ps2xRuntime's CPU backend renders presentation into a fixed 640x512 buffer
// (gs_cpu_backend.cpp: kHostFrameWidth/kHostFrameHeight) and clamps the decoded
// display size to it, so any mode wider than 640 or taller than 512 is silently
// clipped upstream. DQ8's 512x448 fits.

uint64_t loadLE64(const uint8_t *p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void seedPrivRegs(GSRegisters &regs, const std::vector<uint8_t> &blob) {
    if (blob.size() < privreg::kSize)
        return;
    const uint8_t *d = blob.data();
    regs.pmode = loadLE64(d + privreg::kPMODE);
    regs.smode1 = loadLE64(d + privreg::kSMODE1);
    regs.smode2 = loadLE64(d + privreg::kSMODE2);
    regs.srfsh = loadLE64(d + privreg::kSRFSH);
    regs.synch1 = loadLE64(d + privreg::kSYNCH1);
    regs.synch2 = loadLE64(d + privreg::kSYNCH2);
    regs.syncv = loadLE64(d + privreg::kSYNCV);
    regs.dispfb1 = loadLE64(d + privreg::kDISPFB1);
    regs.display1 = loadLE64(d + privreg::kDISPLAY1);
    regs.dispfb2 = loadLE64(d + privreg::kDISPFB2);
    regs.display2 = loadLE64(d + privreg::kDISPLAY2);
    regs.extbuf = loadLE64(d + privreg::kEXTBUF);
    regs.extdata = loadLE64(d + privreg::kEXTDATA);
    regs.extwrite = loadLE64(d + privreg::kEXTWRITE);
    regs.bgcolor = loadLE64(d + privreg::kBGCOLOR);
    regs.imr = loadLE64(d + privreg::kIMR);
    regs.busdir = loadLE64(d + privreg::kBUSDIR);
    regs.siglblid = loadLE64(d + privreg::kSIGLBLID);
    regs.csr.store(loadLE64(d + privreg::kCSR), std::memory_order_release);
}

void seedDrawingEnvironment(GS &gs, const GsFreezeState &fz) {
    if (!fz.valid)
        return;

    // Global environment. TRXDIR is deliberately not replayed: writing it is
    // what *starts* a VRAM transfer in the frontend, and a dump taken mid
    // transfer cannot be resumed from the freeze blob alone.
    gs.writeRegister(GS_REG_PRMODECONT, fz.prmodecont);
    gs.writeRegister(GS_REG_PRIM, fz.prim);
    gs.writeRegister(GS_REG_TEXCLUT, fz.texclut);
    gs.writeRegister(GS_REG_SCANMSK, fz.scanmsk);
    gs.writeRegister(GS_REG_TEXA, fz.texa);
    gs.writeRegister(GS_REG_FOGCOL, fz.fogcol);
    gs.writeRegister(GS_REG_DIMX, fz.dimx);
    gs.writeRegister(GS_REG_DTHE, fz.dthe);
    gs.writeRegister(GS_REG_COLCLAMP, fz.colclamp);
    gs.writeRegister(GS_REG_PABE, fz.pabe);
    gs.writeRegister(GS_REG_BITBLTBUF, fz.bitbltbuf);
    gs.writeRegister(GS_REG_TRXPOS, fz.trxpos);
    gs.writeRegister(GS_REG_TRXREG, fz.trxreg);

    static constexpr uint8_t kCtxRegs[2][12] = {
        {GS_REG_XYOFFSET_1, GS_REG_TEX0_1, GS_REG_TEX1_1, GS_REG_CLAMP_1, GS_REG_MIPTBP1_1,
         GS_REG_MIPTBP2_1, GS_REG_SCISSOR_1, GS_REG_ALPHA_1, GS_REG_TEST_1, GS_REG_FBA_1,
         GS_REG_FRAME_1, GS_REG_ZBUF_1},
        {GS_REG_XYOFFSET_2, GS_REG_TEX0_2, GS_REG_TEX1_2, GS_REG_CLAMP_2, GS_REG_MIPTBP1_2,
         GS_REG_MIPTBP2_2, GS_REG_SCISSOR_2, GS_REG_ALPHA_2, GS_REG_TEST_2, GS_REG_FBA_2,
         GS_REG_FRAME_2, GS_REG_ZBUF_2},
    };

    for (int i = 0; i < 2; ++i) {
        const GsFreezeState::Context &c = fz.ctx[i];
        const uint64_t values[12] = {c.xyoffset, c.tex0, c.tex1, c.clamp, c.miptbp1, c.miptbp2,
                                     c.scissor,  c.alpha, c.test, c.fba,  c.frame,   c.zbuf};
        for (int r = 0; r < 12; ++r)
            gs.writeRegister(kCtxRegs[i][r], values[r]);
    }

    // Latched vertex attributes. XYZ is intentionally excluded: writing it is a
    // vertex kick and would emit a bogus primitive.
    gs.writeRegister(GS_REG_RGBAQ, fz.rgbaq);
    gs.writeRegister(GS_REG_ST, fz.st);
    gs.writeRegister(GS_REG_UV, fz.uv);
    gs.writeRegister(GS_REG_FOG, fz.fog);
}

// GS::copyLatchedHostPresentationFrame already repacks the backend's
// 640-strided presentation buffer into tightly packed `width` rows, so this is
// a straight adopt-and-check rather than a de-stride.
bool toFrameImage(const std::vector<uint8_t> &src, uint32_t width, uint32_t height,
                  FrameImage &out) {
    if (width == 0 || height == 0)
        return false;
    const size_t need = static_cast<size_t>(width) * height * 4u;
    if (src.size() < need)
        return false;
    out.resize(width, height);
    std::memcpy(out.rgba.data(), src.data(), need);
    return true;
}

} // namespace

bool extractScreenshot(const GsDumpFile &dump, FrameImage &out) {
    const uint32_t w = dump.screenshotWidth();
    const uint32_t h = dump.screenshotHeight();
    const std::vector<uint8_t> &px = dump.screenshotRgba();
    if (w == 0 || h == 0 || px.size() < static_cast<size_t>(w) * h * 4u)
        return false;
    out.resize(w, h);
    std::memcpy(out.rgba.data(), px.data(), out.rgba.size());
    return true;
}

bool replayDump(const GsDumpFile &dump, const ReplayOptions &options,
                std::vector<FrameImage> &frames, ReplayStats &stats, std::string &error) {
    frames.clear();
    stats = ReplayStats{};

    std::vector<uint8_t> vram(kVramBytes, 0u);
    GSRegisters privRegs{};
    std::memset(&privRegs, 0, offsetof(GSRegisters, csr));
    privRegs.csr.store(0, std::memory_order_relaxed);
    privRegs.vsyncTick.store(0, std::memory_order_relaxed);

    GS gs;
    const auto installBackend = [&](std::unique_ptr<GSRasterBackend> backend) {
        if (!options.backendTracePath.empty())
            backend = std::make_unique<GsTraceBackend>(std::move(backend),
                options.backendTracePath, 1u, UINT32_MAX);
        gs.setRasterBackend(std::move(backend));
    };
#if defined(DQ8_GSDUMP_HAS_SDLGPU)
    SdlGpuBackend *hardware = nullptr;
    if (options.backend == ReplayBackend::SdlGpu) {
        std::string backendError;
        std::unique_ptr<SdlGpuBackend> backend = createSdlGpuBackend(backendError);
        if (!backend) {
            error = "SDL GPU backend unavailable: " + backendError;
            return false;
        }
        backend->setResolutionScale(options.resolutionScale);
        hardware = backend.get();
        installBackend(std::move(backend));
    } else {
        installBackend(std::make_unique<GSCpuBackend>());
    }
#else
    if (options.backend == ReplayBackend::SdlGpu) {
        error = "this build has no SDL GPU backend (SDL3 was not found at configure time)";
        return false;
    }
    installBackend(std::make_unique<GSCpuBackend>());
#endif
    gs.init(vram.data(), kVramBytes, &privRegs);

    // Order matters: init() resets the frontend and the backend, so VRAM and
    // register seeding both have to happen afterwards.
    if (options.seedVram) {
        const GsFreezeState &fz = dump.freeze();
        const std::vector<uint8_t> &state = dump.stateData();
        if (fz.valid && fz.vramOffset + fz.vramSize <= state.size()) {
            std::memcpy(vram.data(), state.data() + fz.vramOffset,
                        std::min<size_t>(fz.vramSize, kVramBytes));
        } else if (!state.empty()) {
            error = "freeze state present but VRAM could not be located; "
                    "replaying with blank GS local memory";
        }
    }
    if (options.seedRegisters)
        seedDrawingEnvironment(gs, dump.freeze());
    seedPrivRegs(privRegs, dump.regsData());

    std::vector<uint8_t> fifoSink;
    uint64_t packetIndex = 0;
    struct VramDumper {
        const std::string &path;
        const std::vector<uint8_t> &vram;
        ~VramDumper() {
            if (path.empty())
                return;
            if (std::FILE *fp = std::fopen(path.c_str(), "wb")) {
                std::fwrite(vram.data(), 1, vram.size(), fp);
                std::fclose(fp);
                std::fprintf(stderr, "wrote %zu bytes of GS local memory to %s\n", vram.size(),
                             path.c_str());
            }
        }
    } vramDumper{options.vramDumpPath, vram};

    const uint32_t loops = options.loops ? options.loops : 1u;
    bool stop = false;

    for (uint32_t loop = 0; loop < loops && !stop; ++loop) {
        ++stats.loopsRun;
        for (const GsPacket &pkt : dump.packets()) {
            ++packetIndex;
            const uint8_t *payload = pkt.length ? dump.payload(pkt) : nullptr;

            switch (pkt.type) {
            case GsPacketType::Transfer: {
                const uint32_t pathBit = 1u << static_cast<uint32_t>(pkt.path);
                ++stats.transfers;
                stats.transferBytes += pkt.length;
                if (static_cast<uint32_t>(pkt.path) < 5u)
                    ++stats.transfersByPath[static_cast<uint32_t>(pkt.path)];
                if ((options.pathMask & pathBit) == 0u)
                    break;
                if (options.verbose) {
                    std::fprintf(stderr, "[%llu] Transfer %s %u bytes\n",
                                 static_cast<unsigned long long>(packetIndex), toString(pkt.path),
                                 pkt.length);
                }
                gs.processGIFPacket(payload, pkt.length);
                break;
            }

            case GsPacketType::Registers:
                ++stats.registerPackets;
                if (payload) {
                    std::vector<uint8_t> blob(payload, payload + pkt.length);
                    seedPrivRegs(privRegs, blob);
                    if (options.verbose) {
                        std::fprintf(stderr,
                                     "[%llu] Registers DISPFB1=0x%016llX DISPFB2=0x%016llX "
                                     "PMODE=0x%016llX\n",
                                     static_cast<unsigned long long>(packetIndex),
                                     static_cast<unsigned long long>(privRegs.dispfb1),
                                     static_cast<unsigned long long>(privRegs.dispfb2),
                                     static_cast<unsigned long long>(privRegs.pmode));
                    }
                }
                break;

            case GsPacketType::ReadFIFO2: {
                ++stats.readFifos;
                uint32_t qwc = 0;
                if (payload)
                    std::memcpy(&qwc, payload, sizeof(qwc));
                const uint64_t bytes = static_cast<uint64_t>(qwc) * 16ull;
                if (bytes > 0 && bytes < (64ull << 20)) {
                    fifoSink.resize(static_cast<size_t>(bytes));
                    gs.consumeLocalToHostBytes(fifoSink.data(), static_cast<uint32_t>(bytes));
                }
                break;
            }

            case GsPacketType::VSync: {
                ++stats.vsyncs;
                privRegs.vsyncTick.fetch_add(1, std::memory_order_acq_rel);
                gs.latchHostPresentationFrame();

                std::vector<uint8_t> pixels;
                uint32_t w = 0, h = 0, displayFbp = 0, sourceFbp = 0;
                bool usedPreferred = false;
                FrameImage img;
                if (gs.copyLatchedHostPresentationFrame(pixels, w, h, &displayFbp, &sourceFbp,
                                                        &usedPreferred) &&
                    toFrameImage(pixels, w, h, img)) {
                    ++stats.framesPresented;
                    const uint64_t lit = nonBlackPixels(img);
                    if (lit != 0)
                        stats.lastNonBlankFrame = static_cast<int64_t>(frames.size());
                    frames.push_back(std::move(img));
                    if (options.verbose) {
                        std::fprintf(stderr,
                                     "[%llu] VSync -> %ux%u fbp=%u src=%u preferred=%d lit=%llu\n",
                                     static_cast<unsigned long long>(packetIndex), w, h, displayFbp,
                                     sourceFbp, usedPreferred ? 1 : 0,
                                     static_cast<unsigned long long>(lit));
                    }
                } else {
                    ++stats.framesEmpty;
                    if (options.verbose)
                        std::fprintf(stderr, "[%llu] VSync -> no presentable frame\n",
                                     static_cast<unsigned long long>(packetIndex));
                }

                if (options.maxFrames != 0 && frames.size() >= options.maxFrames)
                    stop = true;
                break;
            }
            }

            if (stop)
                break;
        }
    }

#if defined(DQ8_GSDUMP_HAS_SDLGPU)
    if (hardware) {
        // Resolve every render target into local memory, so --dump-vram shows
        // what the GPU actually drew rather than whatever last happened to be
        // written back.
        hardware->Sync(GSSyncReason::Reset);
        const SdlGpuStats gpu = hardware->stats();
        char report[1024];
        std::snprintf(
            report, sizeof(report),
            "SDL GPU (%s, scale %ux): primitives=%llu drawn=%llu triangles=%llu "
            "batches=%llu passes=%llu draws=%llu avg-triangles=%.1f pipelines=%llu\n"
            "  surfaces=%llu resolves=%llu (%llu px) refreshes=%llu (%llu px)\n"
            "  textures: lookups=%llu hits=%llu (%.0f%%) builds=%llu texels=%llu "
            "invalidated=%llu evicted=%llu rebuilt-from-targets=%llu "
            "bound-live-targets=%llu feedback-hazards=%llu\n"
            "  approximated: blends=%llu saturated-blend-factors=%llu "
            "dest-alpha-factors=%llu partial-masks=%llu dest-alpha-tests=%llu "
            "afail-modes=%llu no-colclamp=%llu textured=%llu untranslated-textures=%llu "
            "second-circuit=%llu",
            hardware->driverName().c_str(), hardware->resolutionScale(),
            static_cast<unsigned long long>(gpu.primitivesSubmitted),
            static_cast<unsigned long long>(gpu.primitivesDrawn),
            static_cast<unsigned long long>(gpu.trianglesDrawn),
            static_cast<unsigned long long>(gpu.batches),
            static_cast<unsigned long long>(gpu.renderPasses),
            static_cast<unsigned long long>(gpu.drawCalls),
            gpu.averageTrianglesPerDraw(),
            static_cast<unsigned long long>(gpu.pipelinesCreated),
            static_cast<unsigned long long>(gpu.surfacesCreated),
            static_cast<unsigned long long>(gpu.colorResolves),
            static_cast<unsigned long long>(gpu.resolvedPixels),
            static_cast<unsigned long long>(gpu.colorRefreshes),
            static_cast<unsigned long long>(gpu.refreshedPixels),
            static_cast<unsigned long long>(gpu.textureLookups),
            static_cast<unsigned long long>(gpu.textureHits),
            gpu.textureHitRate() * 100.0,
            static_cast<unsigned long long>(gpu.textureBuilds),
            static_cast<unsigned long long>(gpu.texelsExpanded),
            static_cast<unsigned long long>(gpu.textureInvalidations),
            static_cast<unsigned long long>(gpu.textureEvictions),
            static_cast<unsigned long long>(gpu.texturesFromRenderTargets),
            static_cast<unsigned long long>(gpu.texturesFromLiveTargets),
            static_cast<unsigned long long>(gpu.textureFeedbackHazards),
            static_cast<unsigned long long>(gpu.inexactBlends),
            static_cast<unsigned long long>(gpu.saturatedBlendFactors),
            static_cast<unsigned long long>(gpu.destinationAlphaFactors),
            static_cast<unsigned long long>(gpu.partialChannelMasks),
            static_cast<unsigned long long>(gpu.destinationAlphaTests),
            static_cast<unsigned long long>(gpu.alphaFailModes),
            static_cast<unsigned long long>(gpu.disabledColorClamps),
            static_cast<unsigned long long>(gpu.texturedPrimitives),
            static_cast<unsigned long long>(gpu.untranslatedTextures),
            static_cast<unsigned long long>(gpu.secondaryDisplayCircuits));
        stats.backendReport = report;
        const std::string backendError = hardware->lastError();
        if (!backendError.empty())
            stats.backendReport += "\n  last error: " + backendError;
    }
#endif

    return true;
}

} // namespace dq8::gfx
