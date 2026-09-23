#include "vu_bounds.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace dq8 {
namespace {
using Function = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
std::array<Function, 3> originals{};

float operand(float value) {
    auto bits = std::bit_cast<uint32_t>(value);
    const auto exponent = bits & 0x7f800000u;
    if (exponent == 0u) bits &= 0x80000000u;
    else if (exponent == 0x7f800000u) bits = (bits & 0x80000000u) | 0x7f7fffffu;
    return std::bit_cast<float>(bits);
}

struct Flags { uint32_t current = 0, mac = 0; };
Flags subtract(float (&result)[4], const float (&left)[4], const float (&right)[4]) {
    Flags flags;
    for (unsigned lane = 0; lane < 3; ++lane) {
        const double exact = static_cast<double>(operand(left[lane])) - operand(right[lane]);
        const double magnitude = std::fabs(exact);
        const uint32_t sign = std::signbit(exact) ? 0x80000000u : 0u;
        uint32_t current = sign ? 2u : 0u;
        if (magnitude == 0.0) {
            current |= 1u;
            result[lane] = std::bit_cast<float>(sign);
        } else if (magnitude < std::numeric_limits<float>::min()) {
            current |= 5u;
            result[lane] = std::bit_cast<float>(sign);
        } else if (magnitude > std::numeric_limits<float>::max()) {
            current |= 8u;
            result[lane] = std::bit_cast<float>(sign | 0x7f7fffffu);
        } else {
            result[lane] = static_cast<float>(exact);
        }
        flags.current |= current;
        for (unsigned bit = 0; bit < 4; ++bit)
            if (current & (1u << bit)) flags.mac |= (8u >> lane) << (bit * 4u);
    }
    return flags;
}

template <unsigned comparison>
void compare(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
    originals[comparison](rdram, ctx, runtime);
    repairVuBoundsFlags(*ctx, comparison);
    static const bool trace = std::getenv("DQ8_TRACE_VU_BOUNDS") != nullptr;
    if (trace) {
        static uint64_t calls = 0, rejected = 0;
        ++calls;
        rejected += getRegU32(ctx, 2) == 0u;
        if (calls <= 8 || (calls & (calls - 1u)) == 0u)
            std::fprintf(stderr, "[vu-bounds %u] calls=%llu rejected=%llu ra=%08x\n", comparison,
                         static_cast<unsigned long long>(calls), static_cast<unsigned long long>(rejected),
                         getRegU32(ctx, 31));
    }
}
}

void repairVuBoundsFlags(R5900Context &ctx, unsigned comparison) {
    // These leaf comparisons read STATUS after five NOPs. Their generated SUBs
    // already update VF25 but omit MAC/STATUS, making the result always true.
    constexpr unsigned sources[3][4] = {{1, 10, 11, 2}, {10, 2, 1, 11}, {10, 1, 1, 11}};
    float vectors[4][4];
    for (unsigned i = 0; i < 4; ++i)
        _mm_storeu_ps(vectors[i], ctx.vu0_vf[sources[comparison][i]]);
    float result[4];
    _mm_storeu_ps(result, ctx.vu0_vf[25]);
    const int rounding = std::fegetround();
    std::fesetround(FE_TOWARDZERO);
    const auto first = subtract(result, vectors[0], vectors[1]);
    const auto second = subtract(result, vectors[2], vectors[3]);
    std::fesetround(rounding);
    ctx.vu0_vf[25] = _mm_loadu_ps(result);
    ctx.vu0_mac_flags = second.mac;
    ctx.vu0_status = second.current | ((first.current | second.current) << 6u);
    SET_GPR_U64(&ctx, 2, (ctx.vu0_status & 0x80u) == 0u ? 1u : 0u);
}

bool installVuBoundsComparisons(PS2Runtime &runtime) {
    constexpr uint32_t addresses[] = {0x0013f660u, 0x0013f6b0u, 0x0013f700u};
    constexpr Function wrappers[] = {compare<0>, compare<1>, compare<2>};
    for (unsigned i = 0; i < 3; ++i) {
        originals[i] = runtime.lookupFunction(addresses[i]);
        if (!originals[i]) return false;
    }
    for (unsigned i = 0; i < 3; ++i)
        if (!runtime.replaceFunction(addresses[i], wrappers[i])) return false;
    return true;
}
}
