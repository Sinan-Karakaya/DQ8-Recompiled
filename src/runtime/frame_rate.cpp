#include "frame_rate.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dq8 {
namespace {
using Function = PS2Runtime::RecompiledFunction;

// The renderer object (0x3E4890, at gp-0x70D8), its config at +0x260, and the
// routines around the frame loop. See frame_rate.h for the roles.
constexpr uint32_t kRenderRoutine = 0x00145080u; // waits for the interval, swaps buffers
constexpr uint32_t kWaitFields = 0x001604A0u;    // (start tick, fields): sleeps until that many fields passed
constexpr uint32_t kSetInterval = 0x00145680u;   // (renderer, interval, flag)
constexpr uint32_t kDisplayInit = 0x00146750u;   // (renderer, mode): resets the config, interval included
constexpr uint32_t kAfterFrame = 0x001653C0u;    // the main loop's first call after the frame loop
constexpr uint32_t kConfigOffset = 0x260u;       // renderer -> config pointer
constexpr uint32_t kIntervalOffset = 0x20u;      // config: fields per frame
constexpr uint32_t kDeltaOffset = 0xF00u;        // renderer: last frame's delta, in fields
// Two words the frame loop writes its CPU and GS load percentages into for a
// debug display, and reads back before the render routine. They are free from
// the render routine on, which is when the patched clamp and animation step
// read them.
constexpr uint32_t kClampMinSlot = 0x3D04u;
constexpr uint32_t kAnimationStepSlot = 0x3D08u;
constexpr int32_t kRendererGpOffset = -0x70D8;

std::atomic<double> g_targetHz{0.0};
Function g_renderRoutine = nullptr, g_waitFields = nullptr, g_setInterval = nullptr, g_displayInit = nullptr,
         g_afterFrame = nullptr;

// Executor-only state.
uint32_t g_nativeInterval = 2u; // the interval the game last asked for
bool g_forcedInterval = false;  // the config holds 1 because of a target, not the game
std::chrono::steady_clock::time_point g_nextFrame{};
bool g_pacing = false;

bool active() { return g_targetHz.load(std::memory_order_relaxed) != 0.0; }

uint32_t physical(uint32_t address) { return address & 0x1FFFFFFFu; }

bool readWord(const uint8_t *rdram, uint32_t address, uint32_t &word) {
    const uint32_t offset = physical(address);
    if (!rdram || offset > PS2_RAM_SIZE - sizeof(word))
        return false;
    std::memcpy(&word, rdram + offset, sizeof(word));
    return true;
}

void writeWord(uint8_t *rdram, uint32_t address, uint32_t word) {
    const uint32_t offset = physical(address);
    if (rdram && offset <= PS2_RAM_SIZE - sizeof(word))
        std::memcpy(rdram + offset, &word, sizeof(word));
}

void writeFloat(uint8_t *rdram, uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    writeWord(rdram, address, bits);
}

uint32_t configOf(const uint8_t *rdram, uint32_t renderer) {
    uint32_t config = 0u;
    return renderer != 0u && readWord(rdram, renderer + kConfigOffset, config) ? config : 0u;
}

// Fields per frame at the target rate: the clamp's lower bound.
float periodFields(double hz) {
    if (hz < 0.0)
        return 0.125f; // uncapped: eight frames per field at most, so the delta stays sane
    return static_cast<float>(60.0 / std::max(hz, 1.0));
}

// The render routine: force the interval the game waits for, and publish the
// clamp bound for the frame loop's patched clamp.
void renderRoutine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    const uint32_t renderer = getRegU32(ctx, 4);
    const uint32_t config = configOf(rdram, renderer);
    uint32_t interval = 0u;
    if (config != 0u && readWord(rdram, config + kIntervalOffset, interval)) {
        const double hz = g_targetHz.load(std::memory_order_relaxed);
        if (hz != 0.0) {
            if (interval != 1u) {
                writeWord(rdram, config + kIntervalOffset, 1u);
                g_forcedInterval = true;
            }
            writeFloat(rdram, renderer + kClampMinSlot, periodFields(hz));
        } else {
            // Back to the game's cadence: give it back the interval it asked for.
            if (g_forcedInterval) {
                writeWord(rdram, config + kIntervalOffset, g_nativeInterval);
                interval = g_nativeInterval;
                g_forcedInterval = false;
            }
            writeFloat(rdram, renderer + kClampMinSlot, static_cast<float>(interval));
        }
    }
    g_renderRoutine(rdram, ctx, runtime);
}

// The game's wait for N fields, replaced by a wait on the host clock.
void waitFields(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    const double hz = g_targetHz.load(std::memory_order_relaxed);
    if (hz == 0.0) {
        g_pacing = false;
        g_waitFields(rdram, ctx, runtime);
        return;
    }
    setReturnS32(ctx, 0);
    if (hz < 0.0)
        return; // uncapped: the next frame starts now
    EeScheduler &scheduler = runtime->eeScheduler();
    const auto now = scheduler.hostPacingNow();
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / hz));
    auto next = g_pacing ? g_nextFrame + period : now;
    // Late frames are not made up: a slow stretch would otherwise be followed
    // by a burst of frames without waits.
    if (next < now)
        next = now;
    g_nextFrame = next;
    g_pacing = true;
    if (next <= now)
        return;
    scheduler.waitHostDeadline(next, getRegU32(ctx, 31));
}

