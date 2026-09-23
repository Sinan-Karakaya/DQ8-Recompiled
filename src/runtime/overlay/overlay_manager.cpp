#include "overlay_manager.h"

#include "ps2_runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Declare table accessors directly: the generated overlay headers share an
// include guard, so they cannot be included together.
namespace ovl_title { PS2Runtime::FunctionRegion &functionRegion(); }
namespace ovl_casino { PS2Runtime::FunctionRegion &functionRegion(); }
namespace ovl_viewer { PS2Runtime::FunctionRegion &functionRegion(); }
namespace ovl_shop { PS2Runtime::FunctionRegion &functionRegion(); }
namespace ovl_menu { PS2Runtime::FunctionRegion &functionRegion(); }
namespace ovl_battle { PS2Runtime::FunctionRegion &functionRegion(); }

namespace dq8::overlay
{
namespace
{

struct Descriptor
{
    Id id;
    const char *slug;       // MWo3 header name, minus the .bin
    int loaderSlot;         // index into the game's path table at 0x003950F0
    PS2Runtime::FunctionRegion &(*region)();
};

// Ordered by the game's own path table (0x003950F0), which is NOT id order:
// FUN_00169860(slot) loads kOverlays[slot]. Kept in that order so a slot index
// seen in a trace indexes this array directly.
constexpr Descriptor kOverlays[] = {
    {Id::Title,  "title",  0, &ovl_title::functionRegion},
    {Id::Casino, "casino", 1, &ovl_casino::functionRegion},
    {Id::Viewer, "viewer", 2, &ovl_viewer::functionRegion},
    {Id::Battle, "battle", 3, &ovl_battle::functionRegion},
    {Id::Menu,   "menu",   4, &ovl_menu::functionRegion},
    {Id::Shop,   "shop",   5, &ovl_shop::functionRegion},
};

constexpr uint32_t kMwo3Magic = 0x336F574Du; // "MWo3" little-endian

std::atomic<PS2Runtime::FunctionRegion *> g_activeRegion{nullptr};
std::atomic<uint32_t> g_activeId{static_cast<uint32_t>(Id::None)};
std::atomic<uint32_t> g_loadCount{0u};
std::atomic<bool> g_verbose{false};

const Descriptor *descriptorFor(uint32_t id)
{
    for (const Descriptor &descriptor : kOverlays)
    {
        if (static_cast<uint32_t>(descriptor.id) == id)
        {
            return &descriptor;
        }
    }
    return nullptr;
}

// The MWo3 header as it sits in guest RAM once the loader's sceRead lands.
struct Mwo3Header
{
    uint32_t magic;
    uint32_t id;
    uint32_t loadAddress;
    uint32_t textSize;
    uint32_t dataSize;
    uint32_t bssSize;
    uint32_t ctorBegin;
    uint32_t ctorEnd;
    char name[32];
};
static_assert(sizeof(Mwo3Header) == 0x40, "MWo3 header must be 0x40 bytes");

bool readHeader(const uint8_t *rdram, uint32_t guestAddress, Mwo3Header &out)
{
    const uint32_t offset = guestAddress & PS2_RAM_MASK;
    if (static_cast<uint64_t>(offset) + sizeof(Mwo3Header) > PS2_RAM_SIZE)
    {
        return false;
    }
    std::memcpy(&out, rdram + offset, sizeof(out));
    return true;
}

// Everything the manager is willing to believe about an image before it will
// switch dispatch onto it. Cheap, but it is the only guard between a stray
// call to the notification stub and executing the wrong overlay's code.
bool headerLooksSane(const Mwo3Header &header, uint32_t loadAddress)
{
    if (header.magic != kMwo3Magic)
    {
        return false;
    }
    if (header.loadAddress != loadAddress || loadAddress != kArenaStart)
    {
        return false;
    }
    const uint64_t footprint = static_cast<uint64_t>(kHeaderSize) + header.textSize +
                               header.dataSize + header.bssSize;
    if (loadAddress + footprint > kArenaEnd)
    {
        return false;
    }
    const uint32_t dataEnd = loadAddress + kHeaderSize + header.textSize + header.dataSize;
    if (header.ctorBegin > header.ctorEnd || header.ctorEnd > dataEnd)
    {
        return false;
    }
    return true;
}

// Consulted by PS2Runtime for every guest PC its generated dense table does
// not cover, which for DQ8 means the overlay arena and nothing else.
PS2Runtime::FunctionRegion *resolveRegion(uint32_t address, void *)
{
    if (address < kArenaStart || address >= kArenaEnd)
    {
        return nullptr;
    }
    return g_activeRegion.load(std::memory_order_acquire);
}

// Switch dispatch after loading, before guest BSS clearing and constructors.
// Keeping those operations in guest code preserves reentrancy and scheduling.
void onImageLoaded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)runtime;

