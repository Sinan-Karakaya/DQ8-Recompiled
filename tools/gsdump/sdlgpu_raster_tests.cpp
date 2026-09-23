// SDL GPU backend raster tests.
//
// Two kinds of assertion, deliberately:
//
//   * Structural checks with exact expectations -- which pixels a primitive
//     covers, which way up the image is, which of two overlapping sprites
//     wins. These pin down the conventions (clip-space Y, the half-pixel
//     sample offset, GS depth counting up towards the viewer) that are easy to
//     get backwards and that produce a plausible-looking but mirrored or
//     offset image when wrong.
//
//   * Tolerance comparisons against GSCpuBackend for colour. A hardware
//     rasteriser is not expected to be bit-identical to the software one --
//     that was the previous implementation's goal and the reason it could not
//     scale resolution -- so colour is compared within a threshold.

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "gfx/gs/gs_vram.h"
#include "gfx/gs/gs_transfer.h"

#include "runtime/gs/gs_cpu_backend.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <future>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kVramBytes = dq8::gfx::kGsVramBytes;
constexpr int kSkipped = 77;

constexpr uint32_t kFramePage = 32u;
constexpr uint32_t kDepthPage = 320u;
constexpr uint32_t kFrameWidthBlocks = 2u;  // FBW=2 -> 128 pixels
constexpr uint32_t kSurfaceWidth = 128u;
constexpr uint32_t kSurfaceHeight = 64u;

// TEST with ZTE set and ZTST=ALWAYS.
constexpr uint64_t kDepthAlways = (1ull << 16u) | (1ull << 17u);
constexpr uint64_t kDepthGEqual = (1ull << 16u) | (2ull << 17u);

uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8u) |
           (static_cast<uint32_t>(b) << 16u) | (static_cast<uint32_t>(a) << 24u);
}

GSVertex vertex(float x, float y, uint32_t z, uint32_t color) {
    GSVertex result{};
    result.x = x;
    result.y = y;
    result.z = z;
    result.r = static_cast<uint8_t>(color);
    result.g = static_cast<uint8_t>(color >> 8u);
    result.b = static_cast<uint8_t>(color >> 16u);
    result.a = static_cast<uint8_t>(color >> 24u);
    result.q = 1.0f;
    result.fog = 0xffu;
    return result;
}

GSDrawState baseState() {
    GSDrawState state{};
    state.context.frame = {kFramePage, kFrameWidthBlocks, GS_PSM_CT32, 0u};
    state.context.zbuf = {kDepthPage, GS_PSM_Z32, true};
    state.context.scissor = {0u, static_cast<uint16_t>(kSurfaceWidth - 1u),
                             0u, static_cast<uint16_t>(kSurfaceHeight - 1u)};
    state.context.test = kDepthAlways;
    state.colclamp = 1u;
    return state;
}

GSPrimitiveBatch spriteBatch(const GSDrawState &state,
                             float x0, float y0, float x1, float y1,
                             uint32_t color, uint32_t z = 1u) {
    GSPrimitiveBatch batch{};
    batch.vertexCount = 2u;
    batch.vertices[0] = vertex(x0, y0, z, color);
    batch.vertices[1] = vertex(x1, y1, z, color);
    batch.state = state;
    batch.state.prim.type = GS_PRIM_SPRITE;
    return batch;
}

GSPrimitiveBatch triangleBatch(const GSDrawState &state,
                               float x0, float y0, float x1, float y1,
                               float x2, float y2, uint32_t color) {
    GSPrimitiveBatch batch{};
    batch.vertexCount = 3u;
    batch.vertices[0] = vertex(x0, y0, 1u, color);
    batch.vertices[1] = vertex(x1, y1, 1u, color);
    batch.vertices[2] = vertex(x2, y2, 1u, color);
    batch.state = state;
    batch.state.prim.type = GS_PRIM_TRIANGLE;
    return batch;
}

struct Harness {
    explicit Harness(std::unique_ptr<dq8::gfx::SdlGpuBackend> backend)
        : gpu(std::move(backend)), gpuVram(kVramBytes), cpuVram(kVramBytes) {}

    void begin(uint32_t fill = 0u) {
        std::fill(gpuVram.begin(), gpuVram.end(), static_cast<uint8_t>(fill));
        std::fill(cpuVram.begin(), cpuVram.end(), static_cast<uint8_t>(fill));
        gpu->Initialize(gpuVram.data(), kVramBytes);
        cpu.Initialize(cpuVram.data(), kVramBytes);
        gpu->resetStats();
        reader.attach(nullptr, 0u);
    }

    void submit(const GSPrimitiveBatch &batch) {
        gpu->Submit(batch);
        cpu.Submit(batch);
    }

