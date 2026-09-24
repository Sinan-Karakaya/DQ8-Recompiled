// DQ8_LINK_GENERATED enables the game launcher. Without it, the executable
// reports its version so the project can build without a game dump.

#include <cstdio>

#ifndef DQ8_VERSION
#define DQ8_VERSION "0.0.0-dev"
#endif

#if defined(DQ8_LINK_GENERATED)

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "runtime/ps2_native_iop.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "vu_bounds.h"
#include "../../tools/runtime/render_cadence.h"

#if defined(DQ8_HAS_SDLGPU)
#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "gfx/gsdump/gs_backend_trace.h"
#include "../../tools/runtime/gs_fan_probe.h"
#include "runtime/ps2_pad_host.h"

namespace
{
    // Publish input after presenting when SDL owns the window.
    bool pumpSdlGpuPresenter(void *userData)
    {
        auto *backend = static_cast<dq8::gfx::SdlGpuBackend *>(userData);
        if (!backend->pumpEvents())
        {
            return false;
        }
        uint32_t held = 0u;
        uint32_t pressed = 0u;
        uint32_t sticks = 0u;
        backend->hostPadState(held, pressed, sticks);
        ps2PadPublishHostState(held, pressed, sticks);
        return true;
    }
}
#endif

#if defined(DQ8_LINK_OVERLAYS)
#include "overlay/overlay_manager.h"
#endif

namespace
{
    // Redirect the retail no-op wrappers to the surviving guest printf.
    // This matches the PCSX2 trace patch and preserves guest formatting.
    constexpr uint32_t kDebugPrintfWrapper = 0x00146E20u;
    constexpr uint32_t kBattleLogWrapper = 0x00321620u;

    // The ELF header points at a scratchpad-address table at 0x00100008.
    // Start at the first executable instruction, crt0_start, instead.
    constexpr uint32_t kDefaultEntryPoint = 0x0010008Cu;
}

int main(int argc, char **argv)
{
    std::string elfPath;
    std::string isoPath;
    std::string cdRoot;
    uint32_t entryPoint = kDefaultEntryPoint;
    bool tracePrintf = false;
    bool sound = true;
    std::string rasterBackend = "sw";
    uint32_t resolutionScale = 1u;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg.rfind("--entry=", 0) == 0)
        {
            entryPoint = static_cast<uint32_t>(std::strtoul(arg.c_str() + 8, nullptr, 0));
        }
        else if (arg == "--trace-printf")
        {
            tracePrintf = true;
        }
        else if (arg == "--mute")
        {
            sound = false;
        }
        else if (arg.rfind("--gs=", 0) == 0)
        {
            rasterBackend = arg.substr(5);
        }
        else if (arg.rfind("--scale=", 0) == 0)
        {
            resolutionScale = static_cast<uint32_t>(std::strtoul(arg.c_str() + 8, nullptr, 10));
        }
        else if (arg.rfind("--iso=", 0) == 0)
        {
            isoPath = arg.substr(6);
        }
        else if (arg.rfind("--cd-root=", 0) == 0)
        {
            cdRoot = arg.substr(10);
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::printf("usage: dq8 <path-to-SLUS_212.07> [options]\n"
                        "  --entry=0xADDR   override the start PC (default 0x%08X = crt0_start)\n"
                        "  --iso=PATH       disc image for sector reads, including HD6 archives\n"
                        "  --cd-root=DIR    cdrom0: directory (default: the ELF's directory)\n"
                        "  --gs=BACKEND     raster backend: sw (default) or sdlgpu\n"
                        "  --scale=N        internal resolution multiplier for --gs=sdlgpu\n"
                        "  --mute           run the sound driver without playing anything\n"
                        "  --trace-printf   restore retail debug logging through the guest formatter\n"
                        "                   Set PS2X_DECI2_LOG_LIMIT=0 to remove the log limit.\n",
                        kDefaultEntryPoint);
            return 0;
        }
        else if (elfPath.empty())
        {
            elfPath = arg;
        }
    }

    if (elfPath.empty())
    {
        std::fprintf(stderr,
                     "DQ8Recomp %s\n"
                     "usage: dq8 <path-to-SLUS_212.07> [--entry=0xADDR]\n",
                     DQ8_VERSION);
        return 1;
    }

    std::printf("DQ8Recomp %s -- loading %s, entry 0x%08X\n",
               DQ8_VERSION, elfPath.c_str(), entryPoint);

    PS2Runtime runtime;