    uint32_t loadAddress = getRegU32(ctx, 4); // $a0 = destination, from 0x0010037C
    Mwo3Header header{};
    if (!readHeader(rdram, loadAddress, header) || !headerLooksSane(header, loadAddress))
    {
        // Fall back to the one address every US overlay is linked for, in case
        // a future build reaches this stub by another route.
        loadAddress = kArenaStart;
        if (!readHeader(rdram, loadAddress, header) || !headerLooksSane(header, loadAddress))
        {
            std::fprintf(stderr,
                         "[dq8-overlay] post-load hook fired but no valid MWo3 image at "
                         "0x%08X (magic 0x%08X)\n",
                         loadAddress, header.magic);
            return;
        }
    }

    const Descriptor *descriptor = descriptorFor(header.id);
    if (!descriptor)
    {
        std::fprintf(stderr, "[dq8-overlay] unknown MWo3 overlay id %u ('%.*s')\n",
                     header.id, static_cast<int>(sizeof(header.name)), header.name);
        return;
    }

    PS2Runtime::FunctionRegion &region = descriptor->region();
    g_activeRegion.store(&region, std::memory_order_release);
    g_activeId.store(static_cast<uint32_t>(descriptor->id), std::memory_order_release);
    const uint32_t count = g_loadCount.fetch_add(1u, std::memory_order_acq_rel) + 1u;

    if (g_verbose.load(std::memory_order_relaxed))
    {
        std::printf("[dq8-overlay] #%u %s.bin (id %u, slot %d) resident: "
                    ".text 0x%08X+0x%X .data +0x%X .bss +0x%X ctors 0x%08X..0x%08X; "
                    "dispatch 0x%08X..0x%08X\n",
                    count, descriptor->slug, header.id, descriptor->loaderSlot,
                    loadAddress + kHeaderSize, header.textSize, header.dataSize,
                    header.bssSize, header.ctorBegin, header.ctorEnd,
                    region.base, region.end);
        std::fflush(stdout);
    }
}

} // namespace

bool install(PS2Runtime &runtime)
{
    if (const char *env = std::getenv("DQ8_OVERLAY_VERBOSE"))
    {
        if (env[0] != '\0' && env[0] != '0')
        {
            g_verbose.store(true, std::memory_order_relaxed);
        }
    }

    // The resolver returns null until the first overlay becomes active.
    PS2Runtime::setFunctionRegionResolver(&resolveRegion, nullptr);

    if (!runtime.replaceFunction(kImageLoadedHook, &onImageLoaded))
    {
        std::fprintf(stderr,
                     "[dq8-overlay] could not hook 0x%08X; overlays will never become "
                     "active. Is it in the recompiled function table, and was install() "
                     "called after loadELF()?\n",
                     kImageLoadedHook);
        PS2Runtime::setFunctionRegionResolver(nullptr, nullptr);
        return false;
    }

    if (g_verbose.load(std::memory_order_relaxed))
    {
        std::printf("[dq8-overlay] arena 0x%08X..0x%08X dispatches through the active "
                    "overlay; post-load hook on 0x%08X\n",
                    kArenaStart, kArenaEnd, kImageLoadedHook);
    }
    return true;
}

Id active()
{
    return static_cast<Id>(g_activeId.load(std::memory_order_acquire));
}

const char *activeName()
{
    const Descriptor *descriptor = descriptorFor(g_activeId.load(std::memory_order_acquire));
    return descriptor ? descriptor->slug : "none";
}

uint32_t loadCount()
{
    return g_loadCount.load(std::memory_order_acquire);
}

void setVerbose(bool verbose)
{
    g_verbose.store(verbose, std::memory_order_relaxed);
}

} // namespace dq8::overlay
