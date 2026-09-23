#pragma once

#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace dq8::diagnostics
{
class RenderCadenceProbe
{
public:
    bool configure(PS2Runtime &runtime)
    {
        const auto enabled = [](const char *name) {
            const char *value = std::getenv(name);
            return value && std::strcmp(value, "0") != 0;
        };
        m_logEnabled = enabled("DQ8_RENDER_CADENCE");
        if (!m_logEnabled && !enabled("DQ8_GFX_SHOW_FPS"))
            return false;
        m_limit = envBounded("DQ8_RENDER_CADENCE_LIMIT", 600u, 10000u);
        m_intervalMs = envBounded("DQ8_RENDER_CADENCE_INTERVAL_MS", 2000u, 60000u);
        m_intervalMs = std::max(m_intervalMs, 100u);
        if (!runtime.hasFunction(kEntry))
        {
            std::fprintf(stderr, "[render-cadence] unavailable: guest render routine is not registered\n");
            return false;
        }
        const auto original = runtime.lookupFunction(kEntry);
        if (original == &executeRenderRoutine)
            return true;
        uint32_t entries = 0u;
        for (uint32_t pc = kEntry; pc < kEnd; pc += 4u)
        {
            if (!runtime.hasFunction(pc))
                continue;
            if (runtime.lookupFunction(pc) != original)
            {
                std::fprintf(stderr, "[render-cadence] unavailable: unexpected interior function at %08x\n", pc);
                return false;
            }
            ++entries;
        }
        m_original = original;
        for (uint32_t pc = kEntry; pc < kEnd; pc += 4u)
            if (runtime.hasFunction(pc))
                runtime.replaceFunction(pc, &executeRenderRoutine);
        std::fprintf(stderr,
                     "[render-cadence] enabled: completed guest edge 145650->1439b8; "
                     "wrapped=%u entries, interval=%u ms, limit=%u samples; not a simulation-step counter\n",
                     entries, m_intervalMs, m_limit);
        return true;
    }

    // Read on the executor thread, or after it stops.
    uint64_t completedRoutines() const noexcept { return m_total; }
    // Only the counter crosses threads; the presenter never reads guest state.
    uint64_t publishedCompletedRoutines() const noexcept {
        return m_publishedTotal.load(std::memory_order_relaxed);
    }

    static RenderCadenceProbe &instance()
    {
        static RenderCadenceProbe probe;
        return probe;
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr uint32_t kEntry = 0x00145080u, kEnd = 0x00145658u;

    static void executeRenderRoutine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto &probe = instance();
        const uint64_t before = probe.m_total;
        probe.m_original(rdram, ctx, runtime);
        // Normal generated JR returns bypass dispatchGuestBranch. All registered
        // interior entries must be wrapped too, because yields resume there.
        // A nested wrapped invocation can leave the same context while its
        // native callers unwind. Count that completion only at the inner edge.
        if (probe.m_total == before && ctx->pc == 0x001439B8u &&
            ctx->branch_pc == 0x00145650u && !ctx->in_delay_slot)
            probe.sample(rdram, ctx, runtime);
    }

    static uint32_t envBounded(const char *name, uint32_t fallback, uint32_t maximum)
    {
        const char *value = std::getenv(name);
        if (!value || !*value)
            return fallback;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        return end != value && *end == '\0' && parsed > 0u && parsed <= maximum
                   ? static_cast<uint32_t>(parsed)
                   : fallback;
    }

    static bool readWord(const uint8_t *rdram, uint32_t base, int32_t offset, uint32_t &word)
    {
        const int64_t physical = static_cast<int64_t>(base & 0x1FFFFFFFu) + offset;
        if (!rdram || physical < 0 || physical > static_cast<int64_t>(PS2_RAM_SIZE - sizeof(word)))
            return false;
        std::memcpy(&word, rdram + physical, sizeof(word));
        return true;
    }

    void sample(const uint8_t *rdram, const R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!ctx || !runtime)
            return;
        ++m_total;
        m_publishedTotal.store(m_total, std::memory_order_relaxed);
        if (!m_logEnabled || m_samples >= m_limit)
            return;
        const uint32_t gp = getRegU32(ctx, 28);
        uint32_t renderer = 0u, config = 0u, fields = 0u, savedTick = 0u;
        uint32_t interval = 0u, deltaBits = 0u;
        const bool valid = readWord(rdram, gp, -0x70D8, renderer) && renderer != 0u &&
                           readWord(rdram, renderer, 0x260, config) && config != 0u &&
                           readWord(rdram, config, 0x20, interval) &&
                           readWord(rdram, gp, -0x70C0, fields) &&
                           readWord(rdram, renderer, 0xEF0, savedTick) &&
                           readWord(rdram, renderer, 0xF00, deltaBits);
        const uint64_t tick = runtime->memory().gs().vsyncTick.load(std::memory_order_acquire);
        const auto now = Clock::now();
        if (m_start == Clock::time_point{})
        {
            resetWindow(now, tick, fields, valid);
            return;
        }
        ++m_count;
        m_invalid += valid ? 0u : 1u;
        const double seconds = std::chrono::duration<double>(now - m_start).count();
        if (seconds * 1000.0 < m_intervalMs)
            return;
        float previousDelta = 0.0f;
        std::memcpy(&previousDelta, &deltaBits, sizeof(previousDelta));
        char guestData[192];
        if (valid)
            std::snprintf(guestData, sizeof(guestData),
                          " renderer=%08x config=%08x interval=%u saved-field=%u previous-delta=%.6g",
                          renderer, config, interval, savedTick, static_cast<double>(previousDelta));
        else
            std::snprintf(guestData, sizeof(guestData), " guest-data=invalid");
        std::fprintf(stderr,
                     "[render-cadence] %.2f completed/s, %.2f guest-vblank/s, "
                     "%.2f guest-field/s over %.3f s; total=%llu tick=%llu "
                     "invalid-window=%u%s\n",
                     m_count / seconds,
                     tick >= m_startTick ? (tick - m_startTick) / seconds : 0.0,
                     m_startValid && m_invalid == 0u
                         ? ((fields - m_startField) & 0x7FFFFFFFu) / seconds
                         : std::numeric_limits<double>::quiet_NaN(),
                     seconds, static_cast<unsigned long long>(m_total),
                     static_cast<unsigned long long>(tick), m_invalid, guestData);
        ++m_samples;
        resetWindow(now, tick, fields, valid);
    }

    void resetWindow(Clock::time_point now, uint64_t tick, uint32_t field, bool valid)
    {
        m_start = now;
        m_startTick = tick;
        m_startField = field;
        m_startValid = valid;
        m_count = 0u;
        m_invalid = 0u;
    }

    // All samples run on the sole guest executor; the host never reads RDRAM.
    Clock::time_point m_start{};
    uint64_t m_startTick = 0u, m_total = 0u, m_count = 0u;
    std::atomic<uint64_t> m_publishedTotal{0u};
    uint32_t m_startField = 0u, m_invalid = 0u, m_samples = 0u;
    uint32_t m_intervalMs = 2000u, m_limit = 600u;
    bool m_startValid = false;
    bool m_logEnabled = false;
    PS2Runtime::RecompiledFunction m_original = nullptr;
};

inline bool configureRenderCadenceProbe(PS2Runtime &runtime)
{
    return RenderCadenceProbe::instance().configure(runtime);
}
} // namespace dq8::diagnostics
