#include "MiniTest.h"
#include "render_cadence.h"

#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

void reset_ps2_test_function_table();
#ifdef DQ8_CADENCE_GENERATED_SMOKE
void FUN_00145080_0x145080(uint8_t *, R5900Context *, PS2Runtime *);
#endif

namespace
{
constexpr uint32_t entry = 0x145080u, end = 0x145658u, resume = 0x14546Cu;
enum class Mode { Complete, WrongTarget, WrongBranch, DelaySlot, Yield, Nested, Throw };
Mode mode = Mode::Complete;

void setEnvironment(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

void fakeRender(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (mode == Mode::Throw)
        throw std::runtime_error("simulated dispatcher unwind");
    if (mode == Mode::Yield && ctx->pc == entry)
    {
        ctx->pc = resume;
        ctx->branch_pc = 0x145620u;
        return;
    }
    if (mode == Mode::Nested && ctx->pc == entry)
    {
        ctx->pc = resume;
        runtime->lookupFunction(resume)(ram, ctx, runtime);
        return;
    }
    ctx->pc = mode == Mode::WrongTarget ? 0x1439BCu : 0x1439B8u;
    ctx->branch_pc = mode == Mode::WrongBranch ? 0x14564Cu : 0x145650u;
    ctx->in_delay_slot = mode == Mode::DelaySlot;
}

void unrelated(uint8_t *, R5900Context *, PS2Runtime *) {}

void registerRange(PS2Runtime &runtime, PS2Runtime::RecompiledFunction function)
{
    reset_ps2_test_function_table();
    for (uint32_t pc = entry; pc < end; pc += 4u)
        runtime.registerFunction(pc, function);
}
}

int main()
{
    auto runtime = std::make_unique<PS2Runtime>();
    runtime->memory().initialize();
    auto *ram = runtime->memory().getRDRAM();
    auto &probe = dq8::diagnostics::RenderCadenceProbe::instance();
    setEnvironment("DQ8_RENDER_CADENCE", "1");
    setEnvironment("DQ8_GFX_SHOW_FPS", nullptr);
    setEnvironment("DQ8_RENDER_CADENCE_INTERVAL_MS", "60000");
    MiniTest::Case("DQ8 completed render cadence", [&](TestCase &tc) {
        tc.Run("disabled and unexpected interior leave registry intact", [&](TestCase &t) {
            registerRange(*runtime, fakeRender);
            setEnvironment("DQ8_RENDER_CADENCE", nullptr);
            t.IsTrue(!probe.configure(*runtime), "disabled probe installs no wrappers");
            t.IsTrue(runtime->lookupFunction(entry) == fakeRender, "original remains installed");
            setEnvironment("DQ8_RENDER_CADENCE", "1");
            runtime->replaceFunction(resume, unrelated);
            t.IsTrue(!probe.configure(*runtime), "unexpected interior rejects the whole installation");
            t.IsTrue(runtime->lookupFunction(entry) == fakeRender, "failed install is atomic");
            t.IsTrue(runtime->lookupFunction(resume) == unrelated, "unrelated function is preserved");
        });
        tc.Run("all registered entries and exact return state", [&](TestCase &t) {
            registerRange(*runtime, fakeRender);
            runtime->replaceFunction(entry + 4u, nullptr);
            runtime->registerFunction(end, unrelated);
            t.IsTrue(probe.configure(*runtime), "install succeeds");
            t.IsTrue(probe.configure(*runtime), "repeat install does not wrap the wrapper");
            t.IsTrue(!runtime->hasFunction(entry + 4u), "unregistered slot remains unregistered");
            t.IsTrue(runtime->lookupFunction(end) == unrelated, "end of range is excluded");
            const auto before = probe.completedRoutines();
            mode = Mode::Complete;
            unsigned entries = 0u;
            for (uint32_t pc = entry; pc < end; pc += 4u)
            {
                if (!runtime->hasFunction(pc)) continue;
                R5900Context ctx{};
                ctx.pc = pc;
                runtime->lookupFunction(pc)(ram, &ctx, runtime.get());
                ++entries;
            }
            t.Equals(probe.completedRoutines() - before, uint64_t(entries), "each interior return counts once");
            for (const auto control : {Mode::WrongTarget, Mode::WrongBranch, Mode::DelaySlot})
            {
                mode = control;
                R5900Context ctx{};
                ctx.pc = entry;
                runtime->lookupFunction(entry)(ram, &ctx, runtime.get());
            }
            t.Equals(probe.completedRoutines() - before, uint64_t(entries), "all nonmatching states are ignored");
        });
        tc.Run("direct dispatch, resumed return, nested unwind and exception", [&](TestCase &t) {
            registerRange(*runtime, fakeRender);
            t.IsTrue(probe.configure(*runtime), "fixture registry is wrapped");
            const auto before = probe.completedRoutines();
            R5900Context ctx{};
            ctx.pc = entry;
            mode = Mode::Complete;
            t.IsTrue(runtime->dispatchGuestBranch(ram, &ctx, entry, 0x1439B0u, 0x1439B8u,
                         PS2Runtime::GuestBranchKind::DirectCall, "cadence test"), "direct call resumes caller");
            t.Equals(probe.completedRoutines() - before, uint64_t(1), "direct dispatch counts once");
            ctx.pc = entry;
            mode = Mode::Yield;
            runtime->lookupFunction(ctx.pc)(ram, &ctx, runtime.get());
            t.Equals(probe.completedRoutines() - before, uint64_t(1), "yield before completion does not count");
            t.Equals(ctx.pc, resume, "yield preserves interior continuation");
            runtime->lookupFunction(ctx.pc)(ram, &ctx, runtime.get());
            t.Equals(probe.completedRoutines() - before, uint64_t(2), "scheduler-style interior resume counts once");
            ctx.pc = entry;
            mode = Mode::Nested;
            runtime->lookupFunction(ctx.pc)(ram, &ctx, runtime.get());
            t.Equals(probe.completedRoutines() - before, uint64_t(3), "nested native unwind cannot duplicate completion");
            ctx.pc = entry;
            mode = Mode::Throw;
            bool caught = false;
            try { runtime->lookupFunction(ctx.pc)(ram, &ctx, runtime.get()); }
            catch (const std::runtime_error &) { caught = true; }
            t.IsTrue(caught, "exception propagates unchanged");
            t.Equals(probe.completedRoutines() - before, uint64_t(3), "exception is not a completion");
        });
        tc.Run("FPS display counts completed returns with cadence logging disabled", [&](TestCase &t) {
            registerRange(*runtime, fakeRender);
            setEnvironment("DQ8_RENDER_CADENCE", "0");
            setEnvironment("DQ8_GFX_SHOW_FPS", "1");
            t.IsTrue(probe.configure(*runtime), "FPS alone enables the completion hook");
            const auto before = probe.publishedCompletedRoutines();
            mode = Mode::Complete;
            for (unsigned frame = 0; frame < 60u; ++frame) {
                R5900Context ctx{};
                ctx.pc = entry;
                runtime->lookupFunction(entry)(ram, &ctx, runtime.get());
                for (unsigned repeat = 0; repeat < 4u; ++repeat)
                    t.Equals(probe.publishedCompletedRoutines() - before, uint64_t(frame + 1u),
                             "repeated host observations do not invent game frames");
            }
            t.Equals(probe.publishedCompletedRoutines(), probe.completedRoutines(),
                     "published count matches the executor without guest memory reads");
            setEnvironment("DQ8_GFX_SHOW_FPS", nullptr);
            setEnvironment("DQ8_RENDER_CADENCE", "1");
        });
        tc.Run("logging cap does not stop the published render counter", [&](TestCase &t) {
            registerRange(*runtime, fakeRender);
            setEnvironment("DQ8_RENDER_CADENCE_INTERVAL_MS", "100");
            setEnvironment("DQ8_RENDER_CADENCE_LIMIT", "1");
            t.IsTrue(probe.configure(*runtime), "bounded logger is installed");
            mode = Mode::Complete;
            const auto complete = [&] {
                R5900Context ctx{};
                ctx.pc = entry;
                runtime->lookupFunction(entry)(ram, &ctx, runtime.get());
            };
            complete();
            std::this_thread::sleep_for(std::chrono::milliseconds(125));
            complete();
            const auto before = probe.publishedCompletedRoutines();
            for (unsigned frame = 0; frame < 10u; ++frame)
                complete();
            t.Equals(probe.publishedCompletedRoutines() - before, uint64_t{10},
                     "FPS counting outlives bounded log output");
            setEnvironment("DQ8_RENDER_CADENCE_LIMIT", nullptr);
            setEnvironment("DQ8_RENDER_CADENCE_INTERVAL_MS", "60000");
        });
#ifdef DQ8_CADENCE_GENERATED_SMOKE
        tc.Run("actual normal-build generated epilogue preserves context and RAM", [&](TestCase &t) {
            registerRange(*runtime, FUN_00145080_0x145080);
            t.IsTrue(probe.configure(*runtime), "actual generated function is wrapped");
            R5900Context initial{};
            initial.pc = resume;
            initial.r[20] = _mm_set_epi64x(0, 0x110000u);
            initial.r[28] = _mm_set_epi64x(0, 0x100000u);
            initial.r[29] = _mm_set_epi64x(0, 0x120000u);
            const uint64_t ra = 0x1439B8u;
            std::memcpy(ram + 0x120090u, &ra, sizeof(ra));
            const std::vector<uint8_t> originalRam(ram, ram + PS2_RAM_SIZE);
            R5900Context expected = initial;
            FUN_00145080_0x145080(ram, &expected, runtime.get());
            const std::vector<uint8_t> expectedRam(ram, ram + PS2_RAM_SIZE);
            std::memcpy(ram, originalRam.data(), originalRam.size());
            R5900Context actual = initial;
            const auto before = probe.completedRoutines();
            runtime->lookupFunction(resume)(ram, &actual, runtime.get());
            t.Equals(actual.pc, uint32_t(0x1439B8u), "normal JR reaches the actual caller continuation");
            t.Equals(actual.branch_pc, uint32_t(0x145650u), "normal JR retains the exact source branch");
            t.Equals(probe.completedRoutines() - before, uint64_t(1), "normal build observes actual generated JR");
            t.IsTrue(std::memcmp(&actual, &expected, sizeof(actual)) == 0, "entire guest context is unchanged");
            t.IsTrue(std::memcmp(ram, expectedRam.data(), PS2_RAM_SIZE) == 0, "entire RDRAM is unchanged");
        });
#endif
    });
    return MiniTest::Run();
}
