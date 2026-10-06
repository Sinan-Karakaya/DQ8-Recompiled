#include "screen_size.h"

#include "ps2_runtime.h"

#include <atomic>

namespace dq8 {
namespace {
// The option is a byte at gp-0x7014: 0x168FF0 stores it, and 0x168F90 turns
// it into the projection's 0.75 or 0.5625. Reading it before the original
// runs makes the entries enough: a call resumed after a yield needs no hook.
constexpr uint32_t kSetScreenSize = 0x00168FF0u;
constexpr uint32_t kApplyScreenSize = 0x00168F90u;
constexpr int32_t kScreenSizeOffset = -0x7014;

// Written by the executor, read by the presenter; never guest memory itself.
std::atomic<int> g_screenSize{-1};
PS2Runtime::RecompiledFunction g_setScreenSize = nullptr;
PS2Runtime::RecompiledFunction g_applyScreenSize = nullptr;

void publish(int8_t value) { g_screenSize.store(value == 1 ? 1 : 0, std::memory_order_relaxed); }

void setScreenSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    publish(static_cast<int8_t>(getRegU32(ctx, 4)));
    g_setScreenSize(rdram, ctx, runtime);
}

void applyScreenSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    const uint32_t address = (getRegU32(ctx, 28) + static_cast<uint32_t>(kScreenSizeOffset)) & 0x1FFFFFFFu;
    if (address < PS2_RAM_SIZE)
        publish(static_cast<int8_t>(rdram[address]));
    g_applyScreenSize(rdram, ctx, runtime);
}
} // namespace

bool installScreenSizeProbe(PS2Runtime &runtime) {
    if (g_setScreenSize)
        return true;
    if (!runtime.hasFunction(kSetScreenSize) || !runtime.hasFunction(kApplyScreenSize))
        return false;
    g_setScreenSize = runtime.lookupFunction(kSetScreenSize);
    g_applyScreenSize = runtime.lookupFunction(kApplyScreenSize);
    return runtime.replaceFunction(kSetScreenSize, &setScreenSize) &&
           runtime.replaceFunction(kApplyScreenSize, &applyScreenSize);
}

int gameScreenSize() { return g_screenSize.load(std::memory_order_relaxed); }

} // namespace dq8