    // Pulls GS local memory back from the GPU. Also the point at which any
    // render target has to have resolved.
    void finish() {
        gpu->SnapshotVram(snapshot);
        reader.attach(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
        cpuReader.attach(cpuVram.data(), kVramBytes);
    }

    uint32_t gpuPixel(uint32_t x, uint32_t y) {
        return reader.read(GS_PSM_CT32, kFramePage << 5u, kFrameWidthBlocks, x, y);
    }
    uint32_t cpuPixel(uint32_t x, uint32_t y) {
        return cpuReader.read(GS_PSM_CT32, kFramePage << 5u, kFrameWidthBlocks, x, y);
    }

    std::unique_ptr<dq8::gfx::SdlGpuBackend> gpu;
    GSCpuBackend cpu;
    std::vector<uint8_t> gpuVram;
    std::vector<uint8_t> cpuVram;
    std::vector<uint8_t> snapshot;
    dq8::gfx::GsVram reader;
    dq8::gfx::GsVram cpuReader;
};

bool reportBackendError(Harness &harness, const char *caseName) {
    const std::string error = harness.gpu->lastError();
    if (!error.empty()) {
        std::fprintf(stderr, "FAIL: %s: backend error: %s\n", caseName, error.c_str());
        return true;
    }
    return false;
}

// Which way is up, and where does a sprite's edge land? A sprite from (8,4) to
// (24,12) must cover exactly that half-open rectangle. Getting the clip-space
// Y direction wrong mirrors the image; getting the sample offset wrong shifts
// it by a pixel. Both look "nearly right" in a screenshot and are caught here.
bool orientationCase(Harness &harness) {
    harness.begin();
    const GSDrawState state = baseState();
    const uint32_t color = rgba(0xffu, 0x40u, 0x20u, 0x80u);
    harness.submit(spriteBatch(state, 8.0f, 4.0f, 24.0f, 12.0f, color));
    harness.finish();
    if (reportBackendError(harness, "sprite orientation"))
        return false;

    uint32_t covered = 0u;
    uint32_t stray = 0u;
    uint32_t missing = 0u;
    for (uint32_t y = 0u; y < kSurfaceHeight; ++y) {
        for (uint32_t x = 0u; x < kSurfaceWidth; ++x) {
            const bool inside = x >= 8u && x < 24u && y >= 4u && y < 12u;
            const bool drawn = harness.gpuPixel(x, y) != 0u;
            if (inside && drawn)
                ++covered;
            else if (inside && !drawn)
                ++missing;
            else if (drawn)
                ++stray;
        }
    }
    if (missing != 0u || stray != 0u) {
        std::fprintf(stderr,
                     "FAIL: sprite orientation: covered=%u missing=%u stray=%u "
                     "(expected 128/0/0). A mirrored image means the clip-space "
                     "Y direction is wrong; an offset one means the sample "
                     "offset is.\n",
                     covered, missing, stray);
        return false;
    }
    return true;
}

// The sprite's colour must survive the round trip through a scaled render
// target, a resolve and the PSMCT32 swizzle unchanged.
bool colorRoundTripCase(Harness &harness) {
    harness.begin();
    const GSDrawState state = baseState();
    const uint32_t color = rgba(0x12u, 0x34u, 0x56u, 0x78u);
    harness.submit(spriteBatch(state, 2.0f, 2.0f, 10.0f, 10.0f, color));
    harness.finish();
    if (reportBackendError(harness, "colour round trip"))
        return false;

    const uint32_t actual = harness.gpuPixel(5u, 5u);
    if (actual != color) {
        std::fprintf(stderr,
                     "FAIL: colour round trip: pixel is %08x, expected %08x\n",
                     actual, color);
        return false;
    }
    return true;
}

// GS depth counts up towards the viewer, so the larger Z must win a GEQUAL
// test regardless of submission order. Inverting the compare would let the
// second sprite win in one order and lose in the other.
bool depthOrderCase(Harness &harness) {
    for (int nearFirst = 0; nearFirst < 2; ++nearFirst) {
        harness.begin();
        GSDrawState state = baseState();
        state.context.test = kDepthGEqual;
        state.context.zbuf.zmask = false;

        const uint32_t nearColor = rgba(0x00u, 0xffu, 0x00u, 0x80u);
        const uint32_t farColor = rgba(0xffu, 0x00u, 0x00u, 0x80u);
        const GSPrimitiveBatch nearSprite =
            spriteBatch(state, 4.0f, 4.0f, 20.0f, 20.0f, nearColor, 5000u);
        const GSPrimitiveBatch farSprite =
            spriteBatch(state, 4.0f, 4.0f, 20.0f, 20.0f, farColor, 100u);

        if (nearFirst != 0) {
            harness.submit(nearSprite);
            harness.submit(farSprite);
        } else {
            harness.submit(farSprite);
            harness.submit(nearSprite);
        }
        harness.finish();
        if (reportBackendError(harness, "depth ordering"))
            return false;

        const uint32_t actual = harness.gpuPixel(10u, 10u);
        if (actual != nearColor) {
            std::fprintf(stderr,
                         "FAIL: depth ordering (near %s): pixel is %08x, "
                         "expected the nearer sprite %08x\n",
                         nearFirst != 0 ? "first" : "second", actual, nearColor);
            return false;
        }
    }
    return true;
}

// Two sprites at equal depth: the later one wins, so batching must not reorder
// primitives.
bool submissionOrderCase(Harness &harness) {
    harness.begin();
    const GSDrawState state = baseState();
    const uint32_t first = rgba(0xffu, 0x00u, 0x00u, 0x80u);
    const uint32_t second = rgba(0x00u, 0x00u, 0xffu, 0x80u);
    harness.submit(spriteBatch(state, 4.0f, 4.0f, 20.0f, 20.0f, first));
    harness.submit(spriteBatch(state, 8.0f, 8.0f, 24.0f, 24.0f, second));
    harness.finish();
    if (reportBackendError(harness, "submission order"))
        return false;

    if (harness.gpuPixel(10u, 10u) != second) {
        std::fprintf(stderr, "FAIL: submission order: overlap shows %08x, expected %08x\n",
                     harness.gpuPixel(10u, 10u), second);
        return false;
    }
    if (harness.gpuPixel(5u, 5u) != first) {
        std::fprintf(stderr, "FAIL: submission order: first sprite was overwritten outside the overlap\n");
        return false;
    }
    return true;
}

bool scissorCase(Harness &harness) {
    harness.begin();
    GSDrawState state = baseState();
    state.context.scissor = {10u, 20u, 6u, 14u};
    harness.submit(spriteBatch(state, 0.0f, 0.0f, 40.0f, 40.0f,
                               rgba(0x40u, 0xa0u, 0xf0u, 0x80u)));
    harness.finish();
    if (reportBackendError(harness, "scissor"))
        return false;

    uint32_t stray = 0u;
    uint32_t missing = 0u;
    for (uint32_t y = 0u; y < kSurfaceHeight; ++y) {
        for (uint32_t x = 0u; x < kSurfaceWidth; ++x) {
            // SCISSOR bounds are inclusive on both edges.
            const bool inside = x >= 10u && x <= 20u && y >= 6u && y <= 14u;
            const bool drawn = harness.gpuPixel(x, y) != 0u;
            if (inside && !drawn)
                ++missing;
            else if (!inside && drawn)
                ++stray;
        }
    }
    if (missing != 0u || stray != 0u) {
        std::fprintf(stderr, "FAIL: scissor: missing=%u stray=%u (expected 0/0)\n",
                     missing, stray);
        return false;
    }
    return true;
}

// XYOFFSET is subtracted from every vertex. The software backend truncates it
// to whole pixels; this one keeps the 1/16 fraction, so the offsets here are
// whole pixels to keep the two comparable.
bool xyOffsetCase(Harness &harness) {
    harness.begin();
    GSDrawState state = baseState();
    state.context.xyoffset = {static_cast<uint16_t>(16u << 4u),
                              static_cast<uint16_t>(8u << 4u)};
    harness.submit(spriteBatch(state, 16.0f + 4.0f, 8.0f + 6.0f,
                               16.0f + 12.0f, 8.0f + 14.0f,
                               rgba(0x20u, 0xd0u, 0x60u, 0x80u)));
    harness.finish();
    if (reportBackendError(harness, "XYOFFSET"))
        return false;

    if (harness.gpuPixel(6u, 8u) == 0u) {
        std::fprintf(stderr, "FAIL: XYOFFSET: expected coverage at (6,8) after the offset\n");
        return false;
    }
    if (harness.gpuPixel(20u, 14u) != 0u) {
        std::fprintf(stderr, "FAIL: XYOFFSET: drew at the unoffset position\n");
        return false;
    }
    return true;
}

// Colour against the software rasteriser, within a threshold. Geometry is
// compared as coverage; the interior colour is compared per channel.
bool softwareComparisonCase(Harness &harness) {
    harness.begin();
    GSDrawState state = baseState();
    state.prim.iip = true;

    harness.submit(triangleBatch(state, 10.0f, 6.0f, 50.0f, 10.0f, 24.0f, 40.0f,
                                 rgba(0xc0u, 0x30u, 0x40u, 0x80u)));
    harness.submit(spriteBatch(state, 60.0f, 20.0f, 90.0f, 45.0f,
                               rgba(0x20u, 0x80u, 0xd0u, 0x80u)));
    harness.finish();
    if (reportBackendError(harness, "software comparison"))
        return false;

    uint64_t compared = 0u;
    uint64_t coverageMismatch = 0u;
    uint32_t worstChannel = 0u;
    for (uint32_t y = 0u; y < kSurfaceHeight; ++y) {
        for (uint32_t x = 0u; x < kSurfaceWidth; ++x) {
            const uint32_t gpuValue = harness.gpuPixel(x, y);
            const uint32_t cpuValue = harness.cpuPixel(x, y);
            const bool gpuDrawn = gpuValue != 0u;
            const bool cpuDrawn = cpuValue != 0u;
            if (gpuDrawn != cpuDrawn) {
                ++coverageMismatch;
                continue;
            }
            if (!gpuDrawn)
                continue;
            ++compared;
            for (int shift = 0; shift < 32; shift += 8) {
                const int a = static_cast<int>((gpuValue >> shift) & 0xffu);
                const int b = static_cast<int>((cpuValue >> shift) & 0xffu);
                worstChannel = std::max(worstChannel,
                                        static_cast<uint32_t>(std::abs(a - b)));
            }
        }
    }

    // Edge pixels are where the two rasterisers legitimately disagree; a whole
    // triangle in the wrong place would be far more than this.
    const uint64_t total = static_cast<uint64_t>(kSurfaceWidth) * kSurfaceHeight;
    if (coverageMismatch * 100u > total) {
        std::fprintf(stderr,
                     "FAIL: software comparison: %llu of %llu pixels differ in "
                     "coverage (>1%%)\n",
                     static_cast<unsigned long long>(coverageMismatch),
                     static_cast<unsigned long long>(total));
        return false;
    }
    if (compared == 0u) {
        std::fprintf(stderr, "FAIL: software comparison: nothing was drawn\n");
        return false;
    }
    if (worstChannel > 2u) {
        std::fprintf(stderr,
                     "FAIL: software comparison: worst channel difference %u "
                     "(tolerance 2)\n",
                     worstChannel);
        return false;
    }
    return true;
}

// The point of the whole design: the same scene at 2x must resolve back to
// local memory as the same native-resolution image.
bool resolutionScaleCase(Harness &harness) {
    auto drawScene = [&](uint32_t scale, std::vector<uint32_t> &out) -> bool {
        harness.gpu->setResolutionScale(scale);
        harness.begin();
        if (harness.gpu->resolutionScale() != scale) {
            std::fprintf(stderr, "FAIL: resolution scale: asked for %ux, backend reports %ux\n",
                         scale, harness.gpu->resolutionScale());
            return false;
        }
        GSDrawState state = baseState();
        harness.submit(spriteBatch(state, 8.0f, 8.0f, 40.0f, 32.0f,
                                   rgba(0xa0u, 0x50u, 0x20u, 0x80u)));
        harness.submit(triangleBatch(state, 50.0f, 10.0f, 100.0f, 16.0f,
                                     70.0f, 50.0f, rgba(0x10u, 0x90u, 0xe0u, 0x80u)));
        harness.finish();
        out.resize(static_cast<size_t>(kSurfaceWidth) * kSurfaceHeight);
        for (uint32_t y = 0u; y < kSurfaceHeight; ++y)
            for (uint32_t x = 0u; x < kSurfaceWidth; ++x)
                out[static_cast<size_t>(y) * kSurfaceWidth + x] = harness.gpuPixel(x, y);
        return true;
    };

    std::vector<uint32_t> native;
    std::vector<uint32_t> scaled;
    if (!drawScene(1u, native) || reportBackendError(harness, "resolution scale"))
        return false;
    if (!drawScene(2u, scaled) || reportBackendError(harness, "resolution scale"))
        return false;
    harness.gpu->setResolutionScale(1u);

    if (harness.gpu->resolutionScale() != 1u) {
        std::fprintf(stderr, "FAIL: resolution scale did not return to 1\n");
        return false;
    }

    // Point-sampling a 2x target back down lands on the same texels except
    // along primitive edges, where the extra samples change which side of the
    // edge the chosen sample falls on.
    uint64_t drawn = 0u;
    uint64_t differing = 0u;
    for (size_t i = 0u; i < native.size(); ++i) {
        if (native[i] == 0u && scaled[i] == 0u)
            continue;
        ++drawn;
        if (native[i] != scaled[i])
            ++differing;
    }
    if (drawn == 0u) {
        std::fprintf(stderr, "FAIL: resolution scale: nothing was drawn at either scale\n");
        return false;
    }
    if (differing * 20u > drawn) {
        std::fprintf(stderr,
                     "FAIL: resolution scale: %llu of %llu drawn pixels differ "
                     "between 1x and 2x (>5%%)\n",
                     static_cast<unsigned long long>(differing),
                     static_cast<unsigned long long>(drawn));
        return false;
    }
    return true;
}

// GS ALPHA against the software rasteriser, over a known destination.
//
// out = (A - B) * C / 128 + D. Each case is a real encoding: the standard
// source-over, additive, fixed-alpha and destination-weighted forms, at
// several alpha values including the 0x80 that means 1.0 on this hardware.
bool blendMatrixCase(Harness &harness) {
    struct Case {
        const char *name;
        uint64_t alpha;
    };
    const Case cases[] = {
        {"(Cs-Cd)*As+Cd", 0x44ull},
        {"Cs*As+Cd", 0x48ull},
        {"Cs*As", 0x88ull},
        {"(Cs-Cd)*FIX+Cd", 0x64ull | (0x60ull << 32u)},
        {"(Cd-Cs)*Ad+Cs", 0x11ull},
        {"Cd*As", 0x89ull},
    };
    const uint8_t alphaValues[] = {0x00u, 0x20u, 0x40u, 0x80u, 0xffu};
    const uint32_t destination = rgba(0xb0u, 0x70u, 0x30u, 0x40u);
    const uint32_t source = rgba(0x20u, 0x40u, 0xa0u, 0x00u);

    for (const Case &entry : cases) {
        for (uint8_t alphaValue : alphaValues) {
            harness.begin();
            dq8::gfx::GsVram writer;
            writer.attach(harness.gpuVram.data(), kVramBytes);
            dq8::gfx::GsVram cpuWriter;
            cpuWriter.attach(harness.cpuVram.data(), kVramBytes);
            for (uint32_t y = 0u; y < 8u; ++y) {
                for (uint32_t x = 0u; x < 8u; ++x) {
                    writer.write(GS_PSM_CT32, kFramePage << 5u, kFrameWidthBlocks, x, y,
                                 destination);
                    cpuWriter.write(GS_PSM_CT32, kFramePage << 5u, kFrameWidthBlocks, x, y,
                                    destination);
                }
            }

            GSDrawState state = baseState();
            state.prim.abe = true;
            state.context.alpha = entry.alpha;
            const uint32_t sourceColor =
                (source & 0x00ffffffu) | (static_cast<uint32_t>(alphaValue) << 24u);
            harness.submit(spriteBatch(state, 0.0f, 0.0f, 8.0f, 8.0f, sourceColor));
            harness.finish();
            if (reportBackendError(harness, "blend matrix"))
                return false;

            const uint32_t got = harness.gpuPixel(4u, 4u);
            const uint32_t want = harness.cpuPixel(4u, 4u);
            int worst = 0;
            for (int shift = 0; shift < 24; shift += 8) {
                const int a = static_cast<int>((got >> shift) & 0xffu);
                const int b = static_cast<int>((want >> shift) & 0xffu);
                worst = std::max(worst, std::abs(a - b));
            }
            // Fixed-function backends count unsupported factors; framebuffer
            // fetch can evaluate the /128 equation exactly, including As > 128.
            const uint32_t factorSelect = static_cast<uint32_t>((entry.alpha >> 4u) & 3ull);
            const bool usesDestinationAlpha = factorSelect == 1u;
            const bool factorExceedsOne = factorSelect == 0u && alphaValue > 0x80u;
            const dq8::gfx::SdlGpuStats stats = harness.gpu->stats();
            if ((usesDestinationAlpha || factorExceedsOne) && worst > 2) {
                const uint64_t counted = usesDestinationAlpha
                                             ? stats.destinationAlphaFactors
                                             : stats.saturatedBlendFactors;
                if (counted == 0u) {
                    std::fprintf(stderr,
                                 "FAIL: blend %s at alpha=%02x deviates without "
                                 "being counted\n",
                                 entry.name, alphaValue);
                    return false;
                }
                continue;
            }
            if (worst > 2) {
                std::fprintf(stderr,
                             "FAIL: blend %s at As=%02x: got %08x want %08x "
                             "(worst channel %d)\n",
                             entry.name, alphaValue, got, want, worst);
                return false;
            }
        }
    }
    return true;
}

// Texturing, from local memory through the CLUT to the fragment shader.
//
// DECAL first because it passes the texel straight through: if this fails the
// fault is in expansion, binding or sampling, not in the TFX arithmetic.
bool texturedSpriteCase(Harness &harness) {
    constexpr uint32_t kTexturePage = 200u;
    constexpr uint32_t kTextureBlock = kTexturePage << 5u;

    auto texel = [](uint32_t x, uint32_t y) {
        return rgba(static_cast<uint8_t>(16u + x * 24u),
                    static_cast<uint8_t>(32u + y * 20u),
                    static_cast<uint8_t>(200u - x * 8u), 0x80u);
    };

    struct Variant {
        const char *name;
        uint8_t tfx;
        uint8_t psm;
        uint8_t sizeShift;
    };
    const Variant variants[] = {
        {"CT32 DECAL", 1u, GS_PSM_CT32, 3u},
        {"CT32 MODULATE", 0u, GS_PSM_CT32, 3u},
        {"Z32 DECAL", 1u, GS_PSM_Z32, 6u},
        {"Z32 MODULATE", 0u, GS_PSM_Z32, 6u},
    };

    for (const Variant &variant : variants) {
        const uint32_t kTextureSize = 1u << variant.sizeShift;
        harness.begin();
        // Written through a GsVram view of the same buffer the backend was
        // initialised with, so the texture is in local memory before the draw.
        dq8::gfx::GsVram writer;
        writer.attach(harness.gpuVram.data(), kVramBytes);
        dq8::gfx::GsVram cpuWriter;
        cpuWriter.attach(harness.cpuVram.data(), kVramBytes);
        for (uint32_t y = 0u; y < kTextureSize; ++y) {
            for (uint32_t x = 0u; x < kTextureSize; ++x) {
                writer.write(variant.psm, kTextureBlock, 1u, x, y, texel(x, y));
                cpuWriter.write(variant.psm, kTextureBlock, 1u, x, y, texel(x, y));
            }
        }

        GSDrawState state = baseState();
        state.prim.tme = true;
        state.prim.fst = true;
        state.context.tex0.tbp0 = kTextureBlock;
        state.context.tex0.tbw = 1u;
        state.context.tex0.psm = variant.psm;
        state.context.tex0.tw = variant.sizeShift;
        state.context.tex0.th = variant.sizeShift;
        state.context.tex0.tfx = variant.tfx;
        state.context.tex0.tcc = 1u;
        // The frontend derives these from TW/TH; this test builds the state by
        // hand, so it has to supply them too.
        state.textureWidth = kTextureSize;
        state.textureHeight = kTextureSize;

        // One screen pixel per texel, crossing a page row in the Z32 cases.
        // The neutral vertex colour for MODULATE is 0x80, not 0xff: the GS
        // computes Ct * Cv >> 7, so 0x80 is unity and 0xff doubles.
        const uint32_t neutral = rgba(0x80u, 0x80u, 0x80u, 0x80u);
        GSPrimitiveBatch batch{};
        batch.vertexCount = 2u;
        batch.vertices[0] = vertex(0.0f, 0.0f, 1u, neutral);
        batch.vertices[1] = vertex(static_cast<float>(kTextureSize),
                                   static_cast<float>(kTextureSize), 1u, neutral);
        batch.vertices[0].u = 0u;
        batch.vertices[0].v = 0u;
        batch.vertices[1].u = static_cast<uint16_t>(kTextureSize * 16u);
        batch.vertices[1].v = static_cast<uint16_t>(kTextureSize * 16u);
        batch.state = state;
        batch.state.prim.type = GS_PRIM_SPRITE;

        harness.submit(batch);
        harness.finish();
        if (reportBackendError(harness, "textured sprite"))
            return false;

        uint32_t mismatches = 0u;
        uint32_t firstX = 0u, firstY = 0u, firstGot = 0u, firstWant = 0u;
        for (uint32_t y = 0u; y < kTextureSize; ++y) {
            for (uint32_t x = 0u; x < kTextureSize; ++x) {
                const uint32_t got = harness.gpuPixel(x, y) & 0x00ffffffu;
                // MODULATE against a white 0x80 vertex colour is identity: the
                // GS scale is /128, so 0x80 means 1.0.
                const uint32_t want = texel(x, y) & 0x00ffffffu;
                if (got != want) {
                    if (mismatches == 0u) {
                        firstX = x;
                        firstY = y;
                        firstGot = got;
                        firstWant = want;
                    }
                    ++mismatches;
                }
            }
        }
        if (mismatches != 0u) {
            std::fprintf(stderr,
                         "FAIL: textured sprite (%s): %u of %u texels wrong; "
                         "first at (%u,%u) got %06x want %06x\n",
                         variant.name, mismatches, kTextureSize * kTextureSize,
                         firstX, firstY, firstGot, firstWant);
            return false;
        }
    }
    return true;
}

// A framebuffer the game filled with a transfer must become drawable: the
// render target adopts local memory rather than starting blank.
bool transferThenDrawCase(Harness &harness) {
    harness.begin();
    const uint32_t background = rgba(0x11u, 0x22u, 0x33u, 0x44u);

    GSTransferCommand command{};
    command.direction = 0u;
    command.bitbltbuf.dbp = kFramePage << 5u;
    command.bitbltbuf.dbw = static_cast<uint8_t>(kFrameWidthBlocks);
    command.bitbltbuf.dpsm = GS_PSM_CT32;
    command.trxreg.rrw = static_cast<uint16_t>(kSurfaceWidth);
    command.trxreg.rrh = 16u;
    harness.gpu->BeginTransfer(command);

    std::vector<uint8_t> payload(static_cast<size_t>(kSurfaceWidth) * 16u * 4u);
    for (size_t i = 0u; i < payload.size(); i += 4u)
        std::memcpy(payload.data() + i, &background, 4u);
    harness.gpu->UploadImage(payload.data(), static_cast<uint32_t>(payload.size()));

    // Draw over part of it; the rest must still read back as the transfer left it.
    const GSDrawState state = baseState();
    const uint32_t drawn = rgba(0xf0u, 0x0fu, 0xa0u, 0x80u);
    harness.gpu->Submit(spriteBatch(state, 4.0f, 4.0f, 12.0f, 12.0f, drawn));
    harness.finish();
    if (reportBackendError(harness, "transfer then draw"))
        return false;

    if (harness.gpuPixel(8u, 8u) != drawn) {
        std::fprintf(stderr, "FAIL: transfer then draw: drawn pixel is %08x, expected %08x\n",
                     harness.gpuPixel(8u, 8u), drawn);
        return false;
    }
    if (harness.gpuPixel(40u, 8u) != background) {
        std::fprintf(stderr,
                     "FAIL: transfer then draw: transferred pixel is %08x, "
                     "expected %08x -- the render target discarded local memory\n",
                     harness.gpuPixel(40u, 8u), background);
        return false;
    }
    return true;
}

bool dualCircuitGpuCompositionCase(Harness &harness) {
    constexpr const char *disableVariable = "DQ8_GFX_DISABLE_GPU_COMPOSE";
    const char *savedValue = std::getenv(disableVariable);
    const std::string saved = savedValue ? savedValue : "";
    struct RestoreEnvironment {
        const char *name;
        bool present;
        std::string value;
        ~RestoreEnvironment() {
            if (present) SDL_setenv_unsafe(name, value.c_str(), 1);
            else SDL_unsetenv_unsafe(name);
        }
    } restore{disableVariable, savedValue != nullptr, saved};
    const auto display = [](uint32_t width, uint32_t height) {
        return (uint64_t(width - 1u) << 32u) | (uint64_t(height - 1u) << 44u);
    };
    const auto buffer = [](uint32_t page, uint32_t psm, uint32_t x, uint32_t y) {
        return uint64_t(page) | (uint64_t(kFrameWidthBlocks) << 9u) |
               (uint64_t(psm) << 15u) | (uint64_t(x) << 32u) | (uint64_t(y) << 43u);
    };
    const uint32_t formats[] = {GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S};
    const uint32_t alphas[] = {0u, 1u, 63u, 64u, 127u, 128u, 254u, 255u};
    for (uint32_t scale : {1u, 2u}) {
        harness.gpu->setResolutionScale(scale);
        for (uint32_t psm : formats) {
            harness.begin();
            for (uint32_t source = 0; source < 2u; ++source) {
                auto state = baseState();
                state.context.frame.fbp += source * 64u;
                state.context.frame.psm = source ? GS_PSM_CT32 : psm;
                for (uint32_t row = 0; row < 8u; ++row) {
                    for (uint32_t column = 0; column < 4u; ++column) {
                        const uint32_t color = rgba((row * 31u + source * 103u) & 255u,
                                                    (column * 67u + source * 41u) & 255u,
                                                    (255u - row * 17u - column * 11u) & 255u,
                                                    alphas[row]);
                        harness.gpu->Submit(spriteBatch(state, column * 32.0f, row * 8.0f,
                                                        (column + 1u) * 32.0f,
                                                        (row + 1u) * 8.0f, color));
                    }
                }
                // Distinct scaled subrows must survive FFMD row doubling.
                harness.gpu->Submit(spriteBatch(state, 0.0f, 5.5f, 128.0f, 6.0f,
                                                rgba(19u, 227u, 53u, 63u)));
            }
            for (uint32_t variant = 0; variant < 16u; ++variant) {
                GSPresentationRequest request{};
                const bool fixedAlpha = variant < 8u;
                request.pmode = 3u | (uint64_t(fixedAlpha) << 5u) |
                                (uint64_t((variant >> 1u) & 1u) << 6u) |
                                (uint64_t((variant >> 2u) & 1u) << 7u) |
                                (uint64_t(alphas[variant & 7u]) << 8u);
                request.smode2 = (variant & 1u) ? 3u : 0u;
                request.bgcolor = rgba(217u, 7u, 93u, 0u);
                request.dispfb1 = buffer(kFramePage, psm, 3u, 1u);
                request.dispfb2 = variant == 15u ? buffer(kFramePage, psm, 3u, 2u)
                                                : buffer(kFramePage + 64u, GS_PSM_CT32, 7u, 3u);
                request.display1 = display(96u, 64u);
                request.display2 = display(128u, 80u);
                SDL_setenv_unsafe(disableVariable, "0", 1);
                const uint64_t before = harness.gpu->stats().gpuComposedPresents;
                const PresentationFrame gpu = harness.gpu->Present(request);
                if (reportBackendError(harness, "GPU display composition") ||
                    !gpu.HasHostPixels() || harness.gpu->stats().gpuComposedPresents != before + 1u) {
                    std::fprintf(stderr, "FAIL: GPU display path was not used: scale=%u psm=%u variant=%u\n",
                                 scale, psm, variant);
                    return false;
                }
                if (variant == 0u) {
                    uint32_t pixel = 0u;
                    std::memcpy(&pixel, gpu.pixels.data() +
                                (size_t(10u * scale) * gpu.width + 10u * scale) * 4u, 4u);
                    if (pixel != rgba(134u, 41u, 238u, 255u)) {
                        std::fprintf(stderr, "FAIL: zero display alpha must retain circuit 2: %08x\n", pixel);
                        return false;
                    }
                }
                SDL_setenv_unsafe(disableVariable, "1", 1);
                const PresentationFrame cpu = harness.gpu->Present(request);
                if (reportBackendError(harness, "CPU display composition") ||
                    !cpu.HasHostPixels() || gpu.width != cpu.width || gpu.height != cpu.height ||
                    gpu.rowPitchBytes != cpu.rowPitchBytes || gpu.pixels != cpu.pixels) {
                    const auto mismatch = std::mismatch(gpu.pixels.begin(), gpu.pixels.end(),
                                                        cpu.pixels.begin(), cpu.pixels.end());
                    std::fprintf(stderr, "FAIL: GPU/CPU display mismatch: scale=%u psm=%u variant=%u byte=%zu\n",
                                 scale, psm, variant, size_t(mismatch.first - gpu.pixels.begin()));
                    return false;
                }
            }
        }
    }
    harness.gpu->setResolutionScale(1u);
    return true;
}

bool preparedPresentationSlotsCase(Harness &harness) {
    harness.begin();
    GSPresentationRequest request{};
    request.pmode = 3u | (1u << 5u) | (255u << 8u);
    request.dispfb1 = request.dispfb2 = kFramePage | (uint64_t(kFrameWidthBlocks) << 9u);
    std::array<GSPresentationTicket, 3> tickets;
    const uint32_t colors[] = {rgba(21, 53, 187, 128), rgba(173, 37, 91, 128), rgba(77, 201, 17, 128)};
    const uint32_t widths[] = {128, 64, 128}, heights[] = {64, 64, 128};
    auto state = baseState();
    state.context.scissor.y1 = 127;
    for (size_t index = 0; index < tickets.size(); ++index) {
        request.display1 = request.display2 = (uint64_t(widths[index] - 1) << 32u) |
                                             (uint64_t(heights[index] - 1) << 44u);
        harness.gpu->Submit(spriteBatch(state, 0, 0, 128, 128, colors[index]));
        tickets[index] = harness.gpu->PreparePresentation(request);
    }
    if (harness.gpu->stats().gpuComposedPresents != 3u) {
        std::fprintf(stderr, "FAIL: presentation slots did not exercise GPU textures\n");
        return false;
    }
    const auto matches = [&](const GSPresentationTicket &ticket, uint32_t color, uint32_t width, uint32_t height) {
        const auto frame = harness.gpu->DisplayPreparedPresentation(ticket);
        if (frame.width != width || frame.height != height || frame.pixels.size() != width * height * 4) {
            std::fprintf(stderr, "FAIL: prepared size %ux%u, %zu bytes\n", frame.width, frame.height, frame.pixels.size());
            return false;
        }
        for (size_t pixel = 0; pixel < frame.pixels.size(); pixel += 4) {
            uint32_t actual = 0;
            std::memcpy(&actual, frame.pixels.data() + pixel, 4);
            if (actual != (color | 0xff000000u)) {
                std::fprintf(stderr, "FAIL: prepared pixel %zu = %08x, expected %08x\n", pixel / 4,
                             actual, color | 0xff000000u);
                return false;
            }
        }
        return true;
    };
    harness.gpu->Reset();
    request.display1 = request.display2 = (63ull << 32u) | (127ull << 44u);
    harness.gpu->Submit(spriteBatch(state, 0, 0, 128, 128, colors[0]));
    std::promise<void> started;
    auto fourth = std::async(std::launch::async, [&] {
        started.set_value();
        return harness.gpu->PreparePresentation(request);
    });
    started.get_future().wait();
    const bool bounded = fourth.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout;
    const bool firstCorrect = matches(tickets[0], colors[0], widths[0], heights[0]);
    tickets[0].reset();
    auto reused = fourth.get();
    if (!bounded || !firstCorrect || !matches(tickets[1], colors[1], widths[1], heights[1]) ||
        !matches(tickets[2], colors[2], widths[2], heights[2]) || !matches(reused, colors[0], 64, 128) ||
        !matches(tickets[1], colors[1], widths[1], heights[1])) {
        std::fprintf(stderr, "FAIL: retained GPU frame changed, or presentation slots were unbounded\n");
        return false;
    }
    return true;
}

bool preparedPresentationCancellationCase() {
    std::string error;
    auto backend = dq8::gfx::createSdlGpuBackend(error);
    if (!backend) return false;
    std::vector<uint8_t> vram(kVramBytes);
    backend->Initialize(vram.data(), kVramBytes);
    std::array<GSPresentationTicket, 3> held;
    for (auto &ticket : held) ticket = backend->PreparePresentation({});
    auto blocked = std::async(std::launch::async, [&] {
        try { backend->PreparePresentation({}); }
        catch (const std::runtime_error &) { return true; }
        return false;
    });
    const bool bounded = blocked.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout;
    backend->CancelPreparedPresentations();
    const bool cancelled = blocked.get();
    backend.reset();
    held = {};
    if (!bounded || !cancelled) {
        std::fprintf(stderr, "FAIL: shutdown did not wake blocked GPU frame preparation\n");
        return false;
    }
    return true;
}

bool drawThenTransferPresentCase(Harness &harness) {
    harness.begin();
    harness.gpu->Submit(spriteBatch(baseState(), 0.0f, 0.0f,
                                    kSurfaceWidth, kSurfaceHeight, rgba(0u, 0u, 0u, 0x80u)));
    harness.gpu->Flush();

    const uint32_t movieColor = rgba(0x20u, 0x90u, 0xe0u, 0x80u);
    GSTransferCommand command{};
    command.direction = 0u;
    command.bitbltbuf.dbp = kFramePage << 5u;
    command.bitbltbuf.dbw = kFrameWidthBlocks;
    command.bitbltbuf.dpsm = GS_PSM_CT32;
    command.trxreg.rrw = kSurfaceWidth;
    command.trxreg.rrh = kSurfaceHeight;
    harness.gpu->BeginTransfer(command);
    std::vector<uint32_t> pixels(kSurfaceWidth * kSurfaceHeight, movieColor);
    harness.gpu->UploadImage(reinterpret_cast<const uint8_t *>(pixels.data()),
                             static_cast<uint32_t>(pixels.size() * sizeof(uint32_t)));

    GSPresentationRequest request{};
    request.pmode = 3u | (1ull << 5u) | (0x80ull << 8u);
    request.dispfb1 = request.dispfb2 = kFramePage | (static_cast<uint64_t>(kFrameWidthBlocks) << 9u);
    request.display1 = request.display2 = (static_cast<uint64_t>(kSurfaceWidth - 1u) << 32u) |
                                          (static_cast<uint64_t>(kSurfaceHeight - 1u) << 44u);
    const PresentationFrame frame = harness.gpu->Present(request);
    if (reportBackendError(harness, "draw then transfer present") || !frame.HasHostPixels())
        return false;
    for (size_t i = 0; i < frame.pixels.size(); i += 4u) {
        uint32_t got = 0u;
        std::memcpy(&got, frame.pixels.data() + i, sizeof(got));
        if ((got & 0xffffffu) != (movieColor & 0xffffffu)) {
            std::fprintf(stderr, "FAIL: draw then transfer present: got %08x expected %08x at pixel %zu\n",
                         got, movieColor, i / 4u);
            return false;
        }
    }
    // A partial upload can leave a pending patch on an otherwise valid target.
    harness.begin();
    harness.gpu->Submit(spriteBatch(baseState(), 0.0f, 0.0f, kSurfaceWidth, kSurfaceHeight,
                                    rgba(0u, 0u, 0u, 0x80u)));
    harness.gpu->Flush();
    command.trxpos.dsax = 7u;
    command.trxpos.dsay = 9u;
    command.trxreg.rrw = command.trxreg.rrh = 1u;
    harness.gpu->BeginTransfer(command);
    harness.gpu->UploadImage(reinterpret_cast<const uint8_t *>(&movieColor), sizeof(movieColor));
    const PresentationFrame patched = harness.gpu->Present(request);
    uint32_t pixel = 0u;
    if (reportBackendError(harness, "partial upload then present") || !patched.HasHostPixels())
        return false;
    std::memcpy(&pixel, patched.pixels.data() + (9u * patched.width + 7u) * 4u, 4u);
    if (harness.gpu->stats().gpuComposedPresents != 0u ||
        pixel != (movieColor | 0xff000000u)) {
        std::fprintf(stderr, "FAIL: display must consume a pending CPU patch: pixel=%08x GPU compositions=%llu\n",
                     pixel, static_cast<unsigned long long>(harness.gpu->stats().gpuComposedPresents));
        return false;
    }
    return true;
}

bool bilinearTexelCenterCase(Harness &harness) {
    harness.begin();
    constexpr uint32_t textureBase = 200u << 5u;
    const auto texel = [](uint32_t x, uint32_t y) {
        return rgba(x & 1u ? 0xe0u : 0x10u, y & 1u ? 0xd0u : 0x20u, 0x60u, 0x80u);
    };
    dq8::gfx::GsVram writer;
    writer.attach(harness.gpuVram.data(), kVramBytes);
    for (uint32_t y = 0; y < 8u; ++y)
        for (uint32_t x = 0; x < 8u; ++x)
            writer.write(GS_PSM_CT32, textureBase, 1u, x, y, texel(x, y));
    auto state = baseState();
    state.prim.tme = true;
    state.prim.fst = true;
    state.linearFilter = true;
    state.context.tex0.tbp0 = textureBase;
    state.context.tex0.tbw = 1u;
    state.context.tex0.psm = GS_PSM_CT32;
    state.context.tex0.tw = state.context.tex0.th = 3u;
    state.context.tex0.tfx = 1u;
    state.context.tex0.tcc = 1u;
    state.context.clamp = 5u;
    state.textureWidth = state.textureHeight = 8u;
    auto batch = spriteBatch(state, 4, 4, 12, 12, rgba(0x80, 0x80, 0x80, 0x80));
    batch.vertices[0].u = batch.vertices[0].v = 8u;
    batch.vertices[1].u = batch.vertices[1].v = 8u * 16u + 8u;
    harness.gpu->Submit(batch);
    harness.finish();
    for (uint32_t y = 0; y < 8u; ++y)
        for (uint32_t x = 0; x < 8u; ++x)
            if (harness.gpuPixel(x + 4u, y + 4u) != texel(x, y)) {
                std::fprintf(stderr, "FAIL: bilinear texel center (%u,%u): got %08x expected %08x\n",
                    x, y, harness.gpuPixel(x + 4u, y + 4u), texel(x, y));
                return false;
            }
    return !reportBackendError(harness, "bilinear texel centers");
}

bool aliasedColorTargetsCase(Harness &harness) {
    harness.begin();
    GSDrawState state32 = baseState();
    GSDrawState state24 = state32;
    state24.context.frame.psm = GS_PSM_CT24;
    harness.submit(spriteBatch(state32, 0.0f, 0.0f, kSurfaceWidth, kSurfaceHeight,
                               rgba(0xa0u, 0x20u, 0x10u, 0x80u)));
    harness.submit(spriteBatch(state24, 8.0f, 8.0f, 24.0f, 24.0f,
                               rgba(0x10u, 0xb0u, 0x20u, 0u)));
    harness.submit(spriteBatch(state32, 32.0f, 8.0f, 48.0f, 24.0f,
                               rgba(0x20u, 0x10u, 0xc0u, 0x80u)));
    harness.finish();
    if (reportBackendError(harness, "aliased color targets"))
        return false;
    for (uint32_t y = 0u; y < kSurfaceHeight; ++y) {
        for (uint32_t x = 0u; x < kSurfaceWidth; ++x) {
            const uint32_t got = harness.gpuPixel(x, y) & 0xffffffu;
            const uint32_t want = harness.cpuPixel(x, y) & 0xffffffu;
            if (got != want) {
                std::fprintf(stderr, "FAIL: aliased color targets: (%u,%u) got %06x expected %06x\n",
                             x, y, got, want);
                return false;
            }
        }
    }
    return true;
}

bool smallDrawReadbackCase(Harness &harness) {
    harness.begin();
    const GSDrawState state = baseState();
    harness.submit(spriteBatch(state, 8.0f, 8.0f, 16.0f, 16.0f,
                               rgba(0x20u, 0x40u, 0x60u, 0x80u)));
    harness.submit(spriteBatch(state, 24.0f, 8.0f, 32.0f, 16.0f,
                               rgba(0x60u, 0x40u, 0x20u, 0x80u)));
    harness.submit(spriteBatch(state, -32.0f, -32.0f, -16.0f, -16.0f,
                               rgba(0xffu, 0xffu, 0xffu, 0x80u)));
    harness.finish();
    if (reportBackendError(harness, "small draw readback"))
        return false;
    for (uint32_t y = 0u; y < kSurfaceHeight; ++y) {
        for (uint32_t x = 0u; x < kSurfaceWidth; ++x) {
            if (harness.gpuPixel(x, y) != harness.cpuPixel(x, y)) {
                std::fprintf(stderr, "FAIL: small draw readback: pixel (%u,%u)\n", x, y);
                return false;
            }
        }
    }
    const auto stats = harness.gpu->stats();
    if (stats.resolvedPixels > 512u || stats.primitivesDrawn != 2u) {
        std::fprintf(stderr, "FAIL: small draw readback: resolved=%llu drawn=%llu\n",
                     static_cast<unsigned long long>(stats.resolvedPixels),
                     static_cast<unsigned long long>(stats.primitivesDrawn));
        return false;
    }
    return true;
}

bool partialTargetRefreshCase(Harness &harness) {
    for (uint32_t scale : {1u, 2u}) {
        harness.gpu->setResolutionScale(scale);
        harness.begin();
        const auto state = baseState();
        harness.submit(spriteBatch(state, 0, 0, kSurfaceWidth, kSurfaceHeight,
                                   rgba(24, 64, 96, 128)));
        harness.gpu->Flush();
        harness.cpu.Flush();
        const uint32_t replacement = rgba(192, 48, 16, 64);
        harness.gpu->WriteVram(GS_PSM_CT32, kFramePage << 5u, kFrameWidthBlocks,
                               83, 51, replacement);
        harness.cpu.WriteVram(GS_PSM_CT32, kFramePage << 5u, kFrameWidthBlocks,
                              83, 51, replacement);
        // A refresh at a nonzero origin must retain the target's other pages.
        harness.submit(spriteBatch(state, 8, 8, 16, 16, rgba(48, 160, 32, 128)));
        harness.finish();
        if (reportBackendError(harness, "partial target refresh") ||
            harness.gpu->stats().colorRefreshes != (scale == 1u ? 1u : 2u)) {
            std::fprintf(stderr, "FAIL: partial target refresh upload count\n");
            return false;
        }
        for (uint32_t y = 0; y < kSurfaceHeight; ++y) {
            for (uint32_t x = 0; x < kSurfaceWidth; ++x) {
                if (harness.gpuPixel(x, y) != harness.cpuPixel(x, y)) {
                    std::fprintf(stderr, "FAIL: partial target refresh scale=%u at (%u,%u)\n",
                                 scale, x, y);
                    return false;
                }
            }
        }
    }
    harness.gpu->setResolutionScale(1u);
    return true;
}

bool opaqueSpriteRefreshCase(Harness &harness) {
    for (uint32_t mode = 0; mode < 6; ++mode) {
        harness.begin(0x20);
        auto state = baseState();
        if (mode == 1u) {
            state.prim.abe = true;
            state.context.alpha = 0x44u;
        } else if (mode == 2u) {
            state.context.frame.fbmsk = 0xffu;
        } else if (mode == 3u) {
            state.context.test |= 1u; // Alpha NEVER.
        } else if (mode == 4u) {
            state.context.test = 1u << 16u; // Depth NEVER.
        }
        const float x0 = mode == 5u ? 1.0f : 0.0f;
        harness.submit(spriteBatch(state, x0, 0, kSurfaceWidth, kSurfaceHeight,
                                   rgba(96, 64, 48, 128)));
        harness.finish();
        if (reportBackendError(harness, "opaque sprite refresh") ||
            harness.gpu->stats().colorRefreshes != (mode == 0u ? 0u : 1u)) {
            std::fprintf(stderr, "FAIL: opaque sprite refresh mode=%u upload count\n", mode);
            return false;
        }
        for (uint32_t y = 0; y < kSurfaceHeight; ++y) {
            for (uint32_t x = 0; x < kSurfaceWidth; ++x) {
                if (harness.gpuPixel(x, y) != harness.cpuPixel(x, y)) {
                    std::fprintf(stderr, "FAIL: opaque sprite refresh mode=%u at (%u,%u)\n", mode, x, y);
                    return false;
                }
            }
        }
    }
    return true;
}

bool independentTargetResolveCase(Harness &harness) {
    for (uint32_t scale : {1u, 2u}) {
        harness.gpu->setResolutionScale(scale);
        harness.begin();
        for (uint32_t target = 0; target < 3u; ++target) {
            auto state = baseState();
            state.context.frame.fbp += target * 16u;
            state.context.frame.psm = target == 2u ? GS_PSM_CT24 : GS_PSM_CT32;
            harness.submit(spriteBatch(state, 9, 13, 18 + target, 24 + target,
                                       rgba(32 + target * 48, 96, 24, 128)));
        }
        harness.finish();
        if (reportBackendError(harness, "independent target resolve") ||
            harness.gpu->stats().colorResolves != 3u)
            return false;
        // Different sizes exercise transfer offsets; CT24 preserves the VRAM alpha byte.
        for (uint32_t target = 0; target < 3u; ++target) {
            const uint32_t base = (kFramePage + target * 16u) << 5u;
            for (uint32_t y = 0; y < kSurfaceHeight; ++y) {
                for (uint32_t x = 0; x < kSurfaceWidth; ++x) {
                    // Point-downsampling can select either side of a scaled edge.
                    if (scale != 1u &&
                        ((x >= 8u && x <= 10u) || (x >= 17u + target && x <= 19u + target) ||
                         (y >= 12u && y <= 14u) || (y >= 23u + target && y <= 25u + target)))
                        continue;
                    const auto got = harness.reader.read(GS_PSM_CT32, base, kFrameWidthBlocks, x, y);
                    const auto want = harness.cpuReader.read(GS_PSM_CT32, base, kFrameWidthBlocks, x, y);
                    if (got != want) {
                        std::fprintf(stderr, "FAIL: independent target resolve scale=%u target=%u at (%u,%u)\n",
                                     scale, target, x, y);
                        return false;
                    }
                }
            }
        }
    }
    harness.gpu->setResolutionScale(1u);
    return true;
}

bool feedbackSnapshotCase(Harness &harness) {
    for (uint32_t scale : {1u, 2u}) {
        harness.gpu->setResolutionScale(scale);
        harness.begin();
        const auto state = baseState();
        harness.submit(spriteBatch(state, 8.0f, 8.0f, 16.0f, 16.0f,
                                   rgba(0xc0u, 0x40u, 0x20u, 0x40u)));
        auto textureState = state;
        textureState.prim.tme = true;
        textureState.prim.fst = true;
        // Bilinear feedback at 2x retains subpixel edge detail absent in software.
        textureState.linearFilter = scale == 1u;
        textureState.textureWidth = 256u;
        textureState.textureHeight = 128u;
        textureState.context.tex0.tbp0 = kFramePage << 5u;
        textureState.context.tex0.tbw = kFrameWidthBlocks;
        textureState.context.tex0.psm = GS_PSM_CT32;
        textureState.context.tex0.tfx = 1u;
        textureState.context.tex0.tcc = 1u;
        auto copy = [&](uint16_t fromX, uint16_t fromY, float toX) {
            auto batch = spriteBatch(textureState, toX, 8.0f, toX + 8.0f, 16.0f,
                                      rgba(0x80u, 0x80u, 0x80u, 0x80u));
            batch.vertices[0].u = fromX * 16u + 8u;
            batch.vertices[0].v = fromY * 16u + 8u;
            batch.vertices[1].u = (fromX + 8u) * 16u + 8u;
            batch.vertices[1].v = (fromY + 8u) * 16u + 8u;
            harness.submit(batch);
        };
        copy(8u, 8u, 40.0f);
        copy(40u, 8u, 56.0f);
        harness.gpu->Flush();
        const auto stats = harness.gpu->stats();
        if (stats.feedbackCopies != 2u || stats.colorResolves != 0u) {
            std::fprintf(stderr, "FAIL: feedback snapshot: copies=%llu resolves=%llu\n",
                         static_cast<unsigned long long>(stats.feedbackCopies),
                         static_cast<unsigned long long>(stats.colorResolves));
            return false;
        }
        // TEX0 is bigger than the target: taps outside it must use local memory.
        copy(8u, 80u, 72.0f);
        harness.finish();
        if (reportBackendError(harness, "feedback snapshot") ||
            harness.gpu->stats().feedbackCopies != 2u)
            return false;
        for (uint32_t y = 0u; y < kSurfaceHeight; ++y) {
            for (uint32_t x = 0u; x < kSurfaceWidth; ++x) {
                if (harness.gpuPixel(x, y) != harness.cpuPixel(x, y)) {
                    std::fprintf(stderr, "FAIL: feedback snapshot: scale=%u pixel (%u,%u)\n",
                                 scale, x, y);
                    return false;
                }
            }
        }
    }
    harness.gpu->setResolutionScale(1u);
    return true;
}

bool liveTargetTexaCase(Harness &harness) {
    for (uint32_t psm : {uint32_t(GS_PSM_CT24), uint32_t(GS_PSM_CT16), uint32_t(GS_PSM_CT32)}) {
        harness.begin();
        auto source = baseState();
        source.context.frame.fbp += 8u;
        source.context.frame.psm = psm;
        harness.submit(spriteBatch(source, 0.0f, 0.0f, kSurfaceWidth, kSurfaceHeight, 0u));
        harness.submit(spriteBatch(source, 8.0f, 8.0f, 16.0f, 16.0f,
                                   rgba(0xffu, 0x43u, 0x27u, 0u)));
        harness.submit(spriteBatch(source, 24.0f, 8.0f, 32.0f, 16.0f,
                                   rgba(0x27u, 0xffu, 0x43u, 0x80u)));
        auto state = baseState();
        state.prim.tme = state.prim.fst = true;
        state.linearFilter = true;
        state.textureWidth = kSurfaceWidth;
        state.textureHeight = kSurfaceHeight;
        state.context.tex0.tbp0 = source.context.frame.fbp << 5u;
        state.context.tex0.tbw = kFrameWidthBlocks;
        state.context.tex0.psm = psm == GS_PSM_CT32 ? GS_PSM_CT24 : psm;
        state.context.tex0.tfx = state.context.tex0.tcc = 1u;
        state.texa.ta0 = 0x20u;
        state.texa.ta1 = 0xa0u;
        state.texa.aem = true;
        auto batch = spriteBatch(state, 0.0f, 0.0f, kSurfaceWidth, kSurfaceHeight,
                                  rgba(0x80u, 0x80u, 0x80u, 0x80u));
        batch.vertices[0].u = batch.vertices[0].v = 8u;
        batch.vertices[1].u = kSurfaceWidth * 16u + 8u;
        batch.vertices[1].v = kSurfaceHeight * 16u + 8u;
        harness.submit(batch);
        harness.finish();
        if (reportBackendError(harness, "live target TEXA") ||
            harness.gpu->stats().texturesFromLiveTargets != 1u)
            return false;
        for (uint32_t y = 0u; y < kSurfaceHeight; ++y) {
            for (uint32_t x = 0u; x < kSurfaceWidth; ++x) {
                const uint32_t got = harness.gpuPixel(x, y), want = harness.cpuPixel(x, y);
                if (got != want) {
                    std::fprintf(stderr, "FAIL: live target TEXA: psm=%u (%u,%u) got=%08x want=%08x\n",
                                 psm, x, y, got, want);
                    return false;
                }
            }
        }
    }
    return true;
}

bool blendAlphaStorageCase(Harness &harness) {
    for (const uint64_t equation : {0x44ull, 0x8000000068ull, 0x4000000064ull}) {
        harness.begin();
        auto state = baseState();
        harness.submit(spriteBatch(state, 0, 0, 32, 32, rgba(30, 40, 50, 128)));
        state.prim.abe = true;
        state.context.alpha = equation;
        harness.submit(spriteBatch(state, 0, 0, 32, 32, rgba(80, 64, 48, 32)));
        harness.finish();
        if (reportBackendError(harness, "blend alpha storage")) return false;
        const auto got = harness.gpuPixel(8, 8), want = harness.cpuPixel(8, 8);
        if ((got >> 24u) != 32u || (want >> 24u) != 32u) {
            std::fprintf(stderr, "FAIL: GS blending must store source alpha: ALPHA=%llx got=%08x want=%08x\n",
                         static_cast<unsigned long long>(equation), got, want);
            return false;
        }
    }
    return true;
}

bool feedbackEdgeSamplesCase(Harness &harness) {
    harness.begin();
    auto state = baseState();
    const uint32_t background = rgba(32, 64, 96, 128);
    const uint32_t lastRow = rgba(192, 48, 24, 128);
    harness.submit(spriteBatch(state, 0, 0, kSurfaceWidth, kSurfaceHeight, background));
    harness.submit(spriteBatch(state, 0, 63, kSurfaceWidth, 64, lastRow));
    state.prim.tme = state.prim.fst = true;
    state.linearFilter = true;
    state.textureWidth = state.textureHeight = 128u;
    state.context.tex0.tbp0 = kFramePage << 5u;
    state.context.tex0.tbw = kFrameWidthBlocks;
    state.context.tex0.psm = GS_PSM_CT32;
    state.context.tex0.tfx = state.context.tex0.tcc = 1u;
    state.context.clamp = 5u;
    auto copy = spriteBatch(state, 0.625f, -0.5f, 32.625f, 63.5f, 0x80808080u);
    copy.vertices[1].u = 32u * 16u;
    copy.vertices[1].v = 64u * 16u;
    harness.submit(copy);
    harness.gpu->Flush();
    if (harness.gpu->stats().feedbackCopies != 1u || harness.gpu->stats().colorResolves != 0u) {
        std::fprintf(stderr, "FAIL: covered sprite samples should fit the GPU snapshot\n");
        return false;
    }
    harness.finish();
    if (harness.gpuPixel(16, 0) != background || harness.gpuPixel(16, 63) != lastRow) {
        std::fprintf(stderr, "FAIL: feedback must preserve the first and last filter samples\n");
        return false;
    }
    // More than one outside row exceeds the bounded padding path.
    copy.vertices[1].v += 32u;
    harness.submit(copy);
    harness.gpu->Flush();
    if (reportBackendError(harness, "feedback edge samples") ||
        harness.gpu->stats().feedbackCopies != 1u) {
        std::fprintf(stderr, "FAIL: taps beyond the padded row must use local memory\n");
        return false;
    }
    return true;
}

bool programmableBlendCase(Harness &harness) {
#if defined(__aarch64__)
    if (harness.gpu->driverName() != "metal" ||
        std::getenv("DQ8_GFX_DISABLE_FRAMEBUFFER_FETCH")) return true;
    struct Case {
        const char *name;
        uint64_t equation, test, fba;
        uint32_t source, mask, expected;
        bool clamp, pabe;
    };
    const Case cases[] = {
        {"destination alpha", 0x54, kDepthAlways, 0, rgba(160,48,80,192), 0,
         rgba(140,60,100,192), true, false},
        {"super-unity wrap", 0x48, kDepthAlways, 0, rgba(160,48,80,192), 0,
         rgba(64,168,24,192), false, false},
        {"partial FBMSK", 0x54, kDepthAlways, 0, rgba(160,48,80,192), 0x0ff00ff0,
         rgba(92,48,164,192), true, false},
        {"PABE and FBA", 0x44, kDepthAlways, 1, rgba(160,48,80,32), 0,
         rgba(160,48,80,160), true, true},
        {"DATE reject", 0x54, kDepthAlways | (3ull << 14u), 0, rgba(160,48,80,192), 0,
         rgba(80,96,160,96), true, false},
        {"DATE accept", 0x54, kDepthAlways | (1ull << 14u), 0, rgba(160,48,80,192), 0,
         rgba(140,60,100,192), true, false},
    };
    for (const auto &entry : cases) {
        harness.begin();
        auto state = baseState();
        harness.submit(spriteBatch(state, 0, 0, 16, 16, rgba(80,96,160,96)));
        state.prim.abe = true;
        state.context.alpha = entry.equation;
        state.context.test = entry.test;
        state.context.fba = entry.fba;
        state.context.frame.fbmsk = entry.mask;
        state.colclamp = entry.clamp;
        state.pabe = entry.pabe;
        harness.submit(spriteBatch(state, 0, 0, 16, 16, entry.source));
        harness.finish();
        const auto got = harness.gpuPixel(8, 8);
        if (reportBackendError(harness, entry.name) || got != entry.expected) {
            std::fprintf(stderr, "FAIL: %s: got=%08x want=%08x\n",
                         entry.name, got, entry.expected);
            return false;
        }
    }
#endif
    return true;
}

bool feedbackTailRowCase(Harness &harness) {
    for (const uint32_t psm : {uint32_t(GS_PSM_CT32), uint32_t(GS_PSM_CT16)}) {
        for (const bool gpuTail : {false, true}) {
            harness.begin();
            auto state = baseState();
            state.context.frame.psm = psm;
            dq8::gfx::GsVram writer;
            writer.attach(harness.gpuVram.data(), kVramBytes);
            for (uint32_t x = 0; x < kSurfaceWidth; ++x)
                writer.write(psm, kFramePage << 5u, kFrameWidthBlocks, x, kSurfaceHeight,
                             psm == GS_PSM_CT32 ? (gpuTail ? rgba(0,248,0,128) : rgba(0,0,248,128))
                                                : (gpuTail ? 0x83e0u : 0xfc00u));
            harness.submit(spriteBatch(state, 0, 0, kSurfaceWidth, kSurfaceHeight, rgba(248,0,0,128)));
            if (gpuTail) {
                auto neighbor = state;
                neighbor.context.frame.fbp += psm == GS_PSM_CT32 ? 4u : 2u;
                harness.submit(spriteBatch(neighbor, 0, 0, kSurfaceWidth, kSurfaceHeight, rgba(0,0,248,128)));
            }
            state.prim.tme = state.prim.fst = true;
            state.linearFilter = true;
            state.textureWidth = state.textureHeight = 128;
            state.context.tex0.tbp0 = kFramePage << 5u;
            state.context.tex0.tbw = kFrameWidthBlocks;
            state.context.tex0.psm = psm;
            state.context.tex0.tfx = state.context.tex0.tcc = 1;
            state.texa.ta0 = state.texa.ta1 = 128;
            auto copy = spriteBatch(state, 0, 16, 16, 32, 0x80808080);
            copy.vertices[0].u = 8;
            copy.vertices[1].u = 16 * 16 + 8;
            copy.vertices[0].v = copy.vertices[1].v = 64 * 16 + 4;
            harness.submit(copy);
            harness.gpu->Flush();
            if (harness.gpu->stats().feedbackCopies != 1 ||
                harness.gpu->stats().colorResolves != (gpuTail ? 1u : 0u)) {
                std::fprintf(stderr, "FAIL: feedback tail should upload padding without reading back the source\n");
                return false;
            }
            harness.finish();
            const uint32_t got = harness.reader.read(psm, kFramePage << 5u, kFrameWidthBlocks, 8, 24);
            const uint32_t want = psm == GS_PSM_CT32 ? rgba(62,0,186,128) : 0xdc07u;
            if (reportBackendError(harness, "feedback tail row") || got != want) {
                std::fprintf(stderr, "FAIL: feedback tail psm=%u got=%08x want=%08x\n", psm, got, want);
                return false;
            }
        }
    }
    return true;
}

bool feedbackRightColumnCase(Harness &harness) {
    harness.begin();
    dq8::gfx::GsVram writer;
    writer.attach(harness.gpuVram.data(), kVramBytes);
    // Check the page-row mapping through the scalar GS swizzle, including
    // unaligned bases and the VRAM wrap, rather than assuming linear storage.
    for (uint32_t base : {1024u, 1025u, 16352u}) {
        for (uint32_t bw : {1u, 3u, 8u}) {
            for (uint32_t y = 0; y < 224u; ++y) {
                for (uint32_t x = 0; x < 64u; ++x) {
                    const uint32_t value = 0x80000000u | (y << 8u) | x;
                    writer.write(GS_PSM_CT32, base, bw, x, y + 32u, value);
                    if (writer.read(GS_PSM_CT32, base, bw, bw * 64u + x, y) != value) {
                        std::fprintf(stderr, "FAIL: CT32 right column must alias the next page row\n");
                        return false;
                    }
                }
            }
        }
    }

    for (bool gpuTail : {false, true}) {
        for (bool extraColumn : {false, true}) {
            harness.begin();
            auto state = baseState();
            state.context.frame.fbw = 3u;
            state.context.scissor = {0u, 191u, 0u, 191u};
            const uint32_t base = state.context.frame.fbp << 5u;
            for (auto *storage : {&harness.gpuVram, &harness.cpuVram}) {
                writer.attach(storage->data(), kVramBytes);
                for (uint32_t y = 192u; y < 224u; ++y)
                    for (uint32_t x = 0; x < 3u; ++x)
                        writer.write(GS_PSM_CT32, base, 3u, x, y,
                                     gpuTail ? rgba(0,248,0,128) : rgba(0,0,248,128));
            }
            // GPU rows must supply the aliased column: their backing VRAM is
            // still zero. The tail is either CPU data or a separate live target.
            harness.submit(spriteBatch(state, 0, 0, 192, 192, rgba(248,0,0,128)));
            harness.submit(spriteBatch(state, 0, 32, 3, 192, rgba(0,248,0,128)));
            if (gpuTail) {
                auto neighbor = baseState();
                neighbor.context.frame.fbp += 18u;
                neighbor.context.frame.fbw = 1u;
                neighbor.context.scissor = {0u, 63u, 0u, 31u};
                harness.submit(spriteBatch(neighbor, 0, 0, 64, 32, rgba(0,0,248,128)));
            }
            state.prim.tme = state.prim.fst = true;
            state.linearFilter = true;
            state.textureWidth = state.textureHeight = 256u;
            state.context.tex0.tbp0 = base;
            state.context.tex0.tbw = 3u;
            state.context.tex0.psm = GS_PSM_CT32;
            state.context.tex0.tfx = state.context.tex0.tcc = 1u;
            state.context.clamp = 5u;
            auto copy = spriteBatch(state, 160, 0, 176, 192, 0x80808080u);
            copy.vertices[0].u = copy.vertices[1].u = (extraColumn ? 193u : 192u) * 16u + 4u;
            copy.vertices[0].v = 8u;
            copy.vertices[1].v = 192u * 16u + 8u;
            harness.submit(copy);
            harness.gpu->Flush();
            const auto stats = harness.gpu->stats();
            if (stats.feedbackCopies != (extraColumn ? 0u : 1u) ||
                stats.colorResolves != uint32_t(gpuTail) + uint32_t(extraColumn)) {
                std::fprintf(stderr, "FAIL: one-column feedback must copy GPU rows and settle only its tail "
                                     "(gpuTail=%u extra=%u copies=%llu resolves=%llu)\n",
                             gpuTail, extraColumn, static_cast<unsigned long long>(stats.feedbackCopies),
                             static_cast<unsigned long long>(stats.colorResolves));
                return false;
            }
            harness.finish();
            for (uint32_t y = 0; y < 192u; ++y) {
                const uint32_t got = harness.reader.read(GS_PSM_CT32, base, 3u, 168u, y);
                const uint32_t want = extraColumn
                    ? (y < 160u ? rgba(0,248,0,128) : rgba(0,0,248,128))
                    : (y < 160u ? rgba(62,186,0,128) : rgba(62,0,186,128));
                if (reportBackendError(harness, "feedback right column") || got != want) {
                    std::fprintf(stderr, "FAIL: feedback right column: gpuTail=%u extra=%u y=%u "
                                         "got=%08x want=%08x\n", gpuTail, extraColumn, y, got, want);
                    return false;
                }
            }
        }
    }
    return true;
}

bool reinterpretedColorTargetsCase(Harness &harness) {
    for (uint32_t variant = 0u; variant < 4u; ++variant) {
        const bool compatible = variant == 0u || variant == 3u;
        harness.begin();
        // Every halfword participates, including alpha and low channel bits.
        // A round trip alone could hide mutually inverse mapping mistakes, so
        // compare complete VRAM independently after each conversion direction.
        uint32_t random = 0x763a2905u;
        for (uint32_t i = 0u; i < kVramBytes; i += 4u) {
            random ^= random << 13u;
            random ^= random >> 17u;
            random ^= random << 5u;
            std::memcpy(harness.gpuVram.data() + i, &random, sizeof(random));
        }
        harness.cpuVram = harness.gpuVram;
        // Assignment must not leave the CPU backend pointing at old storage.
        harness.cpu.Initialize(harness.cpuVram.data(), kVramBytes);
        auto source = baseState();
        if (variant == 3u) source.context.frame.fbp = 510u; // wraps through VRAM page zero
        harness.submit(spriteBatch(source, 8, 4, 16, 12, rgba(48,96,144,160)));
        auto target = source;
        target.context.frame.psm = GS_PSM_CT16;
        target.context.frame.fbmsk = 0xffffffffu;
        target.context.scissor.y1 = variant == 2u ? 126u : 127u;
        if (variant == 1u) {
            target.context.frame.fbw = 1u;
            target.context.scissor.x1 = 63u;
        }
        harness.submit(spriteBatch(target, 0, 0, 16, 16, 0u));
        harness.gpu->Flush();
        if (reportBackendError(harness, "CT32 to CT16") ||
            harness.gpu->stats().colorResolves != (compatible ? 0u : 1u)) {
            std::fprintf(stderr, "FAIL: exact CT32/CT16 views should transfer ownership on GPU; "
                                 "mismatched pitches or extents must resolve (variant=%u)\n", variant);
            return false;
        }
        harness.finish();
        if (harness.snapshot != harness.cpuVram) {
            std::fprintf(stderr, "FAIL: CT32 to CT16 must preserve every GS byte (variant=%u)\n", variant);
            return false;
        }
        const auto before = harness.gpu->stats().colorResolves;
        target.context.frame.fbmsk = 0u;
        harness.submit(spriteBatch(target, 9, 17, 31, 39, rgba(40,80,120,128)));
        source.context.frame.fbmsk = 0xffffffffu;
        harness.submit(spriteBatch(source, 0, 0, 16, 16, 0u));
        harness.gpu->Flush();
        if (reportBackendError(harness, "CT16 to CT32") ||
            harness.gpu->stats().colorResolves != before + (compatible ? 0u : 1u)) {
            std::fprintf(stderr, "FAIL: CT16 to CT32 ownership must follow the compatible view\n");
            return false;
        }
        if (variant == 0u) {
            // A CPU write after ownership moved must survive later aliases.
            GSTransferCommand command{};
            command.direction = 0u;
            command.bitbltbuf.dbp = source.context.frame.fbp << 5u;
            command.bitbltbuf.dbw = kFrameWidthBlocks;
            command.bitbltbuf.dpsm = GS_PSM_CT32;
            command.trxpos.dsax = 64u;
            command.trxpos.dsay = 32u;
            command.trxreg.rrw = command.trxreg.rrh = 8u;
            const std::vector<uint32_t> payload(64u, rgba(17,34,51,68));
            harness.gpu->BeginTransfer(command);
            harness.cpu.BeginTransfer(command);
            harness.gpu->UploadImage(reinterpret_cast<const uint8_t *>(payload.data()), 256u);
            harness.cpu.UploadImage(reinterpret_cast<const uint8_t *>(payload.data()), 256u);
            target.context.frame.fbmsk = 0xffffffffu;
            harness.submit(spriteBatch(target, 0, 0, 16, 16, 0u));
            harness.submit(spriteBatch(source, 0, 0, 16, 16, 0u));
        }
        harness.finish();
        if (harness.snapshot != harness.cpuVram) {
            std::fprintf(stderr, "FAIL: CT16 to CT32 must preserve every GS byte (variant=%u)\n", variant);
            return false;
        }
    }
    return true;
}

bool hostPatchOwnershipCase(Harness &harness) {
    for (uint32_t variant = 0u; variant < 6u; ++variant) {
        harness.begin();
        dq8::gfx::GsVram cpuMemory;
        cpuMemory.attach(harness.cpuVram.data(), kVramBytes);
        dq8::gfx::GsTransferEngine cpuTransfer(cpuMemory);
        auto source = baseState();
        source.context.frame.fbw = 3u;
        source.context.scissor = {0u, 191u, 0u, 191u};
        if (variant == 1u) source.context.frame.fbp = 510u;
        const uint32_t base = source.context.frame.fbp << 5u;
        harness.submit(spriteBatch(source, 0, 0, 192, 192, rgba(24,48,96,128)));
        GSTransferCommand command{};
        command.direction = 0u;
        command.bitbltbuf.dbp = (base + 28u) & 0x3fffu;
        command.bitbltbuf.dbw = 1u;
        command.bitbltbuf.dpsm = GS_PSM_CT32;
        command.trxpos.dsax = 3u;
        command.trxpos.dsay = 5u;
        command.trxreg.rrw = 13u;
        command.trxreg.rrh = 11u;
        std::vector<uint32_t> payload(143u);
        for (uint32_t i = 0u; i < payload.size(); ++i) payload[i] = 0x80402010u + i * 0x010203u;
        harness.gpu->BeginTransfer(command);
        cpuTransfer.begin(command);
        auto upload = [&](uint32_t offset, uint32_t count) {
            const auto *data = reinterpret_cast<const uint8_t *>(payload.data()) + offset;
            harness.gpu->UploadImage(data, count);
            cpuTransfer.upload(data, count);
        };
        upload(0u, 3u); // no complete pixel yet
        upload(3u, 62u); // one carried byte and a partial row
        if (variant == 2u)
            harness.submit(spriteBatch(source, 8, 8, 24, 24, rgba(96,48,24,128)));
        if (variant == 3u) {
            // Starting another transfer abandons the byte carry, but retains
            // the already completed writes in the old transfer.
            command.trxpos.dsax = 19u;
            harness.gpu->BeginTransfer(command);
            cpuTransfer.begin(command);
            upload(0u, 133u);
        } else {
            upload(65u, 133u);
            upload(198u, static_cast<uint32_t>(payload.size() * 4u) - 198u);
        }
        if (harness.gpu->stats().colorResolves != 0u) {
            std::fprintf(stderr, "FAIL: pitched partial uploads should retain GPU neighbours (variant=%u)\n", variant);
            return false;
        }
        if (variant == 5u) {
            harness.gpu->ClearFramebuffer(source.context, rgba(16,32,48,128));
            harness.cpu.ClearFramebuffer(source.context, rgba(16,32,48,128));
        } else if (variant == 4u) {
            // An unsupported transfer must materialize pending CPU patches
            // before reading or modifying their neighbouring GPU pixels.
            command.direction = 2u;
            command.bitbltbuf.sbp = base;
            command.bitbltbuf.sbw = 3u;
            command.bitbltbuf.spsm = GS_PSM_CT32;
            command.bitbltbuf.dbp = base + 32u;
            command.trxpos.dsax = command.trxpos.dsay = 0u;
            harness.gpu->BeginTransfer(command);
            cpuTransfer.begin(command);
        }
        harness.finish();
        if (reportBackendError(harness, "host patch ownership") || harness.snapshot != harness.cpuVram) {
            std::fprintf(stderr, "FAIL: host patches must preserve every GS byte (variant=%u)\n", variant);
            for (uint32_t i = 0u; i < kVramBytes; ++i) {
                if (harness.snapshot[i] != harness.cpuVram[i]) {
                    std::fprintf(stderr, "first mismatch at %x: gpu=%02x cpu=%02x\n", i,
                                 harness.snapshot[i], harness.cpuVram[i]);
                    break;
                }
            }
            return false;
        }
    }
    return true;
}

bool gpuPageOwnershipCase(Harness &harness) {
    for (uint32_t variant = 0u; variant < 4u; ++variant) {
        const bool supported = variant < 2u;
        harness.begin(0x63u);
        auto source = baseState();
        source.context.scissor.y1 = variant == 2u ? 94u : 95u;
        if (variant == 1u) source.context.frame.fbp = 510u;
        harness.submit(spriteBatch(source, 0, 0, 128, 96, rgba(32,64,96,128)));
        auto target = source;
        target.context.frame.fbp = (source.context.frame.fbp + 1u) % 512u;
        target.context.frame.fbw = 1u;
        target.context.scissor = {0u, 63u, 0u, 127u};
        if (variant == 3u) target.context.frame.psm = GS_PSM_CT16;
        harness.submit(spriteBatch(target, 9, 13, 41, 51, rgba(96,64,32,128)));
        harness.gpu->Flush();
        if ((harness.gpu->stats().colorResolves == 0u) != supported) {
            std::fprintf(stderr, "FAIL: page import must gate partial pages and formats (variant=%u)\n", variant);
            return false;
        }
        // The old owner retains disconnected first and last physical pages.
        // A later write must re-adopt the middle without an old view winning
        // over either the new draw or the two surviving islands.
        if (variant == 1u) {
            harness.submit(spriteBatch(source, 2, 3, 7, 9, rgba(16,48,80,128)));
            harness.gpu->Flush();
            if (harness.gpu->stats().colorResolves != 0u) {
                std::fprintf(stderr, "FAIL: returning to a split GPU owner should copy its shared pages\n");
                return false;
            }
        }
        harness.finish();
        if (reportBackendError(harness, "GPU page ownership") || harness.snapshot != harness.cpuVram) {
            std::fprintf(stderr, "FAIL: split GPU page ownership must preserve all GS bytes (variant=%u)\n", variant);
            return false;
        }
    }
    return true;
}

bool hostPaletteOwnershipCase(Harness &harness) {
    harness.begin();
    dq8::gfx::GsVram cpuMemory;
    cpuMemory.attach(harness.cpuVram.data(), kVramBytes);
    dq8::gfx::GsTransferEngine cpuTransfer(cpuMemory);
    auto source = baseState();
    harness.submit(spriteBatch(source, 0, 0, 128, 64, rgba(32,64,96,128)));
    GSTransferCommand command{};
    command.direction = 0u;
    command.bitbltbuf.dbp = (kFramePage << 5u) + 28u;
    command.bitbltbuf.dbw = 1u;
    command.bitbltbuf.dpsm = GS_PSM_CT32;
    command.trxreg.rrw = command.trxreg.rrh = 16u;
    std::vector<uint32_t> palette(256u);
    for (uint32_t i = 0u; i < palette.size(); ++i) palette[i] = rgba(i, 255u - i, i / 2u, 128u);
    harness.gpu->BeginTransfer(command);
    cpuTransfer.begin(command);
    for (uint32_t offset = 0u; offset < 1024u;) {
        const uint32_t count = std::min(71u, 1024u - offset);
        const auto *data = reinterpret_cast<const uint8_t *>(palette.data()) + offset;
        harness.gpu->UploadImage(data, count);
        cpuTransfer.upload(data, count);
        offset += count;
    }
    auto destination = baseState();
    destination.context.frame.fbp += 32u;
    destination.prim.tme = destination.prim.fst = true;
    destination.textureWidth = destination.textureHeight = 16u;
    destination.context.tex0.tbp0 = 0x2000u;
    destination.context.tex0.tbw = 2u;
    destination.context.tex0.psm = GS_PSM_T8;
    destination.context.tex0.cbp = command.bitbltbuf.dbp;
    destination.context.tex0.cpsm = GS_PSM_CT32;
    destination.context.tex0.tfx = destination.context.tex0.tcc = 1u;
    auto batch = spriteBatch(destination, 8, 8, 24, 24, 0x80808080u);
    batch.vertices[1].u = batch.vertices[1].v = 16u * 16u;
    harness.submit(batch);
    harness.gpu->Flush();
    if (harness.gpu->stats().colorResolves != 0u) {
        std::fprintf(stderr, "FAIL: CPU-current palette words must not resolve their GPU-owned neighbours\n");
        return false;
    }
    harness.finish();
    if (reportBackendError(harness, "host palette ownership") || harness.snapshot != harness.cpuVram) {
        std::fprintf(stderr, "FAIL: palette upload/read must preserve every GS byte\n");
        return false;
    }
    return true;
}

bool disjointHostInvalidationCase(Harness &harness) {
    for (uint32_t variant = 0u; variant < 2u; ++variant) {
        harness.gpu->setResolutionScale(variant == 0u ? 1u : 2u);
        harness.begin();
        auto state = baseState();
        state.context.frame.fbw = 3u;
        state.context.frame.psm = variant == 0u ? GS_PSM_CT16 : GS_PSM_CT32;
        const uint32_t height = variant == 0u ? 64u : 32u;
        state.context.scissor = {64u, 127u, 0u, static_cast<uint16_t>(height - 1u)};
        harness.submit(spriteBatch(state, 64, 0, 128, height, rgba(32,64,96,128)));
        GSTransferCommand command{};
        command.direction = 0u;
        command.bitbltbuf.dbw = 1u;
        command.bitbltbuf.dpsm = GS_PSM_CT32;
        command.trxreg.rrw = command.trxreg.rrh = 8u;
        std::vector<uint32_t> payload(64u, rgba(96,64,32,128));
        for (uint32_t page : {0u, 2u}) {
            command.bitbltbuf.dbp = (state.context.frame.fbp + page) << 5u;
            harness.gpu->BeginTransfer(command);
            harness.cpu.BeginTransfer(command);
            harness.gpu->UploadImage(reinterpret_cast<const uint8_t *>(payload.data()), 256u);
            harness.cpu.UploadImage(reinterpret_cast<const uint8_t *>(payload.data()), 256u);
        }
        // The two invalidated rectangles enclose an untouched GPU-owned page.
        // Unsupported patch formats must materialize it before that merge.
        state.context.scissor.x0 = 0u;
        state.context.scissor.x1 = 191u;
        state.context.frame.fbmsk = 0xffffffffu;
        harness.submit(spriteBatch(state, 0, 0, 192, height, rgba(16,48,80,128)));
        harness.finish();
        if (reportBackendError(harness, "disjoint host invalidation") || harness.snapshot != harness.cpuVram) {
            std::fprintf(stderr, "FAIL: disjoint CPU writes must preserve the intervening GPU page (variant=%u)\n", variant);
            for (uint32_t i = 0u; i < kVramBytes; ++i) {
                if (harness.snapshot[i] != harness.cpuVram[i]) {
                    std::fprintf(stderr, "first mismatch at %x: gpu=%02x cpu=%02x\n", i,
                                 harness.snapshot[i], harness.cpuVram[i]);
                    break;
                }
            }
            return false;
        }
    }
    harness.gpu->setResolutionScale(1u);
    return true;
}

bool paddedLiveTargetCase(Harness &harness) {
    for (uint32_t variant = 0u; variant < 4u; ++variant) {
        const bool gpuTail = (variant & 1u) != 0u;
        const bool texture24 = (variant & 2u) != 0u;
        harness.begin();
        auto source = baseState();
        source.context.frame.fbp += 32u;
        source.context.frame.fbw = 3u;
        source.context.scissor = {0u, 191u, 0u, 191u};
        const uint32_t base = source.context.frame.fbp << 5u;
        dq8::gfx::GsVram writer;
        for (auto *storage : {&harness.gpuVram, &harness.cpuVram}) {
            writer.attach(storage->data(), kVramBytes);
            for (uint32_t y = 192u; y < 256u; ++y)
                for (uint32_t x = 0; x < 192u; ++x)
                    writer.write(GS_PSM_CT32, base, 3u, x, y,
                                 gpuTail ? rgba(128,128,0,128) : rgba(0,0,128,128));
            if (!gpuTail) {
                for (uint32_t x = 0; x < 192u; ++x)
                    writer.write(GS_PSM_CT32, base, 3u, x, 192u, rgba(0,128,0,128));
                for (uint32_t y = 192u; y < 224u; ++y)
                    writer.write(GS_PSM_CT32, base, 3u, 0u, y, rgba(0,0,128,128));
                writer.write(GS_PSM_CT32, base, 3u, 0u, 224u, rgba(128,128,128,128));
            }
        }
        harness.submit(spriteBatch(source, 0, 0, 192, 192, rgba(128,0,0,128)));
        harness.submit(spriteBatch(source, 0, 32, 1, 192, rgba(0,128,0,128)));
        if (gpuTail) {
            auto neighbor = source;
            neighbor.context.frame.fbp += 18u;
            neighbor.context.scissor.y1 = 63u;
            harness.submit(spriteBatch(neighbor, 0, 0, 192, 64, rgba(0,0,128,128)));
            harness.submit(spriteBatch(neighbor, 0, 0, 192, 1, rgba(0,128,0,128)));
            harness.submit(spriteBatch(neighbor, 0, 0, 1, 32, rgba(0,0,128,128)));
            harness.submit(spriteBatch(neighbor, 0, 32, 1, 33, rgba(128,128,128,128)));
        }
        auto state = baseState();
        state.prim.tme = state.prim.fst = true;
        state.linearFilter = true;
        state.textureWidth = state.textureHeight = 256u;
        state.context.tex0.tbp0 = base;
        state.context.tex0.tbw = 3u;
        state.context.tex0.psm = texture24 ? GS_PSM_CT24 : GS_PSM_CT32;
        state.context.tex0.tfx = state.context.tex0.tcc = 1u;
        state.texa.ta0 = 64u;
        state.context.clamp = 5u;
        auto sample = [&](float x, uint16_t u, uint16_t v) {
            auto batch = spriteBatch(state, x, 8, x + 16, 24, 0x80808080u);
            batch.vertices[0].u = batch.vertices[1].u = u;
            batch.vertices[0].v = batch.vertices[1].v = v;
            harness.submit(batch);
        };
        sample(8, 192u * 16u + 4u, 192u * 16u + 4u); // four corner taps
        sample(24, 192u * 16u + 4u, 64u * 16u + 8u); // live aliased column
        sample(40, 64u * 16u + 8u, 192u * 16u + 4u); // CPU-supplied bottom row
        harness.gpu->Flush();
        auto stats = harness.gpu->stats();
        if (reportBackendError(harness, "padded live target") || stats.feedbackCopies != 3u ||
            stats.texturesFromLiveTargets != 3u || stats.textureFeedbackHazards != 0u ||
            stats.colorResolves != uint32_t(gpuTail)) {
            std::fprintf(stderr, "FAIL: padded live targets must snapshot without resolving the source\n");
            return false;
        }
        sample(56, 193u * 16u + 4u, 64u * 16u + 8u); // unsupported second column
        harness.gpu->Flush();
        stats = harness.gpu->stats();
        if (stats.feedbackCopies != 3u || stats.colorResolves != 1u + uint32_t(gpuTail)) {
            std::fprintf(stderr, "FAIL: wider live-target samples must retain readback\n");
            return false;
        }
        harness.finish();
        const uint8_t alpha = texture24 ? 64u : 128u;
        const uint32_t expected[] = {rgba(80,96,96,alpha), rgba(32,96,0,alpha),
                                     rgba(32,96,0,alpha), rgba(96,32,0,alpha)};
        for (uint32_t i = 0u; i < 4u; ++i) {
            const uint32_t got = harness.gpuPixel(16u + i * 16u, 16u);
            if (got != expected[i]) {
                std::fprintf(stderr, "FAIL: padded live sample %u variant=%u got=%08x want=%08x\n",
                             i, variant, got, expected[i]);
                return false;
            }
        }
    }
    return true;
}

bool disabledDepthWriteCase(Harness &harness) {
    harness.begin();
    auto state = baseState();
    state.context.zbuf.zmask = false;
    harness.submit(spriteBatch(state, 0, 0, 16, 16, rgba(64,0,0,128), 100));
    state.context.test = 0u;
    harness.submit(spriteBatch(state, 0, 0, 16, 16, rgba(0,64,0,128), 300));
    state.context.test = kDepthGEqual;
    harness.submit(spriteBatch(state, 0, 0, 16, 16, rgba(0,0,64,128), 200));
    harness.finish();
    if (reportBackendError(harness, "ZTE disabled") ||
        harness.gpuPixel(8, 8) != rgba(0,0,64,128)) {
        std::fprintf(stderr, "FAIL: disabling ZTE must also disable depth writes\n");
        return false;
    }
    return true;
}

bool inactiveDepthAttachmentCase(Harness &harness) {
    harness.begin();
    auto state = baseState();
    state.context.test = kDepthGEqual;
    state.context.zbuf.zmask = false;
    const uint32_t nearColor = rgba(0, 128, 0, 128);
    harness.submit(spriteBatch(state, 0, 0, 16, 16, nearColor, 5000));
    harness.finish();

    // A larger colour-only pass must not resize and discard the live depth.
    auto colorOnly = baseState();
    colorOnly.context.scissor.y1 = 127;
    harness.submit(spriteBatch(colorOnly, 32, 32, 48, 48, rgba(0, 0, 128, 128)));
    harness.submit(spriteBatch(state, 0, 0, 16, 16, rgba(128, 0, 0, 128), 100));
    harness.finish();
    if (reportBackendError(harness, "inactive depth attachment") ||
        harness.gpuPixel(8, 8) != nearColor || harness.cpuPixel(8, 8) != nearColor) {
        std::fprintf(stderr, "FAIL: masked ALWAYS depth must preserve earlier depth writes\n");
        return false;
    }
    return true;
}

bool undersizedLiveTargetCase(Harness &harness) {
    harness.begin();
    const uint32_t outside = rgba(192,48,24,128);
    auto source = baseState();
    source.context.frame.fbp += 8u;
    dq8::gfx::GsVram writer;
    writer.attach(harness.gpuVram.data(), kVramBytes);
    for (uint32_t y = 64; y < 128; ++y)
        for (uint32_t x = 0; x < 128; ++x)
            writer.write(GS_PSM_CT32, source.context.frame.fbp << 5u,
                         kFrameWidthBlocks, x, y, outside);
    harness.submit(spriteBatch(source, 0, 0, 128, 64, rgba(32,64,96,128)));
    auto state = baseState();
    state.prim.tme = state.prim.fst = true;
    state.textureWidth = state.textureHeight = 128;
    state.context.tex0.tbp0 = source.context.frame.fbp << 5u;
    state.context.tex0.tbw = kFrameWidthBlocks;
    state.context.tex0.psm = GS_PSM_CT24;
    state.context.tex0.tfx = state.context.tex0.tcc = 1;
    state.texa.ta0 = 128;
    auto copy = spriteBatch(state, 0, 0, 16, 16, 0x80808080);
    copy.vertices[0].v = 80 * 16;
    copy.vertices[1].v = 96 * 16;
    copy.vertices[1].u = 16 * 16;
    harness.submit(copy);
    harness.finish();
    if (reportBackendError(harness, "undersized live target") ||
        harness.gpuPixel(8, 8) != outside || harness.gpu->stats().texturesFromLiveTargets != 0) {
        std::fprintf(stderr, "FAIL: live texture taps outside a target must read local memory\n");
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::string error;
    std::unique_ptr<dq8::gfx::SdlGpuBackend> backend = dq8::gfx::createSdlGpuBackend(error);
    if (!backend) {
        std::fprintf(stderr, "SKIP: SDL GPU device unavailable: %s\n", error.c_str());
        return kSkipped;
    }
    const std::string driver = backend->driverName();

    Harness harness(std::move(backend));
    if (!orientationCase(harness) || !colorRoundTripCase(harness) ||
        !depthOrderCase(harness) || !submissionOrderCase(harness) ||
        !scissorCase(harness) || !xyOffsetCase(harness) ||
        !softwareComparisonCase(harness) || !blendMatrixCase(harness) ||
        !texturedSpriteCase(harness) ||
        !resolutionScaleCase(harness) || !transferThenDrawCase(harness) ||
        !dualCircuitGpuCompositionCase(harness) ||
        !preparedPresentationSlotsCase(harness) ||
        !preparedPresentationCancellationCase() ||
        !drawThenTransferPresentCase(harness) || !aliasedColorTargetsCase(harness) ||
        !bilinearTexelCenterCase(harness) || !smallDrawReadbackCase(harness) ||
        !feedbackSnapshotCase(harness) || !liveTargetTexaCase(harness) ||
        !blendAlphaStorageCase(harness) || !feedbackEdgeSamplesCase(harness) ||
        !programmableBlendCase(harness) || !disabledDepthWriteCase(harness) ||
        !inactiveDepthAttachmentCase(harness) ||
        !undersizedLiveTargetCase(harness) || !feedbackTailRowCase(harness) ||
        !feedbackRightColumnCase(harness) ||
        !reinterpretedColorTargetsCase(harness) ||
        !hostPatchOwnershipCase(harness) ||
        !gpuPageOwnershipCase(harness) ||
        !hostPaletteOwnershipCase(harness) ||
        !disjointHostInvalidationCase(harness) ||
        !paddedLiveTargetCase(harness) ||
        !partialTargetRefreshCase(harness) || !independentTargetResolveCase(harness) ||
        !opaqueSpriteRefreshCase(harness))
        return 1;

    const dq8::gfx::SdlGpuStats stats = harness.gpu->stats();
    std::printf("PASS: SDL GPU raster (%s): batches=%llu draws=%llu "
                "avg-triangles=%.2f pipelines=%llu surfaces=%llu resolves=%llu\n",
                driver.c_str(),
                static_cast<unsigned long long>(stats.batches),
                static_cast<unsigned long long>(stats.drawCalls),
                stats.averageTrianglesPerDraw(),
                static_cast<unsigned long long>(stats.pipelinesCreated),
                static_cast<unsigned long long>(stats.surfacesCreated),
                static_cast<unsigned long long>(stats.colorResolves));
    return 0;
}
