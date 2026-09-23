#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "vu_bounds.h"

#include <bit>
#include <cstring>
#include <memory>

int main() {
    MiniTest::Case("DQ8 VU bounds", [](TestCase &tc) {
        tc.Run("comparison state matches VU arithmetic", [](TestCase &t) {
            auto vu = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU0);
            auto gs = std::make_unique<GS>();
            constexpr unsigned sources[3][4] = {{1, 10, 11, 2}, {10, 2, 1, 11}, {10, 1, 1, 11}};
            constexpr uint32_t edges[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff,
                0x00800000, 0x80800000, 0x3f800000, 0xbf800000, 0x7f7fffff,
                0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00001, 0xffc00001};
            uint32_t seed = 1;
            unsigned accepted = 0, rejected = 0;
            for (unsigned comparison = 0; comparison < 3; ++comparison) {
                uint32_t code[16]{};
                uint8_t data[16]{};
                for (unsigned pair = 0; pair < 8; ++pair) code[pair * 2 + 1] = 0x800002ffu;
                const auto sub = [](unsigned fs, unsigned ft) {
                    return 0x8000002cu | (0xeu << 21) | (ft << 16) | (fs << 11) | (25u << 6);
                };
                code[1] = sub(sources[comparison][0], sources[comparison][1]);
                code[3] = sub(sources[comparison][2], sources[comparison][3]);
                code[13] |= 0x40000000u;
                for (unsigned sample = 0; sample < 512; ++sample) {
                    R5900Context ctx{};
                    vu->reset();
                    for (unsigned reg : {1u, 2u, 10u, 11u, 25u}) {
                        float lanes[4];
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            seed = seed * 1664525u + 1013904223u;
                            lanes[lane] = sample < 256 ? std::bit_cast<float>(edges[(seed >> 16) % 15])
                                                       : static_cast<float>(static_cast<int32_t>(seed) % 10000);
                        }
                        ctx.vu0_vf[reg] = _mm_loadu_ps(lanes);
                        std::memcpy(vu->state().vf[reg], lanes, sizeof(lanes));
                    }
                    dq8::repairVuBoundsFlags(ctx, comparison);
                    vu->execute(reinterpret_cast<uint8_t *>(code), sizeof(code), data, sizeof(data),
                                *gs, nullptr, 0, 0, 0, 64);
                    t.Equals(ctx.vu0_status, vu->state().status, "all current and sticky status bits must match");
                    t.Equals(ctx.vu0_mac_flags, vu->state().mac, "all MAC lane flags must match");
                    t.IsTrue(std::memcmp(&ctx.vu0_vf[25], vu->state().vf[25], 16) == 0,
                             "VF25, including its untouched W lane, must match");
                    const uint32_t expected = (vu->state().status & 0x80u) == 0u;
                    t.Equals(getRegU32(&ctx, 2), expected, "the guest result must depend on the comparisons");
                    expected ? ++accepted : ++rejected;
                }
            }
            t.IsTrue(accepted > 0 && rejected > 0, "exercise both accepted and rejected bounds");
        });
    });
    return MiniTest::Run();
}
