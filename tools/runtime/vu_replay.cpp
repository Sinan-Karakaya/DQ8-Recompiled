#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> read(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

uint64_t hash(const void *bytes, size_t size, uint64_t value = 14695981039346656037ull) {
    const auto *data = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < size; ++i)
        value = (value ^ data[i]) * 1099511628211ull;
    return value;
}

bool verifyBoundaries(const std::vector<uint8_t> &code, const std::vector<uint8_t> &data,
                      const VU1State &initial, bool vu0, const char *profilePath = nullptr) {
    std::set<std::array<uint64_t, 4>> blocks;
    struct Context {
        PS2Memory memory;
        GS gs;
        VU1Interpreter vu;
        uint8_t *code = nullptr;
        uint8_t *data = nullptr;
        std::vector<uint8_t> uncachedCode;
        std::vector<std::vector<uint8_t>> packets;
        bool patchOnPacket = false;
        bool patchedDuringPacket = false;
        uint32_t patchedPC = 0u;
        uint64_t patchedOriginal = 0u;
        bool isVu0;
        explicit Context(bool vu0) : vu(vu0 ? VU1Interpreter::Unit::VU0 : VU1Interpreter::Unit::VU1), isVu0(vu0) {}
        void writeCode(uint32_t pc, uint64_t instruction) {
            memory.write64((isVu0 ? PS2_VU0_CODE_BASE : PS2_VU1_CODE_BASE) + pc, instruction);
            if (!uncachedCode.empty())
                std::memcpy(code + pc, &instruction, sizeof(instruction));
        }
    };
    auto referenceContext = std::make_unique<Context>(vu0);
    auto compiledContext = std::make_unique<Context>(vu0);
    auto &reference = *referenceContext;
    auto &compiled = *compiledContext;
    for (auto *ctx : {&reference, &compiled}) {
        if (!ctx->memory.initialize()) return false;
        ctx->gs.init(ctx->memory.getGSVRAM(), PS2_GS_VRAM_SIZE, &ctx->memory.gs());
        ctx->code = vu0 ? ctx->memory.getVU0Code() : ctx->memory.getVU1Code();
        ctx->data = vu0 ? ctx->memory.getVU0Data() : ctx->memory.getVU1Data();
        std::memcpy(ctx->code, code.data(), code.size());
        if (vu0) ctx->memory.markVU0CodeModified();
        else ctx->memory.markVU1CodeModified();
        // The oracle decodes raw bytes, independently of either execution cache.
        if (ctx == &reference) {
            ctx->uncachedCode = code;
            ctx->code = ctx->uncachedCode.data();
        }
        ctx->memory.setGifPacketCallback([ctx](const uint8_t *packet, uint32_t size) {
            ctx->packets.emplace_back(packet, packet + size);
            if (ctx->patchOnPacket) {
                ctx->patchOnPacket = false;
                ctx->patchedDuringPacket = true;
                ctx->patchedPC = ctx->vu.state().pc;
                std::memcpy(&ctx->patchedOriginal, ctx->code + ctx->patchedPC, sizeof(uint64_t));
                ctx->writeCode(ctx->patchedPC, 0x400002ff10060007ull);
            }
        });
    }
    reference.vu.setCompiledExecutionEnabled(false);
    compiled.vu.setCompiledExecutionEnabled(true);
    const auto reset = [&](Context &ctx) {
        ctx.vu.reset();
        ctx.vu.state() = initial;
        std::memcpy(ctx.data, data.data(), data.size());
        ctx.packets.clear();
    };
    const auto run = [&](Context &ctx, uint32_t cycles) {
        ctx.vu.resume(ctx.code, static_cast<uint32_t>(code.size()), ctx.data,
                      static_cast<uint32_t>(data.size()), ctx.gs, &ctx.memory,
                      initial.top, initial.itop, cycles);
    };
    const auto equal = [&] {
        return std::memcmp(&reference.vu.state(), &compiled.vu.state(), sizeof(VU1State)) == 0 &&
               std::memcmp(reference.data, compiled.data, data.size()) == 0 &&
               reference.packets == compiled.packets;
    };
    reset(reference);
    run(reference, 65536u);
    const uint64_t endCycle = reference.vu.state().cycles;
    const VU1State finalState = reference.vu.state();
    const std::vector<uint8_t> finalData(reference.data, reference.data + data.size());
    const auto finalPackets = reference.packets;
    uint64_t boundaries = 0;
    for (uint32_t budget : {1u, 2u, 3u, 7u, 31u, 127u, 65536u}) {
        reset(reference);
        reset(compiled);
        uint32_t blockRemaining = 0u;
        for (uint64_t previous = 0; previous < endCycle;) {
            const uint32_t pc = reference.vu.state().pc;
            if (profilePath && budget == 1u && blockRemaining == 0u && pc + 32ull <= code.size()) {
                std::array<uint64_t, 4> words;
                std::memcpy(words.data(), code.data() + pc, sizeof(words));
                blocks.insert(words);
                blockRemaining = 4u;
            }
            const uint32_t slice = static_cast<uint32_t>(std::min<uint64_t>(budget, endCycle - previous));
            run(reference, slice);
            run(compiled, slice);
            if (blockRemaining && reference.vu.state().pc != pc) {
                --blockRemaining;
                if (reference.vu.state().pc != pc + 8u)
                    blockRemaining = 0u;
            }
            if (!equal()) {
                std::fprintf(stderr, "Differential mismatch: budget=%u boundary=%llu ref PC=%x native PC=%x\n",
                             budget, static_cast<unsigned long long>(previous),
                             reference.vu.state().pc, compiled.vu.state().pc);
                return false;
            }
            ++boundaries;
            const auto current = reference.vu.state().cycles;
            if (current <= previous) return false;
            previous = current;
        }
        if (std::memcmp(&reference.vu.state(), &finalState, sizeof(VU1State)) != 0 ||
            std::memcmp(reference.data, finalData.data(), data.size()) != 0 ||
            reference.packets != finalPackets) {
            std::fprintf(stderr, "Resumed execution differs from uninterrupted execution: budget=%u\n", budget);
            return false;
        }
    }
    // Editing any pair must invalidate every overlapping native block.
    for (uint32_t offset : {0u, 8u, 16u, 24u}) {
        const uint32_t patchPC = (initial.pc + offset) % static_cast<uint32_t>(code.size());
        uint64_t original;
        std::memcpy(&original, code.data() + patchPC, sizeof(original));
        for (auto *ctx : {&reference, &compiled}) reset(*ctx);
        for (uint32_t slice = 0; slice < 64u; ++slice) {
            if (slice == 1u || slice == 16u) {
                const uint64_t instruction = slice == 1u ? 0x000002ff10060007ull : original;
                for (auto *ctx : {&reference, &compiled})
                    ctx->writeCode(patchPC, instruction);
            }
            run(reference, 1u);
            run(compiled, 1u);
            if (!equal()) {
                std::fprintf(stderr, "Microcode invalidation mismatch: offset=%u slice=%u\n", offset, slice);
                return false;
            }
            ++boundaries;
        }
        // Enter at the block head with a wide budget after changing a later pair.
        for (uint64_t instruction : std::array<uint64_t, 2>{0x000002ff10060007ull, original}) {
            for (auto *ctx : {&reference, &compiled}) {
                ctx->writeCode(patchPC, instruction);
                reset(*ctx);
                run(*ctx, 127u);
            }
            if (!equal()) {
                std::fprintf(stderr, "Overlapping block invalidation mismatch: offset=%u\n", offset);
                return false;
            }
            ++boundaries;
        }
    }
    if (!finalPackets.empty()) {
        for (auto *ctx : {&reference, &compiled}) {
            reset(*ctx);
            ctx->patchOnPacket = true;
            run(*ctx, 65536u);
        }
        if (!equal() || !reference.patchedDuringPacket || !compiled.patchedDuringPacket) {
            std::fprintf(stderr, "Code modification during a GIF callback differs\n");
            return false;
        }
        for (auto *ctx : {&reference, &compiled})
            ctx->writeCode(ctx->patchedPC, ctx->patchedOriginal);
        ++boundaries;
    }
    std::printf("Verified %llu resumed boundaries; native pairs=%llu fallback pairs=%llu\n",
                static_cast<unsigned long long>(boundaries),
                static_cast<unsigned long long>(compiled.vu.compiledPairsExecuted()),
                static_cast<unsigned long long>(compiled.vu.interpretedPairsExecuted()));
    if (profilePath) {
        std::ofstream output(profilePath);
        output << "VU-BLOCKS 1\n";
        for (const auto &block : blocks) {
            output << (vu0 ? 0 : 1);
            for (uint64_t word : block)
                output << ' ' << std::hex << std::setfill('0') << std::setw(16) << word;
            output << '\n';
        }
        if (!output) return false;
        std::printf("Recorded %zu executed block entries in %s\n", blocks.size(), profilePath);
    }
    return true;
}
} // namespace