#if defined(DQ8_HAS_SDLGPU)
    // Built before initialize(), because that is where the runtime would open
    // its own window: with a presenter installed it opens none, and raylib is
    // never initialised at all.
    std::unique_ptr<dq8::gfx::SdlGpuBackend> sdlBackend;
    std::string backendError;
    if (rasterBackend == "sdlgpu")
    {
        sdlBackend = dq8::gfx::createSdlGpuBackend(backendError);
        if (sdlBackend)
        {
            sdlBackend->setResolutionScale(resolutionScale);
            const uint32_t scale = sdlBackend->resolutionScale();
            std::string windowError;
            // Allow renderer comparisons using the runtime's presenter.
            const char *noWindow = std::getenv("DQ8_GFX_NO_WINDOW");
            if (noWindow != nullptr && *noWindow != '\0')
            {
                std::printf("[dq8] DQ8_GFX_NO_WINDOW: presenting through the runtime\n");
            }
            else if (!sdlBackend->openWindow("DQ8Recomp", 640u * scale, 448u * scale, windowError))
            {
                std::fprintf(stderr,
                             "[dq8] could not open a window (%s); presenting through "
                             "the runtime instead\n",
                             windowError.c_str());
            }
            else
            {
                runtime.setExternalPresenter(&pumpSdlGpuPresenter, sdlBackend.get());
            }
        }
    }
    auto *sdlWindowBackend = sdlBackend.get();
