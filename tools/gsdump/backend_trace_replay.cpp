#include "gfx/gsdump/gs_backend_trace.h"
#include "gfx/gs/gs_vram.h"
#include "gfx/gsdump/gs_frame_image.h"
#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include <chrono>
#include <cfenv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

using namespace dq8::gfx;

namespace {
void read(std::ifstream &file, void *bytes, size_t size) {
    if (!file.read(static_cast<char *>(bytes), size))
        throw std::runtime_error("Truncated trace");
}
template <class T> T decode(const std::vector<uint8_t> &data) {
    if (data.size() != sizeof(T)) throw std::runtime_error("Incompatible record size");
    T value{};
    std::memcpy(&value, data.data(), sizeof(T));
    return value;
}
} // namespace

int main(int argc, char **argv) try {
    if (const char *rounding = std::getenv("DQ8_GS_REPLAY_ROUNDING")) {
        if (std::strcmp(rounding, "zero") == 0) std::fesetround(FE_TOWARDZERO);
        else if (std::strcmp(rounding, "nearest") != 0) throw std::runtime_error("Unknown replay rounding mode");
    }
    if (argc != 4 || (std::string(argv[2]) != "sw" && std::string(argv[2]) != "sdlgpu")) {
        std::fprintf(stderr, "Usage: dq8-gs-trace-replay TRACE sw|sdlgpu OUTPUT_PREFIX\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    std::array<uint32_t, 6> header{};
    read(file, header.data(), sizeof(header));
    if (header != kGsTraceHeader) throw std::runtime_error("Incompatible trace ABI");
    uint32_t size = 0;
    read(file, &size, sizeof(size));
    if (size != 4u * 1024u * 1024u) throw std::runtime_error("Invalid VRAM size");
    std::vector<uint8_t> vram(size);
    read(file, vram.data(), vram.size());
    std::string error;
    std::unique_ptr<GSRasterBackend> backend;
    SdlGpuBackend *gpu = nullptr;
    if (std::string(argv[2]) == "sw") backend = std::make_unique<GSCpuBackend>();
    else {
        auto hardware = createSdlGpuBackend(error);
        if (!hardware) throw std::runtime_error(error);
        gpu = hardware.get();
        backend = std::move(hardware);
    }
    const char *workerValue = std::getenv("DQ8_GS_WORKER");
    const bool worker = workerValue && std::strcmp(workerValue, "1") == 0;
    if (worker)
        backend = std::make_unique<GSThreadedBackend>(std::move(backend));
    backend->Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    const bool traceDraws = std::getenv("DQ8_GS_TRACE_DRAWS") != nullptr;
    const bool traceTransfers = std::getenv("DQ8_GS_TRACE_TRANSFERS") != nullptr;
    if (traceDraws)
        std::setvbuf(stdout, nullptr, _IONBF, 0u);
    const char *probeValue = std::getenv("DQ8_GS_TRACE_PROBE_DRAW");
    const uint64_t probeDraw = probeValue ? std::stoull(probeValue) : std::numeric_limits<uint64_t>::max();
    uint64_t records = 0, draws = 0, frames = 0;
    double seconds = 0;
    while (file.peek() != std::char_traits<char>::eof()) {
        GsTraceOp op;
        read(file, &op, sizeof(op));
        read(file, &size, sizeof(size));
        if (size > 16u * 1024u * 1024u) throw std::runtime_error("Oversized record");
        std::vector<uint8_t> data(size);
        read(file, data.data(), data.size());
        ++records;
        if (traceTransfers)
            std::fprintf(stderr, "[replay] record=%llu frame=%llu op=%u bytes=%u\n",
                static_cast<unsigned long long>(records), static_cast<unsigned long long>(frames),
                static_cast<unsigned>(op), size);
        const auto start = std::chrono::steady_clock::now();
        switch (op) {
        case GsTraceOp::Submit: {
            const auto batch = decode<GSPrimitiveBatch>(data);
            if (batch.vertexCount > 3u) throw std::runtime_error("Invalid primitive");
            if (traceDraws) {
                const auto &s = batch.state;
                const auto &c = s.context;
                std::printf("draw=%llu frame=%llu type=%u dst=%x/%u/%u mask=%08x src=%x/%u/%u %ux%u tme=%u abe=%u alpha=%llx test=%llx linear=%u clamp=%llx zbuf=%x/%u/%u\n",
                    static_cast<unsigned long long>(draws), static_cast<unsigned long long>(frames),
                    s.prim.type, c.frame.fbp * 32u, c.frame.fbw, c.frame.psm, c.frame.fbmsk,
                    c.tex0.tbp0, c.tex0.tbw, c.tex0.psm, s.textureWidth, s.textureHeight,
                    s.prim.tme, s.prim.abe, static_cast<unsigned long long>(c.alpha),
                    static_cast<unsigned long long>(c.test), s.linearFilter,
                    static_cast<unsigned long long>(c.clamp), c.zbuf.zbp * 32u,
                    c.zbuf.psm, c.zbuf.zmask);
                for (unsigned i = 0; i < batch.vertexCount; ++i) {
                    const auto &v = batch.vertices[i];
                    std::printf("  xy=%.4f,%.4f z=%.0f uv=%u,%u stq=%.5g,%.5g,%.5g rgba=%u,%u,%u,%u\n",
                        v.x - c.xyoffset.ofx / 16.0f, v.y - c.xyoffset.ofy / 16.0f,
                        double(v.z), v.u, v.v, v.s, v.t, v.q, v.r, v.g, v.b, v.a);
                }
            }
            backend->Submit(batch);
            if (draws == probeDraw) {
                std::vector<uint8_t> snapshot;
                backend->SnapshotVram(snapshot);
                GsVram pixels;
                pixels.attach(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
                const auto &context = batch.state.context;
                FrameImage color, alpha;
                color.resize(context.scissor.x1 + 1u, context.scissor.y1 + 1u);
                alpha.resize(color.width, color.height);
                for (uint32_t y = 0u; y < color.height; ++y) {
                    for (uint32_t x = 0u; x < color.width; ++x) {
                        uint32_t rgba = pixels.read(context.frame.psm, context.frame.fbp << 5u,
                                                    context.frame.fbw, x, y);
                        if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S) {
                            const uint32_t r = rgba & 31u, g = (rgba >> 5u) & 31u, b = (rgba >> 10u) & 31u;
                            rgba = ((r << 3u) | (r >> 2u)) | (((g << 3u) | (g >> 2u)) << 8u) |
                                   (((b << 3u) | (b >> 2u)) << 16u) | ((rgba & 0x8000u) << 16u);
                        }
                        const size_t offset = (static_cast<size_t>(y) * color.width + x) * 4u;
                        std::memcpy(color.rgba.data() + offset, &rgba, 4u);
                        color.rgba[offset + 3u] = 255u;
                        std::memset(alpha.rgba.data() + offset, static_cast<uint8_t>(rgba >> 24u), 3u);
                        alpha.rgba[offset + 3u] = 255u;
                    }
                }
                if (!writePng(std::string(argv[3]) + "-probe-color.png", color, error) ||
                    !writePng(std::string(argv[3]) + "-probe-alpha.png", alpha, error))
                    throw std::runtime_error(error);
                std::printf("Probed draw=%llu target=%x/%u/%u\n", static_cast<unsigned long long>(draws),
                            context.frame.fbp << 5u, context.frame.fbw, context.frame.psm);
                return 0;
            }
            ++draws; break;
        }
        case GsTraceOp::Transfer: {
            const auto command = decode<GSTransferCommand>(data);
            if (traceTransfers)
                std::fprintf(stderr, "[transfer] record=%llu frame=%llu dir=%u src=%x/%u/%u@%u,%u dst=%x/%u/%u@%u,%u size=%ux%u order=%u\n",
                    static_cast<unsigned long long>(records), static_cast<unsigned long long>(frames), command.direction,
                    command.bitbltbuf.sbp, command.bitbltbuf.sbw, command.bitbltbuf.spsm,
                    command.trxpos.ssax, command.trxpos.ssay,
                    command.bitbltbuf.dbp, command.bitbltbuf.dbw, command.bitbltbuf.dpsm,
                    command.trxpos.dsax, command.trxpos.dsay,
                    command.trxreg.rrw, command.trxreg.rrh, command.trxpos.dir);
            backend->BeginTransfer(command);
            break;
        }
        case GsTraceOp::Upload: backend->UploadImage(data.data(), size); break;
        case GsTraceOp::Flush: backend->Flush(); break;
        case GsTraceOp::TextureFlush: backend->TextureFlush(); break;
        case GsTraceOp::Reset: backend->Reset(); break;
        case GsTraceOp::Sync: backend->Sync(decode<GSSyncReason>(data)); break;
        case GsTraceOp::Clear: {
            uint32_t rgba;
            read(file, &rgba, sizeof(rgba));
            backend->ClearFramebuffer(decode<GSContext>(data), rgba); break;
        }
        case GsTraceOp::Write: {
            const auto a = decode<std::array<uint32_t, 6>>(data);
            backend->WriteVram(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        }
        case GsTraceOp::Read: {
            const auto a = decode<std::array<uint32_t, 5>>(data);
            backend->ReadVram(a[0], a[1], a[2], a[3], a[4]); break;
        }
        case GsTraceOp::Consume: {
            const auto count = decode<uint32_t>(data);
            if (count > vram.size()) throw std::runtime_error("Oversized readback");
            std::vector<uint8_t> destination(count);
            backend->ConsumeLocalToHostBytes(destination.data(), count); break;
        }
        case GsTraceOp::Present: {
            const auto frame = backend->Present(decode<GSPresentationRequest>(data));
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (!frame.HasHostPixels()) throw std::runtime_error("No host frame");
            FrameImage image;
            image.resize(frame.width, frame.height);
            const uint32_t pitch = frame.rowPitchBytes ? frame.rowPitchBytes : frame.width * 4u;
            for (uint32_t y = 0; y < frame.height; ++y)
                std::memcpy(image.rgba.data() + y * frame.width * 4u, frame.pixels.data() + y * pitch, frame.width * 4u);
            const std::string path = std::string(argv[3]) + "-" + std::to_string(++frames);
            if (!writePng(path + ".png", image, error) || !writeRaw32(path + ".raw32", image, error))
                throw std::runtime_error(error);
            continue;
        }
        default: throw std::runtime_error("Unknown trace record");
        }
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    if (worker) {
        const auto start = std::chrono::steady_clock::now();
        backend->Sync(GSSyncReason::Finish);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("execution=worker (backend-seconds measures caller time, not worker CPU time)\n");
    }
    if (gpu && !gpu->lastError().empty()) throw std::runtime_error(gpu->lastError());
    std::printf("records=%llu draws=%llu frames=%llu backend-seconds=%.3f\n",
        static_cast<unsigned long long>(records), static_cast<unsigned long long>(draws),
        static_cast<unsigned long long>(frames), seconds);
    if (gpu) {
        const auto s = gpu->stats();
        std::printf("resolves=%llu resolved-pixels=%llu refreshes=%llu draws=%llu passes=%llu feedback-copies=%llu\n",
            static_cast<unsigned long long>(s.colorResolves), static_cast<unsigned long long>(s.resolvedPixels),
            static_cast<unsigned long long>(s.colorRefreshes), static_cast<unsigned long long>(s.drawCalls),
            static_cast<unsigned long long>(s.renderPasses),
            static_cast<unsigned long long>(s.feedbackCopies));
        std::printf("presents: native=%llu cpu-composed=%llu gpu-composed=%llu\n",
            static_cast<unsigned long long>(s.nativePresents),
            static_cast<unsigned long long>(s.composedPresents),
            static_cast<unsigned long long>(s.gpuComposedPresents));
        std::printf("approximations: blends=%llu saturated=%llu destination-alpha-factor=%llu "
                    "bit-masks=%llu destination-alpha-test=%llu alpha-fail=%llu "
                    "color-wrap=%llu untranslated-textures=%llu\n",
            static_cast<unsigned long long>(s.inexactBlends),
            static_cast<unsigned long long>(s.saturatedBlendFactors),
            static_cast<unsigned long long>(s.destinationAlphaFactors),
            static_cast<unsigned long long>(s.partialChannelMasks),
            static_cast<unsigned long long>(s.destinationAlphaTests),
            static_cast<unsigned long long>(s.alphaFailModes),
            static_cast<unsigned long long>(s.disabledColorClamps),
            static_cast<unsigned long long>(s.untranslatedTextures));
    }
} catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what()); return 1;
}