// The game asks for an interval at scene changes: remember it, and ask for 1
// instead while a target is set.
void setInterval(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    const uint32_t requested = getRegU32(ctx, 5);
    if (requested == 1u || requested == 2u)
        g_nativeInterval = requested;
    if (active()) {
        SET_GPR_U32(ctx, 5, 1u);
        g_forcedInterval = requested != 1u;
    } else
        g_forcedInterval = false;
    g_setInterval(rdram, ctx, runtime);
}

// Display init resets the config, interval included.
void displayInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    const uint32_t renderer = getRegU32(ctx, 4);
    const uint32_t returnPc = getRegU32(ctx, 31);
    g_displayInit(rdram, ctx, runtime);
    if (ctx->pc != returnPc && ctx->pc != kDisplayInit)
        return; // left on a yield; the next frame's render routine reapplies the interval
    const uint32_t config = configOf(rdram, renderer);
    uint32_t interval = 0u;
    if (config != 0u && readWord(rdram, config + kIntervalOffset, interval) && (interval == 1u || interval == 2u)) {
        g_nativeInterval = interval;
        g_forcedInterval = false;
        if (active() && interval != 1u) {
            writeWord(rdram, config + kIntervalOffset, 1u);
            g_forcedInterval = true;
        }
    }
}

// Right after the frame loop: the fresh delta, in fields, becomes the step of
// the fixed-step animations, in the frames the game natively ran at.
void afterFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    uint32_t renderer = 0u;
    if (readWord(rdram, getRegU32(ctx, 28) + static_cast<uint32_t>(kRendererGpOffset), renderer) && renderer != 0u) {
        float step = 1.0f;
        uint32_t deltaBits = 0u;
        if (active() && readWord(rdram, renderer + kDeltaOffset, deltaBits)) {
            float delta;
            std::memcpy(&delta, &deltaBits, sizeof(delta));
            if (std::isfinite(delta) && delta > 0.0f && delta < 16.0f)
                step = delta / static_cast<float>(std::max(g_nativeInterval, 1u));
        }
        writeFloat(rdram, renderer + kAnimationStepSlot, step);
    }
    g_afterFrame(rdram, ctx, runtime);
}

struct Hook {
    uint32_t address;
    Function replacement;
    Function *original;
};
} // namespace

bool installFrameRateHooks(PS2Runtime &runtime) {
    const Hook hooks[] = {
        {kRenderRoutine, &renderRoutine, &g_renderRoutine}, {kWaitFields, &waitFields, &g_waitFields},
        {kSetInterval, &setInterval, &g_setInterval},       {kDisplayInit, &displayInit, &g_displayInit},
        {kAfterFrame, &afterFrame, &g_afterFrame},
    };
    for (const Hook &hook : hooks) {
        if (*hook.original)
            continue;
        if (!runtime.hasFunction(hook.address)) {
            std::fprintf(stderr, "[frame-rate] unavailable: no function at %08x\n", hook.address);
            return false;
        }
        *hook.original = runtime.lookupFunction(hook.address);
    }
    for (const Hook &hook : hooks)
        if (!runtime.replaceFunction(hook.address, hook.replacement))
            return false;
    return true;
}

void setFrameRateTarget(double hz) {
    if (!std::isfinite(hz))
        hz = 0.0;
    if (hz > 0.0)
        hz = std::clamp(hz, 10.0, 1000.0);
    g_targetHz.store(hz, std::memory_order_relaxed);
}

double frameRateTarget() { return g_targetHz.load(std::memory_order_relaxed); }

bool applyFrameRateEnvironment(double displayHz) {
    const char *value = std::getenv("DQ8_FRAME_RATE");
    if (!value || !*value)
        return false;
    const std::string text = value;
    double hz = 0.0;
    if (text == "original" || text == "game")
        hz = 0.0;
    else if (text == "uncapped" || text == "unlimited")
        hz = -1.0;
    else if (text == "display")
        hz = displayHz > 0.0 ? displayHz : 60.0;
    else {
        char *end = nullptr;
        hz = std::strtod(value, &end);
        if (end == value || *end != '\0')
            return false;
        if (hz == 0.0)
            hz = -1.0;
        else if (hz <= 30.0)
            hz = 0.0;
    }
    setFrameRateTarget(hz);
    std::printf("[frame-rate] DQ8_FRAME_RATE=%s: %s\n", value,
                hz == 0.0 ? "the game's own cadence" : hz < 0.0 ? "uncapped" : "paced");
    return true;
}

} // namespace dq8
