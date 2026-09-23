// Six MWo3 overlays share one guest arena. Dispatch arena addresses through
// the separately translated function table of the currently loaded image.
#ifndef DQ8_OVERLAY_MANAGER_H
#define DQ8_OVERLAY_MANAGER_H

#include <cstdint>

class PS2Runtime;

namespace dq8::overlay
{
// MWo3 header ids, from the `id` field at offset 0x04 of each BIN. These are
// distinct from the loader's path-table order in kOverlays.
enum class Id : uint32_t
{
    None = 0,
    Casino = 1,
    Viewer = 2,
    Shop = 3,
    Menu = 4,
    Battle = 5,
    Title = 6,
};

// The shared arena, verified against every PT_LOAD in the main ELF and every
// MWo3 header on the US disc. The guest heap begins at exactly kArenaEnd, so
// nothing here may ever write past it.
inline constexpr uint32_t kArenaStart = 0x00461C80u;
inline constexpr uint32_t kArenaEnd = 0x00489A80u;
inline constexpr uint32_t kHeaderSize = 0x40u;
inline constexpr uint32_t kTextStart = kArenaStart + kHeaderSize; // 0x00461CC0

// FUN_00100320 -- Metrowerks' post-load notification stub, `jr $ra; nop` in
// retail. Its single call site is 0x001003D4 inside the overlay loader
// FUN_00100330, reached only after the image has been read into RAM and before
// .bss is cleared or the constructor range is run, with $a0 = load address.
inline constexpr uint32_t kImageLoadedHook = 0x00100320u;

// The loader itself: `int __load_overlay(const char *path, void *dest)`.
// Not hooked -- documented here because it is the anchor for everything above.
inline constexpr uint32_t kOverlayLoader = 0x00100330u;

// Installs the arena dispatch resolver and the post-load hook. Call once,
// after PS2Runtime::loadELF() (which is what populates the main function
// table the hook is written into). Returns false if the hook could not be
// installed, in which case no overlay will ever become active.
bool install(PS2Runtime &runtime);

// Which image the manager believes is resident. Id::None before the first
// successful load.
Id active();
const char *activeName();

// How many images have been switched in since install(). Cheap boot-progress
// signal for the oracle loop: it should become 1 (title.bin) on the way to the
// title screen.
uint32_t loadCount();

// Per-load logging to stdout. Also enabled by setting DQ8_OVERLAY_VERBOSE=1.
void setVerbose(bool verbose);

} // namespace dq8::overlay

#endif // DQ8_OVERLAY_MANAGER_H