// Captures are local .code/.data/.state files; state uses this build's VU1State ABI.
int main(int argc, char **argv) {
    const char *profilePath = argc == 4 && std::strncmp(argv[3], "--emit-aot=", 11u) == 0 ? argv[3] + 11u : nullptr;
    if (argc < 2 || argc > 4 || (argc == 4 && !profilePath && std::strcmp(argv[3], "--verify") != 0) ||
        (profilePath && !*profilePath)) {
        std::fprintf(stderr, "Usage: dq8-vu-replay CAPTURE_PREFIX [REPETITIONS [--verify|--emit-aot=PATH]]\n");
        return 2;
    }
    char *end = nullptr;
    const unsigned long repetitions = argc >= 3 ? std::strtoul(argv[2], &end, 10) : 1000u;
    if (repetitions == 0 || repetitions > 1000000u || (end && *end))
        return 2;
    const std::string base = argv[1];
    const auto code = read(base + ".code");
    const auto data = read(base + ".data");
    const auto state = read(base + ".state");
    const bool vu0 = code.size() == PS2_VU0_CODE_SIZE;
    if ((!vu0 && code.size() != PS2_VU1_CODE_SIZE) ||
        data.size() != (vu0 ? PS2_VU0_DATA_SIZE : PS2_VU1_DATA_SIZE) ||
        state.size() != sizeof(VU1State)) {
        std::fprintf(stderr, "Missing capture or incompatible sizes: code=%zu data=%zu state=%zu\n",
                     code.size(), data.size(), state.size());
        return 2;
    }
    VU1State initial{};
    std::memcpy(&initial, state.data(), sizeof(initial));
    if (initial.pc + 8ull > code.size())
        return 2;
    PS2Memory memory;
    if (!memory.initialize())
        return 2;
    GS gs;
    gs.init(memory.getGSVRAM(), PS2_GS_VRAM_SIZE, &memory.gs());
    VU1Interpreter vu(vu0 ? VU1Interpreter::Unit::VU0 : VU1Interpreter::Unit::VU1);
    uint8_t *vuCode = vu0 ? memory.getVU0Code() : memory.getVU1Code();
    uint8_t *vuData = vu0 ? memory.getVU0Data() : memory.getVU1Data();
    std::memcpy(vuCode, code.data(), code.size());
    if (vu0) memory.markVU0CodeModified();
    else memory.markVU1CodeModified();

    uint64_t packets = 0, bytes = 0, packetHash = 0;
    memory.setGifPacketCallback([&](const uint8_t *packet, uint32_t size) {
        ++packets;
        bytes += size;
        packetHash = hash(packet, size, packetHash);
    });
    uint64_t expected = 0;
    double elapsed = 0;
    const bool reuploadCode = std::getenv("PS2_VU_REPLAY_CODE_UPLOAD") != nullptr;
    for (unsigned long i = 0; i <= repetitions; ++i) {
        vu.reset();
        vu.state() = initial;
        std::memcpy(vuData, data.data(), data.size());
        packets = bytes = 0;
        packetHash = 14695981039346656037ull;
        if (reuploadCode) {
            std::memcpy(vuCode, code.data(), code.size());
            if (vu0) memory.markVU0CodeModified();
            else memory.markVU1CodeModified();
        }
        const auto start = std::chrono::steady_clock::now();
        vu.resume(vuCode, static_cast<uint32_t>(code.size()), vuData,
                  static_cast<uint32_t>(data.size()), gs, &memory, initial.top, initial.itop);
        const auto finish = std::chrono::steady_clock::now();
        uint64_t result = hash(&vu.state(), sizeof(VU1State));
        result = hash(vuData, data.size(), result);
        result = hash(&packetHash, sizeof(packetHash), result);
        result = hash(&packets, sizeof(packets), result);
        result = hash(&bytes, sizeof(bytes), result);
        if (i == 0) expected = result;
        else if (result != expected) {
            std::fprintf(stderr, "Non-deterministic replay at iteration %lu\n", i);
            return 1;
        } else elapsed += std::chrono::duration<double>(finish - start).count();
    }
    std::printf("VU%u start=%x end=%x cycles=%llu packets=%llu bytes=%llu hash=%016llx us/run=%.3f n=%lu\n",
                vu0 ? 0u : 1u, initial.pc, vu.state().pc,
                static_cast<unsigned long long>(vu.state().cycles),
                static_cast<unsigned long long>(packets), static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(expected), elapsed * 1e6 / repetitions, repetitions);
    std::printf("Execution: native pairs=%llu fallback pairs=%llu\n",
                static_cast<unsigned long long>(vu.compiledPairsExecuted()),
                static_cast<unsigned long long>(vu.interpretedPairsExecuted()));
    if (argc == 4 && !verifyBoundaries(code, data, initial, vu0, profilePath))
        return 1;
}