#endif

    if (!runtime.initialize("DQ8Recomp"))
    {
        std::fprintf(stderr, "[dq8] PS2Runtime::initialize() failed\n");
        return 1;
    }

    // The sound drivers from the disc run unmodified on an emulated IOP, and
    // the game's libsdr and Sound Kit reach them over SIF. Audio output starts
    // when sceSifLoadModule loads the first of them.
    ps2_native_iop::setModules(runtime, {"LIBSD.IRX", "SDRDRV.IRX", "MODHSYN.IRX", "MODMIDI.IRX",
                                         "MODMSIN.IRX", "SKSOUND.IRX", "SKHSYNTH.IRX", "SKMIDI.IRX",
                                         "SKMSIN.IRX", "PCMPLAY.IRX"});
    if (!sound)
        ps2_native_iop::setVolume(0.0f);

    // After initialize(), which is what creates GS local memory and hands it to
    // the default software backend. setRasterBackend flushes and carries local
    // memory across, so swapping here is safe.
    if (rasterBackend == "sdlgpu")
    {
#if defined(DQ8_HAS_SDLGPU)
        if (!sdlBackend)
        {
            std::fprintf(stderr,
                         "[dq8] --gs=sdlgpu: %s\n"
                         "[dq8] falling back to the software rasteriser\n",
                         backendError.c_str());
        }
        else
        {
            std::printf("[dq8] GS backend: sdlgpu (%s), video %s, internal scale %ux, %s\n",
                        sdlBackend->driverName().c_str(),
                        sdlBackend->videoDriverName().c_str(),
                        sdlBackend->resolutionScale(),
                        sdlBackend->hasWindow()
                            ? "presenting natively (raylib not initialised)"
                            : "presenting through the runtime");
            std::unique_ptr<GSRasterBackend> backend = std::move(sdlBackend);
            if (const char *worker = std::getenv("DQ8_GS_WORKER"); !worker || std::string(worker) != "0") {
                backend = std::make_unique<GSThreadedBackend>(std::move(backend));
                std::printf("[dq8] ordered GS worker enabled\n");
            }
            if (const char *trace = std::getenv("DQ8_GS_TRACE")) {
                const char *start = std::getenv("DQ8_GS_TRACE_START");
                const char *trigger = std::getenv("DQ8_GS_TRACE_TRIGGER");
                backend = std::make_unique<dq8::gfx::GsTraceBackend>(
                    std::move(backend), trace, start ? std::strtoul(start, nullptr, 10) : 7550u,
                    4u, trigger ? trigger : "");
            }
            runtime.gs().setRasterBackend(dq8::diagnostics::wrapGsFanProbe(
                std::move(backend), runtime.memory().getRDRAM(), PS2_RAM_SIZE));
        }
#else
        std::fprintf(stderr,
                     "[dq8] --gs=sdlgpu: this build has no SDL GPU backend "
                     "(SDL3 was not found at configure time)\n");
        return 1;
#endif
    }
    else if (rasterBackend != "sw")
    {
        std::fprintf(stderr, "[dq8] --gs=%s: expected sw or sdlgpu\n", rasterBackend.c_str());
        return 1;
    }

    if (!runtime.loadELF(elfPath))
    {
        std::fprintf(stderr, "[dq8] PS2Runtime::loadELF(%s) failed\n", elfPath.c_str());
        return 1;
    }

    // Both of these must come after loadELF(): it calls
    // configureIoPathsFromElf(), which overwrites cdRoot with the ELF's own
    // directory. That default is already correct when the ELF is read straight
    // out of an extracted disc tree, so --cd-root only matters when it is not.
    if (!isoPath.empty() || !cdRoot.empty())
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        if (!isoPath.empty())
        {
            paths.cdImage = isoPath;
        }
        if (!cdRoot.empty())
        {
            paths.cdRoot = cdRoot;
        }
        PS2Runtime::setIoPaths(paths);
        std::printf("[dq8] cd root = %s\n", paths.cdRoot.string().c_str());
        if (!paths.cdImage.empty())
        {
            std::printf("[dq8] cd image = %s\n", paths.cdImage.string().c_str());
        }
    }

    // Keep the runtime arena above the guest allocator's ceiling to prevent overlap.
    {
        // The guest reserves most available RAM, so keep this arena small.
        constexpr uint32_t kRuntimeArenaBytes = 0x00010000u;
        const uint32_t heapTop = runtime.guestHeapHardLimit();
        const uint32_t guestCeiling = heapTop - kRuntimeArenaBytes;
        runtime.setGuestHeapCeiling(guestCeiling);
        runtime.configureGuestHeap(guestCeiling, heapTop);
        std::printf("[dq8] guest heap ends 0x%08X, runtime arena 0x%08X-0x%08X\n",
                    guestCeiling, guestCeiling, heapTop);
    }

    // loadELF() just set cpu().pc to the raw ELF header entry (see above);
    // override it with crt0_start (or the caller's --entry) before the
    // scheduler starts executing.
    runtime.cpu().pc = entryPoint;

    if (!std::getenv("DQ8_DISABLE_VU_BOUNDS_FIX") && !dq8::installVuBoundsComparisons(runtime))
        std::fprintf(stderr, "[dq8] VU bounds comparison repair could not be installed\n");

    if (tracePrintf)
    {
        // Must come after loadELF(): it is what populates the function table
        // these addresses are replaced in.
        const bool redirected =
            runtime.replaceFunction(kDebugPrintfWrapper, &FUN_00119508_0x119508) &&
            runtime.replaceFunction(kBattleLogWrapper, &FUN_00119508_0x119508);
        if (redirected)
        {
            std::printf("[dq8] debug-printf tracing on: 0x%08X and 0x%08X -> 0x00119508\n",
                        kDebugPrintfWrapper, kBattleLogWrapper);
        }
        else
        {
            std::fprintf(stderr,
                         "[dq8] --trace-printf: replaceFunction failed; are 0x%08X/0x%08X "
                         "in the recompiled function table?\n",
                         kDebugPrintfWrapper, kBattleLogWrapper);
        }
    }

#if defined(DQ8_LINK_OVERLAYS)
    // Arena addresses must dispatch through the currently resident overlay.
    if (!dq8::overlay::install(runtime))
    {
        std::fprintf(stderr, "[dq8] overlay manager failed to install\n");
    }
#endif

    std::printf("[dq8] starting execution at 0x%08X\n", entryPoint);
    [[maybe_unused]] const bool renderCounterReady = dq8::diagnostics::configureRenderCadenceProbe(runtime);
#if defined(DQ8_HAS_SDLGPU)
    if (renderCounterReady && sdlWindowBackend)
        sdlWindowBackend->setCompletedRenderCounter([] {
            return dq8::diagnostics::RenderCadenceProbe::instance().publishedCompletedRoutines();
        });
#endif
    runtime.run();

    std::printf("[dq8] PS2Runtime::run() returned\n");
    return 0;
}

#else // !DQ8_LINK_GENERATED

int main(int /*argc*/, char ** /*argv*/)
{
    std::printf("DQ8Recomp %s - runtime stub "
               "(configure with -DDQ8_LINK_GENERATED=ON to link the recompiled game)\n",
               DQ8_VERSION);
    return 0;
}

#endif
