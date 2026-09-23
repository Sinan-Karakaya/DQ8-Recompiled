#include "MiniTest.h"
#include "runtime/ee_fiber.h"
#include <cfenv>
#include <chrono>
#include <cstdio>

void register_ee_fiber_tests();

int main() {
    register_ee_fiber_tests();
    if (MiniTest::Run() != 0)
        return 1;
    struct State {
        EeFiber fiber;
        bool intact = true;
    } state;
    if (!state.fiber.create([](void *user) {
        auto &s = *static_cast<State *>(user);
        std::fesetround(FE_DOWNWARD);
        for (;;) {
            s.intact = s.intact && std::fegetround() == FE_DOWNWARD;
            s.fiber.suspend();
        }
    }, &state, 64 * 1024))
        return 1;
    std::fesetround(FE_UPWARD);
    constexpr int rounds = 1000000;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i)
        state.fiber.resume();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const bool intact = state.intact && std::fegetround() == FE_UPWARD;
    std::fesetround(FE_TONEAREST);
    std::printf("Fiber %s: %.1f ns/round-trip; separate FP modes: %s\n",
                EeFiber::usingFastSwitch() ? "native" : "ucontext",
                std::chrono::duration<double, std::nano>(elapsed).count() / rounds,
                intact ? "PASS" : "FAIL");
    return intact ? 0 : 1;
}
